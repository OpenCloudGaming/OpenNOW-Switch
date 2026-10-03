#include "deko_renderer_under_test.hpp"

#include <iostream>

int main()
{
    for (const std::string mode : {"Original", "Clarity", "Adaptive"})
    {
        deko_test::reset();
        AVFrame* frame = av_frame_alloc();
        assert(frame);
        frame->format = AV_PIX_FMT_YUV420P;
        frame->width = 64;
        frame->height = 32;
        assert(av_frame_get_buffer(frame, 32) == 0);
        const auto expected = opennow::video::ResolveQualityTuning(mode);
        {
            DKVideoRenderer renderer(mode);
            for (const int screen_width : {1280, 1920, 1280})
            {
                deko_test::completed = deko_test::submitted;
                renderer.drawLatest(nullptr, screen_width, screen_width * 9 / 16, frame, 0, 1);
                assert(deko_test::last_transform[22] == expected.denoise_strength);
                assert(deko_test::last_transform[23] == expected.sharpen_strength);
            }
        }
        av_frame_free(&frame);
        assert(deko_test::Memory::live.empty() && deko_test::descriptors.empty());
    }
    std::cout << "Deko3D quality snapshot through screen reconfiguration: PASS\n";
}
