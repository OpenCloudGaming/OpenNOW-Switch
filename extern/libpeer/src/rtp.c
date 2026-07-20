#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "address.h"
#include "config.h"
#include "peer_connection.h"
#include "rtp.h"
#include "utils.h"

typedef enum RtpH264Type {

  NALU = 23,
  STAP_A = 24,
  FU_A = 28,

} RtpH264Type;

typedef struct RtpPayloadView {
  uint8_t* payload;
  size_t payload_size;
  uint8_t marker;
  uint8_t payload_type;
  uint16_t seq_number;
  uint32_t timestamp;
  uint32_t ssrc;
} RtpPayloadView;

typedef struct NaluHeader {
  uint8_t type : 5;
  uint8_t nri : 2;
  uint8_t f : 1;
} NaluHeader;

typedef struct FuHeader {
  uint8_t type : 5;
  uint8_t r : 1;
  uint8_t e : 1;
  uint8_t s : 1;
} FuHeader;

#define RTP_PAYLOAD_SIZE (CONFIG_MTU - sizeof(RtpHeader))
#define FU_PAYLOAD_SIZE (CONFIG_MTU - sizeof(RtpHeader) - sizeof(FuHeader) - sizeof(NaluHeader))

static uint16_t read_be16(const uint8_t* data) {
  return (uint16_t)((data[0] << 8) | data[1]);
}

static uint32_t read_be32(const uint8_t* data) {
  return ((uint32_t)data[0] << 24) |
         ((uint32_t)data[1] << 16) |
         ((uint32_t)data[2] << 8) |
         (uint32_t)data[3];
}

static int rtp_parse_payload(uint8_t* packet, size_t size, RtpPayloadView* view) {
  if (!packet || !view || size < 12)
    return -1;

  if ((packet[0] >> 6) != 2)
    return -1;

  const uint8_t padding = (packet[0] & 0x20) != 0;
  const uint8_t extension = (packet[0] & 0x10) != 0;
  const uint8_t csrc_count = packet[0] & 0x0f;
  size_t offset = 12 + ((size_t)csrc_count * 4);
  size_t payload_end = size;

  if (offset > size)
    return -1;

  if (extension) {
    if (offset + 4 > size)
      return -1;

    const size_t extension_size = (size_t)read_be16(packet + offset + 2) * 4;
    offset += 4;
    if (offset + extension_size > size)
      return -1;
    offset += extension_size;
  }

  if (padding) {
    const uint8_t padding_size = packet[size - 1];
    if (padding_size == 0 || padding_size > payload_end - offset)
      return -1;
    payload_end -= padding_size;
  }

  if (offset >= payload_end)
    return -1;

  view->payload = packet + offset;
  view->payload_size = payload_end - offset;
  view->marker = (packet[1] & 0x80) != 0;
  view->payload_type = packet[1] & 0x7f;
  view->seq_number = read_be16(packet + 2);
  view->timestamp = read_be32(packet + 4);
  view->ssrc = read_be32(packet + 8);
  return 0;
}

int rtp_packet_validate(uint8_t* packet, size_t size) {
  if (size < 12)
    return 0;

  if ((packet[0] >> 6) != 2)
    return 0;

  const uint8_t payload_type = packet[1] & 0x7f;
  return ((payload_type < 64) || (payload_type >= 96));
}

uint32_t rtp_get_ssrc(uint8_t* packet) {
  return read_be32(packet + 8);
}

