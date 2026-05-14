#include "opennow/input/ControllerMapper.hpp"

#include <algorithm>
#include <cmath>

namespace opennow::input {

gfn::GamepadInput ControllerMapper::map(const ControllerState& state, const ControllerMappingOptions& options) const {
    std::uint16_t buttons = 0;

    if (state.dpadUp) buttons |= 0x0001;
    if (state.dpadDown) buttons |= 0x0002;
    if (state.dpadLeft) buttons |= 0x0004;
    if (state.dpadRight) buttons |= 0x0008;
    if (state.plus) buttons |= 0x0010;
    if (state.minus) buttons |= 0x0020;
    if (state.leftStickPress) buttons |= 0x0040;
    if (state.rightStickPress) buttons |= 0x0080;
    if (state.l) buttons |= 0x0100;
    if (state.r) buttons |= 0x0200;
    if (state.home) buttons |= 0x0400;

    const bool remoteA = options.swapNintendoFaceButtons ? state.b : state.a;
    const bool remoteB = options.swapNintendoFaceButtons ? state.a : state.b;
    const bool remoteX = options.swapNintendoFaceButtons ? state.y : state.x;
    const bool remoteY = options.swapNintendoFaceButtons ? state.x : state.y;

    if (remoteA) buttons |= gfn::GAMEPAD_A;
    if (remoteB) buttons |= gfn::GAMEPAD_B;
    if (remoteX) buttons |= gfn::GAMEPAD_X;
    if (remoteY) buttons |= gfn::GAMEPAD_Y;

    const auto lx = applyDeadzone1D(state.leftX, options.leftDeadzone);
    const auto ly = applyDeadzone1D(state.leftY, options.leftDeadzone);
    const auto rx = applyDeadzone1D(state.rightX, options.rightDeadzone);
    const auto ry = applyDeadzone1D(state.rightY, options.rightDeadzone);

    return {
        .controllerId = options.controllerId,
        .buttons = buttons,
        .leftTrigger = toUint8(std::max(state.leftTrigger, state.zl ? 1.0f : 0.0f)),
        .rightTrigger = toUint8(std::max(state.rightTrigger, state.zr ? 1.0f : 0.0f)),
        .leftStickX = toInt16(lx),
        .leftStickY = toInt16(-ly),
        .rightStickX = toInt16(rx),
        .rightStickY = toInt16(-ry),
        .timestampUs = options.timestampUs,
    };
}

float ControllerMapper::applyDeadzone1D(float value, float deadzone) {
    if (std::fabs(value) < deadzone) {
        return 0.0f;
    }
    return std::clamp(value, -1.0f, 1.0f);
}

std::int16_t ControllerMapper::toInt16(float value) {
    const auto clamped = std::clamp(value, -1.0f, 1.0f);
    return static_cast<std::int16_t>(std::lround(clamped * 32767.0f));
}

std::uint8_t ControllerMapper::toUint8(float value) {
    const auto clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0f));
}

} // namespace opennow::input
