#pragma once

namespace opennow::gfn
{

// CloudMatch wire values used by the official client:
// 1 = default, 2 = gamepad-friendly, 3 = touch-friendly.
inline constexpr int AppLaunchModeWireValue(bool launch_in_console_mode)
{
    return launch_in_console_mode ? 2 : 1;
}

} // namespace opennow::gfn