static int rtp_encoder_encode_h264_single(RtpEncoder* rtp_encoder, uint8_t* buf, size_t size) {
  RtpPacket* rtp_packet = (RtpPacket*)rtp_encoder->buf;

  rtp_packet->header.version = 2;
  rtp_packet->header.padding = 0;
  rtp_packet->header.extension = 0;
  rtp_packet->header.csrccount = 0;
  rtp_packet->header.markerbit = 0;
  rtp_packet->header.type = rtp_encoder->type;
  rtp_packet->header.seq_number = htons(rtp_encoder->seq_number++);
  rtp_packet->header.timestamp = htonl(rtp_encoder->timestamp);
  rtp_packet->header.ssrc = htonl(rtp_encoder->ssrc);

  // I frame and P frame
  if ((*buf & 0x1f) == 0x05 || (*buf & 0x1f) == 0x01) {
    rtp_packet->header.markerbit = 1;
    rtp_encoder->timestamp += rtp_encoder->timestamp_increment;
  }
#if 0
  LOGI("markbit: %d, timestamp: %d, nalu type: %d", rtp_packet->header.markerbit, rtp_encoder->timestamp, buf[0] & 0x1f);
#endif

  memcpy(rtp_packet->payload, buf, size);
  rtp_encoder->on_packet(rtp_encoder->buf, size + sizeof(RtpHeader), rtp_encoder->user_data);
  return 0;
}

static int rtp_encoder_encode_h264_fu_a(RtpEncoder* rtp_encoder, uint8_t* buf, size_t size) {
  RtpPacket* rtp_packet = (RtpPacket*)rtp_encoder->buf;

  rtp_packet->header.version = 2;
  rtp_packet->header.padding = 0;
  rtp_packet->header.extension = 0;
  rtp_packet->header.csrccount = 0;
  rtp_packet->header.markerbit = 0;
  rtp_packet->header.type = rtp_encoder->type;
  rtp_packet->header.timestamp = htonl(rtp_encoder->timestamp);
  rtp_packet->header.ssrc = htonl(rtp_encoder->ssrc);
  uint8_t type = buf[0] & 0x1f;
  uint8_t nri = (buf[0] & 0x60) >> 5;
  buf = buf + 1;
  size = size - 1;

  // increase timestamp if I, P frame
  if (type == 0x05 || type == 0x01) {
    rtp_encoder->timestamp += rtp_encoder->timestamp_increment;
  }

  NaluHeader* fu_indicator = (NaluHeader*)rtp_packet->payload;
  FuHeader* fu_header = (FuHeader*)rtp_packet->payload + sizeof(NaluHeader);
  fu_header->s = 1;

  while (size > 0) {
    fu_indicator->type = FU_A;
    fu_indicator->nri = nri;
    fu_indicator->f = 0;
    fu_header->type = type;
    fu_header->r = 0;
    rtp_packet->header.seq_number = htons(rtp_encoder->seq_number++);

    if (size <= FU_PAYLOAD_SIZE) {
      fu_header->e = 1;
      rtp_packet->header.markerbit = 1;
      memcpy(rtp_packet->payload + sizeof(NaluHeader) + sizeof(FuHeader), buf, size);
      rtp_encoder->on_packet(rtp_encoder->buf, size + sizeof(RtpHeader) + sizeof(NaluHeader) + sizeof(FuHeader), rtp_encoder->user_data);
      break;
    }

    fu_header->e = 0;

    memcpy(rtp_packet->payload + sizeof(NaluHeader) + sizeof(FuHeader), buf, FU_PAYLOAD_SIZE);
    rtp_encoder->on_packet(rtp_encoder->buf, CONFIG_MTU, rtp_encoder->user_data);
    size -= FU_PAYLOAD_SIZE;
    buf += FU_PAYLOAD_SIZE;

    fu_header->s = 0;
  }
  return 0;
}

static uint8_t* h264_find_nalu(uint8_t* buf_start, uint8_t* buf_end) {
  uint8_t* p = buf_start + 2;

  while (p < buf_end) {
    if (*(p - 2) == 0x00 && *(p - 1) == 0x00 && *p == 0x01)
      return p + 1;
    p++;
  }

  return buf_end;
}

