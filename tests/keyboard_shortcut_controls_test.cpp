#include "keyboard_input_policy.hpp"

#include <array>
#include <cassert>

int main()
{
    using namespace opennow::input;

    constexpr std::array<KeyboardStroke, 8> expected {{
        {0x1b, 0x01, 0}, {0x09, 0x0f, 0}, {0x09, 0x0f, 4}, {0x5b, 0x5b, 8},
        {'D', 0x20, 8}, {'E', 0x12, 8}, {'R', 0x13, 8}, {0x09, 0x0f, 8},
    }};
    for (std::size_t i = 0; i < kKeyboardShortcutControls.size(); ++i)
    {
        const auto stroke = MapKeyboardShortcut(kKeyboardShortcutControls[i].shortcut);
        assert(stroke.keycode == expected[i].keycode);
        assert(stroke.scancode == expected[i].scancode);
        assert(stroke.modifiers == expected[i].modifiers);
        assert(kKeyboardShortcutControls[i].label[0] != '\0');
        assert(kKeyboardShortcutControls[i].chord[0] != '\0');

        const auto tap = MakeKeyboardTap(stroke);
        assert(tap.size >= 2 && tap.size <= tap.events.size());
        assert(!tap.events[tap.size - 1].pressed);
        assert(tap.events[tap.size - 1].stroke.modifiers == 0);
        std::array<bool, 256> held {};
        for (std::size_t j = 0; j < tap.size; ++j)
        {
            const auto& event = tap.events[j];
            assert(event.stroke.keycode < held.size());
            assert(held[event.stroke.keycode] != event.pressed);
            held[event.stroke.keycode] = event.pressed;
        }
        for (bool down : held)
            assert(!down);

        bool latched = false;
        const auto button = static_cast<std::uint16_t>(1u << i);
        assert(PollKeyboardShortcut(button, latched) == static_cast<int>(i));
        for (int frame = 0; frame < 120; ++frame)
            assert(PollKeyboardShortcut(button, latched) == -1);
        assert(PollKeyboardShortcut(0, latched) == -1);
        assert(!latched);
        assert(PollKeyboardShortcut(button, latched) == static_cast<int>(i));
    }

    bool latched = false;
    assert(PollKeyboardShortcut(0x09, latched) == -1);
    assert(PollKeyboardShortcut(0x01, latched) == -1);
    assert(PollKeyboardShortcut(0, latched) == -1);
    assert(PollKeyboardShortcut(0x01, latched) == 0);
    assert(PollKeyboardShortcut(0x02, latched) == -1);
    latched = true;
    assert(PollKeyboardShortcut(0x80, latched) == -1);
    assert(PollKeyboardShortcut(0, latched) == -1);
    assert(PollKeyboardShortcut(0x80, latched) == 7);

    const auto alt_tab = MakeKeyboardTap(MapKeyboardShortcut(KeyboardShortcut::AltTab));
    assert(alt_tab.size == 4);
    assert(alt_tab.events[0].stroke.keycode == 0x12 && alt_tab.events[0].pressed);
    assert(alt_tab.events[0].stroke.modifiers == 4);
    assert(alt_tab.events[1].stroke.keycode == 0x09 && alt_tab.events[1].pressed);
    assert(alt_tab.events[1].stroke.modifiers == 4);
    assert(alt_tab.events[2].stroke.keycode == 0x09 && !alt_tab.events[2].pressed);
    assert(alt_tab.events[2].stroke.modifiers == 4);
    assert(alt_tab.events[3].stroke.keycode == 0x12 && !alt_tab.events[3].pressed);
    assert(alt_tab.events[3].stroke.modifiers == 0);

    const auto windows = MakeKeyboardTap(MapKeyboardShortcut(KeyboardShortcut::Windows));
    assert(windows.size == 2);
    assert(windows.events[0].stroke.keycode == 0x5b && windows.events[0].pressed);
    assert(windows.events[0].stroke.modifiers == 8);
    assert(windows.events[1].stroke.keycode == 0x5b && !windows.events[1].pressed);
    assert(windows.events[1].stroke.modifiers == 0);

    const auto win_r = MakeKeyboardTap(MapKeyboardShortcut(KeyboardShortcut::WindowsRun));
    assert(win_r.size == 4);
    assert(win_r.events[0].stroke.keycode == 0x5b && win_r.events[0].pressed);
    assert(win_r.events[1].stroke.keycode == 'R' && win_r.events[1].stroke.modifiers == 8);
    assert(win_r.events[2].stroke.keycode == 'R' && !win_r.events[2].pressed);
    assert(win_r.events[3].stroke.keycode == 0x5b && win_r.events[3].stroke.modifiers == 0);

    KeyboardStroke uppercase;
    assert(MapAsciiKey('A', uppercase));
    const auto shift_a = MakeKeyboardTap(uppercase);
    assert(shift_a.size == 4);
    assert(shift_a.events[0].stroke.keycode == 0x10);
    assert(shift_a.events[3].stroke.keycode == 0x10);
    assert(shift_a.events[3].stroke.modifiers == 0);

    const auto multi = MakeKeyboardTap({'A', 0x1e, 0x0f});
    assert(multi.size == 10);
    assert(multi.events[0].stroke.modifiers == 1);
    assert(multi.events[1].stroke.modifiers == 3);
    assert(multi.events[2].stroke.modifiers == 7);
    assert(multi.events[3].stroke.modifiers == 15);
    assert(multi.events[6].stroke.keycode == 0x5b && multi.events[6].stroke.modifiers == 7);
    assert(multi.events[7].stroke.keycode == 0x12 && multi.events[7].stroke.modifiers == 3);
    assert(multi.events[8].stroke.keycode == 0x11 && multi.events[8].stroke.modifiers == 1);
    assert(multi.events[9].stroke.keycode == 0x10 && multi.events[9].stroke.modifiers == 0);

    for (float width : {960.0f, 1280.0f, 1920.0f})
    {
        for (std::size_t i = 0; i < kKeyboardShortcutControls.size(); ++i)
        {
            const auto rect = KeyboardShortcutBounds(i, 10, 20, width);
            assert(rect.width > 0 && rect.height >= 48);
            assert(rect.x >= 10 && rect.x + rect.width <= 10 + width);
            assert(rect.Contains(rect.x, rect.y));
            assert(rect.Contains(rect.x + rect.width / 2, rect.y + rect.height / 2));
            assert(!rect.Contains(rect.x - 1, rect.y));
            assert(!rect.Contains(rect.x + rect.width, rect.y));
            assert(!rect.Contains(rect.x, rect.y + rect.height));
            for (std::size_t j = 0; j < kKeyboardShortcutControls.size(); ++j)
            {
                if (i == j)
                    continue;
                assert(!KeyboardShortcutBounds(j, 10, 20, width).Contains(
                    rect.x + rect.width / 2, rect.y + rect.height / 2));
            }
        }
    }
}
