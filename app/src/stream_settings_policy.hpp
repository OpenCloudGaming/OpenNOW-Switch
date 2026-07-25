#pragma once

#include "stream_settings.hpp"
#include "video_quality_policy.hpp"

#include <array>

namespace opennow::settings
{

inline void MarkCustom(StreamSettings& value)
{
    value.preset_id = "custom";
    value.label = "Custom";
}

inline void CycleResolution(StreamSettings& value)
{
    if (value.height >= 1080)
    {
        value.width = 1280;
        value.height = 720;
    }
    else
    {
        value.width = 1920;
        value.height = 1080;
    }
    MarkCustom(value);
}

inline void CycleFrameRate(StreamSettings& value)
{
    value.fps = value.fps >= 60 ? 30 : 60;
    MarkCustom(value);
}

inline void CycleBitrate(StreamSettings& value)
{
    // Keep the lower steps for constrained Wi-Fi while exposing the same
    // 75 Mbps ceiling used by the PC client for high-motion scenes.
    constexpr std::array<int, 10> bitrates {
        8000, 12000, 16000, 20000, 25000,
        30000, 40000, 50000, 60000, 75000,
    };
    int next = bitrates.front();
    for (int bitrate : bitrates)
    {
        if (bitrate > value.bitrate_kbps)
        {
            next = bitrate;
            break;
        }
    }
    value.bitrate_kbps = next;
    MarkCustom(value);
}

inline void CycleVideoBackend(StreamSettings& value)
{
    if (value.video_backend == "Auto")
        value.video_backend = "NVDEC";
    else if (value.video_backend == "NVDEC")
        value.video_backend = "Software";
    else
        value.video_backend = "Auto";
}

inline void CycleImageQuality(StreamSettings& value)
{
    value.image_quality_mode = video::NextQualityMode(value.image_quality_mode);
}

} // namespace opennow::settings