static int rtp_encoder_encode_h264(RtpEncoder* rtp_encoder, uint8_t* buf, size_t size) {
  uint8_t* buf_end = buf + size;
  uint8_t *pstart, *pend;
  size_t nalu_size;

  for (pstart = h264_find_nalu(buf, buf_end); pstart < buf_end; pstart = pend) {
    pend = h264_find_nalu(pstart, buf_end);
    nalu_size = pend - pstart;

    if (pend != buf_end)
      nalu_size--;

    while (pstart[nalu_size - 1] == 0x00)
      nalu_size--;

    if (nalu_size <= RTP_PAYLOAD_SIZE) {
      rtp_encoder_encode_h264_single(rtp_encoder, pstart, nalu_size);

    } else {
      rtp_encoder_encode_h264_fu_a(rtp_encoder, pstart, nalu_size);
    }
  }

  return 0;
}

static int rtp_encoder_encode_generic(RtpEncoder* rtp_encoder, uint8_t* buf, size_t size) {
  RtpHeader* rtp_header = (RtpHeader*)rtp_encoder->buf;
  rtp_header->version = 2;
  rtp_header->padding = 0;
  rtp_header->extension = 0;
  rtp_header->csrccount = 0;
  rtp_header->markerbit = 0;
  rtp_header->type = rtp_encoder->type;
  rtp_header->seq_number = htons(rtp_encoder->seq_number++);
  rtp_header->timestamp = htonl(rtp_encoder->timestamp);
  rtp_encoder->timestamp += rtp_encoder->timestamp_increment;
  rtp_header->ssrc = htonl(rtp_encoder->ssrc);
  memcpy(rtp_encoder->buf + sizeof(RtpHeader), buf, size);

  rtp_encoder->on_packet(rtp_encoder->buf, size + sizeof(RtpHeader), rtp_encoder->user_data);

  return 0;
}

void rtp_encoder_init(RtpEncoder* rtp_encoder, MediaCodec codec, RtpOnPacket on_packet, void* user_data) {
  rtp_encoder->on_packet = on_packet;
  rtp_encoder->user_data = user_data;
  rtp_encoder->timestamp = 0;
  rtp_encoder->seq_number = 0;

  switch (codec) {
    case CODEC_H264:
      rtp_encoder->type = PT_H264;
      rtp_encoder->ssrc = SSRC_H264;
      rtp_encoder->timestamp_increment = 90000 / 30;  // 30 FPS.
      rtp_encoder->encode_func = rtp_encoder_encode_h264;
      break;
    case CODEC_PCMA:
      rtp_encoder->type = PT_PCMA;
      rtp_encoder->ssrc = SSRC_PCMA;
      rtp_encoder->timestamp_increment = CONFIG_AUDIO_DURATION * 8000 / 1000;
      rtp_encoder->encode_func = rtp_encoder_encode_generic;
      break;
    case CODEC_PCMU:
      rtp_encoder->type = PT_PCMU;
      rtp_encoder->ssrc = SSRC_PCMU;
      rtp_encoder->timestamp_increment = CONFIG_AUDIO_DURATION * 8000 / 1000;
      rtp_encoder->encode_func = rtp_encoder_encode_generic;
      break;
    case CODEC_OPUS:
      rtp_encoder->type = PT_OPUS;
      rtp_encoder->ssrc = SSRC_OPUS;
      rtp_encoder->timestamp_increment = CONFIG_AUDIO_DURATION * 48000 / 1000;
      rtp_encoder->encode_func = rtp_encoder_encode_generic;
      break;
    default:
      break;
  }
}

int rtp_encoder_encode(RtpEncoder* rtp_encoder, const uint8_t* buf, size_t size) {
  return rtp_encoder->encode_func(rtp_encoder, (uint8_t*)buf, size);
}

static void rtp_decoder_reset_fragment(RtpDecoder* rtp_decoder) {
  rtp_decoder->nalu_offset = 0;
  rtp_decoder->fragment_started = 0;
  rtp_decoder->fragment_damaged = 0;
}

