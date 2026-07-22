#pragma once

#include <algorithm>
#include <cstddef>
#include <string>

namespace opennow::ui
{

inline size_t Utf8CodePointCount(const std::string& text)
{
    return static_cast<size_t>(std::count_if(text.begin(), text.end(), [](unsigned char ch) {
        return (ch & 0xc0) != 0x80;
    }));
}

inline float ToolbarButtonWidth(const std::string& text)
{
    const size_t estimated = Utf8CodePointCount(text) * 9 + 38;
    return static_cast<float>(std::clamp<size_t>(estimated, 170, 250));
}

} // namespace opennow::ui
