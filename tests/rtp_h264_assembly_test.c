#include "rtp.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t now_ms;
static int completed_frames;
static uint32_t last_ssrc;
static uint32_t last_timestamp;
static size_t last_size;
static uint8_t last_data[64];

uint32_t ports_get_epoch_time(void) {
  return now_ms;
}

static void on_video_packet(const PeerVideoPacket* packet, void* user_data) {
  (void)user_data;
  assert(packet->size > 4);
  completed_frames++;
  last_ssrc = packet->ssrc;
  last_timestamp = packet->timestamp;
  assert(packet->size <= sizeof(last_data));
  last_size = packet->size;
  memcpy(last_data, packet->data, packet->size);
}

static int receive(RtpDecoder* decoder, uint16_t sequence, uint32_t timestamp,
                   int marker, const uint8_t* payload, size_t size) {
  uint8_t packet[64] = {0x80, 96};
  assert(size <= sizeof(packet) - 12);
  packet[1] |= marker ? 0x80 : 0;
  packet[2] = (uint8_t)(sequence >> 8);
  packet[3] = (uint8_t)sequence;
  packet[4] = (uint8_t)(timestamp >> 24);
  packet[5] = (uint8_t)(timestamp >> 16);
  packet[6] = (uint8_t)(timestamp >> 8);
  packet[7] = (uint8_t)timestamp;
  packet[11] = 42;
  memcpy(packet + 12, payload, size);
  return rtp_decoder_decode(decoder, packet, size + 12);
}

static const uint8_t slice[] = {0x61, 0x80};
static const uint8_t start[] = {0x7c, 0x81, 0x80};
static const uint8_t end[] = {0x7c, 0x41, 0x80};

static void timestamp_gap(RtpDecoder* decoder) {
  receive(decoder, 100, 1000, 0, slice, sizeof(slice));
  receive(decoder, 102, 2000, 1, slice, sizeof(slice));
  now_ms += RTP_REORDER_MAX_HOLD_MS;
  rtp_decoder_poll(decoder);
  assert(completed_frames == 0);
  assert(decoder->access_units_dropped == 2);
  receive(decoder, 103, 3000, 1, slice, sizeof(slice));
  assert(completed_frames == 1);
}

static void fragment_timestamp(RtpDecoder* decoder) {
  receive(decoder, 100, 1000, 0, start, sizeof(start));
  receive(decoder, 101, 2000, 1, end, sizeof(end));
  assert(completed_frames == 0);
  assert(decoder->access_units_dropped == 2);
  receive(decoder, 102, 3000, 1, slice, sizeof(slice));
  assert(completed_frames == 1);
}

static void lost_fragment(RtpDecoder* decoder) {
  receive(decoder, 100, 1000, 0, start, sizeof(start));
  receive(decoder, 102, 1000, 1, end, sizeof(end));
  now_ms += RTP_REORDER_MAX_HOLD_MS;
  rtp_decoder_poll(decoder);
  assert(completed_frames == 0);
  assert(decoder->access_units_dropped == 1);
}

static void parameter_sets(RtpDecoder* decoder) {
  const uint8_t sps[] = {0x67, 0x42};
  const uint8_t pps[] = {0x68, 0x42};
  const uint8_t idr[] = {0x65, 0x80};
  receive(decoder, 100, 1000, 1, sps, sizeof(sps));
  receive(decoder, 101, 2000, 1, pps, sizeof(pps));
  assert(decoder->access_units_dropped == 0);
  receive(decoder, 102, 3000, 1, idr, sizeof(idr));
  assert(completed_frames == 1);
  assert(decoder->cached_sps_size == sizeof(sps));
  assert(decoder->cached_pps_size == sizeof(pps));
  const uint8_t expected[] = {
    0, 0, 0, 1, 0x67, 0x42,
    0, 0, 0, 1, 0x68, 0x42,
    0, 0, 0, 1, 0x65, 0x80,
  };
  assert(last_size == sizeof(expected));
  assert(memcmp(last_data, expected, sizeof(expected)) == 0);
}

static void timestamp_ssrc(RtpDecoder* decoder) {
  receive(decoder, 100, 1000, 0, slice, sizeof(slice));
  receive(decoder, 101, 2000, 1, slice, sizeof(slice));
  assert(completed_frames == 2);
  assert(last_timestamp == 2000);
  assert(last_ssrc == 42);
}

static void malformed_stap(RtpDecoder* decoder) {
  const uint8_t stap[] = {0x78, 0, 2, 0x61, 0x80, 0xff};
  receive(decoder, 100, 1000, 1, stap, sizeof(stap));
  assert(completed_frames == 0);
  assert(decoder->access_units_dropped == 1);
}

static void interrupted_fragment(RtpDecoder* decoder) {
  const uint8_t stap[] = {0x78, 0, 2, 0x61, 0x80};
  receive(decoder, 100, 1000, 0, start, sizeof(start));
  receive(decoder, 101, 1000, 1, stap, sizeof(stap));
  assert(completed_frames == 0);
  assert(decoder->access_units_dropped == 1);
}

