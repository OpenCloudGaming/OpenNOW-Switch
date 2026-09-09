#include <assert.h>

#include "../extern/libpeer/src/peer_connection.c"

static uint32_t now_ms;
static int sender_reports;
static int keyframe_requests;
static int protect_calls;
static int protect_result;
static int send_calls;
static int send_result;
static uint8_t sent_packet[128];
static int sent_size;

uint32_t ports_get_monotonic_time(void) {
  return now_ms;
}

int dtls_srtp_encrypt_rctp_packet(DtlsSrtp* dtls, uint8_t* packet, int* bytes) {
  (void)dtls;
  protect_calls++;
  if (protect_result != 0)
    return protect_result;
  assert(rtcp_validate_compound(packet, *bytes));
  memset(packet + *bytes, 0xa5, 4);
  *bytes += 4;
  return 0;
}

int agent_send(Agent* agent, const uint8_t* packet, int size) {
  (void)agent;
  send_calls++;
  assert(size <= (int)sizeof(sent_packet));
  memcpy(sent_packet, packet, size);
  sent_size = size;
  return send_result < 0 ? send_result : size;
}

static void on_sender_report(uint32_t ssrc, uint64_t ntp_us, uint32_t timestamp,
                              void* userdata) {
  (void)userdata;
  assert(ssrc == 42 || ssrc == 43);
  assert(ntp_us == 1500000);
  assert(timestamp == 90000);
  sender_reports++;
}

static void on_keyframe_request(void* userdata) {
  (void)userdata;
  keyframe_requests++;
}

static void incoming_validation(PeerConnection* pc) {
  uint8_t compound[64] = {
    0x80, RTCP_SR, 0, 6, 0, 0, 0, 42,
    0, 0, 0, 1, 0x80, 0, 0, 0, 0, 1, 0x5f, 0x90,
    0, 0, 0, 0, 0, 0, 0, 0,
    0x81, RTCP_PSFB, 0, 2, 0, 0, 0, 42, 0, 0, 0, 1,
  };
  now_ms = 100;
  peer_connection_incoming_rtcp(pc, compound, 40);
  assert(sender_reports == 1 && keyframe_requests == 1);
  assert(pc->video_receiver_stats.last_sr == 0x00018000);
  assert(pc->video_receiver_stats.last_sr_received_ms == 100);

  for (size_t size = 0; size < 28; ++size)
    peer_connection_incoming_rtcp(pc, compound, size);
  for (size_t size = 29; size < 40; ++size)
    peer_connection_incoming_rtcp(pc, compound, size);
  for (size_t size = 41; size <= 43; ++size)
    peer_connection_incoming_rtcp(pc, compound, size);
  assert(sender_reports == 1 && keyframe_requests == 1);
  compound[3] = 1;
  peer_connection_incoming_rtcp(pc, compound, 40);
  assert(sender_reports == 1 && keyframe_requests == 1);
  compound[3] = 6;
  compound[0] = 0x81;
  peer_connection_incoming_rtcp(pc, compound, 40);
  assert(sender_reports == 1 && keyframe_requests == 1);
  compound[0] = 0x80;
  compound[7] = 43;
  now_ms = 200;
  peer_connection_incoming_rtcp(pc, compound, 28);
  assert(sender_reports == 2);
  assert(pc->video_receiver_stats.last_sr_received_ms == 100);
}

static void sending_reports(PeerConnection* pc) {
  uint8_t rtp[14] = {0x80, 96, 0, 100, 0, 0, 0, 90, 0, 0, 0, 42, 0x61, 0x80};
  now_ms = 300;
  assert(rtcp_receiver_record_rtp(&pc->video_receiver_stats, rtp, sizeof(rtp), now_ms));
  peer_connection_maybe_send_receiver_report(pc);
  assert(protect_calls == 0 && send_calls == 0);
  now_ms = 1300;
  peer_connection_maybe_send_receiver_report(pc);
  assert(protect_calls == 1 && send_calls == 1);
  assert(sent_size == 60 && sent_packet[59] == 0xa5);
  assert(rtcp_validate_compound(sent_packet, sent_size - 4));
  assert(pc->video_receiver_stats.received_prior == 1);
  peer_connection_maybe_send_receiver_report(pc);
  assert(protect_calls == 1 && send_calls == 1);

  rtp[3] = 102;
  assert(rtcp_receiver_record_rtp(&pc->video_receiver_stats, rtp, sizeof(rtp), 1400));
  protect_result = -1;
  now_ms = 2300;
  peer_connection_maybe_send_receiver_report(pc);
  assert(protect_calls == 2 && send_calls == 1);
  assert(pc->video_receiver_stats.received_prior == 1);
  now_ms = 2301;
  peer_connection_maybe_send_receiver_report(pc);
  assert(protect_calls == 2);
  protect_result = 0;
  send_result = -1;
  now_ms = 3300;
  peer_connection_maybe_send_receiver_report(pc);
  assert(protect_calls == 3 && send_calls == 2);
  assert(pc->video_receiver_stats.received_prior == 1);
  send_result = 0;
  now_ms = 4300;
  peer_connection_maybe_send_receiver_report(pc);
  assert(protect_calls == 4 && send_calls == 3);
  assert(pc->video_receiver_stats.received_prior == 2);
  assert(sent_packet[12] == 128 && sent_packet[15] == 1);
  pc->state = PEER_CONNECTION_DISCONNECTED;
  now_ms = 5300;
  peer_connection_maybe_send_receiver_report(pc);
  assert(protect_calls == 4 && send_calls == 3);
}

int main(void) {
  PeerConnection pc = {0};
  pc.remote_vssrc = 42;
  pc.vrtp_encoder.ssrc = 1;
  pc.state = PEER_CONNECTION_COMPLETED;
  pc.config.onrtpsenderreport = on_sender_report;
  pc.config.on_request_keyframe = on_keyframe_request;
  incoming_validation(&pc);
  sending_reports(&pc);
  return 0;
}
