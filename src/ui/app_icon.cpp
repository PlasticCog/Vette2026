#include "ui/app_icon.h"

#include <array>
#include <cstddef>
#include <string_view>

namespace vette::ui {
namespace {

// One letter per pixel, EGA colours.
constexpr std::uint32_t colour(char c) {
    switch (c) {
    case '.': return 0x55FFFF;  // light cyan: sky
    case 'w': return 0xFFFFFF;  // white
    case 'c': return 0xAAAAAA;  // light grey
    case 'k': return 0x555555;  // dark grey
    case 'R': return 0xAA0000;  // red
    case 'r': return 0xFF5555;  // light red
    case 'n': return 0xAA5500;  // brown
    case 'y': return 0xFFFF55;  // yellow
    case 'g': return 0x00AA00;  // green
    case 'G': return 0x55FF55;  // light green
    case 'b': return 0x0000AA;  // blue
    case 'B': return 0x5555FF;  // light blue
    default: return 0;          // not a colour (checked below)
    }
}

constexpr std::array<std::string_view, 32> kArt32 = {
    "..........................c.....",
    "..........................c.....",
    "...............www........c.....",
    "...R..Rwww...wwwwwww......c.....",
    "...R.wRwwwwwwwwwwwwwww...kc.....",
    "...RRRRRwwwwwwwwwwwwwww..kc.....",
    ".wRRwwRwRwwwwwwwwwwwwww..kc.....",
    "wRwRwwRwwRwwwwwwwwwww...kwcc....",
    ".RwRwwRw.Rwwwwwwww......kwcc....",
    "R..RRRRw..Rw.w.w.w......kwcc....",
    "R..R..R....R............kwcc....",
    "...R..R....R...........kwwccc...",
    "...R..R.....Rggyyg.....kwwccc...",
    "...RRRR..ggGRgyyyyggg..kwwccc...",
    "...R..RggggggyyyyyygRRgkwwccc...",
    "...R.gRgggggrrrrrrrryRkwwwccccBB",
    "RRRRRRRRRRRRRyyyyyyRRRnnwwRRRcwB",
    "nnnnnnnnnnnnRbRbbRbRyRynrryRRnBB",
    "BBBRbBRBBBbBRbRbbRbRRRnnwrRRRnwB",
    "BbBRBBRbBBBBRRRRRRRRRRnnrrRRRnBB",
    "cccccccccccykkckkckkyccccccccccc",
    "ccccccccccykkckyykckkycccccccccc",
    "cccccccccykkckkkkkkckkyccccccccc",
    "ccccccccykkkckkyykkckkkycccccccc",
    "cccccccykkkckkkyykkkckkkyccccccc",
    "ccccccykkkckkkkkkkkkkckkkycccccc",
    "ccccykkkkckkkkkyykkkkkckkkkycccc",
    "cccykkkkckkkkkkyykkkkkkckkkkyccc",
    "ccykkkkckkkkkkkyykkkkkkkckkkkycc",
    "cykkkkkckkkkkkkkkkkkkkkkckkkkkyc",
    "ykkkkkckkkkkkkkyykkkkkkkkckkkkky",
    "kkkkkckkkkkkkkkyykkkkkkkkkckkkkk",
};

constexpr std::array<std::string_view, 16> kArt16 = {
    "............c...",
    ".R.R.....ww.c...",
    ".RRR.wwwwww.c...",
    ".R.Rwwwwwwwkwc..",
    ".R.R.wwwwwwkwc..",
    ".RRR...ww..kwc..",
    ".R.R.ggyygkwwcc.",
    ".R.RggyyyykwwccB",
    "RRRRRRRbbRRnrRRB",
    "BBBBBBRRRRRnrRyB",
    "cccccykkkkyccccc",
    "ccccykkyykkycccc",
    "cccykkkkkkkkyccc",
    "cykkkkkyykkkkkyc",
    "ykkkkkkyykkkkkky",
    "kkkkkkkkkkkkkkkk",
};

template <size_t N>
constexpr bool well_formed(const std::array<std::string_view, N>& art) {
    for (const std::string_view row : art) {
        if (row.size() != N)
            return false;
        for (const char c : row) {
            if (colour(c) == 0)
                return false;
        }
    }
    return true;
}
static_assert(well_formed(kArt32) && well_formed(kArt16), "icon rows: one known colour letter per pixel");

}  // namespace

std::vector<std::uint32_t> app_icon(int size) {
    const bool large = size >= 32 && size % 32 == 0;
    const int n = large ? 32 : 16;
    const std::string_view* art = large ? kArt32.data() : kArt16.data();
    std::vector<std::uint32_t> pixels(static_cast<size_t>(size) * static_cast<size_t>(size));
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x)
            pixels[static_cast<size_t>(y) * size + x] = 0xFF000000 | colour(art[y * n / size][x * n / size]);
    }
    return pixels;
}

}  // namespace vette::ui
