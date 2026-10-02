#include "stream/ffmpeg/FFmpegVideoDecoder.hpp"
#include "stream_settings.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

namespace opennow {
StreamSettings LoadStreamSettings() { return {}; }
}

static void decode_stream(FFmpegVideoDecoder& decoder, const char* path,
                          int width, int height)
{
    std::ifstream input(path, std::ios::binary);
    assert(input);
    std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(input), {});
    const size_t size = bytes.size();
    assert(size > 0);
    bytes.resize(size + AV_INPUT_BUFFER_PADDING_SIZE);
    AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_H264);
    AVCodecContext* context = avcodec_alloc_context3(nullptr);
    assert(parser && context);
    size_t offset = 0;
    int frames = 0;
    int access_units = 0;
    while (offset < size) {
        uint8_t* packet = nullptr;
        int packet_size = 0;
        const int consumed = av_parser_parse2(parser, context, &packet, &packet_size,
            bytes.data() + offset, static_cast<int>(size - offset),
            AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        assert(consumed >= 0 && (consumed > 0 || packet_size > 0));
        offset += consumed;
        if (packet_size == 0)
            continue;
        std::vector<uint8_t> unpadded(packet, packet + packet_size);
        const int decoded = decoder.submit_decode_unit(
            unpadded.data(), packet_size, 90000 + access_units++ * 1500);
        assert(decoded >= 0);
        std::fill(unpadded.begin(), unpadded.end(), 0xff);
        frames += decoded;
        if (decoded > 0) {
            bool observed = false;
            AVFrameHolder::instance().get([&](AVFrame* frame, uint64_t generation, bool reused) {
                assert(!reused && generation > 0);
                assert(frame->width == width && frame->height == height);
                assert(frame->format == AV_PIX_FMT_YUV420P && frame->buf[0]);
                assert(frame->data[0][0] >= 120 && frame->data[0][0] <= 130);
                observed = true;
            });
            assert(observed);
        }
    }
    assert(access_units >= 15 && frames >= 10);
    av_parser_close(parser);
    avcodec_free_context(&context);
    std::printf("Real FFmpeg decode %dx%d: %d access units, %d frames PASS\n",
                width, height, access_units, frames);
}

int main(int argc, char** argv)
{
    assert(argc == 3);
    FFmpegVideoDecoder decoder("Adaptive");
    assert(decoder.setup(VIDEO_FORMAT_H264, 64, 32, 60, nullptr,
                         VIDEO_DECODER_FORCE_SOFTWARE) == 0);
    assert(!decoder.uses_hardware_frames());
    decode_stream(decoder, argv[1], 64, 32);
    decoder.reset_stream();
    decode_stream(decoder, argv[2], 32, 64);
    decoder.cleanup();
}
