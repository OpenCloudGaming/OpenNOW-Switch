#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent.h"
#include "config.h"
#include "dtls_srtp.h"
#include "ice_candidate_pair_policy.h"
#include "peer_connection.h"
#include "ports.h"
#include "rtcp.h"
#include "rtp.h"
#include "sctp.h"
#include "sdp.h"

#define STATE_CHANGED(pc, curr_state)                                  \
  if (pc->state != curr_state) {                                       \
    pc->state = curr_state;                                            \
    if (pc->oniceconnectionstatechange) {                              \
      pc->oniceconnectionstatechange(curr_state, pc->config.user_data); \
    }                                                                 \
  }

struct PeerConnection {
  PeerConfiguration config;
  PeerConnectionState state;
  Agent agent;
  DtlsSrtp dtls_srtp;
  Sctp sctp;

  char sdp[CONFIG_SDP_BUFFER_SIZE];

  void (*onicecandidate)(char* sdp, void* user_data);
  void (*oniceconnectionstatechange)(PeerConnectionState state, void* user_data);
  void (*on_connected)(void* userdata);
  void (*on_receiver_packet_loss)(float fraction_loss, uint32_t total_loss, void* user_data);

  uint8_t temp_buf[CONFIG_MTU];
  uint8_t agent_buf[CONFIG_MTU];
  int agent_ret;
  int b_local_description_created;
  int remote_ice_lite;

  RtpEncoder artp_encoder;
  RtpEncoder vrtp_encoder;
  RtpDecoder vrtp_decoder;
  RtpDecoder artp_decoder;

  uint32_t remote_assrc;
  uint32_t remote_vssrc;
  uint16_t video_last_nack_expected;
  uint32_t video_last_nack_ms;
  int video_has_last_nack;
  uint32_t video_nack_requests;
  uint32_t video_nack_packets_requested;

  int dtls_handshake_attempts;
  uint32_t dtls_handshake_started_ms;
  int sctp_create_attempted;
  int completed_udp_packets;
  int completed_rtcp_packets;
  int completed_dtls_packets;
  int completed_rtp_packets;
  int completed_unknown_packets;
  int rtp_decrypt_failures;
  int rtcp_decrypt_failures;
  int sctp_read_events;
};

static int peer_connection_send_rtcp_nack(PeerConnection* pc, uint32_t ssrc,
                                          uint16_t packet_id, uint16_t bitmask);

static int peer_connection_diagnostics_enabled;

void peer_connection_set_diagnostics_enabled(int enabled) {
  peer_connection_diagnostics_enabled = enabled != 0;
  sctp_set_diagnostics_enabled(enabled);
}

static void peer_connection_diag_log(const char* fmt, ...) {
  if (!peer_connection_diagnostics_enabled)
    return;
  FILE* file = fopen("sdmc:/switch/OpenNOWSwitch/signaling.log", "a");
  FILE* trace = fopen("sdmc:/switch/OpenNOWSwitch/stream_trace.log", "a");
  const int input_relevant = strstr(fmt, "sctp") != NULL ||
                             strstr(fmt, "dtls") != NULL ||
                             strstr(fmt, "transport_completed") != NULL;
  FILE* input = input_relevant ? fopen("sdmc:/switch/OpenNOWSwitch/input.log", "a") : NULL;

  if (file) {
    fputs("LIBPEER ", file);
  }
  if (trace) {
    fputs("LIBPEER ", trace);
  }
  if (input) {
    fputs("LIBPEER ", input);
  }

  va_list args;
  va_start(args, fmt);
  if (file) {
    va_list copy;
    va_copy(copy, args);
    vfprintf(file, fmt, copy);
    va_end(copy);
  }
  if (trace) {
    va_list copy;
    va_copy(copy, args);
    vfprintf(trace, fmt, copy);
    va_end(copy);
  }
  if (input) {
    vfprintf(input, fmt, args);
  }
  va_end(args);

  if (file) {
    fputc('\n', file);
    fclose(file);
  }
  if (trace) {
    fputc('\n', trace);
    fclose(trace);
  }
  if (input) {
    fputc('\n', input);
    fclose(input);
  }
}

static int peer_connection_video_packet_buffered(const RtpDecoder* decoder,
                                                 uint16_t sequence) {
  const size_t slot = sequence % RTP_REORDER_WINDOW;
  return decoder->reorder_used[slot] && decoder->reorder_sequences[slot] == sequence;
}

static uint32_t peer_connection_nack_packet_count(uint16_t bitmask) {
  uint32_t count = 1;
  while (bitmask != 0) {
    count += bitmask & 1u;
    bitmask >>= 1;
  }
  return count;
}

static void peer_connection_maybe_send_video_nacks(PeerConnection* pc,
                                                   uint16_t newest_sequence) {
  RtpDecoder* decoder = &pc->vrtp_decoder;
  if (!decoder->reorder_has_expected_sequence || pc->remote_vssrc == 0)
    return;

  const uint16_t expected = decoder->reorder_expected_sequence;
  int16_t distance = (int16_t)(newest_sequence - expected);
  if (distance <= 0)
    return;
  if (distance > RTP_REORDER_MAX_HOLD_PACKETS)
    distance = RTP_REORDER_MAX_HOLD_PACKETS;

  const uint32_t now_ms = ports_get_epoch_time();
  if (pc->video_has_last_nack && pc->video_last_nack_expected == expected &&
      (uint32_t)(now_ms - pc->video_last_nack_ms) < 10) {
    return;
  }

  int offset = 0;
  while (offset < distance) {
    while (offset < distance && peer_connection_video_packet_buffered(
             decoder, (uint16_t)(expected + offset))) {
      offset++;
    }
    if (offset >= distance)
      break;

    const uint16_t packet_id = (uint16_t)(expected + offset);
    uint16_t bitmask = 0;
    for (int bit = 0; bit < 16 && offset + bit + 1 < distance; ++bit) {
      const uint16_t sequence = (uint16_t)(packet_id + bit + 1);
      if (!peer_connection_video_packet_buffered(decoder, sequence))
        bitmask |= (uint16_t)(1u << bit);
    }

    if (peer_connection_send_rtcp_nack(
          pc, pc->remote_vssrc, packet_id, bitmask) >= 0) {
      pc->video_nack_requests++;
      pc->video_nack_packets_requested += peer_connection_nack_packet_count(bitmask);
    }
    offset += 17;
  }

  pc->video_has_last_nack = 1;
  pc->video_last_nack_expected = expected;
  pc->video_last_nack_ms = now_ms;
}

