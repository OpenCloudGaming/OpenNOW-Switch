#include "rtcp.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(RtcpReceiverStats) <= 128, "Receiver statistics must remain bounded");

static void write_u32(uint8_t* packet, uint32_t value) {
  packet[0] = (uint8_t)(value >> 24);
  packet[1] = (uint8_t)(value >> 16);
  packet[2] = (uint8_t)(value >> 8);
  packet[3] = (uint8_t)value;
}

static uint32_t read_u32(const uint8_t* packet) {
  return ((uint32_t)packet[0] << 24) | ((uint32_t)packet[1] << 16) |
         ((uint32_t)packet[2] << 8) | packet[3];
}

static int receive(RtcpReceiverStats* stats, uint16_t sequence, uint32_t timestamp,
                   uint32_t arrival_ms, uint32_t ssrc) {
  uint8_t packet[14] = {0x80, 96};
  packet[2] = (uint8_t)(sequence >> 8);
  packet[3] = (uint8_t)sequence;
  write_u32(packet + 4, timestamp);
  write_u32(packet + 8, ssrc);
  packet[12] = 0x61;
  return rtcp_receiver_record_rtp(stats, packet, sizeof(packet), arrival_ms);
}

static void loss_and_intervals(void) {
  RtcpReceiverStats stats = {0};
  uint8_t packet[128];
  assert(!rtcp_receiver_report_due(&stats, 5000));
  assert(rtcp_get_receiver_report(packet, sizeof(packet), 1, &stats, 0, "webrtc-h264") < 0);
  assert(receive(&stats, 100, 9000, 100, 0x11223344));
  assert(receive(&stats, 102, 9180, 102, 0x11223344));
  assert(!rtcp_receiver_report_due(&stats, 1099));
  assert(rtcp_receiver_report_due(&stats, 1100));
  assert(rtcp_get_receiver_report(packet, sizeof(packet), 0x01020304,
                                  &stats, 1100, "webrtc-h264") == 56);
  const uint8_t expected[] = {
    0x81, 201, 0, 7, 1, 2, 3, 4, 0x11, 0x22, 0x33, 0x44,
    85, 0, 0, 1, 0, 0, 0, 102, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0x81, 202, 0, 5, 1, 2, 3, 4, 1, 11,
    'w', 'e', 'b', 'r', 't', 'c', '-', 'h', '2', '6', '4', 0, 0, 0,
  };
  assert(sizeof(expected) == 56);
  assert(memcmp(packet, expected, sizeof(expected)) == 0);
  assert(rtcp_validate_compound(packet, sizeof(expected)));
  rtcp_receiver_report_sent(&stats, 1100, 0);
  assert(stats.expected_prior == 0 && stats.received_prior == 0);
  assert(!rtcp_receiver_report_due(&stats, 1101));
  rtcp_receiver_report_sent(&stats, 2100, 1);
  assert(stats.expected_prior == 3 && stats.received_prior == 2);
  assert(receive(&stats, 101, 9090, 2101, 0x11223344));
  assert(receive(&stats, 103, 9270, 2102, 0x11223344));
  assert(rtcp_get_receiver_report(packet, sizeof(packet), 1, &stats, 3100, "c") > 0);
  assert(read_u32(packet + 12) == 0);
  rtcp_receiver_report_sent(&stats, 3100, 1);
  assert(rtcp_get_receiver_report(packet, sizeof(packet), 1, &stats, 4100, "c") > 0);
  assert(read_u32(packet + 12) == 0);
  assert(receive(&stats, 103, 9270, 4101, 0x11223344));
  assert(rtcp_get_receiver_report(packet, sizeof(packet), 1, &stats, 5100, "c") > 0);
  assert(read_u32(packet + 12) == 0x00ffffff);
  assert(rtcp_get_receiver_report(packet, 31, 1, &stats, 5100, "c") < 0);
  stats.cycles = 0x1000000;
  stats.expected_prior = 0;
  stats.received_prior = 0;
  stats.received = 1;
  assert(rtcp_get_receiver_report(packet, sizeof(packet), 1, &stats, 5100, "c") > 0);
  assert((read_u32(packet + 12) & 0xffffff) == 0x7fffff);
  stats.cycles = 0;
  stats.received = 0x1000000;
  assert(rtcp_get_receiver_report(packet, sizeof(packet), 1, &stats, 5100, "c") > 0);
  assert(read_u32(packet + 12) == 0x00800000);
}

