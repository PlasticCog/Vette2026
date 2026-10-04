#pragma once
// A gamepad (SDL's gamepad API) standing in for the PC's analog joystick on the game port, plus a
// few keyboard keys, because VETTE!'s menus are keyboard-driven.
//
//   left stick X            -> joystick X
//   right / left trigger    -> joystick Y: right is forward/up (-1), left is back/down (+1), the two
//                              combined; with both released, the left stick's Y is used instead
//   A (south) / B (east)    -> joystick buttons 1 / 2
//   D-pad, Start, Back      -> arrow keys, Enter, Esc (a make code on press, a break on release)
//
// The left stick has a small radial dead zone.

#include "platform/sdl_util.h"

#include <cstdint>
#include <vector>

namespace vette {

// What the game port reports.
struct JoystickState {
    float x = 0;               // -1 (left) .. 1 (right)
    float y = 0;               // -1 (up/forward) .. 1 (down/back)
    std::uint8_t buttons = 0;  // bit0 button 1, bit1 button 2
};

// Uses the first connected gamepad and follows hot-plugging. Initializes SDL's gamepad subsystem
// itself; if that fails, it logs a warning and never connects.
class Gamepad {
public:
    Gamepad();
    ~Gamepad();
    Gamepad(const Gamepad&) = delete;
    Gamepad& operator=(const Gamepad&) = delete;

    bool connected() const { return pad_ != nullptr; }

    // Handles gamepad hot-plug and button events and ignores all others. Menu buttons append their
    // set-1 scan codes to `scancodes`; so does unplugging the pad while one is held (its release).
    void handle_event(const SDL_Event& event, std::vector<std::uint8_t>& scancodes);

    // The current stick and buttons: centered with no buttons while no gamepad is connected.
    JoystickState joystick() const;

private:
    void open_first(SDL_JoystickID except);
    void release_menu_keys(std::vector<std::uint8_t>& scancodes);

    bool initialized_ = false;
    SdlPtr<SDL_Gamepad> pad_;
    SDL_JoystickID pad_id_ = 0;
    std::uint8_t menu_held_ = 0;  // bit i: menu button i (see gamepad.cpp) is down
};

}  // namespace vette
