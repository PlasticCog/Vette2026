#pragma once
// SDL key -> IBM PC scan-code set 1, as the hosted game's keyboard handler (port 60h) expects.
// VETTE! predates the 101-key keyboard, so by default the cursor block is sent as the plain keypad
// codes of an 84-key keyboard (no E0 prefix), which 1989 keyboard handlers understand.

#include <cstdint>
#include <vector>

#include <SDL3/SDL_scancode.h>

namespace vette {

// Appends the set-1 bytes for a press or release to `out`. Returns false if the key has no mapping.
bool xt_scancode(SDL_Scancode key, bool pressed, std::vector<uint8_t>& out, bool enhanced = false);

} // namespace vette
