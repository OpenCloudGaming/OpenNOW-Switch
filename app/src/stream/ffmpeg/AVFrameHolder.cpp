//
//  DiscoverManager.cpp
//  Moonlight
//
// Created by XITRIX on 31.03.2025.
//

#include "AVFrameHolder.hpp"
#include "VideoFrameTiming.hpp"

#include "borealis.hpp"

AVFrameQueue::AVFrameQueue() {}

AVFrameQueue::~AVFrameQueue() {
    cleanup();
}

namespace
{

AVFrame* referenceFrame(const AVFrame* source)
{
    if (!source)
        return nullptr;
    return av_frame_clone(source);
}

} // namespace

void AVFrameQueue::push(const AVFrame* item) {
    AVFrame* copy = referenceFrame(item);
    if (!copy) {
        brls::Logger::warning("Frame queue: failed to copy decoded frame");
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    queue.push_back(copy);
    generations.push_back(++nextGeneration);
    framesCopiedStat++;

    if (queue.size() > limit) {
        AVFrame* dropped = queue.front();
        queue.pop_front();
        generations.pop_front();
        av_frame_free(&dropped);
        framesDroppedStat++;
        overflowDroppedStat++;
    }
}

AVFrame* AVFrameQueue::pop(bool& reused, uint64_t& generation, int64_t target_pts) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (target_pts != AV_NOPTS_VALUE) {
        while (!queue.empty() && queue.front()->pts != AV_NOPTS_VALUE) {
            const bool has_next = queue.size() > 1 && queue[1]->pts != AV_NOPTS_VALUE;
            const auto decision = opennow::video::DecideFrame(
                static_cast<uint32_t>(queue.front()->pts), has_next,
                has_next ? static_cast<uint32_t>(queue[1]->pts) : 0,
                bufferFrame != nullptr, static_cast<uint32_t>(target_pts));
            if (decision == opennow::video::FrameDecision::HoldPrevious) {
                fakeFrameUsedStat++;
                timingHoldStat++;
                reused = true;
                generation = bufferGeneration;
                return bufferFrame;
            }
            if (decision != opennow::video::FrameDecision::DropSuperseded)
                break;
            AVFrame* dropped = queue.front();
            queue.pop_front();
            generations.pop_front();
            av_frame_free(&dropped);
            framesDroppedStat++;
            timingDroppedStat++;
        }
    }

    if (!queue.empty()) {
        if (bufferFrame)
            av_frame_free(&bufferFrame);

        AVFrame* item = queue.front();
        queue.pop_front();
        bufferGeneration = generations.front();
        generations.pop_front();
        bufferFrame = item;
        reused = false;
    } else {
        fakeFrameUsedStat++;
        reused = true;
    }

    generation = bufferGeneration;
    return bufferFrame;
}

size_t AVFrameQueue::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return queue.size();
}

size_t AVFrameQueue::getFakeFrameUsage() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return fakeFrameUsedStat;
}

size_t AVFrameQueue::getFramesDropStat() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return framesDroppedStat;
}

size_t AVFrameQueue::getTimingDropStat() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return timingDroppedStat;
}

size_t AVFrameQueue::getOverflowDropStat() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return overflowDroppedStat;
}

size_t AVFrameQueue::getTimingHoldStat() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return timingHoldStat;
}

void AVFrameQueue::cleanup() {
    std::lock_guard<std::mutex> lock(m_mutex);
    while (!queue.empty()) {
        AVFrame* frame = queue.front();
        queue.pop_front();
        av_frame_free(&frame);
    }
    while (!generations.empty())
        generations.pop_front();

    if (bufferFrame)
        av_frame_free(&bufferFrame);

    fakeFrameUsedStat = 0;
    framesDroppedStat = 0;
    timingDroppedStat = 0;
    overflowDroppedStat = 0;
    timingHoldStat = 0;
    framesCopiedStat = 0;
    nextGeneration = 0;
    bufferGeneration = 0;
    bufferFrame = nullptr;
}
