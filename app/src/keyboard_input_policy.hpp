#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace opennow::input
{

struct KeyboardStroke
{
    std::uint16_t keycode   = 0;
    std::uint16_t scancode  = 0;
    std::uint16_t modifiers = 0;
};

enum class KeyboardShortcut
{
    Escape,
    Tab,
    AltTab,
    Windows,
    WindowsDesktop,
    WindowsExplorer,
    WindowsRun,
    WindowsTaskView,
    Backspace,
    Delete,
    ArrowLeft,
    ArrowRight,
};

struct KeyboardShortcutControl
{
    KeyboardShortcut shortcut;
    const char* label;
    const char* chord;
};

inline constexpr std::array<KeyboardShortcutControl, 12> kKeyboardShortcutControls {{
    {KeyboardShortcut::Escape, "Esc", "MINUS + ZL"},
    {KeyboardShortcut::Tab, "Tab", "MINUS + L"},
    {KeyboardShortcut::AltTab, "Alt + Tab", "MINUS + R"},
    {KeyboardShortcut::Windows, "Win / Super", "MINUS + ZR"},
    {KeyboardShortcut::WindowsDesktop, "Win + D", "MINUS + LEFT"},
    {KeyboardShortcut::WindowsExplorer, "Win + E", "MINUS + UP"},
    {KeyboardShortcut::WindowsRun, "Win + R", "MINUS + RIGHT"},
    {KeyboardShortcut::WindowsTaskView, "Win + Tab", "MINUS + DOWN"},
    // Direct keys: bypass the applet diff so they work on pre-existing
    // remote text even when nothing was typed locally. Touch-only
    // (no MINUS chord left); tap them in the shortcut bar.
    {KeyboardShortcut::Backspace, "Bksp", "TAP"},
    {KeyboardShortcut::Delete, "Del", "TAP"},
    {KeyboardShortcut::ArrowLeft, "Left", "TAP"},
    {KeyboardShortcut::ArrowRight, "Right", "TAP"},
}};

inline int PollKeyboardShortcut(std::uint16_t buttons, bool& latched)
{
    const bool was_latched = latched;
    latched = buttons != 0;
    if (was_latched || buttons == 0 || (buttons & (buttons - 1)) != 0)
        return -1;
    for (std::size_t i = 0; i < kKeyboardShortcutControls.size(); ++i)
        if (buttons == (1u << i))
            return static_cast<int>(i);
    return -1;
}

inline KeyboardStroke MapKeyboardShortcut(KeyboardShortcut shortcut)
{
    constexpr std::uint16_t kAlt  = 0x0004;
    constexpr std::uint16_t kMeta = 0x0008;

    switch (shortcut)
    {
        case KeyboardShortcut::Escape: return {0x1b, 0x01, 0};
        case KeyboardShortcut::Tab: return {0x09, 0x0f, 0};
        case KeyboardShortcut::AltTab: return {0x09, 0x0f, kAlt};
        case KeyboardShortcut::Windows: return {0x5b, 0x5b, kMeta};
        case KeyboardShortcut::WindowsDesktop: return {'D', 0x20, kMeta};
        case KeyboardShortcut::WindowsExplorer: return {'E', 0x12, kMeta};
        case KeyboardShortcut::WindowsRun: return {'R', 0x13, kMeta};
        case KeyboardShortcut::WindowsTaskView: return {0x09, 0x0f, kMeta};
        case KeyboardShortcut::Backspace: return {0x08, 0x0e, 0};
        case KeyboardShortcut::Delete: return {0x2e, 0x53, 0};
        case KeyboardShortcut::ArrowLeft: return {0x25, 0x4b, 0};
        case KeyboardShortcut::ArrowRight: return {0x27, 0x4d, 0};
    }

    return {};
}

struct KeyboardEvent
{
    KeyboardStroke stroke;
    bool pressed = false;
};

struct KeyboardTap
{
    std::array<KeyboardEvent, 10> events {};
    std::size_t size = 0;
};

inline KeyboardTap MakeKeyboardTap(KeyboardStroke stroke)
{
    constexpr std::array<KeyboardStroke, 4> modifiers {{
        {0x10, 0x2a, 0x0001},
        {0x11, 0x1d, 0x0002},
        {0x12, 0x38, 0x0004},
        {0x5b, 0x5b, 0x0008},
    }};
    KeyboardTap tap;
    std::uint16_t active = 0;
    for (const auto& modifier : modifiers)
    {
        if ((stroke.modifiers & modifier.modifiers) == 0 ||
            stroke.keycode == modifier.keycode)
            continue;
        active |= modifier.modifiers;
        tap.events[tap.size++] = {{modifier.keycode, modifier.scancode, active}, true};
    }
    tap.events[tap.size++] = {stroke, true};
    tap.events[tap.size++] = {{stroke.keycode, stroke.scancode, active}, false};
    for (auto it = modifiers.rbegin(); it != modifiers.rend(); ++it)
    {
        if ((active & it->modifiers) == 0)
            continue;
        active &= ~it->modifiers;
        tap.events[tap.size++] = {{it->keycode, it->scancode, active}, false};
    }
    return tap;
}

struct KeyboardShortcutRect
{
    float x;
    float y;
    float width;
    float height;

    bool Contains(float touch_x, float touch_y) const
    {
        return touch_x >= x && touch_x < x + width &&
               touch_y >= y && touch_y < y + height;
    }
};

inline KeyboardShortcutRect KeyboardShortcutBounds(
    std::size_t index, float x, float y, float width)
{
    const float key_width = (width - 60.0f) / 4.0f;
    return {x + 18.0f + static_cast<float>(index % 4) * (key_width + 8.0f),
            y + 48.0f + static_cast<float>(index / 4) * 64.0f,
            key_width, 56.0f};
}

inline bool MapAsciiKey(char character, KeyboardStroke& stroke)
{
    constexpr std::uint16_t kShift = 0x0001;
    static constexpr std::uint8_t letter_scans[26] = {
        0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
        0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c,
    };
    if (character >= 'a' && character <= 'z')
    {
        const int index = character - 'a';
        stroke = {static_cast<std::uint16_t>('A' + index), letter_scans[index], 0};
        return true;
    }
    if (character >= 'A' && character <= 'Z')
    {
        const int index = character - 'A';
        stroke = {static_cast<std::uint16_t>('A' + index), letter_scans[index], kShift};
        return true;
    }
    if (character >= '1' && character <= '9')
    {
        stroke = {
            static_cast<std::uint16_t>(character),
            static_cast<std::uint16_t>(0x02 + character - '1'),
            0,
        };
        return true;
    }
    if (character == '0')
    {
        stroke = {'0', 0x0b, 0};
        return true;
    }

    switch (character)
    {
        case ' ': stroke = {0x20, 0x39, 0}; return true;
        case '@': stroke = {'2', 0x03, kShift}; return true;
        case '.': stroke = {0xbe, 0x34, 0}; return true;
        case ',': stroke = {0xbc, 0x33, 0}; return true;
        case '-': stroke = {0xbd, 0x0c, 0}; return true;
        case '_': stroke = {0xbd, 0x0c, kShift}; return true;
        case '=': stroke = {0xbb, 0x0d, 0}; return true;
        case '+': stroke = {0xbb, 0x0d, kShift}; return true;
        case '/': stroke = {0xbf, 0x35, 0}; return true;
        case '?': stroke = {0xbf, 0x35, kShift}; return true;
        case '\\': stroke = {0xdc, 0x2b, 0}; return true;
        case '|': stroke = {0xdc, 0x2b, kShift}; return true;
        case '[': stroke = {0xdb, 0x1a, 0}; return true;
        case '{': stroke = {0xdb, 0x1a, kShift}; return true;
        case ']': stroke = {0xdd, 0x1b, 0}; return true;
        case '}': stroke = {0xdd, 0x1b, kShift}; return true;
        case ':': stroke = {0xba, 0x27, kShift}; return true;
        case ';': stroke = {0xba, 0x27, 0}; return true;
        case '\'': stroke = {0xde, 0x28, 0}; return true;
        case '"': stroke = {0xde, 0x28, kShift}; return true;
        case '`': stroke = {0xc0, 0x29, 0}; return true;
        case '~': stroke = {0xc0, 0x29, kShift}; return true;
        case '<': stroke = {0xbc, 0x33, kShift}; return true;
        case '>': stroke = {0xbe, 0x34, kShift}; return true;
        case '!': stroke = {'1', 0x02, kShift}; return true;
        case '#': stroke = {'3', 0x04, kShift}; return true;
        case '$': stroke = {'4', 0x05, kShift}; return true;
        case '%': stroke = {'5', 0x06, kShift}; return true;
        case '^': stroke = {'6', 0x07, kShift}; return true;
        case '&': stroke = {'7', 0x08, kShift}; return true;
        case '*': stroke = {'8', 0x09, kShift}; return true;
        case '(': stroke = {'9', 0x0a, kShift}; return true;
        case ')': stroke = {'0', 0x0b, kShift}; return true;
        default: return false;
    }
}

} // namespace opennow::input