static void peer_connection_hex_preview(const uint8_t* data, int len, char* out, size_t out_len) {
  if (!out || out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (!data || len <= 0) {
    return;
  }

  int count = len < 12 ? len : 12;
  size_t pos = 0;
  for (int i = 0; i < count && pos + 4 < out_len; ++i) {
    pos += snprintf(out + pos, out_len - pos, "%s%02x", i == 0 ? "" : " ", data[i]);
  }
  if (len > count && pos + 5 < out_len) {
    snprintf(out + pos, out_len - pos, " ...");
  }
}

static const char* peer_connection_ice_state_name(IceCandidateState state) {
  switch (state) {
    case ICE_CANDIDATE_STATE_FROZEN:
      return "frozen";
    case ICE_CANDIDATE_STATE_WAITING:
      return "waiting";
    case ICE_CANDIDATE_STATE_INPROGRESS:
      return "inprogress";
    case ICE_CANDIDATE_STATE_SUCCEEDED:
      return "succeeded";
    case ICE_CANDIDATE_STATE_FAILED:
      return "failed";
    default:
      return "unknown";
  }
}

static const char* peer_connection_ice_type_name(IceCandidateType type) {
  switch (type) {
    case ICE_CANDIDATE_TYPE_HOST:
      return "host";
    case ICE_CANDIDATE_TYPE_SRFLX:
      return "srflx";
    case ICE_CANDIDATE_TYPE_PRFLX:
      return "prflx";
    case ICE_CANDIDATE_TYPE_RELAY:
      return "relay";
    default:
      return "unknown";
  }
}

static void peer_connection_log_candidate_pairs(PeerConnection* pc, const char* reason) {
  if (!pc) {
    return;
  }

  peer_connection_diag_log("ice_pairs reason=%s total=%d local=%d remote=%d mode=%d iceLite=%d",
                           reason ? reason : "unknown",
                           pc->agent.candidate_pairs_num,
                           pc->agent.local_candidates_count,
                           pc->agent.remote_candidates_count,
                           pc->agent.mode,
                           pc->remote_ice_lite);

  for (int i = 0; i < pc->agent.candidate_pairs_num && i < 12; ++i) {
    IceCandidatePair* pair = &pc->agent.candidate_pairs[i];
    char local[ADDRSTRLEN] = {0};
    char remote[ADDRSTRLEN] = {0};
    if (pair->local) {
      addr_to_string(&pair->local->addr, local, sizeof(local));
    }
    if (pair->remote) {
      addr_to_string(&pair->remote->addr, remote, sizeof(remote));
    }
    peer_connection_diag_log("ice_pair[%d] state=%s selected=%d local=%s/%s:%u remote=%s/%s:%u priority=%" PRIu64,
                             i,
                             peer_connection_ice_state_name(pair->state),
                             pair == pc->agent.selected_pair,
                             pair->local
                                 ? peer_connection_ice_type_name(pair->local->type)
                                 : "none",
                             local,
                             pair->local ? pair->local->addr.port : 0,
                             pair->remote
                                 ? peer_connection_ice_type_name(pair->remote->type)
                                 : "none",
                             remote,
                             pair->remote ? pair->remote->addr.port : 0,
                             pair->priority);
  }
}

static void peer_connection_outgoing_rtp_packet(uint8_t* data, size_t size, void* user_data) {
  PeerConnection* pc = (PeerConnection*)user_data;
  dtls_srtp_encrypt_rtp_packet(&pc->dtls_srtp, data, (int*)&size);
  agent_send(&pc->agent, data, size);
}

static int peer_connection_dtls_srtp_recv(void* ctx, unsigned char* buf, size_t len) {
  DtlsSrtp* dtls_srtp = (DtlsSrtp*)ctx;
  PeerConnection* pc = (PeerConnection*)dtls_srtp->user_data;

  if (pc->agent_ret > 0 && pc->agent_ret <= len) {
    memcpy(buf, pc->agent_buf, pc->agent_ret);
    return pc->agent_ret;
  }

  // DTLS is advanced by peer_connection_loop(). Blocking here starves that
  // loop, so a missing datagram must be reported as "try again", not -1.
  const int ret = agent_recv(&pc->agent, buf, len);
  if (ret > 0)
    return ret;

  return MBEDTLS_ERR_SSL_WANT_READ;
}

static int peer_connection_dtls_srtp_send(void* ctx, const uint8_t* buf, size_t len) {
  DtlsSrtp* dtls_srtp = (DtlsSrtp*)ctx;
  PeerConnection* pc = (PeerConnection*)dtls_srtp->user_data;

  // LOGD("send %.4x %.4x, %ld", *(uint16_t*)buf, *(uint16_t*)(buf + 2), len);
  return agent_send(&pc->agent, buf, len);
}

static void peer_connection_incoming_rtcp(PeerConnection* pc, uint8_t* buf, size_t len) {
  RtcpHeader* rtcp_header;
  size_t pos = 0;

  while (pos < len) {
    rtcp_header = (RtcpHeader*)(buf + pos);

    switch (rtcp_header->type) {
      case RTCP_SR:
        if (pos + 20 <= len && pc->config.onrtpsenderreport) {
          uint32_t sender_ssrc_net, ntp_seconds_net, ntp_fraction_net, rtp_timestamp_net;
          memcpy(&sender_ssrc_net, buf + pos + 4, 4);
          memcpy(&ntp_seconds_net, buf + pos + 8, 4);
          memcpy(&ntp_fraction_net, buf + pos + 12, 4);
          memcpy(&rtp_timestamp_net, buf + pos + 16, 4);
          const uint32_t sender_ssrc = ntohl(sender_ssrc_net);
          const uint32_t ntp_seconds = ntohl(ntp_seconds_net);
          const uint32_t ntp_fraction = ntohl(ntp_fraction_net);
          const uint32_t rtp_timestamp = ntohl(rtp_timestamp_net);
          const uint64_t ntp_us = (uint64_t)ntp_seconds * 1000000ULL +
                                  (((uint64_t)ntp_fraction * 1000000ULL) >> 32);
          pc->config.onrtpsenderreport(sender_ssrc, ntp_us, rtp_timestamp, pc->config.user_data);
        }
        break;
      case RTCP_RR:
        LOGD("RTCP_PR");
        if (rtcp_header->rc > 0) {
// TODO: REMB, GCC ...etc
#if 0
          RtcpRr rtcp_rr = rtcp_parse_rr(buf);
          uint32_t fraction = ntohl(rtcp_rr.report_block[0].flcnpl) >> 24;
          uint32_t total = ntohl(rtcp_rr.report_block[0].flcnpl) & 0x00FFFFFF;
          if(pc->on_receiver_packet_loss && fraction > 0) {

            pc->on_receiver_packet_loss((float)fraction/256.0, total, pc->config.user_data);
          }
#endif
        }
        break;
      case RTCP_PSFB: {
        int fmt = rtcp_header->rc;
        LOGD("RTCP_PSFB %d", fmt);
        // PLI and FIR
        if ((fmt == 1 || fmt == 4) && pc->config.on_request_keyframe) {
          pc->config.on_request_keyframe(pc->config.user_data);
        }
      }
      default:
        break;
    }

    pos += 4 * ntohs(rtcp_header->length) + 4;
  }
}

const char* peer_connection_state_to_string(PeerConnectionState state) {
  switch (state) {
    case PEER_CONNECTION_NEW:
      return "new";
    case PEER_CONNECTION_CHECKING:
      return "checking";
    case PEER_CONNECTION_CONNECTED:
      return "connected";
    case PEER_CONNECTION_COMPLETED:
      return "completed";
    case PEER_CONNECTION_FAILED:
      return "failed";
    case PEER_CONNECTION_CLOSED:
      return "closed";
    case PEER_CONNECTION_DISCONNECTED:
      return "disconnected";
    default:
      return "unknown";
  }
}

PeerConnectionState peer_connection_get_state(PeerConnection* pc) {
  return pc->state;
}

void* peer_connection_get_sctp(PeerConnection* pc) {
  return &pc->sctp;
}

PeerConnection* peer_connection_create(PeerConfiguration* config) {
  PeerConnection* pc = calloc(1, sizeof(PeerConnection));
  if (!pc) {
    return NULL;
  }

  memcpy(&pc->config, config, sizeof(PeerConfiguration));
  pc->state = PEER_CONNECTION_NEW;

  agent_create(&pc->agent);

  memset(&pc->sctp, 0, sizeof(pc->sctp));

  if (pc->config.audio_codec) {
    rtp_encoder_init(&pc->artp_encoder, pc->config.audio_codec,
                     peer_connection_outgoing_rtp_packet, (void*)pc);

    rtp_decoder_init(&pc->artp_decoder, pc->config.audio_codec,
                     pc->config.onaudiotrack, pc->config.user_data);
    rtp_decoder_set_audio_callback(&pc->artp_decoder, pc->config.onaudiopacket);
  }

  if (pc->config.video_codec) {
    rtp_encoder_init(&pc->vrtp_encoder, pc->config.video_codec,
                     peer_connection_outgoing_rtp_packet, (void*)pc);

    rtp_decoder_init(&pc->vrtp_decoder, pc->config.video_codec,
                     pc->config.onvideotrack, pc->config.user_data);
    rtp_decoder_set_video_callback(&pc->vrtp_decoder, pc->config.onvideopacket);
  }

  return pc;
}

void peer_connection_destroy(PeerConnection* pc) {
  if (pc) {
    rtp_decoder_cleanup(&pc->artp_decoder);
    rtp_decoder_cleanup(&pc->vrtp_decoder);
    sctp_destroy_association(&pc->sctp);
    dtls_srtp_deinit(&pc->dtls_srtp);
    agent_destroy(&pc->agent);
    free(pc);
    pc = NULL;
  }
}

void peer_connection_close(PeerConnection* pc) {
  pc->state = PEER_CONNECTION_CLOSED;
}

int peer_connection_send_audio(PeerConnection* pc, const uint8_t* buf, size_t len) {
  if (pc->state != PEER_CONNECTION_COMPLETED) {
    // LOGE("dtls_srtp not connected");
    return -1;
  }
  return rtp_encoder_encode(&pc->artp_encoder, buf, len);
}

int peer_connection_send_video(PeerConnection* pc, const uint8_t* buf, size_t len) {
  if (pc->state != PEER_CONNECTION_COMPLETED) {
    // LOGE("dtls_srtp not connected");
    return -1;
  }
  return rtp_encoder_encode(&pc->vrtp_encoder, buf, len);
}

int peer_connection_datachannel_send(PeerConnection* pc, char* message, size_t len) {
  return peer_connection_datachannel_send_sid(pc, message, len, 0);
}

int peer_connection_datachannel_send_sid(PeerConnection* pc, char* message, size_t len, uint16_t sid) {
  if (!sctp_is_connected(&pc->sctp)) {
    LOGE("sctp not connected");
    return -1;
  }
  if (pc->config.datachannel == DATA_CHANNEL_STRING)
    return sctp_outgoing_data(&pc->sctp, message, len, PPID_STRING, sid);
  else
    return sctp_outgoing_data(&pc->sctp, message, len, PPID_BINARY, sid);
}

int peer_connection_datachannel_send_binary_sid(PeerConnection* pc, char* message, size_t len, uint16_t sid) {
  if (!sctp_is_connected(&pc->sctp)) {
    LOGE("sctp not connected");
    return -1;
  }
  return sctp_outgoing_data(&pc->sctp, message, len, PPID_BINARY, sid);
}

int peer_connection_create_datachannel(PeerConnection* pc, DecpChannelType channel_type, uint16_t priority, uint32_t reliability_parameter, char* label, char* protocol) {
  return peer_connection_create_datachannel_sid(pc, channel_type, priority, reliability_parameter, label, protocol, 0);
}

int peer_connection_create_datachannel_sid(PeerConnection* pc, DecpChannelType channel_type, uint16_t priority, uint32_t reliability_parameter, char* label, char* protocol, uint16_t sid) {
  int rtrn = -1;

  if (!sctp_is_connected(&pc->sctp)) {
#if !CONFIG_USE_USRSCTP
    peer_connection_diag_log("sctp_not_connected waiting_handshake sid=%u label=%s", sid, label ? label : "");
#endif
    LOGE("sctp not connected");
    return rtrn;
  }

  //  0                   1                   2                   3
  //  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |  Message Type |  Channel Type |            Priority           |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |                    Reliability Parameter                      |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |         Label Length          |       Protocol Length         |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |                                                               |
  // |                             Label                             |
  // |                                                               |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |                                                               |
  // |                            Protocol                           |
  // |                                                               |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  int msg_size = 12 + strlen(label) + strlen(protocol);
  uint16_t priority_big_endian = htons(priority);
  uint32_t reliability_big_endian = htonl(reliability_parameter);
  uint16_t label_length = htons(strlen(label));
  uint16_t protocol_length = htons(strlen(protocol));
  char* msg = calloc(1, msg_size);
  if (!msg) {
    return rtrn;
  }

  msg[0] = DATA_CHANNEL_OPEN;
  msg[1] = (char)channel_type;
  memcpy(msg + 2, &priority_big_endian, sizeof(uint16_t));
  memcpy(msg + 4, &reliability_big_endian, sizeof(uint32_t));
  memcpy(msg + 8, &label_length, sizeof(uint16_t));
  memcpy(msg + 10, &protocol_length, sizeof(uint16_t));
  memcpy(msg + 12, label, strlen(label));
  memcpy(msg + 12 + strlen(label), protocol, strlen(protocol));

  rtrn = sctp_outgoing_data(&pc->sctp, msg, msg_size, PPID_CONTROL, sid);
  if (rtrn >= 0) {
    sctp_add_stream_mapping(&pc->sctp, label, sid);
  }
  free(msg);
  return rtrn;
}

static char* peer_connection_dtls_role_setup_value(DtlsSrtpRole d) {
  return d == DTLS_SRTP_ROLE_SERVER ? "a=setup:passive" : "a=setup:active";
}

int peer_connection_loop(PeerConnection* pc) {
  uint32_t ssrc = 0;
  int packet_processed = 0;
  memset(pc->agent_buf, 0, sizeof(pc->agent_buf));
  pc->agent_ret = -1;

  switch (pc->state) {
    case PEER_CONNECTION_NEW:
      break;

    case PEER_CONNECTION_CHECKING:
      if (pc->agent.candidate_pairs_num == 0) {
        break;
      }
      if (agent_select_candidate_pair(&pc->agent) < 0) {
        STATE_CHANGED(pc, PEER_CONNECTION_FAILED);
      } else if (agent_connectivity_check(&pc->agent) == 0) {
        STATE_CHANGED(pc, PEER_CONNECTION_CONNECTED);
      }
      break;

    case PEER_CONNECTION_CONNECTED:
      if (pc->dtls_handshake_attempts == 0) {
        pc->dtls_handshake_started_ms = ports_get_epoch_time();
        peer_connection_diag_log("dtls_start datachannel=%d candidatePairs=%d",
                                 pc->config.datachannel,
                                 pc->agent.candidate_pairs_num);
        peer_connection_log_candidate_pairs(pc, "dtls_start");
      }

      pc->dtls_handshake_attempts++;

      int dtls_ret = dtls_srtp_handshake(&pc->dtls_srtp, NULL);
      if (dtls_ret == 0) {
        LOGD("DTLS-SRTP handshake done");
        peer_connection_diag_log("dtls_done attempts=%d datachannel=%d",
                                 pc->dtls_handshake_attempts,
                                 pc->config.datachannel);

        if (pc->config.datachannel && !pc->sctp_create_attempted) {
          LOGI("SCTP create socket");
#if CONFIG_USE_USRSCTP
          peer_connection_diag_log("sctp_backend=usrsctp");
#else
          peer_connection_diag_log("sctp_backend=internal");
#endif
          peer_connection_diag_log("sctp_create_start");
          pc->sctp_create_attempted = 1;
          int sctp_ret = sctp_create_association(&pc->sctp, &pc->dtls_srtp);
          peer_connection_diag_log("sctp_create_done ret=%d", sctp_ret);
          if (sctp_ret == 0) {
            pc->sctp.userdata = pc->config.user_data;
          } else {
            LOGE("SCTP create socket failed");
          }
        }

        peer_connection_log_candidate_pairs(pc, "transport_completed");
        peer_connection_diag_log("transport_completed");
        STATE_CHANGED(pc, PEER_CONNECTION_COMPLETED);
      } else if ((uint32_t)(ports_get_epoch_time() - pc->dtls_handshake_started_ms) >= 3500) {
        int failed_pairs = agent_fail_nominated_remote(&pc->agent);
        peer_connection_diag_log("dtls_endpoint_timeout attempts=%d failedPairs=%d; trying next remote",
                                 pc->dtls_handshake_attempts,
                                 failed_pairs);
        dtls_srtp_reset_session(&pc->dtls_srtp);
        pc->dtls_handshake_attempts = 0;
        pc->dtls_handshake_started_ms = 0;
        if (failed_pairs > 0) {
          STATE_CHANGED(pc, PEER_CONNECTION_CHECKING);
        } else {
          STATE_CHANGED(pc, PEER_CONNECTION_FAILED);
        }
      } else if (pc->dtls_handshake_attempts == 1 || pc->dtls_handshake_attempts % 600 == 0) {
        peer_connection_diag_log("dtls_pending ret=%d attempts=%d",
                                 dtls_ret,
                                 pc->dtls_handshake_attempts);
      }
      break;
    case PEER_CONNECTION_COMPLETED:
      if ((pc->agent_ret = agent_recv(&pc->agent, pc->agent_buf, sizeof(pc->agent_buf))) > 0) {
        packet_processed = 1;
        LOGD("agent_recv %d", pc->agent_ret);
        pc->completed_udp_packets++;
        char preview[64];
        peer_connection_hex_preview(pc->agent_buf, pc->agent_ret, preview, sizeof(preview));

        if (dtls_srtp_probe(pc->agent_buf)) {
          pc->completed_dtls_packets++;
          if (pc->completed_dtls_packets <= 10 || pc->completed_dtls_packets % 600 == 0) {
            peer_connection_diag_log("udp_class=dtls count=%d bytes=%d preview=%s",
                                     pc->completed_dtls_packets,
                                     pc->agent_ret,
                                     preview);
          }
          int ret = dtls_srtp_read(&pc->dtls_srtp, pc->temp_buf, sizeof(pc->temp_buf));
          LOGD("Got DTLS data %d", ret);

          if (ret > 0) {
            pc->sctp_read_events++;
            if (pc->sctp_read_events <= 10 || pc->sctp_read_events % 600 == 0) {
              char sctp_preview[64];
              peer_connection_hex_preview(pc->temp_buf, ret, sctp_preview, sizeof(sctp_preview));
              peer_connection_diag_log("dtls_read_appdata ret=%d sctpEvents=%d preview=%s",
                                       ret,
                                       pc->sctp_read_events,
                                       sctp_preview);
            }
            sctp_incoming_data(&pc->sctp, (char*)pc->temp_buf, ret);
          } else if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            peer_connection_diag_log("dtls_peer_close_notify packets=%d sctpConnected=%d",
                                     pc->completed_dtls_packets,
                                     sctp_is_connected(&pc->sctp));
            STATE_CHANGED(pc, PEER_CONNECTION_CLOSED);
          } else if (ret < 0 && (pc->completed_dtls_packets <= 10 || pc->completed_dtls_packets % 600 == 0)) {
            peer_connection_diag_log("dtls_read_no_appdata ret=%d dtlsPackets=%d",
                                     ret,
                                     pc->completed_dtls_packets);
          }

        } else if (rtcp_probe(pc->agent_buf, pc->agent_ret)) {
          LOGD("Got RTCP packet");
          pc->completed_rtcp_packets++;
          if (pc->completed_rtcp_packets <= 5 || pc->completed_rtcp_packets % 600 == 0) {
            peer_connection_diag_log("udp_class=rtcp count=%d bytes=%d preview=%s",
                                     pc->completed_rtcp_packets,
                                     pc->agent_ret,
                                     preview);
          }
          if (dtls_srtp_decrypt_rtcp_packet(&pc->dtls_srtp, pc->agent_buf, &pc->agent_ret) != 0) {
            pc->rtcp_decrypt_failures++;
            peer_connection_diag_log("rtcp_decrypt_failed failures=%d bytesAfter=%d",
                                     pc->rtcp_decrypt_failures,
                                     pc->agent_ret);
            break;
          }
          peer_connection_incoming_rtcp(pc, pc->agent_buf, pc->agent_ret);

        } else if (rtp_packet_validate(pc->agent_buf, pc->agent_ret)) {
          LOGD("Got RTP packet");
          pc->completed_rtp_packets++;

          if (pc->completed_rtp_packets <= 10 || pc->completed_rtp_packets % 1200 == 0) {
            peer_connection_diag_log("udp_class=rtp count=%d bytes=%d preview=%s",
                                     pc->completed_rtp_packets,
                                     pc->agent_ret,
                                     preview);
          }

          if (dtls_srtp_decrypt_rtp_packet(&pc->dtls_srtp, pc->agent_buf, &pc->agent_ret) != 0) {
            pc->rtp_decrypt_failures++;
            peer_connection_diag_log("rtp_decrypt_failed failures=%d bytesAfter=%d",
                                     pc->rtp_decrypt_failures,
                                     pc->agent_ret);
            break;
          }

          ssrc = rtp_get_ssrc(pc->agent_buf);
          RtpPacket* rtp = (RtpPacket*)pc->agent_buf;
          const int is_audio_payload = rtp->header.type == PT_PCMU ||
                                       rtp->header.type == PT_PCMA ||
                                       rtp->header.type == PT_OPUS ||
                                       rtp->header.type == 63;
          if (ssrc == pc->remote_assrc || (pc->remote_assrc == 0 && is_audio_payload)) {
            if (pc->remote_assrc == 0) {
              pc->remote_assrc = ssrc;
              LOGI("Learned remote audio SSRC from RTP: %" PRIu32, pc->remote_assrc);
              peer_connection_diag_log("learned_audio_ssrc=%" PRIu32 " pt=%u",
                                       pc->remote_assrc, rtp->header.type);
            }
            rtp_decoder_decode(&pc->artp_decoder, pc->agent_buf, pc->agent_ret);
          } else if (ssrc == pc->remote_vssrc ||
                     (pc->remote_vssrc == 0 &&
                      rtp->header.type != PT_PCMU &&
                      rtp->header.type != PT_PCMA &&
                      rtp->header.type != PT_OPUS &&
                      rtp->header.type != 63)) {
            if (pc->remote_vssrc == 0) {
              pc->remote_vssrc = ssrc;
              LOGI("Learned remote video SSRC from RTP: %" PRIu32, pc->remote_vssrc);
            }
            const uint16_t sequence =
                (uint16_t)(((uint16_t)pc->agent_buf[2] << 8) | pc->agent_buf[3]);
            peer_connection_maybe_send_video_nacks(pc, sequence);
            rtp_decoder_decode(&pc->vrtp_decoder, pc->agent_buf, pc->agent_ret);
            if (pc->completed_rtp_packets % 6000 == 0) {
              peer_connection_diag_log(
                  "rtp_h264 packets=%u gaps=%u reordered=%u late=%u forced=%u buffered=%u nalus=%u auOk=%u auDrop=%u",
                  pc->vrtp_decoder.packets_received,
                  pc->vrtp_decoder.sequence_gaps,
                  pc->vrtp_decoder.reordered_packets,
                  pc->vrtp_decoder.late_packets_dropped,
                  pc->vrtp_decoder.forced_sequence_skips,
                  pc->vrtp_decoder.reorder_buffered_packets,
                  pc->vrtp_decoder.nal_units_completed,
                  pc->vrtp_decoder.access_units_completed,
                  pc->vrtp_decoder.access_units_dropped);
            }
          }

        } else {
          pc->completed_unknown_packets++;
          if (pc->completed_unknown_packets <= 10 || pc->completed_unknown_packets % 600 == 0) {
            peer_connection_diag_log("udp_class=unknown count=%d bytes=%d preview=%s totals udp/rtcp/dtls/rtp/unknown=%d/%d/%d/%d/%d",
                                     pc->completed_unknown_packets,
                                     pc->agent_ret,
                                     preview,
                                     pc->completed_udp_packets,
                                     pc->completed_rtcp_packets,
                                     pc->completed_dtls_packets,
                                     pc->completed_rtp_packets,
                                     pc->completed_unknown_packets);
          }
          LOGW("Unknown data");
        }
      }

      if (CONFIG_KEEPALIVE_TIMEOUT > 0 &&
          pc->agent.binding_request_time > 0 &&
          (ports_get_epoch_time() - pc->agent.binding_request_time) > CONFIG_KEEPALIVE_TIMEOUT) {
        LOGI("binding request timeout; keeping completed transport alive");
        pc->agent.binding_request_time = ports_get_epoch_time();
      }
      rtp_decoder_poll(&pc->vrtp_decoder);

      break;
    case PEER_CONNECTION_FAILED:
      break;
    case PEER_CONNECTION_DISCONNECTED:
      break;
    case PEER_CONNECTION_CLOSED:
      break;
    default:
      break;
  }

  return packet_processed;
}

