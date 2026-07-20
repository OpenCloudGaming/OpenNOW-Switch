#include "input/TouchMapping.hpp"

#include <cassert>
#include <cmath>

namespace {
bool Near(float lhs, float rhs, float tolerance = 0.6f) {
    return std::fabs(lhs - rhs) <= tolerance;
}
}

int main() {
    using opennow::input::MapTouchToStream;

    const auto top_left = MapTouchToStream(0, 0, 0, 0, 1280, 720, 1280, 720);
    const auto center = MapTouchToStream(640, 360, 0, 0, 1280, 720, 1280, 720);
    const auto bottom_right = MapTouchToStream(1279, 719, 0, 0, 1280, 720, 1280, 720);
    assert(Near(top_left.x, 0) && Near(top_left.y, 0));
    assert(Near(center.x, 640) && Near(center.y, 360));
    assert(Near(bottom_right.x, 1279) && Near(bottom_right.y, 719));

    // A non-zero view origin must not shift or mirror the remote X axis.
    const auto left = MapTouchToStream(110, 70, 100, 50, 1000, 500, 1600, 900);
    const auto right = MapTouchToStream(1099, 70, 100, 50, 1000, 500, 1600, 900);
    assert(left.x < right.x);
    assert(left.y == right.y);

    // Cover rendering crops the hidden axis while retaining the visible center.
    const auto cropped_center = MapTouchToStream(640, 360, 0, 0, 1280, 720, 1024, 768);
    assert(Near(cropped_center.x, 512));
    assert(Near(cropped_center.y, 384));
    return 0;
}
