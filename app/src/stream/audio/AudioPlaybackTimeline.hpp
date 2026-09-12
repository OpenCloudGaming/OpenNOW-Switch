#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace opennow::audio {

struct PlaybackPosition {
    uint32_t timestamp;
    uint32_t ssrc;
};

template <size_t Capacity>
class PlaybackTimeline {
public:
    bool full() const { return count_ == Capacity; }

    bool append(uint64_t sample, uint32_t timestamp, uint32_t ssrc) {
        if (full())
            return false;
        segments_[count_++] = {sample, timestamp, ssrc};
        return true;
    }

    void discardBefore(uint64_t sample) {
        size_t discard = 0;
        while (discard + 1 < count_ && segments_[discard + 1].sample <= sample)
            ++discard;
        for (size_t i = discard; i < count_; ++i)
            segments_[i - discard] = segments_[i];
        count_ -= discard;
    }

    std::optional<PlaybackPosition> position(uint64_t sample) const {
        if (count_ == 0 || sample < segments_[0].sample)
            return std::nullopt;
        size_t index = 0;
        while (index + 1 < count_ && segments_[index + 1].sample <= sample)
            ++index;
        const auto& segment = segments_[index];
        return PlaybackPosition{
            segment.timestamp + static_cast<uint32_t>(sample - segment.sample), segment.ssrc};
    }

    void clear() { count_ = 0; }

private:
    struct Segment {
        uint64_t sample;
        uint32_t timestamp;
        uint32_t ssrc;
    };
    std::array<Segment, Capacity> segments_ {};
    size_t count_ = 0;
};

}
