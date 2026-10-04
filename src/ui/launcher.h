#pragma once
// The launch menu: before the game boots, the player picks the game folder, reviews and changes the
// settings (presets, frame rate, PC speed, enhancements, controls, display, sound), then plays or
// quits. Drawn full-window with chunky square-pixel text.

#include <optional>

#include "core/game_dir.h"
#include "core/settings.h"

namespace vette {
class Gamepad;
class Presenter;
}  // namespace vette

namespace vette::ui {

enum class LaunchChoice { Start, Quit };

// Runs until the player starts or quits. Edits `settings` in place (including the game folder).
// `game` starts as the result of `search`. The player can choose another folder, and Start is only
// possible with a valid one, so `game` is set whenever this returns Start. Keyboard, mouse and
// gamepad all work; the display setting applies to the window immediately.
LaunchChoice run_launcher(Presenter& presenter, Gamepad& gamepad, Settings& settings, std::optional<GameDir>& game,
                          const GameDirSearch& search);

}  // namespace vette::ui
