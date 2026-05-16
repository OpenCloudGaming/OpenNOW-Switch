#include "opennow/gfn/InputEncoder.hpp"

#include <cassert>
#include <cstdint>

using opennow::gfn::GAMEPAD_A;
using opennow::gfn::GamepadInput;
using opennow::gfn::InputEncoder;
using opennow::gfn::KeyboardPayload;
using opennow::gfn::MouseMovePayload;

int main() {
    InputEncoder encoder;

    const auto heartbeat = encoder.encodeHeartbeat();
    assert(heartbeat.size() == 4);
    assert(heartbeat[0] == 2);
    assert(heartbeat[1] == 0);
    assert(heartbeat[2] == 0);
    assert(heartbeat[3] == 0);

    const auto keyV2 = encoder.encodeKeyDown(KeyboardPayload{
        .keycode = 0x41,
        .scancode = 0x001e,
        .modifiers = 0,
        .timestampUs = 0x0102030405060708ULL,
    });
    assert(keyV2.size() == 18);
    assert(keyV2[0] == 3);
    assert(keyV2[4] == 0);
    assert(keyV2[5] == 0x41);
    assert(keyV2[8] == 0);
    assert(keyV2[9] == 0x1e);

    encoder.setProtocolVersion(3);

    const auto mouse = encoder.encodeMouseMove(MouseMovePayload{
        .dx = 10,
        .dy = -5,
        .timestampUs = 1,
    });
    assert(mouse.size() == 34);
    assert(mouse[0] == 0x23);
    assert(mouse[8] == 1);
    assert(mouse[9] == 0x21);
    assert(mouse[10] == 0);
    assert(mouse[11] == 22);
    assert(mouse[12] == 7);

    const auto gamepad = encoder.encodeGamepadState(GamepadInput{
        .controllerId = 0,
        .buttons = GAMEPAD_A,
        .leftTrigger = 0,
        .rightTrigger = 255,
        .leftStickX = 123,
        .leftStickY = -123,
        .rightStickX = 456,
        .rightStickY = -456,
        .timestampUs = 0x0102030405060708ULL,
    }, 0x0101, false);
    assert(gamepad.size() == 50);
    assert(gamepad[0] == 0x23);
    assert(gamepad[1] == 0x01);
    assert(gamepad[2] == 0x02);
    assert(gamepad[3] == 0x03);
    assert(gamepad[4] == 0x04);
    assert(gamepad[5] == 0x05);
    assert(gamepad[6] == 0x06);
    assert(gamepad[7] == 0x07);
    assert(gamepad[8] == 0x08);
    assert(gamepad[9] == 0x21);
    assert(gamepad[10] == 0);
    assert(gamepad[11] == 38);
    assert(gamepad[12] == 12);
    assert(gamepad[24] == 0);
    assert(gamepad[25] == 0x10);

    const auto prGamepad = encoder.encodeGamepadState(GamepadInput{
        .controllerId = 1,
        .buttons = GAMEPAD_A,
        .timestampUs = 1,
    }, 0x0202, true);
    assert(prGamepad.size() == 54);
    assert(prGamepad[8] == 1);
    assert(prGamepad[9] == 0x26);
    assert(prGamepad[10] == 1);
    assert(prGamepad[13] == 0x21);
    assert(prGamepad[14] == 0);
    assert(prGamepad[15] == 38);

    return 0;
}
