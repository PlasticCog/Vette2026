#include "ui/key_sheet.h"

#include <algorithm>
#include <span>
#include <string_view>

#include "ui/theme.h"

namespace vette::ui {
namespace {

using namespace theme;

constexpr std::uint32_t kSeeThrough = 0xFF00FF;  // the canvas's background: the game shows there

// A row: keys and what they do; a heading has no keys; an empty row is a gap.
struct Key {
    std::string_view keys, what;
};

// In the race (the original's keys; X, the horn, is this program's: notes 07, 9.1).
constexpr Key kLeft[] = {
    {"", "Driving"},
    {"Up  I", "Gas"},
    {"Down  Space", "Brake"},
    {"Left  J", "Steer left"},
    {"Right  L", "Steer right"},
    {"U  O", "Gas and steer"},
    {"N  ,", "Brake and steer"},
    {"K", "Straighten up"},
    {"A", "Automatic gears"},
    {"1-6  0", "Gear, neutral"},
    {"-  =", "Shift down, up"},
    {"R", "Reverse"},
    {"C", "Cruise control"},
    {"X", "Horn"},
    {"", ""},
    {"", "Race"},
    {"P", "Pause"},
    {"Esc", "Menu, quit race"},
    {"S", "Sound on, off"},
    {"E", "Engine noise"},
};
constexpr Key kRight[] = {
    {"", "View"},
    {"F1  F3", "Look left, right"},
    {"F2", "Look ahead"},
    {"F4  Keypad+", "Helicopter"},
    {"F5", "Full-screen view"},
    {"F6", "Rear-view mirror"},
    {"F7  F8", "Camera up, down"},
    {"F9  F10", "Tilt up, down"},
    {"H", "Map"},
    {"D", "Damage display"},
    {"W", "Building windows"},
    {"B", "Buildings"},
    {"", ""},
    {"", "VETTE! 2026"},
    {"Ctrl+H", "These keys"},
    {"Tab", "2P: tag, arrow"},
    {"Alt+Q", "Quit to desktop"},
    {"F11", "Full screen"},
};

constexpr int kKeyChars = 11;   // the key column
constexpr int kWhatChars = 16;  // the description column
constexpr int kPitch = 11;      // between rows
constexpr int kPad = 12;        // inside the panel's edge
constexpr int kColumnGap = 16;
constexpr int kColumnW = (kKeyChars + 1 + kWhatChars) * kGlyph;
constexpr int kPanelW = 2 * kPad + 2 * kColumnW + kColumnGap;

void column(Canvas& c, int x, int y, std::span<const Key> rows) {
    for (const Key& k : rows) {
        if (k.keys.empty()) {
            c.text(x, y, k.what, kGold);
        } else {
            c.text(x, y, k.keys, kValue);
            c.text(x + (kKeyChars + 1) * kGlyph, y, k.what, kLabel);
        }
        y += kPitch;
    }
}

}  // namespace

void draw_key_sheet(Canvas& canvas, int out_w, int out_h, bool paused) {
    // The largest whole scale that keeps the panel on screen (with a margin), as the launch menu's.
    const int rows = static_cast<int>(std::max(std::size(kLeft), std::size(kRight)));
    const int panel_h = kPad + 24 + rows * kPitch + 8 + 2 * kPitch + kPad;
    const int scale = std::max(1, std::min(out_w / (kPanelW + 24), out_h / (panel_h + 24)));
    canvas.reset(std::max(out_w / scale, 1), std::max(out_h / scale, 1), scale, kSeeThrough);

    const int x = (canvas.width - kPanelW) / 2;
    const int y = (canvas.height - panel_h) / 2;
    canvas.fill_rect(x - 1, y - 1, kPanelW + 2, panel_h + 2, kRule);
    canvas.fill_rect(x, y, kPanelW, panel_h, kBackground);
    canvas.text(x + kPad, y + kPad, "Keys", kGold, 2, true);
    const int top = y + kPad + 24;
    column(canvas, x + kPad, top, kLeft);
    column(canvas, x + kPad + kColumnW + kColumnGap, top, kRight);
    const int foot = top + rows * kPitch + 8;
    canvas.text(x + kPad, foot,
                paused ? "Paused. Esc, Enter or Ctrl+H: back to the game."
                       : "The race goes on. Esc, Enter or Ctrl+H: back to it.",
                kHelp);
    canvas.text(x + kPad, foot + kPitch, "Gamepad: Start or Back closes this.", kHint);
}

}  // namespace vette::ui
