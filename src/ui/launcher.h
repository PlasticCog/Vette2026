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

struct OnlineSession;

enum class LaunchChoice { Start, Online, Quit };

// Runs until the player starts or quits. Edits `settings` in place (including the game folder).
// `game` starts as the result of `search` (the DOS files, and the versions found in the game folder,
// which the menu shows). The player can choose another folder, which is scanned again and replaces
// `search`; Start is only possible with the DOS files, so `game` is set whenever this returns Start.
// Online: the player set up an online race (ui/online.h), which is in `*online`; null hides it.
// Keyboard, mouse and gamepad all work; the display setting applies to the window immediately.
LaunchChoice run_launcher(Presenter& presenter, Gamepad& gamepad, Settings& settings, std::optional<GameDir>& game,
                          GameDirSearch& search, OnlineSession* online = nullptr);

}  // namespace vette::ui
