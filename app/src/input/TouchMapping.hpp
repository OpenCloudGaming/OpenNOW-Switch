#pragma once

#include <algorithm>

namespace opennow::input {

struct TouchMappedPoint {
    float x = 0.0f;
    float y = 0.0f;
};

inline TouchMappedPoint MapTouchToStream(float touch_x, float touch_y,
                                         float view_x, float view_y,
                                         float view_width, float view_height,
                                         int stream_width, int stream_height)
{
    view_width = std::max(1.0f, view_width);
    view_height = std::max(1.0f, view_height);
    stream_width = std::max(1, stream_width);
    stream_height = std::max(1, stream_height);

    float normalized_x = std::clamp((touch_x - view_x) / view_width, 0.0f, 1.0f);
    float normalized_y = std::clamp((touch_y - view_y) / view_height, 0.0f, 1.0f);
    const float frame_aspect = static_cast<float>(stream_height) / stream_width;
    const float screen_aspect = view_height / view_width;

    // Keep this inverse transform identical to the video fragment shader.
    if (frame_aspect > screen_aspect) {
        const float multiplier = frame_aspect / screen_aspect;
        normalized_x = (normalized_x - (0.5f - 0.5f / multiplier)) * multiplier;
    } else {
        const float multiplier = screen_aspect / frame_aspect;
        normalized_y = (normalized_y - (0.5f - 0.5f / multiplier)) * multiplier;
    }

    return {
        std::clamp(std::clamp(normalized_x, 0.0f, 1.0f) * stream_width,
                   0.0f, static_cast<float>(stream_width - 1)),
        std::clamp(std::clamp(normalized_y, 0.0f, 1.0f) * stream_height,
                   0.0f, static_cast<float>(stream_height - 1)),
    };
}

} // namespace opennow::input
