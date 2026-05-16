#include "opennow/media/VideoFrameStore.hpp"

#include <mutex>
#include <utility>

namespace opennow::media {
namespace {

std::mutex gFrameMutex;
RgbaVideoFrame gLatestFrame;
std::uint64_t gNextFrameId = 1;
bool gHasFrame = false;

} // namespace

void publishVideoFrame(std::uint32_t width, std::uint32_t height, const std::uint8_t* rgba, std::size_t bytes) {
    if (!rgba || width == 0 || height == 0 || bytes < static_cast<std::size_t>(width) * height * 4) {
        return;
    }

    std::lock_guard<std::mutex> lock(gFrameMutex);
    if (gHasFrame) {
        return;
    }
    gLatestFrame.width = width;
    gLatestFrame.height = height;
    gLatestFrame.frameId = gNextFrameId++;
    gLatestFrame.rgba.assign(rgba, rgba + bytes);
    gHasFrame = true;
}

bool consumeLatestVideoFrame(RgbaVideoFrame& out) {
    std::lock_guard<std::mutex> lock(gFrameMutex);
    if (!gHasFrame) {
        return false;
    }
    out = std::move(gLatestFrame);
    gLatestFrame = {};
    gHasFrame = false;
    return true;
}

bool hasPendingVideoFrame() {
    std::lock_guard<std::mutex> lock(gFrameMutex);
    return gHasFrame;
}

void clearVideoFrames() {
    std::lock_guard<std::mutex> lock(gFrameMutex);
    gHasFrame = false;
    gLatestFrame = {};
}

} // namespace opennow::media