static void mismatched_fragment(RtpDecoder* decoder) {
  const uint8_t idr_end[] = {0x7c, 0x45, 0x80};
  receive(decoder, 100, 1000, 0, start, sizeof(start));
  receive(decoder, 101, 1000, 1, idr_end, sizeof(idr_end));
  assert(completed_frames == 0);
  assert(decoder->access_units_dropped == 1);
}

static void invalid_rtp(RtpDecoder* decoder) {
  const uint8_t invalid[] = {0x80, 96, 0x40, 0};
  receive(decoder, 100, 1000, 1, slice, sizeof(slice));
  assert(rtp_decoder_decode(decoder, invalid, sizeof(invalid)) < 0);
  receive(decoder, 101, 2000, 1, slice, sizeof(slice));
  assert(completed_frames == 2);
  assert(decoder->forced_sequence_skips == 0);
}

static void valid_fragment(RtpDecoder* decoder) {
  receive(decoder, 65535, 1000, 0, start, sizeof(start));
  receive(decoder, 0, 1000, 1, end, sizeof(end));
  assert(completed_frames == 1);
  assert(decoder->access_units_dropped == 0);
  assert(last_ssrc == 42);
  const uint8_t expected[] = {0, 0, 0, 1, 0x61, 0x80, 0x80};
  assert(last_size == sizeof(expected));
  assert(memcmp(last_data, expected, sizeof(expected)) == 0);
}

static void valid_stap(RtpDecoder* decoder) {
  const uint8_t stap[] = {0x78, 0, 2, 0x67, 0x42, 0, 2, 0x68, 0x42, 0, 2, 0x65, 0x80};
  const uint8_t expected[] = {
    0, 0, 0, 1, 0x67, 0x42,
    0, 0, 0, 1, 0x68, 0x42,
    0, 0, 0, 1, 0x65, 0x80,
  };
  receive(decoder, 100, 1000, 1, stap, sizeof(stap));
  assert(completed_frames == 1);
  assert(decoder->access_units_dropped == 0);
  assert(last_size == sizeof(expected));
  assert(memcmp(last_data, expected, sizeof(expected)) == 0);
}

static void invalid_fragments(RtpDecoder* decoder) {
  const uint8_t fragments[][3] = {
    {0x7c, 0xc1, 0x80},
    {0x7c, 0x80, 0x80},
    {0x7c, 0x98, 0x80},
    {0x7c, 0x41, 0x80},
    {0x7c, 0x81, 0x80},
  };
  for (size_t i = 0; i < sizeof(fragments) / sizeof(fragments[0]); ++i) {
    receive(decoder, (uint16_t)(100 + i), (uint32_t)(1000 + i), 1,
            fragments[i], sizeof(fragments[i]));
    assert(completed_frames == 0);
    assert(decoder->access_units_dropped == i + 1);
    assert(!decoder->fragment_started);
  }
}

static void partial_multislice(RtpDecoder* decoder) {
  receive(decoder, 100, 1000, 0, slice, sizeof(slice));
  receive(decoder, 101, 1000, 0, start, sizeof(start));
  receive(decoder, 102, 2000, 1, slice, sizeof(slice));
  assert(completed_frames == 1);
  assert(last_timestamp == 2000);
  assert(decoder->access_units_dropped == 1);
}

static void reordered_fragments(RtpDecoder* decoder) {
  const uint8_t middle[] = {0x7c, 0x01, 0x42};
  const uint8_t expected[] = {0, 0, 0, 1, 0x61, 0x80, 0x42, 0x80};
  receive(decoder, 100, 1000, 0, start, sizeof(start));
  receive(decoder, 102, 1000, 1, end, sizeof(end));
  assert(completed_frames == 0);
  now_ms += RTP_REORDER_MAX_HOLD_MS - 1;
  receive(decoder, 101, 1000, 0, middle, sizeof(middle));
  assert(completed_frames == 1);
  assert(decoder->access_units_dropped == 0);
  assert(decoder->forced_sequence_skips == 0);
  assert(last_size == sizeof(expected));
  assert(memcmp(last_data, expected, sizeof(expected)) == 0);
}

int main(int argc, char** argv) {
  const struct {
    const char* name;
    void (*run)(RtpDecoder*);
  } cases[] = {
    {"timestamp_gap", timestamp_gap},
    {"fragment_timestamp", fragment_timestamp},
    {"lost_fragment", lost_fragment},
    {"parameter_sets", parameter_sets},
    {"timestamp_ssrc", timestamp_ssrc},
    {"malformed_stap", malformed_stap},
    {"interrupted_fragment", interrupted_fragment},
    {"mismatched_fragment", mismatched_fragment},
    {"invalid_rtp", invalid_rtp},
    {"valid_fragment", valid_fragment},
    {"valid_stap", valid_stap},
    {"invalid_fragments", invalid_fragments},
    {"partial_multislice", partial_multislice},
    {"reordered_fragments", reordered_fragments},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    if (argc > 1 && strcmp(argv[1], cases[i].name) != 0)
      continue;
    RtpDecoder decoder;
    completed_frames = 0;
    now_ms = 100;
    rtp_decoder_init(&decoder, CODEC_H264, NULL, NULL);
    rtp_decoder_set_video_callback(&decoder, on_video_packet);
    cases[i].run(&decoder);
    rtp_decoder_cleanup(&decoder);
    puts(cases[i].name);
  }
  return 0;
}
