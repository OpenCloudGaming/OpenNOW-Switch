#pragma once

#include "Singleton.hpp"
#include <mutex>
#include <functional>
#include <deque>
#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
}

struct AVFrame;

class AVFrameQueue {
public:
    explicit AVFrameQueue();
    ~AVFrameQueue();

    void push(const AVFrame* item);
    AVFrame* pop(bool& reused, uint64_t& generation, int64_t target_pts = AV_NOPTS_VALUE);

    [[nodiscard]] size_t size() const;
    [[nodiscard]] size_t getFakeFrameUsage() const;
    [[nodiscard]] size_t getFramesDropStat() const;
    [[nodiscard]] size_t getTimingDropStat() const;
    [[nodiscard]] size_t getOverflowDropStat() const;
    [[nodiscard]] size_t getTimingHoldStat() const;

    void cleanup();

private:
    friend class AVFrameHolder;
    size_t limit = 3;
    std::deque<AVFrame*> queue;
    AVFrame* bufferFrame = nullptr;
    mutable std::mutex m_mutex;
    size_t fakeFrameUsedStat = 0;
    size_t framesDroppedStat = 0;
    size_t timingDroppedStat = 0;
    size_t overflowDroppedStat = 0;
    size_t timingHoldStat = 0;
    size_t framesCopiedStat = 0;
    uint64_t nextGeneration = 0;
    uint64_t bufferGeneration = 0;
    std::deque<uint64_t> generations;
};

class AVFrameHolder : public Singleton<AVFrameHolder> {
  public:
    void push(const AVFrame* frame) {
        m_frame_queue.push(frame);
    }

    void get(const std::function<void(AVFrame*, uint64_t, bool)>& fn,
             int64_t target_pts = AV_NOPTS_VALUE) {
        bool reused = false;
        uint64_t generation = 0;
        auto frame = m_frame_queue.pop(reused, generation, target_pts);

        if (frame) {
            fn(frame, generation, reused);
        }
    }

    void prepare(size_t queue_limit = 3) {
        // Software frame threading emits short bursts, so one additional slot
        // prevents avoidable drops while adding at most one display interval.
        m_frame_queue.limit = std::max<size_t>(2, queue_limit);
    }

    void cleanup() {
        m_frame_queue.cleanup();
    }

    [[nodiscard]] int getStat() const { return static_cast<int>(m_frame_queue.size()); }
    [[nodiscard]] size_t getFakeFrameStat() const { return m_frame_queue.getFakeFrameUsage(); }
    [[nodiscard]] size_t getFrameDropStat() const { return m_frame_queue.getFramesDropStat(); }
    [[nodiscard]] size_t getTimingDropStat() const { return m_frame_queue.getTimingDropStat(); }
    [[nodiscard]] size_t getOverflowDropStat() const { return m_frame_queue.getOverflowDropStat(); }
    [[nodiscard]] size_t getTimingHoldStat() const { return m_frame_queue.getTimingHoldStat(); }
    [[nodiscard]] size_t getFrameQueueSize() const { return m_frame_queue.size(); }

  private:
    AVFrameQueue m_frame_queue;
};
