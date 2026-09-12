#pragma once

#include <array>
#include <cstddef>

extern "C" {
#include <libavutil/frame.h>
}

namespace opennow::video {

template <typename Fence, size_t Capacity>
class GpuFrameQueue {
public:
    GpuFrameQueue() = default;
    GpuFrameQueue(const GpuFrameQueue&) = delete;
    GpuFrameQueue& operator=(const GpuFrameQueue&) = delete;
    ~GpuFrameQueue() { clear(); }

    bool retain(const AVFrame* frame) {
        if (count_ == Capacity)
            return false;
        AVFrame* reference = frame ? av_frame_clone(frame) : nullptr;
        if (!reference)
            return false;
        frames_[count_++] = {reference, {}};
        return true;
    }

    void discardLast() {
        av_frame_free(&frames_[--count_].frame);
    }

    template <typename IsComplete>
    void releaseCompleted(IsComplete is_complete) {
        while (count_ > 1 && is_complete(frames_.front().fence)) {
            av_frame_free(&frames_.front().frame);
            for (size_t i = 1; i < count_; ++i)
                frames_[i - 1] = frames_[i];
            --count_;
        }
    }

    void setCompletion(const Fence& fence) { frames_[count_ - 1].fence = fence; }
    size_t size() const { return count_; }

    void clear() {
        while (count_ > 0)
            discardLast();
    }

private:
    struct Entry {
        AVFrame* frame;
        Fence fence;
    };
    std::array<Entry, Capacity> frames_ {};
    size_t count_ = 0;
};

}
