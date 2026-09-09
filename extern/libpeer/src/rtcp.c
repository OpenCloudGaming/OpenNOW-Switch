#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include "rtcp.h"

static uint32_t rtcp_read_u32(const uint8_t* data) {
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
         ((uint32_t)data[2] << 8) | data[3];
}

static void rtcp_write_u32(uint8_t* data, uint32_t value) {
  data[0] = (uint8_t)(value >> 24);
  data[1] = (uint8_t)(value >> 16);
  data[2] = (uint8_t)(value >> 8);
  data[3] = (uint8_t)value;
}

int rtcp_parse_packet(const uint8_t* packet, size_t size, RtcpPacketView* view) {
  if (!packet || !view || size < 4 || (packet[0] >> 6) != 2)
    return -1;
  const size_t packet_size = (((size_t)packet[2] << 8) | packet[3]) * 4 + 4;
  if (packet_size > size)
    return -1;
  size_t content_size = packet_size;
  if (packet[0] & 0x20) {
    const uint8_t padding = packet[packet_size - 1];
    if (packet_size != size || padding == 0 || padding > packet_size - 4)
      return -1;
    content_size -= padding;
  }
  const uint8_t count = packet[0] & 0x1f;
  size_t minimum = 4;
  switch (packet[1]) {
    case RTCP_SR: minimum = 28 + 24 * count; break;
    case RTCP_RR: minimum = 8 + 24 * count; break;
    case RTCP_RTPFB:
    case RTCP_PSFB:
      minimum = 12;
      if (packet[1] == RTCP_PSFB && count == 1 && content_size != 12)
        return -1;
      if (packet[1] == RTCP_PSFB && count == 4 &&
          (content_size < 20 || (content_size - 12) % 8 != 0))
        return -1;
      break;
    case RTCP_APP: minimum = 12; break;
    case RTCP_XR: minimum = 8; break;
    case RTCP_BYE: minimum = 4 + 4 * count; break;
    case RTCP_SDES: {
      size_t offset = 4;
      for (uint8_t chunk = 0; chunk < count; ++chunk) {
        if (offset + 4 > content_size)
          return -1;
        offset += 4;
        for (;;) {
          if (offset >= content_size)
            return -1;
          const uint8_t item = packet[offset++];
          if (item == 0) {
            offset = (offset + 3) & ~(size_t)3;
            if (offset > content_size)
              return -1;
            break;
          }
          if (offset >= content_size)
            return -1;
          const size_t item_size = packet[offset++];
          if (item_size > content_size - offset)
            return -1;
          offset += item_size;
        }
      }
      if (offset != content_size)
        return -1;
      break;
    }
    default:
      if (packet[1] < 192 || packet[1] > 223)
        return -1;
      break;
  }
  if (content_size < minimum)
    return -1;
  *view = (RtcpPacketView){packet, packet_size, content_size, packet[1], count};
  return 0;
}

int rtcp_validate_compound(const uint8_t* packet, size_t size) {
  if (!packet || size == 0)
    return 0;
  size_t offset = 0;
  while (offset < size) {
    RtcpPacketView view;
    if (rtcp_parse_packet(packet + offset, size - offset, &view) != 0)
      return 0;
    offset += view.size;
  }
  return 1;
}

