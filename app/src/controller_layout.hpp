#pragma once

#include <cstdint>
#include <string>

namespace opennow
{

constexpr std::uint16_t kXboxButtonA = 0x1000;
constexpr std::uint16_t kXboxButtonB = 0x2000;
constexpr std::uint16_t kXboxButtonX = 0x4000;
constexpr std::uint16_t kXboxButtonY = 0x8000;

inline std::uint16_t MapFaceButtons(
    const std::string& layout,
    bool switch_a,
    bool switch_b,
    bool switch_x,
    bool switch_y)
{
    std::uint16_t buttons = 0;
    const bool switch_labels = layout == "Switch";

    if (switch_a)
        buttons |= switch_labels ? kXboxButtonA : kXboxButtonB;
    if (switch_b)
        buttons |= switch_labels ? kXboxButtonB : kXboxButtonA;
    if (switch_x)
        buttons |= switch_labels ? kXboxButtonX : kXboxButtonY;
    if (switch_y)
        buttons |= switch_labels ? kXboxButtonY : kXboxButtonX;

    return buttons;
}

} // namespace opennow
