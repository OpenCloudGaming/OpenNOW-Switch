#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include "rtcp.h"

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