static int rtp_decoder_append(RtpDecoder* rtp_decoder, const uint8_t* data, size_t size) {
  if (!rtp_decoder->nalu_buf || rtp_decoder->nalu_offset + size > rtp_decoder->nalu_capacity) {
    rtp_decoder->fragment_damaged = 1;
    LOGW("RTP H264: dropping oversized NALU, partial size=%zu add=%zu capacity=%zu",
         rtp_decoder->nalu_offset, size, rtp_decoder->nalu_capacity);
    return -1;
  }

  memcpy(rtp_decoder->nalu_buf + rtp_decoder->nalu_offset, data, size);
  rtp_decoder->nalu_offset += size;
  return 0;
}

static void rtp_decoder_reset_access_unit(RtpDecoder* rtp_decoder) {
  rtp_decoder->au_offset = 0;
  rtp_decoder->au_timestamp = 0;
  rtp_decoder->au_ssrc = 0;
  rtp_decoder->au_started = 0;
  rtp_decoder->au_has_vcl = 0;
  rtp_decoder->au_has_sps = 0;
  rtp_decoder->au_has_pps = 0;
  rtp_decoder->au_damaged = 0;
}

static int rtp_decoder_append_access_unit_bytes(RtpDecoder* rtp_decoder, const uint8_t* data, size_t size) {
  if (!rtp_decoder->au_buf || rtp_decoder->au_offset + size > rtp_decoder->au_capacity) {
    rtp_decoder->au_damaged = 1;
    LOGW("RTP H264: access unit overflow offset=%zu add=%zu capacity=%zu",
         rtp_decoder->au_offset, size, rtp_decoder->au_capacity);
    return -1;
  }

  memcpy(rtp_decoder->au_buf + rtp_decoder->au_offset, data, size);
  rtp_decoder->au_offset += size;
  return 0;
}

static int rtp_decoder_append_access_unit_nalu(RtpDecoder* rtp_decoder, const uint8_t* data, size_t size) {
  static const uint8_t nalu_start_4bytecode[] = {0x00, 0x00, 0x00, 0x01};

  if (!data || size == 0)
    return -1;

  const uint8_t type = data[0] & 0x1f;
  if (type == 7 && size <= sizeof(rtp_decoder->cached_sps)) {
    memcpy(rtp_decoder->cached_sps, data, size);
    rtp_decoder->cached_sps_size = size;
    rtp_decoder->au_has_sps = 1;
  } else if (type == 8 && size <= sizeof(rtp_decoder->cached_pps)) {
    memcpy(rtp_decoder->cached_pps, data, size);
    rtp_decoder->cached_pps_size = size;
    rtp_decoder->au_has_pps = 1;
  }

  if (type == 5) {
    if (!rtp_decoder->au_has_sps && rtp_decoder->cached_sps_size > 0) {
      if (rtp_decoder_append_access_unit_bytes(rtp_decoder, nalu_start_4bytecode, sizeof(nalu_start_4bytecode)) != 0 ||
          rtp_decoder_append_access_unit_bytes(rtp_decoder, rtp_decoder->cached_sps, rtp_decoder->cached_sps_size) != 0)
        return -1;
      rtp_decoder->au_has_sps = 1;
    }
    if (!rtp_decoder->au_has_pps && rtp_decoder->cached_pps_size > 0) {
      if (rtp_decoder_append_access_unit_bytes(rtp_decoder, nalu_start_4bytecode, sizeof(nalu_start_4bytecode)) != 0 ||
          rtp_decoder_append_access_unit_bytes(rtp_decoder, rtp_decoder->cached_pps, rtp_decoder->cached_pps_size) != 0)
        return -1;
      rtp_decoder->au_has_pps = 1;
    }
  }

  if (rtp_decoder_append_access_unit_bytes(rtp_decoder, nalu_start_4bytecode, sizeof(nalu_start_4bytecode)) != 0 ||
      rtp_decoder_append_access_unit_bytes(rtp_decoder, data, size) != 0)
    return -1;

  if (type >= 1 && type <= 5)
    rtp_decoder->au_has_vcl = 1;
  rtp_decoder->nal_units_completed++;

  return 0;
}