void peer_connection_set_remote_description(PeerConnection* pc, const char* sdp, SdpType type) {
  char* start = (char*)sdp;
  char* line = NULL;
  char buf[1024];
  char* val_start = NULL;
  uint32_t* ssrc = NULL;
  DtlsSrtpRole role = DTLS_SRTP_ROLE_SERVER;
  int is_update = 0;
  int video_ssrc_seen = 0;
  int audio_ssrc_seen = 0;
  Agent* agent = &pc->agent;

  while ((line = strstr(start, "\r\n"))) {
    line = strstr(start, "\r\n");
    size_t line_len = (size_t)(line - start);
    if (line_len >= sizeof(buf))
      line_len = sizeof(buf) - 1;
    memcpy(buf, start, line_len);
    buf[line_len] = '\0';

    if (strstr(buf, "a=setup:passive")) {
      role = DTLS_SRTP_ROLE_CLIENT;
    }

    if (strstr(buf, "a=fingerprint")) {
      strncpy(pc->dtls_srtp.remote_fingerprint, buf + 22, DTLS_SRTP_FINGERPRINT_LENGTH);
    }

    if (strstr(buf, "a=ice-ufrag") &&
        strlen(agent->remote_ufrag) != 0 &&
        (strncmp(buf + strlen("a=ice-ufrag:"), agent->remote_ufrag, strlen(agent->remote_ufrag)) == 0)) {
      is_update = 1;
    }

    if (strstr(buf, "m=video")) {
      ssrc = &pc->remote_vssrc;
    } else if (strstr(buf, "m=audio")) {
      ssrc = &pc->remote_assrc;
    }

    if ((val_start = strstr(buf, "a=ssrc:")) && ssrc) {
      int* ssrc_seen = ssrc == &pc->remote_vssrc
          ? &video_ssrc_seen : &audio_ssrc_seen;
      if (*ssrc_seen) {
        start = line + 2;
        continue;
      }
      *ssrc_seen = 1;
      uint32_t parsed_ssrc = strtoul(val_start + 7, NULL, 10);
      if (*ssrc == 0) {
        *ssrc = parsed_ssrc;
        LOGD("SSRC: %" PRIu32, *ssrc);
        peer_connection_diag_log("remote_ssrc_selected media=%s ssrc=%" PRIu32,
                                 ssrc == &pc->remote_vssrc ? "video" : "audio",
                                 *ssrc);
      } else if (*ssrc != parsed_ssrc) {
        const int video = ssrc == &pc->remote_vssrc;
        peer_connection_diag_log("remote_ssrc_changed media=%s previous=%" PRIu32 " current=%" PRIu32,
                                 video ? "video" : "audio", *ssrc, parsed_ssrc);
        *ssrc = parsed_ssrc;
        if (video) {
          rtp_decoder_cleanup(&pc->vrtp_decoder);
          rtp_decoder_init(&pc->vrtp_decoder, pc->config.video_codec,
                           pc->config.onvideotrack, pc->config.user_data);
          rtp_decoder_set_video_callback(&pc->vrtp_decoder, pc->config.onvideopacket);
          pc->video_has_last_nack = 0;
        } else {
          rtp_decoder_cleanup(&pc->artp_decoder);
          rtp_decoder_init(&pc->artp_decoder, pc->config.audio_codec,
                           pc->config.onaudiotrack, pc->config.user_data);
          rtp_decoder_set_audio_callback(&pc->artp_decoder, pc->config.onaudiopacket);
        }
      }
    }

    start = line + 2;
  }

  if (is_update) {
    return;
  }

  pc->remote_ice_lite = strstr(sdp, "a=ice-lite") != NULL;
  agent_set_remote_description(&pc->agent, (char*)sdp);
  if (type == SDP_TYPE_ANSWER) {
    agent_update_candidate_pairs(&pc->agent);
    STATE_CHANGED(pc, PEER_CONNECTION_CHECKING);
  }
}

