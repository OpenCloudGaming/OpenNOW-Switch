#pragma once

#include <cstdint>
#include <memory>
#include <new>
#include <utility>

namespace opennow::video {

template <typename Configuration>
class GpuConfigurationQueue {
public:
    Configuration* active() const { return active_.get(); }
    Configuration* retiring() const { return retiring_.get(); }

    template <typename IsComplete>
    void releaseCompleted(IsComplete is_complete) {
        if (retiring_ && is_complete(*retiring_))
            retiring_.reset();
    }

    template <typename Build>
    bool replace(uint64_t now_ms, Build build) {
        if (retiring_ || now_ms < retry_at_ms_)
            return false;

        std::unique_ptr<Configuration> candidate;
        try {
            candidate = build();
        } catch (const std::bad_alloc&) {
        }
        if (!candidate) {
            retry_at_ms_ = now_ms + 250;
            return false;
        }
        retiring_ = std::move(active_);
        active_ = std::move(candidate);
        retry_at_ms_ = 0;
        return true;
    }

    void clear() {
        retiring_.reset();
        active_.reset();
        retry_at_ms_ = 0;
    }

private:
    std::unique_ptr<Configuration> active_;
    std::unique_ptr<Configuration> retiring_;
    uint64_t retry_at_ms_ = 0;
};

}
