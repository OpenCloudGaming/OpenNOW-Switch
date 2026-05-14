#include "opennow/gfn/InputEncoder.hpp"

#include <algorithm>
#include <array>

namespace opennow::gfn {

namespace {

void writeU16Be(Bytes& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    bytes[offset + 1] = static_cast<std::uint8_t>(value & 0xff);
}

void writeU16Le(Bytes& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xff);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
}

void writeI16Be(Bytes& bytes, std::size_t offset, std::int16_t value) {
    writeU16Be(bytes, offset, static_cast<std::uint16_t>(value));
}

void writeI16Le(Bytes& bytes, std::size_t offset, std::int16_t value) {
    writeU16Le(bytes, offset, static_cast<std::uint16_t>(value));
}

void writeU32Be(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>((value >> 24) & 0xff);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 16) & 0xff);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    bytes[offset + 3] = static_cast<std::uint8_t>(value & 0xff);
}

void writeU32Le(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xff);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
    bytes[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xff);
}

void writeU64Be(Bytes& bytes, std::size_t offset, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        bytes[offset + static_cast<std::size_t>(7 - i)] =
            static_cast<std::uint8_t>((value >> (i * 8)) & 0xff);
    }
}

void writeU64Le(Bytes& bytes, std::size_t offset, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        bytes[offset + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((value >> (i * 8)) & 0xff);
    }
}

Bytes wrapSingleEvent(const Bytes& payload, std::uint32_t protocolVersion) {
    if (protocolVersion <= 2) {
        return payload;
    }

    Bytes wrapped(10 + payload.size());
    wrapped[0] = 0x23;
    writeU64Be(wrapped, 1, 0);
    wrapped[9] = 0x22;
    std::copy(payload.begin(), payload.end(), wrapped.begin() + 10);
    return wrapped;
}

Bytes wrapBatchedEvent(const Bytes& payload, std::uint32_t protocolVersion) {
    if (protocolVersion <= 2) {
        return payload;
    }

    Bytes wrapped(12 + payload.size());
    wrapped[0] = 0x23;
    writeU64Be(wrapped, 1, 0);
    wrapped[9] = 0x21;
    writeU16Be(wrapped, 10, static_cast<std::uint16_t>(payload.size()));
    std::copy(payload.begin(), payload.end(), wrapped.begin() + 12);
    return wrapped;
}

Bytes wrapGamepadPartiallyReliable(
    const Bytes& payload,
    std::uint32_t protocolVersion,
    std::uint16_t gamepadIndex,
    std::uint16_t sequenceNumber) {
    if (protocolVersion <= 2) {
        return payload;
    }

    Bytes wrapped(16 + payload.size());
    wrapped[0] = 0x23;
    writeU64Be(wrapped, 1, 0);
    wrapped[9] = 0x26;
    wrapped[10] = static_cast<std::uint8_t>(gamepadIndex & 0xff);
    writeU16Be(wrapped, 11, sequenceNumber);
    wrapped[13] = 0x21;
    writeU16Be(wrapped, 14, static_cast<std::uint16_t>(payload.size()));
    std::copy(payload.begin(), payload.end(), wrapped.begin() + 16);
    return wrapped;
}

} // namespace

void InputEncoder::setProtocolVersion(std::uint32_t version) {
    protocolVersion_ = version;
}

Bytes InputEncoder::encodeHeartbeat() const {
    Bytes payload(4);
    writeU32Le(payload, 0, INPUT_HEARTBEAT);
    return payload;
}

Bytes InputEncoder::encodeKeyDown(const KeyboardPayload& payload) const {
    return encodeKey(INPUT_KEY_DOWN, payload);
}

Bytes InputEncoder::encodeKeyUp(const KeyboardPayload& payload) const {
    return encodeKey(INPUT_KEY_UP, payload);
}

Bytes InputEncoder::encodeMouseMove(const MouseMovePayload& payload) const {
    Bytes bytes(22);
    writeU32Le(bytes, 0, INPUT_MOUSE_REL);
    writeI16Be(bytes, 4, payload.dx);
    writeI16Be(bytes, 6, payload.dy);
    writeU16Be(bytes, 8, 0);
    writeU32Be(bytes, 10, 0);
    writeU64Be(bytes, 14, payload.timestampUs);
    return wrapBatchedEvent(bytes, protocolVersion_);
}

