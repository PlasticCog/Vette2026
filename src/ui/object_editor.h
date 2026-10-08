#pragma once
// The object editor (launch menu > Objects, Enter): the original's 3D models (the cars, the
// pedestrians, the landmark buildings), and the city's code-drawn buildings and street objects as models
// (enhanced/object_models.h), one at a time in a 3D view to turn round and zoom into, reshaped
// vertex by vertex and face by face: move, add, delete, mirror and recolour them, make new faces. Saved
// as a set of one's own (game/model_pack.h) in the save folder's objects folder, which the launch menu's
// Objects setting then plays with; the game's own files are never changed. Mouse and keyboard.

#include <SDL3/SDL_keycode.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/game_dir.h"
#include "game/model_pack.h"

namespace vette {
class Gamepad;
class Presenter;
}  // namespace vette

namespace vette::ui {

// The sets saved in `dir`, by name (the file's name without ".vobj"), sorted.
std::vector<std::string> list_object_packs(const std::filesystem::path& dir);
// Set `name` from `dir`; nullopt with the reason in `error`.
std::optional<game::ModelPack> load_object_pack(const std::filesystem::path& dir, const std::string& name, std::string& error);

// For checking the editor without hands on it (vette2026 --object-editor): keys pressed and screenshots
// taken at times since it opened, then it closes.
struct EditorScript {
    struct Key {
        uint64_t at_ms = 0;
        SDL_Keycode key = 0;
        SDL_Keymod mod = 0;
    };
    std::vector<Key> keys;
    // The mouse: a button pressed or let go, or moved to (x, y) in window coordinates.
    struct Mouse {
        uint64_t at_ms = 0;
        enum class Kind { Down, Up, Move } kind = Kind::Move;
        float x = 0, y = 0;
        uint8_t button = 1;  // SDL_BUTTON_LEFT
    };
    std::vector<Mouse> mouse;
    std::vector<std::pair<uint64_t, std::string>> shots;  // ms, BMP file
    uint64_t quit_ms = 0;                                  // 0: never
};

// Edits set `name` (empty: a new one, from the original's). Returns the set to play afterwards: the one
// last saved, else `name` as it was.
std::string run_object_editor(Presenter& presenter, Gamepad& gamepad, const GameDir& game, const std::filesystem::path& dir,
                              const std::string& name, const EditorScript* script = nullptr);

}  // namespace vette::ui
