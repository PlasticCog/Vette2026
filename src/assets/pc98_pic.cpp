#include "assets/pc98_pic.h"

#include <algorithm>
#include <cctype>
#include <iterator>

namespace vette::assets {

namespace {

// Sizes from the PC-98 VETTE.EXE's blit calls (0000:8AF8: BH = bytes per row in the DOS version's
// 320-pixel units, which the code doubles; BP = rows) and from the files matching the DOS ones.
constexpr Pc98PicInfo kPics[] = {
    {"TITLE.PIC", 640, 200, "title screen (same pixels as DOS TITLE.BIN)"},
    {"GARAGE.PIC", 640, 200, "garage (DOS GARAGE.BIN with \"NORMAL\" for \"STOCK\" and the graph grid drawn in)"},
    {"HIGHSC.PIC", 640, 200, "high scores (same as DOS)"},
    {"MAPPIC.PIC", 640, 200, "course map (same as DOS)"},
    {"WINNER.PIC", 640, 200, "winner (same as DOS)"},
    {"EGAPIC.PIC", 640, 200, "opponent selection, at twice the DOS EGAPIC.BIN's 320 width"},
    {"DASH.PIC", 640, 80, "race dashboard, twice the DOS dash's width"},
    {"CRASH0.PIC", 352, 128, "crash picture (DOS 176x128)"},
    {"CRASH1.PIC", 352, 128, "crash picture (DOS 176x128)"},
    {"LOSER0.PIC", 352, 128, "lost to the Porsche (DOS 176x128)"},
    {"LOSER1.PIC", 352, 128, "lost to the Lamborghini (DOS 176x128)"},
    {"LOSER2.PIC", 352, 128, "lost to the Testarossa (DOS 176x128)"},
    {"LOSER3.PIC", 352, 128, "lost to the F40 (DOS 176x128)"},
    {"PENALTY.PIC", 208, 121, "notice to appear (same as DOS)"},
    {"EGASKILL.PIC", 160, 99, "skill level licence plates (same as DOS)"},
    {"HORIZON0.PIC", 3200, 24, "horizon panorama (same as DOS)"},
    {"HORIZON1.PIC", 3200, 24, "horizon panorama (same as DOS)"},
    {"HORIZON2.PIC", 3200, 24, "horizon panorama (same as DOS)"},
};

bool iequal(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::toupper(static_cast<unsigned char>(x)) == std::toupper(static_cast<unsigned char>(y));
           });
}

}  // namespace

const Pc98Palette kPc98Palette = {{
    {0, 0, 0},   {0, 0, 11},  {0, 11, 0},  {0, 11, 11},  {11, 0, 0},  {11, 0, 11},  {11, 11, 0},  {11, 11, 11},
    {4, 4, 4},   {4, 4, 15},  {4, 15, 4},  {4, 15, 15},  {15, 4, 4},  {15, 4, 15},  {15, 15, 4},  {15, 15, 15},
}};

const Pc98PicInfo* pc98_pic_info(std::string_view name) {
    for (const auto& p : kPics)
        if (iequal(p.name, name)) return &p;
    return nullptr;
}

std::span<const Pc98PicInfo> pc98_pics() { return kPics; }

bool decode_pc98_pic(std::span<const std::uint8_t> data, int width, int height, Pc98Pic& out, std::string* error) {
    if (width <= 0 || height <= 0 || width % 8 != 0 || width > 8192 || height > 8192) {
        if (error) *error = "invalid picture size";
        return false;
    }
    const std::size_t plane = static_cast<std::size_t>(width / 8) * static_cast<std::size_t>(height);
    if (data.size() < plane * 4) {
        if (error) *error = "picture needs " + std::to_string(plane * 4) + " bytes, got " + std::to_string(data.size());
        return false;
    }
    out.width = width;
    out.height = height;
    out.pixels.assign(plane * 8, 0);
    for (std::size_t i = 0; i < plane; ++i) {
        const unsigned b0 = data[i], b1 = data[plane + i], b2 = data[2 * plane + i], b3 = data[3 * plane + i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            const unsigned s = 7 - bit;
            out.pixels[i * 8 + bit] =
                static_cast<std::uint8_t>(((b0 >> s) & 1) | ((b1 >> s) & 1) << 1 | ((b2 >> s) & 1) << 2 | ((b3 >> s) & 1) << 3);
        }
    }
    return true;
}

bool decode_pc98_pic(std::string_view name, std::span<const std::uint8_t> data, Pc98Pic& out, std::string* error) {
    const Pc98PicInfo* info = pc98_pic_info(name);
    if (!info) {
        if (error) *error = "unknown picture " + std::string(name);
        return false;
    }
    return decode_pc98_pic(data, info->width, info->height, out, error);
}

bool read_pc98_palette(std::span<const std::uint8_t> exe, Pc98Palette& out) {
    // mov al,ah / out A8h,al / lodsb / out ACh,al / lodsb / out AAh,al / lodsb / out AEh,al /
    // inc ah / loop / ret, then the 48-byte table.
    static constexpr std::uint8_t kCode[] = {0x8A, 0xC4, 0xE6, 0xA8, 0xAC, 0xE6, 0xAC, 0xAC, 0xE6,
                                             0xAA, 0xAC, 0xE6, 0xAE, 0xFE, 0xC4, 0xE2, 0xEF, 0xC3};
    const auto it = std::search(exe.begin(), exe.end(), std::begin(kCode), std::end(kCode));
    if (it == exe.end()) return false;
    const auto at = static_cast<std::size_t>(it - exe.begin()) + sizeof kCode;
    if (exe.size() - at < 48) return false;
    Pc98Palette p{};
    for (std::size_t i = 0; i < 16; ++i) {
        p[i] = {exe[at + 3 * i], exe[at + 3 * i + 1], exe[at + 3 * i + 2]};
        if (p[i].r > 15 || p[i].g > 15 || p[i].b > 15) return false;
    }
    out = p;
    return true;
}

std::uint32_t pc98_rgb(Pc98Color c) {
    return static_cast<std::uint32_t>((c.r & 15) * 17) << 16 | static_cast<std::uint32_t>((c.g & 15) * 17) << 8 |
           static_cast<std::uint32_t>((c.b & 15) * 17);
}

}  // namespace vette::assets
