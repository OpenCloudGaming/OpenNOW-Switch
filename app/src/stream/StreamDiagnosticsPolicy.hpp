#pragma once

#include <cstdint>

namespace opennow::diagnostics
{

// Rate helpers for the stream diagnostics summary; header-only for host testing.

// Per-second rate; returns 0 on a zero/negative window or a backwards counter.
constexpr double PerSecondRate(uint64_t current, uint64_t previous, double elapsed_seconds)
{
    return (elapsed_seconds > 0.0 && current >= previous)
        ? static_cast<double>(current - previous) / elapsed_seconds
        : 0.0;
}

// Delivered bitrate in kbps from a byte counter, reusing PerSecondRate.
constexpr double DeliveredKbps(uint64_t bytes_current, uint64_t bytes_previous, double elapsed_seconds)
{
    return PerSecondRate(bytes_current, bytes_previous, elapsed_seconds) * 8.0 / 1000.0;
}

// Short stable label, independent of the free-form videoBackend string.
constexpr const char* FramePathLabel(bool uses_hardware_frames)
{
    return uses_hardware_frames ? "hw" : "sw";
}

// Gap between two presented-frame timestamps (us); 0 if no previous frame yet.
constexpr uint64_t FrameGapUs(uint64_t previous_presented_at_us, uint64_t now_us)
{
    return (previous_presented_at_us != 0 && now_us > previous_presented_at_us)
        ? now_us - previous_presented_at_us
        : 0;
}

} // namespace opennow::diagnostics
