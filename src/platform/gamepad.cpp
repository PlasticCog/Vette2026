#include "platform/gamepad.h"

#include "platform/keymap.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace vette {
namespace {

constexpr float kStickDeadZone = 0.15f;   // radial, as a fraction of full deflection
constexpr float kTriggerDeadZone = 0.05f; // a trigger below this counts as released

struct MenuKey {
    SDL_GamepadButton button;
    SDL_Scancode key;
};

constexpr MenuKey kMenuKeys[] = {
    {SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_SCANCODE_UP},       {SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_SCANCODE_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_SCANCODE_LEFT},   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_SCANCODE_RIGHT},
    {SDL_GAMEPAD_BUTTON_START, SDL_SCANCODE_RETURN},     {SDL_GAMEPAD_BUTTON_BACK, SDL_SCANCODE_ESCAPE},
};
static_assert(std::size(kMenuKeys) <= 8, "menu_held_ has one bit per menu key");

// Sticks -1..1, triggers 0..1.
float axis(SDL_Gamepad* pad, SDL_GamepadAxis which) {
    return std::max(-1.0f, static_cast<float>(SDL_GetGamepadAxis(pad, which)) / SDL_JOYSTICK_AXIS_MAX);
}

// Rescales a deflection so it starts at 0 at the dead zone's edge and still reaches 1.
float past_dead_zone(float amount, float dead_zone) {
    return std::max(0.0f, (amount - dead_zone) / (1.0f - dead_zone));
}

}  // namespace

Gamepad::Gamepad() {
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "No gamepad support: %s", SDL_GetError());
        return;
    }
    initialized_ = true;
    open_first(0);  // 0 is never a valid instance id
}

Gamepad::~Gamepad() {
    pad_.reset();
    if (initialized_)
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

void Gamepad::open_first(SDL_JoystickID except) {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids)
        return;
    for (int i = 0; i < count && !pad_; ++i) {
        if (ids[i] == except)
            continue;
        pad_.reset(SDL_OpenGamepad(ids[i]));
        if (!pad_) {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "Couldn't open gamepad: %s", SDL_GetError());
            continue;
        }
        pad_id_ = ids[i];
        const char* name = SDL_GetGamepadName(pad_.get());
        SDL_Log("Gamepad: %s", name ? name : "(unnamed)");
    }
    SDL_free(ids);
}

void Gamepad::release_menu_keys(std::vector<std::uint8_t>& scancodes) {
    for (std::size_t i = 0; i < std::size(kMenuKeys); ++i) {
        if (menu_held_ & (1u << i))
            xt_scancode(kMenuKeys[i].key, false, scancodes);
    }
    menu_held_ = 0;
}

void Gamepad::handle_event(const SDL_Event& event, std::vector<std::uint8_t>& scancodes) {
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!pad_)
            open_first(0);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (pad_ && event.gdevice.which == pad_id_) {
            SDL_Log("Gamepad disconnected");
            release_menu_keys(scancodes);
            pad_.reset();
            open_first(event.gdevice.which);  // carry on with another pad, if there is one
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        if (!pad_ || event.gbutton.which != pad_id_)
            break;
        const auto button = static_cast<SDL_GamepadButton>(event.gbutton.button);
        for (std::size_t i = 0; i < std::size(kMenuKeys); ++i) {
            const auto bit = static_cast<std::uint8_t>(1u << i);
            if (kMenuKeys[i].button != button || event.gbutton.down == ((menu_held_ & bit) != 0))
                continue;
            xt_scancode(kMenuKeys[i].key, event.gbutton.down, scancodes);
            menu_held_ ^= bit;
        }
        break;
    }
    default:
        break;
    }
}

JoystickState Gamepad::joystick() const {
    JoystickState state;
    SDL_Gamepad* pad = pad_.get();
    if (!pad)
        return state;

    // Left stick with a radial dead zone: inside it the stick reads centered; past it the
    // deflection is rescaled so it still starts at 0 and reaches full travel.
    float x = axis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    float y = axis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    const float magnitude = std::hypot(x, y);
    const float scale = magnitude > kStickDeadZone ? past_dead_zone(magnitude, kStickDeadZone) / magnitude : 0.0f;
    x = std::clamp(x * scale, -1.0f, 1.0f);
    y = std::clamp(y * scale, -1.0f, 1.0f);

    const float forward = past_dead_zone(axis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), kTriggerDeadZone);
    const float back = past_dead_zone(axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER), kTriggerDeadZone);

    state.x = x;
    state.y = forward > 0 || back > 0 ? back - forward : y;
    state.buttons = static_cast<std::uint8_t>((SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH) ? 1 : 0) |
                                              (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST) ? 2 : 0));
    return state;
}

}  // namespace vette
