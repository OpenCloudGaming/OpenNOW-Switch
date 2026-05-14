#pragma once

#include <cstdint>
#include <vector>

namespace opennow::media {

struct RgbaVideoFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t frameId = 0;
    std::vector<std::uint8_t> rgba;
};

void publishVideoFrame(std::uint32_t width, std::uint32_t height, const std::uint8_t* rgba, std::size_t bytes);
bool consumeLatestVideoFrame(RgbaVideoFrame& out);
void clearVideoFrames();

} // namespace opennow::media
