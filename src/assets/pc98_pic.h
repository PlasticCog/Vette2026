#pragma once
// The PC-98 VETTE!'s pictures (.PIC). There is no header and no compression: four bit planes one
// after another (B, R, G, E on the PC-98; the same bit order as the DOS version's EGA planes), each
// `height` rows of width/8 bytes, most significant bit = leftmost pixel. Pixel index =
// p0 | p1 << 1 | p2 << 2 | p3 << 3. The full-screen pictures are byte-identical to the DOS .BIN
// files after RLE decoding; the race view's pictures are twice as wide as the DOS ones, since the
// PC-98 version draws its race at 640x200 where DOS uses 320x200.
//
// The sizes live in the game's drawing code, so they are tabled here (pc98_pic_info). The palette is
// one 16-entry table of 4-bit levels (the PC-98's 4096 colours), also from the game's code.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vette::assets {

struct Pc98Pic {
    int width = 0, height = 0;
    std::vector<std::uint8_t> pixels;  // palette indices 0-15, row-major
};

struct Pc98PicInfo {
    const char* name;  // upper case, e.g. "TITLE.PIC"
    int width, height;
    const char* what;
};

// The known pictures, or nullptr. Names are matched ignoring case.
const Pc98PicInfo* pc98_pic_info(std::string_view name);
std::span<const Pc98PicInfo> pc98_pics();

// Decodes `width` x `height` (width a multiple of 8) from the start of `data`; extra bytes are
// ignored. Returns false with `error` set if the data is too short or the size is invalid.
bool decode_pc98_pic(std::span<const std::uint8_t> data, int width, int height, Pc98Pic& out,
                     std::string* error = nullptr);
// The same with the size looked up by file name.
bool decode_pc98_pic(std::string_view name, std::span<const std::uint8_t> data, Pc98Pic& out,
                     std::string* error = nullptr);

// A palette entry: 4-bit levels (0-15) per channel.
struct Pc98Color {
    std::uint8_t r, g, b;
};
using Pc98Palette = std::array<Pc98Color, 16>;

// The palette the PC-98 VETTE.EXE (1.02J) sets for all its screens.
extern const Pc98Palette kPc98Palette;

// Finds the palette table in a PC-98 VETTE.EXE: the routine that writes it to the palette ports
// (A8h index, ACh red, AAh green, AEh blue) is followed by the table. Returns false if not found.
bool read_pc98_palette(std::span<const std::uint8_t> exe, Pc98Palette& out);

// 0xRRGGBB, each 4-bit level expanded to 8 bits (x * 17).
std::uint32_t pc98_rgb(Pc98Color c);

}  // namespace vette::assets