int rtcp_receiver_record_rtp(RtcpReceiverStats* stats, const uint8_t* packet,
                             size_t size, uint32_t now_ms) {
  if (!stats || !packet || size < 12 || (packet[0] >> 6) != 2)
    return 0;
  size_t offset = 12 + 4 * (size_t)(packet[0] & 0xf);
  if (offset > size)
    return 0;
  if (packet[0] & 0x10) {
    if (size - offset < 4)
      return 0;
    const size_t extension_size = (((size_t)packet[offset + 2] << 8) |
                                   packet[offset + 3]) * 4;
    offset += 4;
    if (extension_size > size - offset)
      return 0;
    offset += extension_size;
  }
  if ((packet[0] & 0x20) &&
      (packet[size - 1] == 0 || packet[size - 1] > size - offset))
    return 0;

  const uint32_t ssrc = rtcp_read_u32(packet + 8);
  const uint16_t sequence = ((uint16_t)packet[2] << 8) | packet[3];
  const uint32_t timestamp = rtcp_read_u32(packet + 4);
  if (stats->ssrc != ssrc)
    memset(stats, 0, sizeof(*stats));
  if (stats->initialized) {
    const uint16_t delta = sequence - stats->max_sequence;
    if (delta < 3000) {
      if (sequence < stats->max_sequence)
        stats->cycles += 65536;
      stats->max_sequence = sequence;
    } else if (delta <= 65536 - 100) {
      if (sequence != stats->bad_sequence) {
        stats->bad_sequence = (sequence + 1u) & 0xffff;
        return 0;
      }
      memset(stats, 0, sizeof(*stats));
    }
  }
  if (!stats->initialized) {
    stats->ssrc = ssrc;
    stats->base_sequence = sequence;
    stats->max_sequence = sequence;
    stats->bad_sequence = 65536;
    stats->last_report_ms = now_ms;
    stats->initialized = 1;
  } else {
    const int32_t difference = (int32_t)((now_ms - stats->last_arrival_ms) * 90u -
                                        (timestamp - stats->last_rtp_timestamp));
    const uint64_t absolute = difference < 0 ? -(int64_t)difference : difference;
    const uint64_t decay = (stats->jitter_q4 + 8) >> 4;
    stats->jitter_q4 += absolute;
    stats->jitter_q4 -= decay;
  }
  stats->received++;
  stats->last_arrival_ms = now_ms;
  stats->last_rtp_timestamp = timestamp;
  return 1;
}

void rtcp_receiver_record_sr(RtcpReceiverStats* stats, const RtcpPacketView* view,
                              uint32_t now_ms) {
  if (!stats || !view || view->type != RTCP_SR || view->content_size < 28)
    return;
  const uint32_t ssrc = rtcp_read_u32(view->data + 4);
  if (stats->ssrc != 0 && stats->ssrc != ssrc)
    return;
  stats->ssrc = ssrc;
  stats->last_sr = (rtcp_read_u32(view->data + 8) << 16) |
                   (rtcp_read_u32(view->data + 12) >> 16);
  stats->last_sr_received_ms = now_ms;
  stats->have_sr = 1;
}

int rtcp_receiver_report_due(const RtcpReceiverStats* stats, uint32_t now_ms) {
  return stats && stats->initialized &&
         (uint32_t)(now_ms - stats->last_report_ms) >= RTCP_RECEIVER_REPORT_INTERVAL_MS;
}

void rtcp_receiver_report_sent(RtcpReceiverStats* stats, uint32_t now_ms, int success) {
  stats->last_report_ms = now_ms;
  if (success) {
    stats->expected_prior = stats->cycles + stats->max_sequence - stats->base_sequence + 1;
    stats->received_prior = stats->received;
  }
}

int rtcp_get_receiver_report(uint8_t* packet, size_t capacity, uint32_t sender_ssrc,
                             const RtcpReceiverStats* stats, uint32_t now_ms,
                             const char* cname) {
  if (!packet || !stats || !stats->initialized || !cname)
    return -1;
  const size_t cname_size = strlen(cname);
  if (cname_size == 0 || cname_size > 255)
    return -1;
  const size_t sdes_size = (11 + cname_size + 3) & ~(size_t)3;
  const size_t size = 32 + sdes_size;
  if (capacity < size)
    return -1;
  const uint64_t expected = stats->cycles + stats->max_sequence - stats->base_sequence + 1;
  const int64_t lost = (int64_t)expected - (int64_t)stats->received;
  const int32_t cumulative = lost > 0x7fffff ? 0x7fffff :
                            lost < -0x800000 ? -0x800000 : (int32_t)lost;
  const uint64_t expected_interval = expected - stats->expected_prior;
  const int64_t lost_interval = (int64_t)expected_interval -
                               (int64_t)(stats->received - stats->received_prior);
  const uint8_t fraction = expected_interval == 0 || lost_interval <= 0 ? 0 :
      (uint8_t)(((uint64_t)lost_interval * 256) / expected_interval);
  const uint64_t delay = stats->have_sr ?
      (uint64_t)(uint32_t)(now_ms - stats->last_sr_received_ms) * 65536 / 1000 : 0;

  memset(packet, 0, size);
  packet[0] = 0x81;
  packet[1] = RTCP_RR;
  packet[3] = 7;
  rtcp_write_u32(packet + 4, sender_ssrc);
  rtcp_write_u32(packet + 8, stats->ssrc);
  rtcp_write_u32(packet + 12, ((uint32_t)fraction << 24) | ((uint32_t)cumulative & 0xffffff));
  rtcp_write_u32(packet + 16, (uint32_t)(stats->cycles + stats->max_sequence));
  rtcp_write_u32(packet + 20, (uint32_t)(stats->jitter_q4 >> 4));
  rtcp_write_u32(packet + 24, stats->have_sr ? stats->last_sr : 0);
  rtcp_write_u32(packet + 28, delay > UINT32_MAX ? UINT32_MAX : (uint32_t)delay);
  packet[32] = 0x81;
  packet[33] = RTCP_SDES;
  packet[35] = (uint8_t)(sdes_size / 4 - 1);
  rtcp_write_u32(packet + 36, sender_ssrc);
  packet[40] = 1;
  packet[41] = (uint8_t)cname_size;
  memcpy(packet + 42, cname, cname_size);
  return (int)size;
}

