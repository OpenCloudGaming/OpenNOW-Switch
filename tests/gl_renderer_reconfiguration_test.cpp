#include "stream/OpenGL/GLVideoRenderer.hpp"

#include <EGL/egl.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace opennow {
bool StreamDiagnosticsEnabled() { return false; }
}

static std::array<uint8_t, 4> center_pixel(int width, int height)
{
    std::array<uint8_t, 4> pixel {};
    glReadPixels(width / 2, height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    return pixel;
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    const std::string scenario = argv[1];
    assert(scenario == "resize" || scenario == "color" || scenario == "p010");
    setenv("EGL_PLATFORM", "surfaceless", 0);
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    assert(display != EGL_NO_DISPLAY);
    assert(eglInitialize(display, nullptr, nullptr));
    assert(eglBindAPI(EGL_OPENGL_API));
    const EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE
    };
    EGLConfig config;
    EGLint count = 0;
    assert(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1);
    const EGLint surface_attributes[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attributes);
    assert(surface != EGL_NO_SURFACE);
    const EGLint context_attributes[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE
    };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    assert(context != EGL_NO_CONTEXT);
    assert(eglMakeCurrent(display, surface, surface, context));
    assert(gladLoadGLLoader(reinterpret_cast<GLADloadproc>(eglGetProcAddress)));

    AVFrame* frame = av_frame_alloc();
    assert(frame);
    frame->format = scenario == "p010" ? AV_PIX_FMT_P010 : AV_PIX_FMT_YUV420P;
    frame->width = frame->height = 32;
    frame->colorspace = AVCOL_SPC_BT709;
    frame->color_range = AVCOL_RANGE_MPEG;
    assert(av_frame_get_buffer(frame, 32) == 0);
    if (scenario == "p010") {
        for (int row = 0; row < frame->height; ++row) {
            auto* pixels = reinterpret_cast<uint16_t*>(frame->data[0] + row * frame->linesize[0]);
            std::fill_n(pixels, frame->width, static_cast<uint16_t>((row < 16 ? 64 : 940) << 6));
        }
        for (int row = 0; row < frame->height / 2; ++row) {
            auto* pixels = reinterpret_cast<uint16_t*>(frame->data[1] + row * frame->linesize[1]);
            std::fill_n(pixels, frame->width, static_cast<uint16_t>(512 << 6));
        }
    } else {
        for (int row = 0; row < frame->height; ++row)
            std::memset(frame->data[0] + row * frame->linesize[0], 180, frame->width);
        for (int plane = 1; plane <= 2; ++plane)
            for (int row = 0; row < frame->height / 2; ++row)
                std::memset(frame->data[plane] + row * frame->linesize[plane], 128, frame->width / 2);
    }
    {
        GLVideoRenderer renderer;
        renderer.drawLatest(nullptr, 64, 64, frame, 0, 1);
        const auto initial = center_pixel(64, 64);
        if (scenario == "resize") {
            renderer.drawLatest(nullptr, 32, 32, frame, 0, 1);
            const auto resized = center_pixel(32, 32);
            std::printf("GL resize RGB before=%u/%u/%u after=%u/%u/%u\n",
                initial[0], initial[1], initial[2], resized[0], resized[1], resized[2]);
            std::fflush(stdout);
            assert(initial == resized);
        } else if (scenario == "color") {
            frame->color_range = AVCOL_RANGE_JPEG;
            renderer.drawLatest(nullptr, 64, 64, frame, 0, 2);
            const auto full = center_pixel(64, 64);
            assert(full[0] >= 178 && full[0] <= 182);
            assert(initial[0] > full[0] + 5);
            frame->format = AV_PIX_FMT_YUVJ420P;
            frame->color_range = AVCOL_RANGE_UNSPECIFIED;
            frame->colorspace = AVCOL_SPC_UNSPECIFIED;
            renderer.drawLatest(nullptr, 64, 64, frame, 0, 3);
            assert(center_pixel(64, 64) == full);
            for (int row = 0; row < frame->height / 2; ++row) {
                std::memset(frame->data[1] + row * frame->linesize[1], 80, frame->width / 2);
                std::memset(frame->data[2] + row * frame->linesize[2], 160, frame->width / 2);
            }
            renderer.drawLatest(nullptr, 64, 64, frame, 0, 4);
            const auto bt601 = center_pixel(64, 64);
            frame->colorspace = AVCOL_SPC_BT709;
            renderer.drawLatest(nullptr, 64, 64, frame, 0, 4);
            assert(center_pixel(64, 64) != bt601);
        } else {
            std::array<uint8_t, 4> bottom {};
            glReadPixels(32, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bottom.data());
            std::printf("GL P010 bottom RGB=%u/%u/%u\n", bottom[0], bottom[1], bottom[2]);
            std::fflush(stdout);
            assert(bottom[0] > 240 && bottom[1] > 240 && bottom[2] > 240);
        }
    }
    av_frame_free(&frame);
    assert(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    assert(eglDestroyContext(display, context));
    assert(eglDestroySurface(display, surface));
    assert(eglTerminate(display));
    std::printf("OpenGL renderer %s: PASS\n", scenario.c_str());
}
