#pragma once

#include <cstdint>
#include <string>

namespace opennow {

enum class VideoCodec {
    H264,
};

struct StreamSettings {
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t fps = 60;
    std::uint32_t bitrateKbps = 15000;
    VideoCodec codec = VideoCodec::H264;
    bool stereoAudio = true;
};

std::string toString(VideoCodec codec);
std::string describe(const StreamSettings& settings);

} // namespace opennow