static const char* peer_connection_create_sdp(PeerConnection* pc, SdpType sdp_type) {
  char* description = (char*)pc->temp_buf;

  memset(pc->temp_buf, 0, sizeof(pc->temp_buf));
  DtlsSrtpRole role = DTLS_SRTP_ROLE_SERVER;

  pc->sctp.connected = 0;

  switch (sdp_type) {
    case SDP_TYPE_OFFER:
      role = DTLS_SRTP_ROLE_SERVER;
      agent_clear_candidates(&pc->agent);
      pc->agent.mode = AGENT_MODE_CONTROLLING;
      break;
    case SDP_TYPE_ANSWER:
      role = DTLS_SRTP_ROLE_CLIENT;
      pc->agent.mode = pc->remote_ice_lite ? AGENT_MODE_CONTROLLING : AGENT_MODE_CONTROLLED;
      break;
    default:
      break;
  }

  dtls_srtp_reset_session(&pc->dtls_srtp);
  pc->dtls_handshake_attempts = 0;
  pc->dtls_handshake_started_ms = 0;
  dtls_srtp_init(&pc->dtls_srtp, role, pc);
  pc->dtls_srtp.udp_recv = peer_connection_dtls_srtp_recv;
  pc->dtls_srtp.udp_send = peer_connection_dtls_srtp_send;

  memset(pc->sdp, 0, sizeof(pc->sdp));
  // TODO: check if we have video or audio codecs
  sdp_create(pc->sdp,
             pc->config.video_codec != CODEC_NONE,
             pc->config.audio_codec != CODEC_NONE,
             pc->config.datachannel);

  agent_create_ice_credential(&pc->agent);
  sdp_append(pc->sdp, "a=ice-ufrag:%s", pc->agent.local_ufrag);
  sdp_append(pc->sdp, "a=ice-pwd:%s", pc->agent.local_upwd);
  sdp_append(pc->sdp, "a=fingerprint:sha-256 %s", pc->dtls_srtp.local_fingerprint);
  sdp_append(pc->sdp, peer_connection_dtls_role_setup_value(role));

  if (pc->config.video_codec == CODEC_H264) {
    sdp_append_h264(pc->sdp);
  }

  switch (pc->config.audio_codec) {
    case CODEC_PCMA:
      sdp_append_pcma(pc->sdp);
      break;
    case CODEC_PCMU:
      sdp_append_pcmu(pc->sdp);
      break;
    case CODEC_OPUS:
      sdp_append_opus(pc->sdp);
    default:
      break;
  }

  if (pc->config.datachannel) {
    sdp_append_datachannel(pc->sdp);
  }

  pc->b_local_description_created = 1;

  agent_gather_candidate(&pc->agent, NULL, NULL, NULL);  // host address
  for (int i = 0; i < sizeof(pc->config.ice_servers) / sizeof(pc->config.ice_servers[0]); ++i) {
    if (pc->config.ice_servers[i].urls) {
      LOGI("ice server: %s", pc->config.ice_servers[i].urls);
      agent_gather_candidate(&pc->agent, pc->config.ice_servers[i].urls, pc->config.ice_servers[i].username, pc->config.ice_servers[i].credential);
    }
  }

  agent_get_local_description(&pc->agent, description, sizeof(pc->temp_buf));
  sdp_append(pc->sdp, description);

  if (pc->onicecandidate) {
    pc->onicecandidate(pc->sdp, pc->config.user_data);
  }

  return pc->sdp;
}

