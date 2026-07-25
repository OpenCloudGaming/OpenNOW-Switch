#include "rtp.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static uint32_t fake_now_ms;
static int decoded_frames;

uint32_t ports_get_epoch_time(void)
{
    return fake_now_ms;
}

static void on_video_packet(const PeerVideoPacket* packet, void* user_data)
{
    (void)user_data;
    assert(packet != NULL);
    assert(packet->size > 0);
    decoded_frames++;
}

static void write_be16(uint8_t* out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static void write_be32(uint8_t* out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static void submit_frame(RtpDecoder* decoder, uint16_t sequence, uint32_t timestamp)
{
    uint8_t packet[13] = {0};
    packet[0] = 0x80;
    packet[1] = 0x80 | 96;
    write_be16(packet + 2, sequence);
    write_be32(packet + 4, timestamp);
    write_be32(packet + 8, 1);
    packet[12] = 0x01;
    assert(rtp_decoder_decode(decoder, packet, sizeof(packet)) >= 0);
}

static void initialize(RtpDecoder* decoder)
{
    memset(decoder, 0, sizeof(*decoder));
    rtp_decoder_init(decoder, CODEC_H264, NULL, NULL);
    rtp_decoder_set_video_callback(decoder, on_video_packet);
}

int main(void)
{
    RtpDecoder decoder;
    initialize(&decoder);
    decoded_frames = 0;

    fake_now_ms = 1000;
    submit_frame(&decoder, 100, 90000);
    fake_now_ms = 1001;
    submit_frame(&decoder, 102, 93000);
    assert(decoded_frames == 1);
    assert(decoder.reorder_buffered_packets == 1);

    fake_now_ms = 1001 + RTP_REORDER_MAX_HOLD_MS;
    submit_frame(&decoder, 103, 96000);
    assert(decoded_frames == 2);
    assert(decoder.access_units_dropped == 1);
    assert(decoder.forced_sequence_skips == 1);
    assert(decoder.reorder_buffered_packets == 0);
    rtp_decoder_cleanup(&decoder);

    initialize(&decoder);
    decoded_frames = 0;
    fake_now_ms = 2000;
    submit_frame(&decoder, 200, 90000);
    fake_now_ms = 2001;
    submit_frame(&decoder, 202, 93000);
    fake_now_ms = 2010;
    submit_frame(&decoder, 201, 91500);
    assert(decoded_frames == 3);
    assert(decoder.forced_sequence_skips == 0);
    assert(decoder.reorder_buffered_packets == 0);
    rtp_decoder_cleanup(&decoder);
    return 0;
}
