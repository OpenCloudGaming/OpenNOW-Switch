#include "twcc.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#define MEDIA_SSRC 0x11223344u
#define SENDER_SSRC 0xA1B2C3D4u

typedef struct ParsedFeedback {
  uint32_t sender_ssrc;
  uint32_t media_ssrc;
  uint16_t base_sequence;
  uint16_t count;
  int32_t reference;
  uint8_t feedback_count;
  uint8_t received[TWCC_HISTORY_SIZE];
  int64_t arrival_us[TWCC_HISTORY_SIZE];
} ParsedFeedback;

static uint16_t read_u16(const uint8_t* data) {
  return (uint16_t)((data[0] << 8) | data[1]);
}

static uint32_t read_u32(const uint8_t* data) {
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
         ((uint32_t)data[2] << 8) | data[3];
}

static size_t rtp_one_byte(uint8_t* packet, uint8_t id, uint16_t sequence, int with_csrc) {
  memset(packet, 0, 64);
  packet[0] = (uint8_t)(0x90 | (with_csrc ? 1 : 0));
  packet[1] = 96;
  packet[8] = (uint8_t)(MEDIA_SSRC >> 24);
  packet[9] = (uint8_t)(MEDIA_SSRC >> 16);
  packet[10] = (uint8_t)(MEDIA_SSRC >> 8);
  packet[11] = (uint8_t)MEDIA_SSRC;
  size_t pos = 12 + (with_csrc ? 4 : 0);
  packet[pos++] = 0xBE;
  packet[pos++] = 0xDE;
  packet[pos++] = 0;
  packet[pos++] = 2;
  packet[pos++] = 0x72;
  packet[pos++] = 1;
  packet[pos++] = 2;
  packet[pos++] = 3;
  packet[pos++] = 0;
  packet[pos++] = (uint8_t)((id << 4) | 1);
  packet[pos++] = (uint8_t)(sequence >> 8);
  packet[pos++] = (uint8_t)sequence;
  packet[pos++] = 0x65;
  return pos;
}

static size_t rtp_two_byte(uint8_t* packet, uint8_t id, uint16_t sequence) {
  memset(packet, 0, 64);
  packet[0] = 0x90;
  packet[1] = 96;
  packet[8] = (uint8_t)(MEDIA_SSRC >> 24);
  packet[9] = (uint8_t)(MEDIA_SSRC >> 16);
  packet[10] = (uint8_t)(MEDIA_SSRC >> 8);
  packet[11] = (uint8_t)MEDIA_SSRC;
  size_t pos = 12;
  packet[pos++] = 0x10;
  packet[pos++] = 0x00;
  packet[pos++] = 0;
  packet[pos++] = 2;
  packet[pos++] = 1;
  packet[pos++] = 1;
  packet[pos++] = 9;
  packet[pos++] = id;
  packet[pos++] = 2;
  packet[pos++] = (uint8_t)(sequence >> 8);
  packet[pos++] = (uint8_t)sequence;
  packet[pos++] = 0;
  return pos;
}

static void parse_feedback(const uint8_t* packet, int size, ParsedFeedback* parsed) {
  memset(parsed, 0, sizeof(*parsed));
  assert(size >= 20 && size % 4 == 0);
  assert((packet[0] >> 6) == 2);
  assert((packet[0] & 0x1F) == 15);
  assert(packet[1] == 205);
  assert((size_t)(read_u16(packet + 2) + 1) * 4 == (size_t)size);
  int end = size;
  if (packet[0] & 0x20) {
    const uint8_t padding = packet[size - 1];
    assert(padding > 0 && padding < 4);
    end -= padding;
  }

  parsed->sender_ssrc = read_u32(packet + 4);
  parsed->media_ssrc = read_u32(packet + 8);
  parsed->base_sequence = read_u16(packet + 12);
  parsed->count = read_u16(packet + 14);
  parsed->reference = (int32_t)(((uint32_t)packet[16] << 16) | ((uint32_t)packet[17] << 8) | packet[18]);
  parsed->feedback_count = packet[19];

  uint8_t symbols[TWCC_HISTORY_SIZE];
  int pos = 20;
  size_t decoded = 0;
  while (decoded < parsed->count) {
    assert(pos + 2 <= end);
    const uint16_t chunk = read_u16(packet + pos);
    pos += 2;
    if (!(chunk & 0x8000)) {
      const uint8_t symbol = (uint8_t)((chunk >> 13) & 3);
      for (uint16_t i = 0; i < (chunk & 0x1FFF) && decoded < parsed->count; i++)
        symbols[decoded++] = symbol;
    } else if (chunk & 0x4000) {
      for (int i = 0; i < 7 && decoded < parsed->count; i++)
        symbols[decoded++] = (uint8_t)((chunk >> (12 - 2 * i)) & 3);
    } else {
      for (int i = 0; i < 14 && decoded < parsed->count; i++)
        symbols[decoded++] = (uint8_t)((chunk >> (13 - i)) & 1);
    }
  }

  int64_t time_us = (int64_t)parsed->reference * 64000;
  for (size_t i = 0; i < parsed->count; i++) {
    if (symbols[i] == 0)
      continue;
    int32_t ticks;
    if (symbols[i] == 1) {
      assert(pos + 1 <= end);
      ticks = packet[pos++];
    } else {
      assert(symbols[i] == 2);
      assert(pos + 2 <= end);
      ticks = (int16_t)read_u16(packet + pos);
      pos += 2;
    }
    time_us += (int64_t)ticks * 250;
    parsed->received[i] = 1;
    parsed->arrival_us[i] = time_us;
  }
  assert(pos == end);
}

