#pragma once

#include <algorithm>
#include <cstdint>

namespace opennow::audio
{

constexpr int ClampTargetBufferMs(int target_buffer_ms)
{
    return std::clamp(target_buffer_ms, 30, 100);
}

constexpr int PrimePacketCount(int target_buffer_ms)
{
    return ClampTargetBufferMs(target_buffer_ms) / 10;
}

constexpr std::uint64_t InitialJitterHoldUs(int target_buffer_ms)
{
    return static_cast<std::uint64_t>(ClampTargetBufferMs(target_buffer_ms)) * 1000;
}

constexpr std::uint64_t ResyncJitterHoldUs(int target_buffer_ms)
{
    return static_cast<std::uint64_t>(
        std::max(30, ClampTargetBufferMs(target_buffer_ms) / 2)) * 1000;
}

constexpr std::uint64_t PacketReorderGraceUs()
{
    return 4000;
}

constexpr std::uint64_t EstimatePlayedSamples(std::uint64_t observed_samples,
                                              std::uint64_t observed_at_us,
                                              std::uint64_t now_us,
                                              std::uint64_t submitted_samples,
                                              std::uint64_t sample_rate)
{
    if (observed_at_us == 0 || now_us <= observed_at_us || sample_rate == 0)
        return std::min(observed_samples, submitted_samples);

    const std::uint64_t elapsed_samples =
        (now_us - observed_at_us) * sample_rate / 1000000;
    return std::min(observed_samples + elapsed_samples, submitted_samples);
}

} // namespace opennow::audio