const char* peer_connection_create_offer(PeerConnection* pc) {
  return peer_connection_create_sdp(pc, SDP_TYPE_OFFER);
}

const char* peer_connection_create_answer(PeerConnection* pc) {
  const char* sdp = peer_connection_create_sdp(pc, SDP_TYPE_ANSWER);
  agent_update_candidate_pairs(&pc->agent);
  STATE_CHANGED(pc, PEER_CONNECTION_CHECKING);
  return sdp;
}

int peer_connection_send_rtcp_pil(PeerConnection* pc, uint32_t ssrc) {
  uint8_t plibuf[128];
  int size = 12;

  if (!pc || pc->state != PEER_CONNECTION_COMPLETED || ssrc == 0)
    return -1;

  if (rtcp_get_pli(plibuf, size, pc->vrtp_encoder.ssrc, ssrc) != size)
    return -1;

  dtls_srtp_encrypt_rctp_packet(&pc->dtls_srtp, plibuf, &size);
  const int ret = agent_send(&pc->agent, plibuf, size);
  peer_connection_diag_log("rtcp_pli sent=%d mediaSsrc=%" PRIu32 " bytes=%d",
                           ret,
                           ssrc,
                           size);
  return ret;
}

static int peer_connection_send_rtcp_nack(PeerConnection* pc, uint32_t ssrc,
                                          uint16_t packet_id, uint16_t bitmask) {
  uint8_t nackbuf[64];
  int size = 16;

  if (!pc || pc->state != PEER_CONNECTION_COMPLETED || ssrc == 0)
    return -1;
  if (rtcp_get_nack(nackbuf, size, pc->vrtp_encoder.ssrc, ssrc,
                    packet_id, bitmask) != size)
    return -1;

  dtls_srtp_encrypt_rctp_packet(&pc->dtls_srtp, nackbuf, &size);
  return agent_send(&pc->agent, nackbuf, size);
}

