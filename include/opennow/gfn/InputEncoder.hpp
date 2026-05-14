#pragma once

#include <cstdint>
#include <vector>

namespace opennow::gfn {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint32_t INPUT_HEARTBEAT = 2;
constexpr std::uint32_t INPUT_KEY_DOWN = 3;
constexpr std::uint32_t INPUT_KEY_UP = 4;
constexpr std::uint32_t INPUT_MOUSE_REL = 7;
constexpr std::uint32_t INPUT_MOUSE_BUTTON_DOWN = 8;
constexpr std::uint32_t INPUT_MOUSE_BUTTON_UP = 9;
constexpr std::uint32_t INPUT_MOUSE_WHEEL = 10;
constexpr std::uint32_t INPUT_GAMEPAD = 12;
constexpr std::uint32_t INPUT_HAPTICS_ENABLED = 13;

constexpr std::uint16_t GAMEPAD_A = 0x1000;
constexpr std::uint16_t GAMEPAD_B = 0x2000;
constexpr std::uint16_t GAMEPAD_X = 0x4000;
constexpr std::uint16_t GAMEPAD_Y = 0x8000;
constexpr std::uint16_t GAMEPAD_PACKET_SIZE = 38;

struct KeyboardPayload {
    std::uint16_t keycode = 0;
    std::uint16_t scancode = 0;
    std::uint16_t modifiers = 0;
    std::uint64_t timestampUs = 0;
};

struct MouseMovePayload {
    std::int16_t dx = 0;
    std::int16_t dy = 0;
    std::uint64_t timestampUs = 0;
};

struct MouseButtonPayload {
    std::uint8_t button = 0;
    std::uint64_t timestampUs = 0;
};

struct MouseWheelPayload {
    std::int16_t delta = 0;
    std::uint64_t timestampUs = 0;
};

struct GamepadInput {
    std::uint16_t controllerId = 0;
    std::uint16_t buttons = 0;
    std::uint8_t leftTrigger = 0;
    std::uint8_t rightTrigger = 0;
    std::int16_t leftStickX = 0;
    std::int16_t leftStickY = 0;
    std::int16_t rightStickX = 0;
    std::int16_t rightStickY = 0;
    std::uint64_t timestampUs = 0;
};

class InputEncoder {
public:
    void setProtocolVersion(std::uint32_t version);

    Bytes encodeHeartbeat() const;
    Bytes encodeKeyDown(const KeyboardPayload& payload) const;
    Bytes encodeKeyUp(const KeyboardPayload& payload) const;
    Bytes encodeMouseMove(const MouseMovePayload& payload) const;
    Bytes encodeMouseButtonDown(const MouseButtonPayload& payload) const;
    Bytes encodeMouseButtonUp(const MouseButtonPayload& payload) const;
    Bytes encodeMouseWheel(const MouseWheelPayload& payload) const;
    Bytes encodeHapticsEnabled(bool enabled) const;
    Bytes encodeGamepadState(const GamepadInput& payload, std::uint16_t bitmap, bool partiallyReliable);

private:
    std::uint16_t nextGamepadSequence(std::uint16_t gamepadIndex);
    Bytes encodeKey(std::uint32_t type, const KeyboardPayload& payload) const;
    Bytes encodeMouseButton(std::uint32_t type, const MouseButtonPayload& payload) const;

    std::uint32_t protocolVersion_ = 2;
    std::uint16_t gamepadSequences_[4] = {1, 1, 1, 1};
};

} // namespace opennow::gfn