static void wrap_restart_and_jitter(void) {
  RtcpReceiverStats stats = {0};
  uint8_t packet[128];
  assert(receive(&stats, 65534, 0xffffff00u, UINT32_MAX - 1, 42));
  assert(receive(&stats, 0, 0xfffffff0u, 0, 42));
  assert(stats.cycles == 65536 && stats.max_sequence == 0);
  assert(stats.jitter_q4 == 60);
  assert(receive(&stats, 65535, 0xffffff80u, 1, 42));
  assert(stats.cycles == 65536 && stats.max_sequence == 0);
  assert(stats.received == 3);
  assert(rtcp_get_receiver_report(packet, sizeof(packet), 1, &stats, 10, "c") > 0);
  assert(read_u32(packet + 12) == 0);
  assert(read_u32(packet + 16) == 65536);
  assert(read_u32(packet + 20) == stats.jitter_q4 / 16);
  assert(!receive(&stats, 20000, 1800000, 2, 42));
  assert(stats.received == 3);
  assert(receive(&stats, 20001, 1800090, 3, 42));
  assert(stats.received == 1 && stats.base_sequence == 20001 && stats.cycles == 0);
  assert(stats.expected_prior == 0 && stats.received_prior == 0 && stats.jitter_q4 == 0);
  assert(receive(&stats, 100, 9000, 4, 43));
  assert(stats.ssrc == 43 && stats.received == 1 && stats.base_sequence == 100);
  assert(!rtcp_receiver_report_due(&stats, 1003));
  assert(rtcp_receiver_report_due(&stats, 1004));

  memset(&stats, 0, sizeof(stats));
  assert(receive(&stats, 1, 0, 0, 42));
  assert(receive(&stats, 2, 0, 16, 42));
  assert(stats.jitter_q4 == 1440);
  assert(receive(&stats, 3, 1440, 32, 42));
  assert(stats.jitter_q4 == 1350);
}

static void sender_reports(void) {
  RtcpReceiverStats stats = {0};
  uint8_t sr[28] = {0x80, RTCP_SR, 0, 6};
  uint8_t report[128];
  RtcpPacketView view;
  write_u32(sr + 4, 42);
  write_u32(sr + 8, 0x12345678);
  write_u32(sr + 12, 0x9abcdef0);
  assert(rtcp_parse_packet(sr, sizeof(sr), &view) == 0);
  rtcp_receiver_record_sr(&stats, &view, UINT32_MAX - 499);
  assert(receive(&stats, 10, 90, UINT32_MAX - 400, 42));
  assert(stats.have_sr && stats.last_sr == 0x56789abc);
  assert(rtcp_get_receiver_report(report, sizeof(report), 1, &stats, 1000, "c") > 0);
  assert(read_u32(report + 24) == 0x56789abc);
  assert(read_u32(report + 28) == 98304);
  write_u32(sr + 4, 43);
  rtcp_receiver_record_sr(&stats, &view, 1000);
  assert(stats.last_sr_received_ms == UINT32_MAX - 499);
  assert(receive(&stats, 20, 900, 1001, 43));
  assert(!stats.have_sr);
  assert(rtcp_get_receiver_report(report, sizeof(report), 1, &stats, 1100, "c") > 0);
  assert(read_u32(report + 24) == 0 && read_u32(report + 28) == 0);
}

static void malformed_packets(void) {
  RtcpReceiverStats stats = {0};
  uint8_t packet[80] = {0x80, RTCP_SR, 0, 6};
  assert(rtcp_validate_compound(packet, 28));
  for (size_t size = 0; size < 28; ++size)
    assert(!rtcp_validate_compound(packet, size));
  for (size_t tail = 1; tail <= 3; ++tail)
    assert(!rtcp_validate_compound(packet, 28 + tail));
  packet[3] = 1;
  assert(!rtcp_validate_compound(packet, 28));
  packet[3] = 6;
  packet[0] = 0x81;
  assert(!rtcp_validate_compound(packet, 28));
  packet[0] = 0x80;
  packet[28] = 0x80;
  packet[29] = RTCP_RR;
  packet[31] = 1;
  assert(rtcp_validate_compound(packet, 36));
  packet[31] = 20;
  assert(!rtcp_validate_compound(packet, 36));
  packet[31] = 1;
  packet[28] = 0xa0;
  assert(!rtcp_validate_compound(packet, 36));
  packet[35] = 4;
  assert(!rtcp_validate_compound(packet, 36));
  packet[31] = 2;
  packet[35] = 0;
  packet[39] = 4;
  assert(rtcp_validate_compound(packet, 40));
  packet[0] = 0xa0;
  packet[27] = 4;
  assert(!rtcp_validate_compound(packet, 40));

  memset(packet, 0, sizeof(packet));
  packet[0] = 0x81;
  packet[1] = RTCP_SDES;
  packet[3] = 2;
  packet[8] = 1;
  packet[9] = 11;
  assert(!rtcp_validate_compound(packet, 12));
  packet[1] = RTCP_RR;
  assert(!rtcp_validate_compound(packet, 12));
  packet[1] = RTCP_PSFB;
  packet[0] = 0x84;
  assert(!rtcp_validate_compound(packet, 12));
  packet[0] = 0x41;
  assert(!rtcp_validate_compound(packet, 12));

  memset(packet, 0, sizeof(packet));
  packet[0] = 0x80;
  packet[1] = 96;
  assert(!rtcp_receiver_record_rtp(&stats, packet, 4, 0));
  packet[0] = 0x8f;
  assert(!rtcp_receiver_record_rtp(&stats, packet, 12, 0));
  packet[0] = 0x90;
  assert(!rtcp_receiver_record_rtp(&stats, packet, 12, 0));
  packet[15] = 1;
  assert(!rtcp_receiver_record_rtp(&stats, packet, 16, 0));
  packet[0] = 0xa0;
  assert(!rtcp_receiver_record_rtp(&stats, packet, 14, 0));
  assert(!stats.initialized);
}

int main(void) {
  loss_and_intervals();
  wrap_restart_and_jitter();
  sender_reports();
  malformed_packets();
  return 0;
}
