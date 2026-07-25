#pragma once

#include <algorithm>
#include <string>

namespace opennow::video
{

constexpr int kMinimumSelectableBitrateKbps = 8000;
constexpr int kMaximumSelectableBitrateKbps = 75000;
constexpr int kMinimumAdaptiveBitrateKbps = 4000;

struct BitrateTuning
{
    int minimum_kbps;
    int initial_kbps;
    int maximum_kbps;
};

inline int ClampSelectableBitrateKbps(int bitrate_kbps)
{
    return std::clamp(
        bitrate_kbps, kMinimumSelectableBitrateKbps, kMaximumSelectableBitrateKbps);
}

inline BitrateTuning ResolveBitrateTuning(int requested_maximum_kbps)
{
    const int maximum_kbps = ClampSelectableBitrateKbps(requested_maximum_kbps);
    return {
        kMinimumAdaptiveBitrateKbps,
        std::max(kMinimumAdaptiveBitrateKbps, maximum_kbps / 4),
        maximum_kbps,
    };
}

struct QualityTuning
{
    int fec_repair_min_percent;
    int fec_repair_percent;
    int fec_repair_max_percent;
    int pacing_groups;
    int pacing_min_packets_per_group;
    int pacing_max_delay_us;
    float denoise_strength;
    float sharpen_strength;
    bool preserve_reference_deblocking;
};

inline QualityTuning ResolveQualityTuning(const std::string& mode)
{
    if (mode == "Original")
        return {5, 5, 35, 5, 15, 1000, 0.0f, 0.0f, false};

    if (mode == "Clarity")
        return {8, 10, 30, 8, 8, 1500, 0.13f, 0.20f, true};

    // A small resilience budget and a single spatial shader pass improve
    // dynamic scenes without adding a queued frame or raising max bitrate.
    return {6, 8, 30, 6, 10, 1250, 0.10f, 0.14f, true};
}

inline std::string NextQualityMode(const std::string& mode)
{
    if (mode == "Adaptive")
        return "Clarity";
    if (mode == "Clarity")
        return "Original";
    return "Adaptive";
}

} // namespace opennow::video
