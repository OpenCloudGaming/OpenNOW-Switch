#include "stream/ffmpeg/FFmpegVideoDecoder.hpp"
#include "stream_settings.hpp"

#include <cassert>
#include <cstdio>

namespace {
bool fail_allocation = false;
bool send_again = false;
int sends = 0;
const uint8_t* rejected_data = nullptr;
const AVBuffer* rejected_buffer = nullptr;
uint8_t rejected_byte = 0;
AVPacket* retained = nullptr;
}

namespace opennow {
StreamSettings LoadStreamSettings() { return {}; }
}

extern "C" {
int __real_av_new_packet(AVPacket* packet, int size);

int __wrap_av_new_packet(AVPacket* packet, int size)
{
    return fail_allocation ? AVERROR(ENOMEM) : __real_av_new_packet(packet, size);
}

int __wrap_avcodec_send_packet(AVCodecContext*, const AVPacket* packet)
{
    ++sends;
    assert(packet->buf && packet->size == 1);
    for (int i = 0; i < AV_INPUT_BUFFER_PADDING_SIZE; ++i)
        assert(packet->data[packet->size + i] == 0);
    if (send_again) {
        send_again = false;
        rejected_data = packet->data;
        rejected_buffer = packet->buf->buffer;
        rejected_byte = packet->data[0];
        return AVERROR(EAGAIN);
    }
    if (rejected_data) {
        assert(packet->data == rejected_data && packet->buf->buffer == rejected_buffer);
        assert(packet->data[0] == rejected_byte);
        rejected_data = nullptr;
    }
    if (!retained) {
        retained = av_packet_alloc();
        assert(retained && av_packet_ref(retained, packet) == 0);
        assert(retained->data == packet->data);
    }
    return 0;
}

int __wrap_avcodec_receive_frame(AVCodecContext*, AVFrame*)
{
    return AVERROR(EAGAIN);
}
}

int main()
{
    FFmpegVideoDecoder decoder;
    assert(decoder.setup(VIDEO_FORMAT_H264, 16, 16, 60, nullptr,
                         VIDEO_DECODER_FORCE_SOFTWARE) == 0);
    uint8_t input[] = {0x55};
    fail_allocation = true;
    assert(decoder.submit_decode_unit(input, sizeof(input), 90000) == AVERROR(ENOMEM));
    assert(sends == 0 && !retained);
    fail_allocation = false;
    send_again = true;
    assert(decoder.submit_decode_unit(input, sizeof(input), 90000) == 0);
    assert(sends == 2 && !rejected_data);
    assert(retained && retained->pts == 90000 && retained->dts == 90000);
    input[0] = 0x66;
    assert(retained->data[0] == 0x55);
    assert(decoder.submit_decode_unit(input, sizeof(input), 91500) == 0);
    assert(sends == 3 && retained->data[0] == 0x55);
    fail_allocation = true;
    assert(decoder.submit_decode_unit(input, sizeof(input), 93000) == AVERROR(ENOMEM));
    assert(sends == 3 && retained->data[0] == 0x55);
    fail_allocation = false;
    assert(decoder.submit_decode_unit(input, sizeof(input), 94500) == 0);
    assert(sends == 4 && retained->data[0] == 0x55);
    decoder.cleanup();
    assert(retained->data[0] == 0x55);
    av_packet_free(&retained);
    std::puts("FFmpeg packet padding, retry, allocation failure and lifetime: PASS");
}
