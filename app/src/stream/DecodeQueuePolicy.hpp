#pragma once

#include <algorithm>
#include <cstddef>

namespace opennow::video
{

constexpr std::size_t MaximumDecodeUnitBytes()
{
    return 2 * 1024 * 1024;
}

constexpr std::size_t MaximumQueuedAccessUnits(int frames_per_second)
{
    const int safe_fps = std::max(1, frames_per_second);
    return static_cast<std::size_t>(std::clamp((safe_fps + 7) / 8, 2, 8));
}

constexpr int MaximumDecodeQueueDelayMs()
{
    return 67;
}

} // namespace opennow::video