static void rtp_decoder_flush_access_unit(RtpDecoder* rtp_decoder) {
  if (!rtp_decoder->au_started)
    return;

  if (!rtp_decoder->au_damaged && rtp_decoder->au_has_vcl && rtp_decoder->au_offset > 0) {
    if (rtp_decoder->on_video_packet != NULL) {
      PeerVideoPacket packet = {
        .data = rtp_decoder->au_buf,
        .size = rtp_decoder->au_offset,
        .timestamp = rtp_decoder->au_timestamp,
        .ssrc = rtp_decoder->au_ssrc,
      };
      rtp_decoder->on_video_packet(&packet, rtp_decoder->user_data);
    } else if (rtp_decoder->on_packet != NULL)
      rtp_decoder->on_packet(rtp_decoder->au_buf, rtp_decoder->au_offset, rtp_decoder->user_data);
    rtp_decoder->access_units_completed++;
  } else if (rtp_decoder->au_offset > 0) {
    rtp_decoder->access_units_dropped++;
  }

  rtp_decoder_reset_access_unit(rtp_decoder);
}

static void rtp_decoder_begin_access_unit(RtpDecoder* rtp_decoder, uint32_t timestamp) {
  if (rtp_decoder->au_started && rtp_decoder->au_timestamp != timestamp)
    rtp_decoder_flush_access_unit(rtp_decoder);

  if (!rtp_decoder->au_started) {
    rtp_decoder->au_started = 1;
    rtp_decoder->au_timestamp = timestamp;
  }
}

static int rtp_decode_h264_stap_a(
    RtpDecoder* rtp_decoder,
    uint8_t* payload,
    size_t payload_size,
    uint32_t timestamp) {
  size_t offset = 1;
  int appended = 0;

  while (offset + 2 <= payload_size) {
    const uint16_t nalu_size = read_be16(payload + offset);
    offset += 2;

    if (nalu_size == 0)
      continue;

    if (offset + nalu_size > payload_size) {
      LOGW("RTP H264: invalid STAP-A packet");
      return -1;
    }

    (void)timestamp;
    if (rtp_decoder_append_access_unit_nalu(rtp_decoder, payload + offset, nalu_size) == 0)
      appended++;

    offset += nalu_size;
  }

  return appended > 0 ? (int)payload_size : -1;
}

static int rtp_decode_h264_fu_a(
    RtpDecoder* rtp_decoder,
    uint8_t* payload,
    size_t payload_size,
    uint32_t timestamp,
    uint8_t marker) {
  static const uint8_t nalu_start_4bytecode[] = {0x00, 0x00, 0x00, 0x01};

  if (payload_size < 3)
    return -1;

  const uint8_t fu_indicator = payload[0];
  const uint8_t fu_header = payload[1];
  const uint8_t start = (fu_header & 0x80) != 0;
  const uint8_t end = (fu_header & 0x40) != 0;
  const uint8_t nal_type = fu_header & 0x1f;
  const uint8_t reconstructed_header = (fu_indicator & 0xe0) | nal_type;

  if (start) {
    if (rtp_decoder->fragment_started)
      rtp_decoder->au_damaged = 1;
    rtp_decoder->nalu_offset = 0;
    rtp_decoder->fragment_started = 1;
    rtp_decoder->fragment_damaged = 0;

    if (rtp_decoder_append(rtp_decoder, nalu_start_4bytecode, sizeof(nalu_start_4bytecode)) != 0 ||
        rtp_decoder_append(rtp_decoder, &reconstructed_header, 1) != 0 ||
        rtp_decoder_append(rtp_decoder, payload + 2, payload_size - 2) != 0) {
      rtp_decoder_reset_fragment(rtp_decoder);
      return -1;
    }
  } else {
    if (!rtp_decoder->fragment_started)
      return 0;

    if (rtp_decoder_append(rtp_decoder, payload + 2, payload_size - 2) != 0) {
      rtp_decoder_reset_fragment(rtp_decoder);
      return -1;
    }
  }

  if (end) {
    if (!rtp_decoder->fragment_damaged && rtp_decoder->nalu_offset > sizeof(nalu_start_4bytecode)) {
      if (rtp_decoder_append_access_unit_nalu(
              rtp_decoder,
              rtp_decoder->nalu_buf + sizeof(nalu_start_4bytecode),
              rtp_decoder->nalu_offset - sizeof(nalu_start_4bytecode)) != 0)
        rtp_decoder->au_damaged = 1;
    } else {
      rtp_decoder->au_damaged = 1;
    }
    rtp_decoder_reset_fragment(rtp_decoder);
    if (marker)
      rtp_decoder_flush_access_unit(rtp_decoder);
  }

  (void)timestamp;
  (void)marker;

  return (int)payload_size;
}