static void assert_arrival(const ParsedFeedback* parsed, size_t index, uint64_t expected_us) {
  assert(parsed->received[index]);
  const int64_t error = parsed->arrival_us[index] - (int64_t)expected_us;
  assert(error >= -125 && error <= 125);
}

static void sdp_extension_id(void) {
  assert(twcc_parse_extension_id("a=extmap:3 " TWCC_EXTENSION_URI) == 3);
  assert(twcc_parse_extension_id("a=extmap:5/recvonly " TWCC_EXTENSION_URI) == 5);
  assert(twcc_parse_extension_id("a=extmap:7 http://www.webrtc.org/experiments/rtp-hdrext/video-timing") == 0);
  assert(twcc_parse_extension_id("a=extmap: " TWCC_EXTENSION_URI) == 0);
  assert(twcc_parse_extension_id("a=rtcp-fb:96 transport-cc") == 0);
}

static void header_extension_parsing(void) {
  uint8_t packet[64];
  uint16_t sequence = 0;
  size_t size = rtp_one_byte(packet, 3, 0xBEEF, 0);
  assert(twcc_read_sequence(packet, size, 3, &sequence) && sequence == 0xBEEF);
  assert(!twcc_read_sequence(packet, size, 4, &sequence));
  size = rtp_one_byte(packet, 3, 0x1234, 1);
  assert(twcc_read_sequence(packet, size, 3, &sequence) && sequence == 0x1234);
  assert(!twcc_read_sequence(packet, 20, 3, &sequence));
  size = rtp_two_byte(packet, 3, 0x0102);
  assert(twcc_read_sequence(packet, size, 3, &sequence) && sequence == 0x0102);
  packet[0] &= (uint8_t)~0x10;
  assert(!twcc_read_sequence(packet, size, 3, &sequence));
}

static void loss_reorder_and_padding(void) {
  static TwccReceiver receiver;
  twcc_receiver_init(&receiver, 3);
  uint8_t rtp[64];
  const uint64_t start_us = 10ull * 1000 * 1000 + 12345;
  const uint16_t sequences[] = {65530, 65531, 65533, 65535, 65534, 0, 1, 3};
  const uint64_t offsets[] = {0, 1000, 2000, 2600, 2700, 90000, 90250, 91000};
  for (size_t i = 0; i < sizeof(sequences) / sizeof(sequences[0]); i++) {
    const size_t size = rtp_one_byte(rtp, 3, sequences[i], 0);
    assert(twcc_receiver_record(&receiver, rtp, size, start_us + offsets[i]));
  }
  assert(!twcc_receiver_record(&receiver, rtp, rtp_one_byte(rtp, 3, 1, 0), start_us + 95000));
  assert(!twcc_receiver_record(&receiver, rtp, rtp_one_byte(rtp, 4, 9, 0), start_us + 95000));

  assert(twcc_receiver_feedback_due(&receiver, start_us + 91000));
  uint8_t packet[TWCC_MAX_FEEDBACK_SIZE];
  const int size = twcc_receiver_build_feedback(&receiver, SENDER_SSRC, packet, sizeof(packet));
  assert(size > 0);
  twcc_receiver_feedback_sent(&receiver, start_us + 91000);
  assert(!twcc_receiver_feedback_due(&receiver, start_us + 91000 + TWCC_FEEDBACK_INTERVAL_US));

  ParsedFeedback parsed;
  parse_feedback(packet, size, &parsed);
  assert(parsed.sender_ssrc == SENDER_SSRC);
  assert(parsed.media_ssrc == MEDIA_SSRC);
  assert(parsed.base_sequence == 65530);
  assert(parsed.count == 10);
  assert(parsed.feedback_count == 0);
  assert(parsed.reference == (int32_t)(start_us / 64000));
  assert_arrival(&parsed, 0, start_us);
  assert_arrival(&parsed, 1, start_us + 1000);
  assert(!parsed.received[2]);
  assert_arrival(&parsed, 3, start_us + 2000);
  assert_arrival(&parsed, 4, start_us + 2700);
  assert_arrival(&parsed, 5, start_us + 2600);
  assert_arrival(&parsed, 6, start_us + 90000);
  assert_arrival(&parsed, 7, start_us + 90250);
  assert(!parsed.received[8]);
  assert_arrival(&parsed, 9, start_us + 91000);

  assert(twcc_receiver_build_feedback(&receiver, SENDER_SSRC, packet, sizeof(packet)) == 0);
  assert(!twcc_receiver_record(&receiver, rtp, rtp_one_byte(rtp, 3, 2, 0), start_us + 96000));
  assert(receiver.late_packets == 1);

  assert(twcc_receiver_record(&receiver, rtp, rtp_one_byte(rtp, 3, 4, 0), start_us + 97000));
  const int next_size = twcc_receiver_build_feedback(&receiver, SENDER_SSRC, packet, sizeof(packet));
  parse_feedback(packet, next_size, &parsed);
  assert(parsed.base_sequence == 4 && parsed.count == 1 && parsed.feedback_count == 1);
  assert_arrival(&parsed, 0, start_us + 97000);
}

