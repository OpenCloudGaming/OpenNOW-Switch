#include "deko_renderer_under_test.hpp"

#include <iostream>
#include <thread>

namespace {

struct FrameDeleter {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
using Frame = std::unique_ptr<AVFrame, FrameDeleter>;

Frame software_frame(int width, int height, int format = AV_PIX_FMT_YUV420P)
{
    Frame frame(av_frame_alloc());
    assert(frame);
    frame->format = format;
    frame->width = width;
    frame->height = height;
    assert(av_frame_get_buffer(frame.get(), 32) == 0);
    return frame;
}

Frame hardware_frame(int width, int height)
{
    Frame frame(av_frame_alloc());
    assert(frame);
    frame->format = AV_PIX_FMT_NVTEGRA;
    frame->width = width;
    frame->height = height;
    auto* map = static_cast<AVNVTegraMap*>(av_mallocz(sizeof(AVNVTegraMap)));
    map->size = width * height * 3 / 2;
    map->address = static_cast<uint8_t*>(av_mallocz(map->size));
    static uint32_t handle = 0;
    map->handle = ++handle;
    auto* descriptor = static_cast<AVNVTegraFrame*>(av_mallocz(sizeof(AVNVTegraFrame)));
    descriptor->map_ref = av_buffer_create(reinterpret_cast<uint8_t*>(map), sizeof(*map),
        [](void*, uint8_t* pointer) {
            auto* map = reinterpret_cast<AVNVTegraMap*>(pointer);
            av_free(map->address);
            av_free(map);
            ++deko_test::hardware_storage_freed;
        }, nullptr, 0);
    frame->buf[0] = av_buffer_create(reinterpret_cast<uint8_t*>(descriptor), sizeof(*descriptor),
        [](void*, uint8_t* pointer) {
            auto* descriptor = reinterpret_cast<AVNVTegraFrame*>(pointer);
            av_buffer_unref(&descriptor->map_ref);
            av_free(descriptor);
        }, nullptr, 0);
    frame->data[0] = map->address;
    frame->data[1] = map->address + width * height;
    return frame;
}

void draw(DKVideoRenderer& renderer, const Frame& frame, uint64_t generation = 1,
          int screen_width = 1280, int screen_height = 720)
{
    renderer.drawLatest(nullptr, screen_width, screen_height, frame.get(), 0, generation);
    assert(deko_test::wait_idle == 0);
}

void failure_sweep(bool hardware)
{
    using namespace deko_test;
    reset();
    auto initial = software_frame(64, 32);
    auto resized = hardware ? hardware_frame(32, 32) : software_frame(32, 32);
    int initialization_allocations = 0;
    {
        DKVideoRenderer renderer;
        draw(renderer, resized);
        initialization_allocations = allocation;
        assert(draws == 1);
    }
    for (int failure = 1; failure <= initialization_allocations; ++failure) {
        reset();
        {
            DKVideoRenderer renderer;
            fail_at = failure;
            draw(renderer, resized);
            assert(draws == 0 && submissions == 0);
            assert(Memory::live.empty() && descriptors.empty());
            const int attempts = allocation;
            fail_at = -1;
            draw(renderer, resized);
            assert(allocation == attempts && draws == 0);
        }
        assert(wait_idle == 0);

        reset();
        {
            DKVideoRenderer renderer;
            draw(renderer, initial);
            const size_t active_memory = Memory::live.size();
            fail_at = allocation + failure;
            draw(renderer, resized, 2);
            assert(draws == 2 && last_luma_bytes == 64 * 32);
            assert(Memory::live.size() == active_memory && descriptors.size() == 2);
            const int attempts = allocation;
            fail_at = -1;
            draw(renderer, resized, 2);
            assert(allocation == attempts && last_luma_bytes == 64 * 32);
        }
        assert(Memory::live.empty() && descriptors.empty());
    }
    std::cout << (hardware ? "hardware" : "software") << " allocation failure sweep: "
              << initialization_allocations << " sites at initialization and replacement\n";
}

void resize_retirement_and_screen()
{
    using namespace deko_test;
    reset();
    auto first = software_frame(64, 32);
    auto second = software_frame(32, 32, AV_PIX_FMT_NV12);
    auto third = software_frame(32, 64);
    {
        DKVideoRenderer renderer;
        draw(renderer, first, 0);
        draw(renderer, first, 0);
        const auto first_fence = submitted;
        draw(renderer, second, 2);
        assert(last_luma_bytes == 32 * 32 && descriptors.size() == 4);
        assert(renderer.video_render_stats()->surface_mappings == 8);
        const int attempts = allocation;
        completed = first_fence - 1;
        for (int repeat = 0; repeat < 20; ++repeat)
            draw(renderer, third, 3);
        assert(allocation == attempts && descriptors.size() == 4);
        assert(last_luma_bytes == 32 * 32);
        completed = first_fence;
        draw(renderer, third, 3);
        assert(last_luma_bytes == 32 * 64 && descriptors.size() == 4);
        completed = submitted;
        draw(renderer, third, 3);
        assert(descriptors.size() == 2);
        const auto old_transform = last_transform;
        draw(renderer, third, 3, 800, 800);
        assert(descriptors.size() == 4 && last_transform != old_transform);
        assert(last_transform[20] == 1.0f / 32.0f);
        assert(last_transform[21] == 1.0f / 64.0f);
        assert(last_luma_bytes == 32 * 64);
        completed = submitted;
        draw(renderer, third, 3, 800, 800);
        assert(descriptors.size() == 2);
    }
    assert(Memory::live.empty() && descriptors.empty() && wait_idle == 1);
}

void backend_retirement()
{
    using namespace deko_test;
    reset();
    auto software = software_frame(64, 32);
    auto hardware = hardware_frame(64, 32);
    {
        DKVideoRenderer renderer;
        draw(renderer, hardware, 1);
        const auto hardware_fence = submitted;
        hardware.reset();
        assert(hardware_storage_freed == 0);
        draw(renderer, software, 1);
        assert(hardware_storage_freed == 0 && descriptors.size() == 4);
        assert(renderer.video_render_stats()->retained_surfaces == 1);
        completed = hardware_fence;
        draw(renderer, software, 1);
        assert(hardware_storage_freed == 1 && descriptors.size() == 2);
        hardware = hardware_frame(64, 32);
        draw(renderer, hardware, 1);
        assert(renderer.video_render_stats()->retained_surfaces == 1);
        assert(renderer.video_render_stats()->surface_mappings == 5);
        hardware.reset();
        assert(hardware_storage_freed == 1);
        completed = submitted;
        draw(renderer, Frame{}, 1);
        assert(renderer.video_render_stats()->surface_mappings == 1);
    }
    assert(hardware_storage_freed == 2 && Memory::live.empty() && descriptors.empty());
}

void bounded_frame_updates()
{
    using namespace deko_test;
    reset();
    auto software = software_frame(64, 32);
    {
        DKVideoRenderer renderer;
        draw(renderer, software, 1);
        const int attempts = allocation;
        for (uint64_t generation = 2; generation <= 30; ++generation)
            draw(renderer, software, generation);
        assert(allocation == attempts && renderer.video_render_stats()->surface_mappings == 4);
        assert(draws == 30);
        completed = 1;
        draw(renderer, software, 31);
        assert(draws == 31);
    }
    reset();
    {
        DKVideoRenderer renderer;
        for (uint64_t generation = 1; generation <= 30; ++generation) {
            auto hardware = hardware_frame(64, 32);
            draw(renderer, hardware, generation);
            assert(renderer.video_render_stats()->retained_surfaces <= 8);
            assert(renderer.video_render_stats()->surface_mappings <= 8);
        }
        assert(renderer.video_render_stats()->retained_surfaces == 8);
        assert(hardware_storage_freed == 22);
        completed = submitted;
        auto hardware = hardware_frame(64, 32);
        draw(renderer, hardware, 31);
        assert(renderer.video_render_stats()->retained_surfaces == 2);
    }
    assert(hardware_storage_freed == 31 && Memory::live.empty() && descriptors.empty());
}

void allocation_retry()
{
    using namespace deko_test;
    reset();
    auto frame = software_frame(64, 32);
    {
        DKVideoRenderer renderer;
        fail_at = 3;
        draw(renderer, frame);
        assert(draws == 0);
        fail_at = -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(260));
        draw(renderer, frame);
        assert(draws == 1 && descriptors.size() == 2);
        auto resized = software_frame(32, 32);
        fail_at = allocation + 14;
        draw(renderer, resized, 2);
        assert(last_luma_bytes == 64 * 32 && descriptors.size() == 2);
        fail_at = -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(260));
        draw(renderer, resized, 2);
        assert(last_luma_bytes == 32 * 32 && descriptors.size() == 4);
    }
    assert(Memory::live.empty() && descriptors.empty());
}

void hardware_mapping_allocation_exception()
{
    using namespace deko_test;
    reset();
    auto initial = hardware_frame(64, 32);
    {
        DKVideoRenderer renderer;
        draw(renderer, initial);
        auto next = hardware_frame(64, 32);
        fail_at = allocation + 1;
        throw_allocation_failure = true;
        draw(renderer, next, 2);
        assert(draws == 2 && renderer.video_render_stats()->retained_surfaces == 1);
        assert(renderer.video_render_stats()->surface_mappings == 1);
        next.reset();
        assert(hardware_storage_freed == 1);
        fail_at = -1;
        throw_allocation_failure = false;
        next = hardware_frame(64, 32);
        draw(renderer, next, 3);
        assert(draws == 3 && renderer.video_render_stats()->retained_surfaces == 2);
    }
    initial.reset();
    assert(hardware_storage_freed == 3 && Memory::live.empty() && descriptors.empty());
}

}

int main()
{
    failure_sweep(false);
    failure_sweep(true);
    resize_retirement_and_screen();
    backend_retirement();
    bounded_frame_updates();
    allocation_retry();
    hardware_mapping_allocation_exception();
    std::cout << "Deko3D renderer reconfiguration: PASS\n";
}
