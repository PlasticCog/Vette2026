#pragma once
// The program's own keys, the same on every screen: Alt+Q quits to the desktop, Ctrl+H shows the keys
// during the game (ui/key_sheet.h). (The original's Ctrl+Q quits a race: it keeps that.)

#include <SDL3/SDL.h>

namespace vette::ui {

// A key press (or its repeat) of `key` with Ctrl or Alt, as `mod` says, and not the other. AltGr arrives
// as Ctrl+Alt: never a shortcut, so the layouts that type characters with it keep them. Shift is ignored.
inline bool shortcut(const SDL_Event& e, SDL_Keycode key, SDL_Keymod mod) {
    constexpr SDL_Keymod kCtrlAlt = SDL_KMOD_CTRL | SDL_KMOD_ALT;
    if (e.type != SDL_EVENT_KEY_DOWN || e.key.key != key)
        return false;
    const auto held = static_cast<SDL_Keymod>(e.key.mod & kCtrlAlt);
    return (held & mod) != 0 && (held & (kCtrlAlt & ~mod)) == 0;
}

// Alt+Q: posts SDL_EVENT_QUIT, which every screen takes as the window closing. True for the key's
// events (its repeats do nothing more): the caller drops them.
inline bool quit_shortcut(const SDL_Event& e) {
    if (!shortcut(e, SDLK_Q, SDL_KMOD_ALT))
        return false;
    if (!e.key.repeat) {
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
    }
    return true;
}

// Ctrl+H: the key sheet, during the game.
inline bool keys_shortcut(const SDL_Event& e) { return shortcut(e, SDLK_H, SDL_KMOD_CTRL); }

}  // namespace vette::ui
