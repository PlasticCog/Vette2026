#pragma once
// The keys, on a panel over the game (Ctrl+H): the original's race keys (its handler for each scan code,
// key_handlers 3009:0D4E in re/symbols.csv) and this program's own (ui/shortcuts.h).

#include "ui/canvas.h"

namespace vette::ui {

// Lays the sheet out on `canvas`, reset for a window of `out_w` x `out_h` pixels: the panel in the middle,
// the rest in the canvas's background colour (Presenter::show_overlay leaves that out). `paused`: the game
// waits while the sheet is open; otherwise (an online race) it goes on.
void draw_key_sheet(Canvas& canvas, int out_w, int out_h, bool paused);

}  // namespace vette::ui