int peer_connection_request_video_keyframe(PeerConnection* pc) {
  if (!pc)
    return -1;
  return peer_connection_send_rtcp_pil(pc, pc->remote_vssrc);
}

// callbacks
void peer_connection_on_connected(PeerConnection* pc, void (*on_connected)(void* userdata)) {
  pc->on_connected = on_connected;
}

void peer_connection_on_receiver_packet_loss(PeerConnection* pc,
                                             void (*on_receiver_packet_loss)(float fraction_loss, uint32_t total_loss, void* userdata)) {
  pc->on_receiver_packet_loss = on_receiver_packet_loss;
}

void peer_connection_onicecandidate(PeerConnection* pc, void (*onicecandidate)(char* sdp, void* userdata)) {
  pc->onicecandidate = onicecandidate;
}

void peer_connection_oniceconnectionstatechange(PeerConnection* pc,
                                                void (*oniceconnectionstatechange)(PeerConnectionState state, void* userdata)) {
  pc->oniceconnectionstatechange = oniceconnectionstatechange;
}

void peer_connection_ondatachannel(PeerConnection* pc,
                                   void (*onmessage)(char* msg, size_t len, void* userdata, uint16_t sid),
                                   void (*onopen)(void* userdata),
                                   void (*onclose)(void* userdata)) {
  if (pc) {
    sctp_onopen(&pc->sctp, onopen);
    sctp_onclose(&pc->sctp, onclose);
    sctp_onmessage(&pc->sctp, onmessage);
  }
}

