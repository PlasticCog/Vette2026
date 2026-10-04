#include "platform/keymap.h"

namespace vette {
namespace {

struct Mapping {
    uint8_t code;
    bool e0;  // sent with an E0 prefix (only when `enhanced`, or for keys that exist only on 101-key boards)
};

Mapping lookup(SDL_Scancode key) {
    if (key >= SDL_SCANCODE_A && key <= SDL_SCANCODE_Z) {
        static constexpr uint8_t kLetters[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17,
                                                 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13,
                                                 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
        return {kLetters[key - SDL_SCANCODE_A], false};
    }
    if (key >= SDL_SCANCODE_1 && key <= SDL_SCANCODE_0) {
        return {static_cast<uint8_t>(0x02 + (key - SDL_SCANCODE_1)), false};
    }
    if (key >= SDL_SCANCODE_F1 && key <= SDL_SCANCODE_F10) {
        return {static_cast<uint8_t>(0x3B + (key - SDL_SCANCODE_F1)), false};
    }
    switch (key) {
    case SDL_SCANCODE_RETURN: return {0x1C, false};
    case SDL_SCANCODE_ESCAPE: return {0x01, false};
    case SDL_SCANCODE_BACKSPACE: return {0x0E, false};
    case SDL_SCANCODE_TAB: return {0x0F, false};
    case SDL_SCANCODE_SPACE: return {0x39, false};
    case SDL_SCANCODE_MINUS: return {0x0C, false};
    case SDL_SCANCODE_EQUALS: return {0x0D, false};
    case SDL_SCANCODE_LEFTBRACKET: return {0x1A, false};
    case SDL_SCANCODE_RIGHTBRACKET: return {0x1B, false};
    case SDL_SCANCODE_BACKSLASH: return {0x2B, false};
    case SDL_SCANCODE_SEMICOLON: return {0x27, false};
    case SDL_SCANCODE_APOSTROPHE: return {0x28, false};
    case SDL_SCANCODE_GRAVE: return {0x29, false};
    case SDL_SCANCODE_COMMA: return {0x33, false};
    case SDL_SCANCODE_PERIOD: return {0x34, false};
    case SDL_SCANCODE_SLASH: return {0x35, false};
    case SDL_SCANCODE_CAPSLOCK: return {0x3A, false};
    case SDL_SCANCODE_F11: return {0x57, false};
    case SDL_SCANCODE_F12: return {0x58, false};
    case SDL_SCANCODE_SCROLLLOCK: return {0x46, false};
    case SDL_SCANCODE_NUMLOCKCLEAR: return {0x45, false};
    case SDL_SCANCODE_LCTRL: return {0x1D, false};
    case SDL_SCANCODE_LSHIFT: return {0x2A, false};
    case SDL_SCANCODE_LALT: return {0x38, false};
    case SDL_SCANCODE_RSHIFT: return {0x36, false};
    case SDL_SCANCODE_RCTRL: return {0x1D, true};
    case SDL_SCANCODE_RALT: return {0x38, true};
    // Cursor block: keypad codes, E0-prefixed on a 101-key keyboard.
    case SDL_SCANCODE_UP: return {0x48, true};
    case SDL_SCANCODE_DOWN: return {0x50, true};
    case SDL_SCANCODE_LEFT: return {0x4B, true};
    case SDL_SCANCODE_RIGHT: return {0x4D, true};
    case SDL_SCANCODE_HOME: return {0x47, true};
    case SDL_SCANCODE_END: return {0x4F, true};
    case SDL_SCANCODE_PAGEUP: return {0x49, true};
    case SDL_SCANCODE_PAGEDOWN: return {0x51, true};
    case SDL_SCANCODE_INSERT: return {0x52, true};
    case SDL_SCANCODE_DELETE: return {0x53, true};
    // Numeric keypad.
    case SDL_SCANCODE_KP_DIVIDE: return {0x35, true};
    case SDL_SCANCODE_KP_ENTER: return {0x1C, true};
    case SDL_SCANCODE_KP_MULTIPLY: return {0x37, false};
    case SDL_SCANCODE_KP_MINUS: return {0x4A, false};
    case SDL_SCANCODE_KP_PLUS: return {0x4E, false};
    case SDL_SCANCODE_KP_7: return {0x47, false};
    case SDL_SCANCODE_KP_8: return {0x48, false};
    case SDL_SCANCODE_KP_9: return {0x49, false};
    case SDL_SCANCODE_KP_4: return {0x4B, false};
    case SDL_SCANCODE_KP_5: return {0x4C, false};
    case SDL_SCANCODE_KP_6: return {0x4D, false};
    case SDL_SCANCODE_KP_1: return {0x4F, false};
    case SDL_SCANCODE_KP_2: return {0x50, false};
    case SDL_SCANCODE_KP_3: return {0x51, false};
    case SDL_SCANCODE_KP_0: return {0x52, false};
    case SDL_SCANCODE_KP_PERIOD: return {0x53, false};
    default: return {0, false};
    }
}

} // namespace

bool xt_scancode(SDL_Scancode key, bool pressed, std::vector<uint8_t>& out, bool enhanced) {
    const Mapping m = lookup(key);
    if (m.code == 0) {
        return false;
    }
    // Right Ctrl/Alt and keypad Enter/Divide only exist with the prefix; the cursor block uses it
    // only when emulating a 101-key keyboard.
    const bool prefix_only_key = key == SDL_SCANCODE_RCTRL || key == SDL_SCANCODE_RALT ||
                                 key == SDL_SCANCODE_KP_ENTER || key == SDL_SCANCODE_KP_DIVIDE;
    if (m.e0 && (enhanced || prefix_only_key)) {
        out.push_back(0xE0);
    }
    out.push_back(static_cast<uint8_t>(pressed ? m.code : m.code | 0x80));
    return true;
}

} // namespace vette