Bytes InputEncoder::encodeMouseButtonDown(const MouseButtonPayload& payload) const {
    return encodeMouseButton(INPUT_MOUSE_BUTTON_DOWN, payload);
}

Bytes InputEncoder::encodeMouseButtonUp(const MouseButtonPayload& payload) const {
    return encodeMouseButton(INPUT_MOUSE_BUTTON_UP, payload);
}

Bytes InputEncoder::encodeMouseWheel(const MouseWheelPayload& payload) const {
    Bytes bytes(22);
    writeU32Le(bytes, 0, INPUT_MOUSE_WHEEL);
    writeI16Be(bytes, 4, 0);
    writeI16Be(bytes, 6, payload.delta);
    writeU16Be(bytes, 8, 0);
    writeU32Be(bytes, 10, 0);
    writeU64Be(bytes, 14, payload.timestampUs);
    return wrapSingleEvent(bytes, protocolVersion_);
}

Bytes InputEncoder::encodeHapticsEnabled(bool enabled) const {
    Bytes bytes(6);
    writeU32Le(bytes, 0, INPUT_HAPTICS_ENABLED);
    writeU16Be(bytes, 4, enabled ? 1 : 0);
    return wrapSingleEvent(bytes, protocolVersion_);
}

Bytes InputEncoder::encodeGamepadState(
    const GamepadInput& payload,
    std::uint16_t bitmap,
    bool partiallyReliable) {
    Bytes bytes(GAMEPAD_PACKET_SIZE);
    writeU32Le(bytes, 0, INPUT_GAMEPAD);
    writeU16Le(bytes, 4, 26);
    writeU16Le(bytes, 6, payload.controllerId & 0x03);
    writeU16Le(bytes, 8, bitmap);
    writeU16Le(bytes, 10, 20);
    writeU16Le(bytes, 12, payload.buttons);
    writeU16Le(bytes, 14, static_cast<std::uint16_t>(
        payload.leftTrigger | (static_cast<std::uint16_t>(payload.rightTrigger) << 8)));
    writeI16Le(bytes, 16, payload.leftStickX);
    writeI16Le(bytes, 18, payload.leftStickY);
    writeI16Le(bytes, 20, payload.rightStickX);
    writeI16Le(bytes, 22, payload.rightStickY);
    writeU16Le(bytes, 24, 0);
    writeU16Le(bytes, 26, 85);
    writeU16Le(bytes, 28, 0);
    writeU64Le(bytes, 30, payload.timestampUs);

    if (partiallyReliable) {
        return wrapGamepadPartiallyReliable(
            bytes,
            protocolVersion_,
            payload.controllerId,
            nextGamepadSequence(payload.controllerId));
    }
    return wrapBatchedEvent(bytes, protocolVersion_);
}

std::uint16_t InputEncoder::nextGamepadSequence(std::uint16_t gamepadIndex) {
    const auto index = static_cast<std::size_t>(gamepadIndex % 4);
    const auto current = gamepadSequences_[index];
    gamepadSequences_[index] = static_cast<std::uint16_t>(current + 1);
    return current;
}

Bytes InputEncoder::encodeKey(std::uint32_t type, const KeyboardPayload& payload) const {
    Bytes bytes(18);
    writeU32Le(bytes, 0, type);
    writeU16Be(bytes, 4, payload.keycode);
    writeU16Be(bytes, 6, payload.modifiers);
    writeU16Be(bytes, 8, payload.scancode);
    writeU64Be(bytes, 10, payload.timestampUs);
    return wrapSingleEvent(bytes, protocolVersion_);
}

Bytes InputEncoder::encodeMouseButton(std::uint32_t type, const MouseButtonPayload& payload) const {
    Bytes bytes(18);
    writeU32Le(bytes, 0, type);
    bytes[4] = payload.button;
    bytes[5] = 0;
    writeU32Be(bytes, 6, 0);
    writeU64Be(bytes, 10, payload.timestampUs);
    return wrapSingleEvent(bytes, protocolVersion_);
}

} // namespace opennow::gfn
