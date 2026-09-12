#include "keyboard_input_policy.hpp"

#include <array>
#include <cassert>

int main()
{
    using namespace opennow::input;
    static_assert(kKeyboardShortcutControls.size() == 8);
    static_assert(kKeyboardTouchControls.size() == 12);
    constexpr std::array<KeyboardStroke, 4> expected {{
        {0x08, 0x0e, 0}, {0x2e, 0x53, 0}, {0x25, 0x4b, 0}, {0x27, 0x4d, 0},
    }};
    for (std::size_t i = 0; i < kKeyboardShortcutControls.size(); ++i)
        assert(kKeyboardTouchControls[i].shortcut == kKeyboardShortcutControls[i].shortcut);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto index = kKeyboardShortcutControls.size() + i;
        const auto stroke = MapKeyboardShortcut(kKeyboardTouchControls[index].shortcut);
        assert(stroke.keycode == expected[i].keycode);
        assert(stroke.scancode == expected[i].scancode);
        assert(stroke.modifiers == 0);
        const auto tap = MakeKeyboardTap(stroke);
        assert(tap.size == 2);
        assert(tap.events[0].pressed && !tap.events[1].pressed);
        assert(tap.events[0].stroke.keycode == stroke.keycode);
        assert(tap.events[1].stroke.keycode == stroke.keycode);
        assert(tap.events[1].stroke.modifiers == 0);
        bool latched = false;
        assert(PollKeyboardShortcut(static_cast<std::uint16_t>(1u << index), latched) == -1);
    }
    for (float width : {960.0f, 1280.0f, 1920.0f}) {
        for (std::size_t i = 0; i < kKeyboardTouchControls.size(); ++i) {
            const auto bounds = KeyboardShortcutBounds(i, 0, 0, width);
            assert(bounds.x >= 0 && bounds.x + bounds.width <= width);
            assert(bounds.y >= 0 && bounds.y + bounds.height < 248);
            for (std::size_t j = 0; j < kKeyboardTouchControls.size(); ++j) {
                if (i != j)
                    assert(!KeyboardShortcutBounds(j, 0, 0, width).Contains(
                        bounds.x + bounds.width / 2, bounds.y + bounds.height / 2));
            }
        }
    }
}