int peer_connection_lookup_sid(PeerConnection* pc, const char* label, uint16_t* sid) {
  for (int i = 0; i < pc->sctp.stream_count; i++) {
    if (strncmp(pc->sctp.stream_table[i].label, label, sizeof(pc->sctp.stream_table[i].label)) == 0) {
      *sid = pc->sctp.stream_table[i].sid;
      return 0;
    }
  }
  return -1;  // Not found
}

char* peer_connection_lookup_sid_label(PeerConnection* pc, uint16_t sid) {
  for (int i = 0; i < pc->sctp.stream_count; i++) {
    if (pc->sctp.stream_table[i].sid == sid) {
      return pc->sctp.stream_table[i].label;
    }
  }
  return NULL;  // Not found
}

int peer_connection_add_ice_candidate(PeerConnection* pc, char* candidate) {
  Agent* agent = &pc->agent;
  if (agent->remote_candidates_count >= AGENT_MAX_CANDIDATES) {
    LOGE("Remote ICE candidate table is full");
    return -1;
  }
  if (ice_candidate_from_description(&agent->remote_candidates[agent->remote_candidates_count], candidate, candidate + strlen(candidate)) != 0) {
    return -1;
  }
  LOGD("Add candidate: %s", candidate);
  IceCandidate* remote = &agent->remote_candidates[agent->remote_candidates_count];
  agent->remote_candidates_count++;

  for (int i = 0; i < agent->local_candidates_count; i++) {
    if (agent->local_candidates[i].addr.family != remote->addr.family)
      continue;
    if (agent->candidate_pairs_num >= AGENT_MAX_CANDIDATE_PAIRS) {
      LOGE("ICE candidate pair table is full");
      return -1;
    }

    IceCandidatePair* pair = &agent->candidate_pairs[agent->candidate_pairs_num++];
    pair->local = &agent->local_candidates[i];
    pair->remote = remote;
    pair->priority = ice_candidate_pair_priority(
        pair->local->priority,
        pair->remote->priority,
        agent->mode == AGENT_MODE_CONTROLLING);
    pair->state = ICE_CANDIDATE_STATE_FROZEN;
  }
  agent_sort_candidate_pairs(agent);

  return 0;
}

