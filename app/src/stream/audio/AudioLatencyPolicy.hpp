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

} // namespace opennow::audio
