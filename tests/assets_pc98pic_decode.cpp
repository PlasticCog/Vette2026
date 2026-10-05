// PC-98 .PIC pictures and palette: synthetic data, and the player's PC-98 files when present (whose
// full-screen pictures must equal the DOS ones).

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "assets/pc98_disk.h"
#include "assets/pc98_pic.h"
#include "graphics/dos_art.h"
#include "test.h"

namespace fs = std::filesystem;
using vette::assets::decode_pc98_pic;
using vette::assets::Pc98Pic;

namespace {

// Plane-sequential 4-bit planar bytes for a width x height picture of indices.
std::vector<std::uint8_t> to_planes(const std::vector<std::uint8_t>& px, int w, int h) {
    const std::size_t plane = static_cast<std::size_t>(w / 8) * h;
    std::vector<std::uint8_t> out(plane * 4, 0);
    for (std::size_t i = 0; i < px.size(); ++i)
        for (int p = 0; p < 4; ++p)
            if (px[i] >> p & 1) out[p * plane + i / 8] |= static_cast<std::uint8_t>(0x80 >> (i % 8));
    return out;
}

fs::path repo_root() { return fs::path(__FILE__).parent_path().parent_path(); }

std::vector<std::uint8_t> read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return f ? std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {}) : std::vector<std::uint8_t>{};
}

}  // namespace

TEST(assets_pc98pic_planes) {
    std::vector<std::uint8_t> px(16 * 2);
    for (std::size_t i = 0; i < px.size(); ++i) px[i] = static_cast<std::uint8_t>(i % 16);
    Pc98Pic pic;
    CHECK(decode_pc98_pic(to_planes(px, 16, 2), 16, 2, pic));
    CHECK_EQ(pic.width, 16);
    CHECK(pic.pixels == px);
    std::string err;
    CHECK(!decode_pc98_pic(std::vector<std::uint8_t>(15), 16, 2, pic, &err));  // too short
    CHECK(!err.empty());
    CHECK(!decode_pc98_pic(std::vector<std::uint8_t>(64), 12, 2, pic));  // width not a multiple of 8
}

TEST(assets_pc98pic_sizes_by_name) {
    const auto* title = vette::assets::pc98_pic_info("title.pic");
    CHECK(title != nullptr);
    if (title) {
        CHECK_EQ(title->width, 640);
        CHECK_EQ(title->height, 200);
    }
    const auto* dash = vette::assets::pc98_pic_info("DASH.PIC");
    CHECK(dash && dash->width == 640 && dash->height == 80);
    CHECK(vette::assets::pc98_pic_info("SPEED.PIC") == nullptr);  // layout not known yet
    Pc98Pic pic;
    CHECK(!decode_pc98_pic("NOPE.PIC", std::vector<std::uint8_t>(100), pic));
}

TEST(assets_pc98pic_palette) {
    // The palette routine's tail (out A8h..AEh loop, ret) followed by the table.
    std::vector<std::uint8_t> exe(100, 0x90);
    const std::uint8_t code[] = {0x8A, 0xC4, 0xE6, 0xA8, 0xAC, 0xE6, 0xAC, 0xAC, 0xE6,
                                 0xAA, 0xAC, 0xE6, 0xAE, 0xFE, 0xC4, 0xE2, 0xEF, 0xC3};
    exe.insert(exe.end(), std::begin(code), std::end(code));
    for (int i = 0; i < 16; ++i) {
        exe.push_back(static_cast<std::uint8_t>(i));
        exe.push_back(static_cast<std::uint8_t>(15 - i));
        exe.push_back(7);
    }
    vette::assets::Pc98Palette pal{};
    CHECK(vette::assets::read_pc98_palette(exe, pal));
    CHECK_EQ(pal[3].r, 3);
    CHECK_EQ(pal[3].g, 12);
    CHECK_EQ(pal[15].b, 7);
    exe[exe.size() - 1] = 16;  // not a 4-bit level
    CHECK(!vette::assets::read_pc98_palette(exe, pal));
    CHECK(!vette::assets::read_pc98_palette(std::vector<std::uint8_t>(64, 0), pal));
    CHECK_EQ(vette::assets::pc98_rgb({15, 0, 11}), 0xFF00BBu);
}

TEST(assets_pc98pic_real_files_match_dos) {
    std::string error;
    const auto pc98 = vette::assets::Pc98Files::open(repo_root() / "Game" / "PC98", error);
    const auto dos_title = read(repo_root() / "Game" / "TITLE.BIN");
    if (!pc98 || dos_title.empty()) {
        std::printf("  (skipped: no PC-98 or DOS game files)\n");
        return;
    }
    // Every known picture decodes at its tabled size.
    for (const auto& info : vette::assets::pc98_pics()) {
        std::vector<std::uint8_t> data;
        if (!pc98->read(info.name, data)) continue;
        Pc98Pic pic;
        std::string err;
        CHECK(decode_pc98_pic(info.name, data, pic, &err));
        CHECK_EQ(data.size(), static_cast<std::size_t>(info.width) * info.height / 2);  // nothing left over
    }
    // The full-screen pictures are the DOS ones, unpacked.
    std::vector<std::uint8_t> data;
    CHECK(pc98->read("TITLE.PIC", data));
    Pc98Pic pic;
    CHECK(decode_pc98_pic("TITLE.PIC", data, pic));
    vette::graphics::DosPicture dos;
    CHECK(vette::graphics::decode_dos_picture(dos_title, 0, 640, 200, dos));
    CHECK(dos.pixels == pic.pixels);
    // The game's palette is the one tabled.
    std::vector<std::uint8_t> exe;
    CHECK(pc98->read("VETTE.EXE", exe));
    vette::assets::Pc98Palette pal{};
    CHECK(vette::assets::read_pc98_palette(exe, pal));
    for (std::size_t i = 0; i < 16; ++i)
        CHECK(pal[i].r == vette::assets::kPc98Palette[i].r && pal[i].g == vette::assets::kPc98Palette[i].g &&
              pal[i].b == vette::assets::kPc98Palette[i].b);
}