static int rtp_decode_h264(RtpDecoder* rtp_decoder, uint8_t* buf, size_t size) {
  RtpPayloadView view;
  if (rtp_parse_payload(buf, size, &view) != 0)
    return -1;

  if (!rtp_decoder->au_started)
    rtp_decoder->au_ssrc = view.ssrc;

  rtp_decoder->packets_received++;

  if (rtp_decoder->packets_received % 600 == 0) {
    LOGI("RTP H264 stats packets=%u gaps=%u reordered=%u late=%u forced=%u nalus=%u auOk=%u auDrop=%u",
         rtp_decoder->packets_received,
         rtp_decoder->sequence_gaps,
         rtp_decoder->reordered_packets,
         rtp_decoder->late_packets_dropped,
         rtp_decoder->forced_sequence_skips,
         rtp_decoder->nal_units_completed,
         rtp_decoder->access_units_completed,
         rtp_decoder->access_units_dropped);
  }

  if (rtp_decoder->has_last_seq_number &&
      (uint16_t)(rtp_decoder->last_seq_number + 1) != view.seq_number) {
    rtp_decoder->sequence_gaps++;
    rtp_decoder->au_damaged = 1;
    if (rtp_decoder->fragment_started) {
      LOGW("RTP H264: sequence gap inside fragmented NALU (%u -> %u)",
           rtp_decoder->last_seq_number, view.seq_number);
      rtp_decoder_reset_fragment(rtp_decoder);
    }
  }

  rtp_decoder->last_seq_number = view.seq_number;
  rtp_decoder->has_last_seq_number = 1;
  rtp_decoder_begin_access_unit(rtp_decoder, view.timestamp);

  const uint8_t nalu_type = view.payload[0] & 0x1f;

  if (nalu_type > 0 && nalu_type < STAP_A) {
    rtp_decoder->single_packets++;
    if (rtp_decoder->fragment_started)
      rtp_decoder->au_damaged = 1;
    rtp_decoder_reset_fragment(rtp_decoder);
    if (rtp_decoder_append_access_unit_nalu(rtp_decoder, view.payload, view.payload_size) != 0)
      return -1;
    if (view.marker)
      rtp_decoder_flush_access_unit(rtp_decoder);
    return (int)size;
  }

  switch (nalu_type) {
    case STAP_A:
      rtp_decoder->stap_packets++;
      rtp_decoder_reset_fragment(rtp_decoder);
      if (rtp_decode_h264_stap_a(rtp_decoder, view.payload, view.payload_size, view.timestamp) < 0) {
        rtp_decoder->au_damaged = 1;
        return -1;
      }
      if (view.marker)
        rtp_decoder_flush_access_unit(rtp_decoder);
      return (int)size;
    case FU_A:
      rtp_decoder->fu_packets++;
      return rtp_decode_h264_fu_a(rtp_decoder, view.payload, view.payload_size, view.timestamp, view.marker);
    default:
      LOGD("RTP H264: unsupported packetization type %u", nalu_type);
      rtp_decoder->au_damaged = 1;
      rtp_decoder_reset_fragment(rtp_decoder);
      return 0;
  }
}

