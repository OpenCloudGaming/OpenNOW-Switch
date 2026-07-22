#pragma once

#include <algorithm>
#include <cstddef>

namespace opennow::video
{

// Retain no more than roughly 67 ms of complete pictures. If decoding falls
// farther behind, resynchronizing at a fresh IDR is preferable to displaying
// an increasingly stale reference chain.
constexpr std::size_t MaximumQueuedAccessUnits(int frames_per_second)
{
    const int safe_fps = std::max(1, frames_per_second);
    return static_cast<std::size_t>(std::clamp((safe_fps + 14) / 15, 2, 4));
}

} // namespace opennow::video