static void large_gap_splits_feedback(void) {
  static TwccReceiver receiver;
  twcc_receiver_init(&receiver, 3);
  uint8_t rtp[64];
  const uint64_t start_us = 5ull * 1000 * 1000;
  assert(twcc_receiver_record(&receiver, rtp, rtp_one_byte(rtp, 3, 100, 0), start_us));
  assert(twcc_receiver_record(&receiver, rtp, rtp_one_byte(rtp, 3, 101, 0), start_us + 9000000));

  uint8_t packet[TWCC_MAX_FEEDBACK_SIZE];
  ParsedFeedback parsed;
  int size = twcc_receiver_build_feedback(&receiver, SENDER_SSRC, packet, sizeof(packet));
  parse_feedback(packet, size, &parsed);
  assert(parsed.base_sequence == 100 && parsed.count == 1);
  size = twcc_receiver_build_feedback(&receiver, SENDER_SSRC, packet, sizeof(packet));
  parse_feedback(packet, size, &parsed);
  assert(parsed.base_sequence == 101 && parsed.count == 1);
  assert_arrival(&parsed, 0, start_us + 9000000);
}

static void bounded_history_and_size(void) {
  static TwccReceiver receiver;
  twcc_receiver_init(&receiver, 3);
  uint8_t rtp[64];
  const uint64_t start_us = 1000000;
  for (uint32_t i = 0; i < TWCC_HISTORY_SIZE + 500; i++) {
    const uint16_t sequence = (uint16_t)(40000 + i);
    assert(twcc_receiver_record(&receiver, rtp, rtp_one_byte(rtp, 3, sequence, 0),
                                start_us + (uint64_t)i * 300));
  }
  assert(receiver.max_sequence - receiver.next_sequence + 1 == TWCC_HISTORY_SIZE);

  uint8_t packet[TWCC_MAX_FEEDBACK_SIZE];
  ParsedFeedback parsed;
  size_t reported = 0;
  uint16_t expected_base = (uint16_t)(40000 + 500);
  int size;
  while ((size = twcc_receiver_build_feedback(&receiver, SENDER_SSRC, packet, sizeof(packet))) > 0) {
    assert(size <= TWCC_MAX_FEEDBACK_SIZE);
    parse_feedback(packet, size, &parsed);
    assert(parsed.base_sequence == expected_base);
    for (size_t i = 0; i < parsed.count; i++)
      assert_arrival(&parsed, i, start_us + (uint64_t)(500 + reported + i) * 300);
    reported += parsed.count;
    expected_base = (uint16_t)(expected_base + parsed.count);
  }
  assert(size == 0);
  assert(reported == TWCC_HISTORY_SIZE);

  uint8_t tiny[16];
  assert(twcc_receiver_record(&receiver, rtp, rtp_one_byte(rtp, 3, expected_base, 0), start_us + 10000000));
  assert(twcc_receiver_build_feedback(&receiver, SENDER_SSRC, tiny, sizeof(tiny)) < 0);
}

static void disabled_without_extension(void) {
  static TwccReceiver receiver;
  twcc_receiver_init(&receiver, 0);
  uint8_t rtp[64];
  assert(!twcc_receiver_record(&receiver, rtp, rtp_one_byte(rtp, 3, 1, 0), 1000));
  assert(!twcc_receiver_feedback_due(&receiver, 1000000));
}

int main(void) {
  sdp_extension_id();
  header_extension_parsing();
  loss_reorder_and_padding();
  large_gap_splits_feedback();
  bounded_history_and_size();
  disabled_without_extension();
  return 0;
}
