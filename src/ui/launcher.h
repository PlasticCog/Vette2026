#pragma once
// The launch menu: before the game boots, the player reviews and changes the settings (presets,
// frame rate, PC speed, enhancements, controls, display, sound), then starts the game or quits.
// It's drawn into a 640x200 frame like the game's own screens, over the title picture.

#include "core/settings.h"
#include "platform/framebuffer.h"

namespace vette {
class Gamepad;
class Presenter;
}  // namespace vette

namespace vette::ui {

enum class LaunchChoice { Start, Quit };

// Runs until the player starts or quits; edits `settings` in place. Keyboard, mouse and gamepad all
// work. The display setting is applied to the window immediately.
LaunchChoice run_launcher(Presenter& presenter, Gamepad& gamepad, const Framebuffer& background, Settings& settings);

}  // namespace vette::ui
