#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace opennow::websocket
{

class WriteQueue
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr size_t MaximumBytes = 4 * 1024 * 1024 + 14;
    static constexpr size_t MaximumFrames = 64;
    static constexpr size_t MaximumWriteBytes = 16 * 1024;
    static constexpr size_t MaximumWritesPerPoll = 16;
    static constexpr auto SendTimeout = std::chrono::seconds(1);

    enum class WriteStatus { Progress, Again, Error };
    struct WriteResult
    {
        WriteStatus status;
        size_t bytes;
    };
    enum class DrainResult { Complete, Pending, TimedOut, WriteError, Stalled };

    bool can_enqueue(size_t bytes) const
    {
        return bytes > 0 && frames_.size() < MaximumFrames &&
               bytes <= MaximumBytes - queued_bytes_;
    }

    bool enqueue(std::vector<uint8_t>&& bytes, Clock::time_point now)
    {
        if (!can_enqueue(bytes.size()))
            return false;
        queued_bytes_ += bytes.size();
        frames_.push_back({std::move(bytes), 0, now + SendTimeout});
        return true;
    }

    void reset()
    {
        frames_.clear();
        queued_bytes_ = 0;
    }

    template <typename Writer, typename Now>
    DrainResult drain(Writer&& writer, Now&& now)
    {
        for (size_t writes = 0; writes < MaximumWritesPerPoll && !frames_.empty(); ++writes) {
            Frame& frame = frames_.front();
            if (now() >= frame.deadline)
                return DrainResult::TimedOut;
            const size_t length = std::min(MaximumWriteBytes, frame.bytes.size() - frame.offset);
            const WriteResult result = writer(frame.bytes.data() + frame.offset, length);
            if (result.status == WriteStatus::Again)
                return DrainResult::Pending;
            if (result.status == WriteStatus::Error || result.bytes > length)
                return DrainResult::WriteError;
            if (result.bytes == 0)
                return DrainResult::Stalled;
            frame.offset += result.bytes;
            if (frame.offset == frame.bytes.size()) {
                queued_bytes_ -= frame.bytes.size();
                frames_.pop_front();
            }
        }
        return frames_.empty() ? DrainResult::Complete : DrainResult::Pending;
    }

private:
    struct Frame
    {
        std::vector<uint8_t> bytes;
        size_t offset;
        Clock::time_point deadline;
    };

    std::deque<Frame> frames_;
    size_t queued_bytes_ = 0;
};

}