static int rtp_decode_generic(RtpDecoder* rtp_decoder, uint8_t* buf, size_t size) {
  RtpPayloadView view;
  if (rtp_parse_payload(buf, size, &view) != 0)
    return -1;

  if (rtp_decoder->on_audio_packet != NULL) {
    PeerAudioPacket packet = {
      .data = view.payload,
      .size = view.payload_size,
      .timestamp = view.timestamp,
      .ssrc = rtp_get_ssrc(buf),
      .sequence = read_be16(buf + 2),
      .payload_type = ((RtpHeader*)buf)->type,
      .marker = view.marker,
    };
    rtp_decoder->on_audio_packet(&packet, rtp_decoder->user_data);
  } else if (rtp_decoder->on_packet != NULL)
    rtp_decoder->on_packet(view.payload, view.payload_size, rtp_decoder->user_data);
  // even if there is no callback set, assume everything is ok for caller and do not return an error
  return (int)size;
}

void rtp_decoder_set_audio_callback(RtpDecoder* rtp_decoder, RtpOnAudioPacket on_packet) {
  if (rtp_decoder)
    rtp_decoder->on_audio_packet = on_packet;
}

void rtp_decoder_set_video_callback(RtpDecoder* rtp_decoder, RtpOnVideoPacket on_packet) {
  if (rtp_decoder)
    rtp_decoder->on_video_packet = on_packet;
}

void rtp_decoder_init(RtpDecoder* rtp_decoder, MediaCodec codec, RtpOnPacket on_packet, void* user_data) {
  memset(rtp_decoder, 0, sizeof(*rtp_decoder));
  rtp_decoder->on_packet = on_packet;
  rtp_decoder->user_data = user_data;

  switch (codec) {
    case CODEC_H264:
      rtp_decoder->type = PT_H264;
      rtp_decoder->nalu_capacity = CONFIG_MAX_NALU_SIZE;
      rtp_decoder->nalu_buf = malloc(rtp_decoder->nalu_capacity);
      rtp_decoder->au_capacity = CONFIG_MAX_NALU_SIZE;
      rtp_decoder->au_buf = malloc(rtp_decoder->au_capacity);
      rtp_decoder->reorder_buf = malloc(RTP_REORDER_WINDOW * RTP_REORDER_PACKET_CAPACITY);
      if (!rtp_decoder->nalu_buf || !rtp_decoder->au_buf || !rtp_decoder->reorder_buf) {
        LOGE("RTP H264: failed to allocate decoder buffers");
        free(rtp_decoder->nalu_buf);
        free(rtp_decoder->au_buf);
        free(rtp_decoder->reorder_buf);
        rtp_decoder->nalu_buf = NULL;
        rtp_decoder->au_buf = NULL;
        rtp_decoder->reorder_buf = NULL;
        rtp_decoder->nalu_capacity = 0;
        rtp_decoder->au_capacity = 0;
        break;
      }
      rtp_decoder->decode_func = rtp_decode_h264;
      break;
    case CODEC_PCMA:
      rtp_decoder->type = PT_PCMA;
      rtp_decoder->decode_func = rtp_decode_generic;
      break;
    case CODEC_PCMU:
      rtp_decoder->type = PT_PCMU;
      rtp_decoder->decode_func = rtp_decode_generic;
      break;
    case CODEC_OPUS:
      rtp_decoder->type = PT_OPUS;
      rtp_decoder->decode_func = rtp_decode_generic;
      break;
    default:
      break;
  }
}

void rtp_decoder_cleanup(RtpDecoder* rtp_decoder) {
  if (!rtp_decoder)
    return;

  free(rtp_decoder->nalu_buf);
  free(rtp_decoder->au_buf);
  free(rtp_decoder->reorder_buf);
  rtp_decoder->nalu_buf = NULL;
  rtp_decoder->au_buf = NULL;
  rtp_decoder->reorder_buf = NULL;
  rtp_decoder->nalu_capacity = 0;
  rtp_decoder->nalu_offset = 0;
  rtp_decoder->au_capacity = 0;
  rtp_decoder_reset_access_unit(rtp_decoder);
  rtp_decoder->fragment_started = 0;
  rtp_decoder->fragment_damaged = 0;
}

