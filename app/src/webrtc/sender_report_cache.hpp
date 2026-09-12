#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace opennow::webrtc
{

struct SenderReport
{
    uint32_t ssrc = 0;
    uint64_t ntp_us = 0;
    uint32_t rtp_timestamp = 0;
};

class SenderReportCache
{
public:
    static constexpr size_t Capacity = 6;

    void remember(SenderReport report, uint32_t audio_ssrc, uint32_t video_ssrc)
    {
        size_t index = 0;
        while (index < size_ && reports_[index].ssrc != report.ssrc)
            ++index;
        if (index == size_) {
            if (size_ < Capacity) {
                reports_[size_++] = report;
                return;
            }
            for (index = 0; index + 1 < size_; ++index) {
                if (reports_[index].ssrc != audio_ssrc && reports_[index].ssrc != video_ssrc)
                    break;
            }
        }
        for (size_t next = index + 1; next < size_; ++next)
            reports_[next - 1] = reports_[next];
        reports_[size_ - 1] = report;
    }

    const SenderReport* find(uint32_t ssrc) const
    {
        for (size_t index = 0; index < size_; ++index) {
            if (reports_[index].ssrc == ssrc)
                return &reports_[index];
        }
        return nullptr;
    }

    size_t size() const
    {
        return size_;
    }

private:
    std::array<SenderReport, Capacity> reports_ {};
    size_t size_ = 0;
};

}
