#pragma once

#include <cstdint>

#include "opennow/gfn/InputEncoder.hpp"

namespace opennow::input {

struct ControllerState {
    bool a = false;
    bool b = false;
    bool x = false;
    bool y = false;
    bool dpadUp = false;
    bool dpadDown = false;
    bool dpadLeft = false;
    bool dpadRight = false;
    bool plus = false;
    bool minus = false;
    bool l = false;
    bool r = false;
    bool zl = false;
    bool zr = false;
    bool leftStickPress = false;
    bool rightStickPress = false;
    bool home = false;
    float leftX = 0.0f;
    float leftY = 0.0f;
    float rightX = 0.0f;
    float rightY = 0.0f;
    float leftTrigger = 0.0f;
    float rightTrigger = 0.0f;
};

struct ControllerMappingOptions {
    bool swapNintendoFaceButtons = true;
    float leftDeadzone = 0.15f;
    float rightDeadzone = 0.15f;
    std::uint16_t controllerId = 0;
    std::uint64_t timestampUs = 0;
};

class ControllerMapper {
public:
    gfn::GamepadInput map(const ControllerState& state, const ControllerMappingOptions& options = {}) const;

private:
    static float applyDeadzone1D(float value, float deadzone);
    static std::int16_t toInt16(float value);
    static std::uint8_t toUint8(float value);
};

} // namespace opennow::input
