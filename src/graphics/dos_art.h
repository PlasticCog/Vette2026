#pragma once
// The DOS pictures the screens are recognised by, decoded from the player's DOS files.
//
// Formats (re/notes/10-graphics.md): PCX-style RLE (assets/rle.h) over 4 bit planes stored one after
// another (assets/planar.h), the same bit layout as EGA video memory. The game unpacks them straight
// into video memory row by row (3009:8666). EGAPIC.BIN and EGASKILL.BIN start with a word holding the
// packed size. The race dashboard has no file: it is packed into VETTE.EXE itself, at offset 100h of
// the program image.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace vette::graphics {

struct DosPicture {
    int width = 0, height = 0;
    std::vector<std::uint8_t> pixels;  // palette indices 0-15, row-major
};

// Decodes an RLE-packed planar picture of width x height, skipping `header` bytes first.
bool decode_dos_picture(std::span<const std::uint8_t> file, int header, int width, int height, DosPicture& out,
                        std::string* error = nullptr);

// Where VETTE.EXE's program image keeps the packed dashboard (320x80, mode 0Dh rows 120-199).
constexpr std::size_t kDashImageOffset = 0x100;
constexpr int kDashWidth = 320, kDashHeight = 80;
// Decodes the dashboard from the unpacked program image (in the emulator: the bytes from linear
// address Machine::kLoadSegment * 16 on, once the EXEPACK stub has run).
bool decode_dos_dash(std::span<const std::uint8_t> program_image, DosPicture& out, std::string* error = nullptr);

}  // namespace vette::graphics
