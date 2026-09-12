#include "stream/deko3d/GpuConfigurationQueue.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <new>

namespace {

struct Configuration {
    static inline int live = 0;
    static inline int destroyed = 0;
    int layout;
    uint64_t completion = 0;
    std::array<std::unique_ptr<int>, 16> allocations;

    explicit Configuration(int value) : layout(value) { ++live; }
    ~Configuration() { --live; ++destroyed; }
};

}

int main()
{
    using opennow::video::GpuConfigurationQueue;
    for (int fail_at = 0; fail_at < 16; ++fail_at) {
        GpuConfigurationQueue<Configuration> queue;
        assert(queue.replace(0, [] { return std::make_unique<Configuration>(720); }));
        auto* original = queue.active();
        original->completion = 10;
        int attempts = 0;
        assert(!queue.replace(1, [&]() -> std::unique_ptr<Configuration> {
            ++attempts;
            auto candidate = std::make_unique<Configuration>(1080);
            for (int allocation = 0; allocation < 16; ++allocation) {
                if (allocation == fail_at)
                    return nullptr;
                candidate->allocations[allocation] = std::make_unique<int>(allocation);
            }
            return candidate;
        }));
        assert(queue.active() == original && queue.active()->layout == 720);
        assert(!queue.retiring() && Configuration::live == 1);
        for (uint64_t now = 2; now < 251; ++now) {
            assert(!queue.replace(now, [&] {
                ++attempts;
                return std::make_unique<Configuration>(1080);
            }));
        }
        assert(attempts == 1);
        assert(queue.replace(251, [] { return std::make_unique<Configuration>(1080); }));
        assert(queue.retiring() == original && Configuration::live == 2);
        queue.releaseCompleted([](const Configuration& state) { return state.completion < 10; });
        assert(queue.retiring() == original);
        assert(!queue.replace(1000, [&] {
            ++attempts;
            return std::make_unique<Configuration>(480);
        }));
        assert(attempts == 1 && Configuration::live == 2);
        queue.active()->completion = 100;
        queue.releaseCompleted([](const Configuration& state) { return state.completion <= 10; });
        assert(!queue.retiring() && Configuration::live == 1);
        assert(queue.replace(1001, [] { return std::make_unique<Configuration>(480); }));
        assert(queue.active()->layout == 480);
        queue.clear();
        assert(Configuration::live == 0);
    }

    GpuConfigurationQueue<Configuration> queue;
    assert(!queue.replace(0, []() -> std::unique_ptr<Configuration> { throw std::bad_alloc(); }));
    assert(!queue.active());
    assert(!queue.replace(249, [] { return std::make_unique<Configuration>(720); }));
    assert(queue.replace(250, [] { return std::make_unique<Configuration>(720); }));
    assert(!queue.retiring());
    queue.clear();
    assert(Configuration::live == 0 && Configuration::destroyed > 0);
}