static int rtp_decoder_drain_reorder_buffer(RtpDecoder* rtp_decoder) {
  int result = 0;

  while (rtp_decoder->reorder_has_expected_sequence) {
    const size_t slot = rtp_decoder->reorder_expected_sequence % RTP_REORDER_WINDOW;
    if (!rtp_decoder->reorder_used[slot] ||
        rtp_decoder->reorder_sequences[slot] != rtp_decoder->reorder_expected_sequence) {
      break;
    }

    uint8_t* packet = rtp_decoder->reorder_buf + (slot * RTP_REORDER_PACKET_CAPACITY);
    result = rtp_decoder->decode_func(rtp_decoder, packet, rtp_decoder->reorder_sizes[slot]);
    rtp_decoder->reorder_used[slot] = 0;
    if (rtp_decoder->reorder_buffered_packets > 0)
      rtp_decoder->reorder_buffered_packets--;
    rtp_decoder->reorder_expected_sequence++;
  }

  return result;
}

static int rtp_decoder_decode_reordered(RtpDecoder* rtp_decoder, const uint8_t* buf, size_t size) {
  if (!rtp_decoder->reorder_buf || size < 4 || size > RTP_REORDER_PACKET_CAPACITY)
    return -1;

  const uint16_t sequence = read_be16(buf + 2);
  if (!rtp_decoder->reorder_has_expected_sequence) {
    rtp_decoder->reorder_has_expected_sequence = 1;
    rtp_decoder->reorder_expected_sequence = sequence;
  }

  int16_t distance = (int16_t)(sequence - rtp_decoder->reorder_expected_sequence);
  if (distance < 0) {
    rtp_decoder->late_packets_dropped++;
    return 0;
  }

  // Leave roughly one network RTT for retransmission, but do not block the
  // decoder behind the full allocation window after permanent packet loss.
  while (distance >= RTP_REORDER_MAX_HOLD_PACKETS) {
    rtp_decoder->reorder_expected_sequence++;
    rtp_decoder->forced_sequence_skips++;
    rtp_decoder_drain_reorder_buffer(rtp_decoder);
    distance = (int16_t)(sequence - rtp_decoder->reorder_expected_sequence);
  }

  if (distance == 0) {
    const int result = rtp_decoder->decode_func(rtp_decoder, (uint8_t*)buf, size);
    rtp_decoder->reorder_expected_sequence++;
    rtp_decoder_drain_reorder_buffer(rtp_decoder);
    return result;
  }

  const size_t slot = sequence % RTP_REORDER_WINDOW;
  if (rtp_decoder->reorder_used[slot]) {
    if (rtp_decoder->reorder_sequences[slot] == sequence)
      return 0;
    rtp_decoder->late_packets_dropped++;
    return 0;
  }

  memcpy(rtp_decoder->reorder_buf + (slot * RTP_REORDER_PACKET_CAPACITY), buf, size);
  rtp_decoder->reorder_sizes[slot] = size;
  rtp_decoder->reorder_sequences[slot] = sequence;
  rtp_decoder->reorder_used[slot] = 1;
  rtp_decoder->reorder_buffered_packets++;
  rtp_decoder->reordered_packets++;
  return 0;
}

int rtp_decoder_decode(RtpDecoder* rtp_decoder, const uint8_t* buf, size_t size) {
  if (rtp_decoder->decode_func == NULL)
    return -1;

  if (rtp_decoder->type == PT_H264)
    return rtp_decoder_decode_reordered(rtp_decoder, buf, size);

  return rtp_decoder->decode_func(rtp_decoder, (uint8_t*)buf, size);
}
