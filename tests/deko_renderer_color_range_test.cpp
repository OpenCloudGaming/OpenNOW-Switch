#include "deko_renderer_under_test.hpp"

#include <iostream>

int main()
{
    deko_test::reset();
    AVFrame* frame = av_frame_alloc();
    assert(frame);
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = 64;
    frame->height = 32;
    frame->colorspace = AVCOL_SPC_BT709;
    assert(av_frame_get_buffer(frame, 32) == 0);
    {
        DKVideoRenderer renderer;
        for (const auto range : {AVCOL_RANGE_MPEG, AVCOL_RANGE_JPEG, AVCOL_RANGE_MPEG}) {
            frame->color_range = range;
            deko_test::completed = deko_test::submitted;
            renderer.drawLatest(nullptr, 1280, 720, frame, 0, 1);
            const bool full = range == AVCOL_RANGE_JPEG;
            assert(renderer.isFrameFullRange(frame) == full);
            assert(deko_test::last_transform[12] == (full ? 0.0f : 16.0f / 255.0f));
            assert(deko_test::last_transform[0] == (full ? 1.0f : 1.1644f));
        }
        frame->format = AV_PIX_FMT_YUVJ420P;
        frame->color_range = AVCOL_RANGE_UNSPECIFIED;
        deko_test::completed = deko_test::submitted;
        renderer.drawLatest(nullptr, 1280, 720, frame, 0, 1);
        assert(renderer.isFrameFullRange(frame));
        assert(deko_test::last_transform[12] == 0.0f);

        AVFrame hardware {};
        hardware.format = AV_PIX_FMT_NVTEGRA;
        hardware.color_range = AVCOL_RANGE_JPEG;
        assert(!renderer.isFrameFullRange(&hardware));
        assert(!renderer.isFrameFullRange(nullptr));
    }
    av_frame_free(&frame);
    assert(deko_test::Memory::live.empty() && deko_test::descriptors.empty());
    std::cout << "Deko3D renderer color range: PASS\n";
}
