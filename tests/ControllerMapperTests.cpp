#include "opennow/input/ControllerMapper.hpp"

#include <cassert>

int main() {
    opennow::input::ControllerMapper mapper;

    const auto swapped = mapper.map({
        .a = true,
        .x = true,
        .dpadUp = true,
        .plus = true,
        .l = true,
        .zr = true,
        .leftX = 0.1f,
        .leftY = 0.5f,
        .rightX = -1.0f,
        .rightY = 1.0f,
    }, {
        .swapNintendoFaceButtons = true,
        .leftDeadzone = 0.15f,
        .rightDeadzone = 0.15f,
        .controllerId = 2,
        .timestampUs = 123,
    });

    assert(swapped.controllerId == 2);
    assert((swapped.buttons & 0x0001) != 0);
    assert((swapped.buttons & 0x0010) != 0);
    assert((swapped.buttons & 0x0100) != 0);
    assert((swapped.buttons & opennow::gfn::GAMEPAD_B) != 0);
    assert((swapped.buttons & opennow::gfn::GAMEPAD_Y) != 0);
    assert((swapped.buttons & opennow::gfn::GAMEPAD_A) == 0);
    assert(swapped.rightTrigger == 255);
    assert(swapped.leftStickX == 0);
    assert(swapped.leftStickY < 0);
    assert(swapped.rightStickX == -32767);
    assert(swapped.rightStickY == -32767);
    assert(swapped.timestampUs == 123);

    const auto unswapped = mapper.map({
        .a = true,
        .b = false,
        .leftTrigger = 0.5f,
    }, {
        .swapNintendoFaceButtons = false,
    });
    assert((unswapped.buttons & opennow::gfn::GAMEPAD_A) != 0);
    assert((unswapped.buttons & opennow::gfn::GAMEPAD_B) == 0);
    assert(unswapped.leftTrigger == 128);

    return 0;
}