int peer_connection_get_ice_candidate_pair_stats(PeerConnection* pc,
                                                 int* total,
                                                 int* frozen,
                                                 int* inprogress,
                                                 int* succeeded,
                                                 int* failed) {
  if (!pc) {
    return -1;
  }

  AgentCandidatePairStats stats;
  agent_get_candidate_pair_stats(&pc->agent, &stats);
  if (total)
    *total = stats.total;
  if (frozen)
    *frozen = stats.frozen;
  if (inprogress)
    *inprogress = stats.inprogress;
  if (succeeded)
    *succeeded = stats.succeeded;
  if (failed)
    *failed = stats.failed;
  return 0;
}

int peer_connection_get_rtt_ms(PeerConnection* pc) {
  if (!pc)
    return -1;

  const int ice_rtt_ms = agent_get_rtt_ms(&pc->agent);
  return ice_rtt_ms >= 0 ? ice_rtt_ms : sctp_get_rtt_ms(&pc->sctp);
}

int peer_connection_get_video_rtp_stats(PeerConnection* pc, PeerVideoRtpStats* stats) {
  if (pc == NULL || stats == NULL)
    return -1;
  stats->packets_received = pc->vrtp_decoder.packets_received;
  stats->sequence_gaps = pc->vrtp_decoder.sequence_gaps;
  stats->reordered_packets = pc->vrtp_decoder.reordered_packets;
  stats->late_packets_dropped = pc->vrtp_decoder.late_packets_dropped;
  stats->forced_sequence_skips = pc->vrtp_decoder.forced_sequence_skips;
  stats->reorder_buffered_packets = pc->vrtp_decoder.reorder_buffered_packets;
  stats->access_units_completed = pc->vrtp_decoder.access_units_completed;
  stats->access_units_dropped = pc->vrtp_decoder.access_units_dropped;
  stats->nack_requests = pc->video_nack_requests;
  stats->nack_packets_requested = pc->video_nack_packets_requested;
  return 0;
}
