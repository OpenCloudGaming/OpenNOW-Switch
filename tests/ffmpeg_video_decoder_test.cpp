#include "stream/ffmpeg/FFmpegVideoDecoder.hpp"
#include "stream_settings.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <new>
#include <set>
#include <string>

namespace {
int frame_allocation_failure = -1;
bool packet_allocation_failure = false;
bool array_allocation_failure = false;
bool send_again = false;
int receive_error = AVERROR(EAGAIN);
std::set<AVFrame*> allocated_frames;
}

namespace opennow {
StreamSettings LoadStreamSettings() { return {}; }
}

void* operator new[](std::size_t size)
{
    if (array_allocation_failure)
        throw std::bad_alloc();
    void* allocation = std::malloc(size);
    if (!allocation)
        throw std::bad_alloc();
    std::memset(allocation, 0xa5, size);
    return allocation;
}

void operator delete[](void* allocation) noexcept
{
    std::free(allocation);
}

void operator delete[](void* allocation, std::size_t) noexcept
{
    std::free(allocation);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    try {
        return operator new[](size);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

void operator delete[](void* allocation, const std::nothrow_t&) noexcept
{
    std::free(allocation);
}

extern "C" {
AVFrame* __real_av_frame_alloc();
void __real_av_frame_free(AVFrame** frame);
AVPacket* __real_av_packet_alloc();

AVFrame* __wrap_av_frame_alloc()
{
    if (frame_allocation_failure == 0)
        return nullptr;
    if (frame_allocation_failure > 0)
        --frame_allocation_failure;
    AVFrame* frame = __real_av_frame_alloc();
    if (frame)
        allocated_frames.insert(frame);
    return frame;
}

void __wrap_av_frame_free(AVFrame** frame)
{
    if (frame && *frame)
        assert(allocated_frames.erase(*frame) == 1);
    __real_av_frame_free(frame);
}

AVPacket* __wrap_av_packet_alloc()
{
    return packet_allocation_failure ? nullptr : __real_av_packet_alloc();
}

int __wrap_avcodec_send_packet(AVCodecContext*, const AVPacket*)
{
    if (send_again) {
        send_again = false;
        return AVERROR(EAGAIN);
    }
    return 0;
}

int __wrap_avcodec_receive_frame(AVCodecContext*, AVFrame*)
{
    return receive_error;
}
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    const std::string scenario = argv[1];
    if (scenario == "packet")
        packet_allocation_failure = true;
    else if (scenario == "array")
        array_allocation_failure = true;
    else if (scenario.starts_with("allocation"))
        frame_allocation_failure = std::stoi(scenario.substr(10));
    else if (scenario == "receive" || scenario == "receive-again")
        receive_error = AVERROR_INVALIDDATA;
    else
        assert(scenario == "again" || scenario == "eof" || scenario == "send-again");
    send_again = scenario == "receive-again" || scenario == "send-again";
    if (scenario == "eof")
        receive_error = AVERROR_EOF;

    FFmpegVideoDecoder decoder;
    const int result = decoder.setup(
        VIDEO_FORMAT_H264, 16, 16, 60, nullptr, VIDEO_DECODER_FORCE_SOFTWARE);
    if (packet_allocation_failure || array_allocation_failure || frame_allocation_failure >= 0) {
        assert(result < 0);
    } else {
        assert(result == 0);
        uint8_t packet[1 + AV_INPUT_BUFFER_PADDING_SIZE] {};
        const int decoded = decoder.submit_decode_unit(packet, 1, 90000);
        assert(decoded == (scenario.starts_with("receive") ? AVERROR_INVALIDDATA : 0));
    }
    decoder.cleanup();
    decoder.cleanup();
    assert(allocated_frames.empty());
}
