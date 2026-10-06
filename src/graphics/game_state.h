#pragma once
// What the replacement art reads from the running DOS game: the dashboard's values, the course
// shown on the map screen, and the game's own text font. All from the emulator's memory (1 MB,
// Memory::ram()); nothing is written. Addresses are the game's (re/symbols.csv): data segment 124Ah,
// code segment 3009h, loaded at image segment + 1000h.

#include <array>
#include <cstdint>
#include <string_view>

#include "graphics/image.h"

namespace vette::graphics {

// Linear addresses in the emulator of the game's data and code segments.
constexpr std::uint32_t kGameDs = (0x124Au + 0x1000u) * 16;
constexpr std::uint32_t kGameCs = (0x3009u + 0x1000u) * 16;
// The second data segment (image 0ACBh, 1ACBh at run time): file names, the excuse list, the
// penalty texts.
constexpr std::uint32_t kGameDs2 = (0x0ACBu + 0x1000u) * 16;
constexpr std::uint32_t kProgramImage = 0x1000u * 16;  // the unpacked VETTE.EXE image

struct DashState {
    int speed_mph = 0;   // DS:3AA1, the value the dashboard shows (hud_speed_value)
    int rpm100 = 0;      // CS:588F, the revs the dashboard shows (the player's DS:2D61), in 100 rpm
    int gear = 0;        // DS:2D45: 0 neutral, 1..max_gear, max_gear + 1 reverse
    int max_gear = 4;    // DS:2D47: 4, 5 or 6
    bool automatic = false;  // DS:2D50
    bool cruise = false;     // DS:2AD7
    bool shift_up = false;   // DS:2D51: the shift light
    int steering = 0;        // DS:2B84: the wheel, signed (the hands' pose)
    int view = 0;            // DS:2B87: 0 ahead, +85 looking right (F3), -85 left (F1)
};
DashState read_dash_state(const std::uint8_t* ram);

// The course the map screen shows (DS:FD10, 1..4), or 0 if the value isn't a course.
int read_map_course(const std::uint8_t* ram);

// The DOS game's 8x10 text font (DS:F3DE, characters 20h..7Fh, one byte per row, MSB = left).
struct DosFont {
    std::array<std::array<std::uint8_t, 10>, 96> glyphs{};
    bool valid = false;
};
DosFont read_dos_font(const std::uint8_t* ram);

// Draws `text` into `image` with its top-left at (x, y), each font pixel a scale x scale square.
// Characters outside 20h..7Fh are drawn as spaces. Returns the x after the last character.
int draw_text(Image& image, const DosFont& font, std::string_view text, int x, int y, int scale,
              std::uint32_t argb);

}  // namespace vette::graphics