int rtcp_probe(uint8_t* packet, size_t size) {
  if (size < 8)
    return 0;

  if ((packet[0] >> 6) != 2)
    return 0;

  const uint8_t packet_type = packet[1];
  return packet_type >= RTCP_FIR && packet_type <= RTCP_XR;
}

int rtcp_get_pli(uint8_t* packet, int len, uint32_t sender_ssrc, uint32_t media_ssrc) {
  if (packet == NULL || len != 12)
    return -1;

  memset(packet, 0, len);
  RtcpHeader* rtcp_header = (RtcpHeader*)packet;
  rtcp_header->version = 2;
  rtcp_header->type = RTCP_PSFB;
  rtcp_header->rc = 1;
  rtcp_header->length = htons((len / 4) - 1);
  const uint32_t network_sender_ssrc = htonl(sender_ssrc);
  const uint32_t network_media_ssrc = htonl(media_ssrc);
  memcpy(packet + 4, &network_sender_ssrc, 4);
  memcpy(packet + 8, &network_media_ssrc, 4);

  return 12;
}

int rtcp_get_nack(uint8_t* packet, int len, uint32_t sender_ssrc,
                  uint32_t media_ssrc, uint16_t packet_id, uint16_t bitmask) {
  if (packet == NULL || len != 16)
    return -1;

  memset(packet, 0, len);
  RtcpHeader* header = (RtcpHeader*)packet;
  header->version = 2;
  header->type = RTCP_RTPFB;
  header->rc = 1;
  header->length = htons((len / 4) - 1);

  const uint32_t network_sender_ssrc = htonl(sender_ssrc);
  const uint32_t network_media_ssrc = htonl(media_ssrc);
  const uint16_t network_packet_id = htons(packet_id);
  const uint16_t network_bitmask = htons(bitmask);
  memcpy(packet + 4, &network_sender_ssrc, sizeof(network_sender_ssrc));
  memcpy(packet + 8, &network_media_ssrc, sizeof(network_media_ssrc));
  memcpy(packet + 12, &network_packet_id, sizeof(network_packet_id));
  memcpy(packet + 14, &network_bitmask, sizeof(network_bitmask));
  return len;
}

int rtcp_get_fir(uint8_t* packet, int len, int* seqnr) {
  if (packet == NULL || len != 20 || seqnr == NULL)
    return -1;

  memset(packet, 0, len);
  RtcpHeader* rtcp = (RtcpHeader*)packet;
  *seqnr = *seqnr + 1;
  if (*seqnr < 0 || *seqnr >= 256)
    *seqnr = 0;

  rtcp->version = 2;
  rtcp->type = RTCP_PSFB;
  rtcp->rc = 4;
  rtcp->length = htons((len / 4) - 1);
  RtcpFb* rtcp_fb = (RtcpFb*)rtcp;
  RtcpFir* fir = (RtcpFir*)rtcp_fb->fci;
  fir->seqnr = htonl(*seqnr << 24);

  return 20;
}

RtcpRr rtcp_parse_rr(uint8_t* packet) {
  RtcpRr rtcp_rr;
  memcpy(&rtcp_rr.header, packet, sizeof(rtcp_rr.header));
  memcpy(&rtcp_rr.report_block[0], packet + 8, 6 * sizeof(uint32_t));

  return rtcp_rr;
}
