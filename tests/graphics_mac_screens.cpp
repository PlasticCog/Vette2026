// Graphics option, the state-driven screens: the game state read from memory, the DOS font, the
// dashboards read from the program image (front and side views), the course map's recognition and
// the Mac dashboard and map drawn from the state (with the player's Mac files when present).

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "graphics/art_files.h"
#include "graphics/composite.h"
#include "graphics/dos_art.h"
#include "graphics/game_state.h"
#include "graphics/substitution.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace vette::graphics;

namespace {

constexpr std::array<std::uint32_t, 16> kEga = {0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA,
                                                0xAA5500, 0xAAAAAA, 0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
                                                0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};

std::vector<std::uint8_t> to_planes(const std::vector<std::uint8_t>& px, int w, int h) {
    const std::size_t plane = static_cast<std::size_t>(w / 8) * h;
    std::vector<std::uint8_t> out(plane * 4, 0);
    for (std::size_t i = 0; i < px.size(); ++i)
        for (int p = 0; p < 4; ++p)
            if (px[i] >> p & 1) out[p * plane + i / 8] |= static_cast<std::uint8_t>(0x80 >> (i % 8));
    return out;
}

std::vector<std::uint8_t> rle(const std::vector<std::uint8_t>& in) {
    std::vector<std::uint8_t> out;
    for (const std::uint8_t b : in) {
        if (b >= 0xC0) out.push_back(0xC1);
        out.push_back(b);
    }
    return out;
}

std::vector<std::uint8_t> pattern(int w, int h, int salt) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) px[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint8_t>((x / 3 + y / 2 + salt) % 16);
    return px;
}

// A one-colour PICT (an 8-bit pixmap) of w x h.
std::vector<std::uint8_t> solid_pict(int w, int h, std::uint32_t rgb) {
    std::vector<std::uint8_t> v;
    const auto u8 = [&](int b) { v.push_back(static_cast<std::uint8_t>(b)); };
    const auto u16 = [&](int x) { u8(x >> 8); u8(x); };
    const auto u32 = [&](std::uint32_t x) { u16(static_cast<int>(x >> 16)); u16(static_cast<int>(x & 0xFFFF)); };
    const auto rect = [&](int t, int l, int b, int r) { u16(t); u16(l); u16(b); u16(r); };
    u16(0);
    rect(0, 0, h, w);
    u16(0x0011); u16(0x02FF); u16(0x0C00);
    for (int i = 0; i < 6; ++i) u32(0);
    const int row_bytes = std::max(w, 8);
    u16(0x0098); u16(0x8000 | row_bytes); rect(0, 0, h, w);
    u16(0); u16(0); u32(0); u32(0x00480000); u32(0x00480000); u16(0); u16(8); u16(1); u16(8); u32(0); u32(0); u32(0);
    u32(0); u16(0); u16(0);
    u16(0); u16(static_cast<int>((rgb >> 16 & 255) * 257)); u16(static_cast<int>((rgb >> 8 & 255) * 257)); u16(static_cast<int>((rgb & 255) * 257));
    rect(0, 0, h, w); rect(0, 0, h, w); u16(0);
    for (int y = 0; y < h; ++y) {
        // PackBits rows of zeros (runs of up to 128 bytes).
        std::vector<std::uint8_t> row;
        for (int left = row_bytes; left > 0; left -= 128) {
            const int n = std::min(left, 128);
            row.push_back(static_cast<std::uint8_t>(1 - n));
            row.push_back(0);
        }
        if (row_bytes > 250) u16(static_cast<int>(row.size()));
        else u8(static_cast<int>(row.size()));
        v.insert(v.end(), row.begin(), row.end());
    }
    if (v.size() & 1) u8(0);
    u16(0x00FF);
    return v;
}

void put16(std::vector<std::uint8_t>& ram, std::uint32_t at, int value) {
    ram[at] = static_cast<std::uint8_t>(value);
    ram[at + 1] = static_cast<std::uint8_t>(value >> 8);
}

fs::path repo_root() { return fs::path(__FILE__).parent_path().parent_path(); }

}  // namespace

TEST(graphics_game_state_from_memory) {
    std::vector<std::uint8_t> ram(0x100000, 0);
    CHECK_EQ(read_dash_state(nullptr).speed_mph, 0);
    put16(ram, kGameDs + 0x3AA1, 43);
    put16(ram, kGameCs + 0x588F, 53);
    put16(ram, kGameDs + 0x2D45, 3);
    ram[kGameDs + 0x2D47] = 6;
    ram[kGameDs + 0x2D50] = 1;
    ram[kGameDs + 0x2AD7] = 0xFF;
    put16(ram, kGameDs + 0x2B84, -5);
    put16(ram, kGameDs + 0x2B87, 0x55);
    const DashState s = read_dash_state(ram.data());
    CHECK_EQ(s.speed_mph, 43);
    CHECK_EQ(s.rpm100, 53);
    CHECK_EQ(s.gear, 3);
    CHECK_EQ(s.max_gear, 6);
    CHECK(s.automatic && s.cruise && !s.shift_up);
    CHECK_EQ(s.steering, -5);
    CHECK_EQ(s.view, 85);
    CHECK_EQ(read_map_course(ram.data()), 0);
    put16(ram, kGameDs + 0xFD10, 3);
    CHECK_EQ(read_map_course(ram.data()), 3);
    put16(ram, kGameDs + 0xFD10, 9);
    CHECK_EQ(read_map_course(ram.data()), 0);
}

TEST(graphics_dos_font_text) {
    std::vector<std::uint8_t> ram(0x100000, 0);
    CHECK(!read_dos_font(ram.data()).valid);  // not loaded yet
    for (int c = 1; c < 96; ++c)
        for (int r = 0; r < 10; ++r) ram[kGameDs + 0xF3DE + static_cast<std::uint32_t>(c * 10 + r)] = r == 0 ? 0x81 : 0x18;
    const DosFont font = read_dos_font(ram.data());
    CHECK(font.valid);
    Image img;
    img.width = 60;
    img.height = 30;
    img.pixels.assign(60 * 30, 0);
    CHECK_EQ(draw_text(img, font, "A B", 2, 3, 2, 0xFF112233), 2 + 3 * 16);
    CHECK_EQ(img.at(2, 3), 0xFF112233u);    // 'A' row 0, leftmost bit, 2x2
    CHECK_EQ(img.at(3, 4), 0xFF112233u);
    CHECK_EQ(img.at(4, 3), 0u);
    CHECK_EQ(img.at(2 + 16, 3), 0u);        // the space
    CHECK_EQ(img.at(2 + 32 + 6, 5), 0xFF112233u);  // 'B' row 1: bits 3-4
}

TEST(graphics_side_view_from_program_image) {
    // A dashboard picture packed into the program image, as VETTE.EXE has the look-left one at A226h.
    std::vector<std::uint8_t> ram(0x100000, 0);
    const auto pic = pattern(320, 80, 5);
    const auto packed = rle(to_planes(pic, 320, 80));
    std::copy(packed.begin(), packed.end(), ram.begin() + kProgramImage + 0xA226);
    ArtFiles files;
    files.dos_file = [](const std::string&) { return std::vector<std::uint8_t>{}; };
    files.pc98_file = files.dos_file;
    files.mac_pict = [](std::int16_t id) { return id == 1091 ? solid_pict(512, 175, 0x804020) : std::vector<std::uint8_t>{}; };
    Substitution sub(Art::Mac, files);
    sub.set_program_memory(ram.data());
    std::vector<std::uint8_t> frame(320 * 200, 3);
    std::copy(pic.begin(), pic.end(), frame.begin() + 320 * 120);
    Composite c;
    CHECK(sub.compose({frame.data(), 320, 200, &kEga}, c));
    CHECK(sub.found().size() == 1 && sub.found()[0].screen == Screen::DashLeft);
    // 512x175 at the frame's width, standing on the bottom edge: 91.1 rows of the 4:3 picture.
    CHECK(c.layers.size() == 1 && c.layers[0].dst.w == 320.0f);
    if (!c.layers.empty()) CHECK(c.layers[0].dst.h > 91.0f && c.layers[0].dst.h < 91.2f && c.layers[0].dst.y + c.layers[0].dst.h == 200.0f);
    CHECK_EQ(c.base[0], 3);  // the 3D view above stays
    // A change to the program image (the game unpacking itself) is noticed: the old picture isn't kept.
    ram[kProgramImage + 0xA226] ^= 0x01;
    CHECK(!sub.compose({frame.data(), 320, 200, &kEga}, c) || sub.found().empty() || sub.found()[0].match < 1.0f);
}

TEST(graphics_course_map_recognised_through_highlights) {
    const auto map = pattern(640, 200, 2);
    ArtFiles files;
    const auto bin = rle(to_planes(map, 640, 200));
    files.dos_file = [bin](const std::string& name) { return name == "MAPPIC.BIN" ? bin : std::vector<std::uint8_t>{}; };
    files.pc98_file = [](const std::string&) { return std::vector<std::uint8_t>{}; };
    files.mac_pict = [](std::int16_t id) { return id == 26478 ? solid_pict(512, 322, 0x2050A0) : std::vector<std::uint8_t>{}; };
    Substitution sub(Art::Mac, files);
    // The DOS screen: text panels over most of the picture, the overview map top right with the
    // other courses' parts XORed with colour 1.
    std::vector<std::uint8_t> frame(640 * 200, 15);
    for (int y = 0; y < 60; ++y)
        for (int x = 480; x < 640; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * 640 + x;
            frame[i] = static_cast<std::uint8_t>(x < 560 ? map[i] ^ 1 : map[i]);
        }
    Composite c;
    CHECK(sub.compose({frame.data(), 640, 200, &kEga}, c));
    CHECK(sub.found().size() == 1 && sub.found()[0].screen == Screen::CourseMap);
    // Nothing of the DOS screen stays over the Mac map.
    int kept = 0;
    for (const auto v : c.over) kept += v != kTransparent;
    CHECK_EQ(kept, 0);
}

TEST(graphics_mac_dash_and_map_from_state) {
    // The player's Mac files: Game/Mac (any form), or the extracted copy in the repository.
    ArtFiles files = ArtFiles::from_game_dir(repo_root() / "Game");
    const fs::path extracted = repo_root() / "Vette_Mac_EN" / "extracted" / "VETTE! Folder" / "(Folder) Color VETTE!" /
                               "Color VETTE!.rsrc";
    std::error_code ec;
    if (files.mac_pict(24055).empty() && fs::exists(extracted, ec))
        files.mac_pict = ArtFiles::from_folders({}, {}, extracted).mac_pict;
    if (files.mac_pict(24055).empty()) {
        std::printf("  (skipped: no Mac Color VETTE! files)\n");
        return;
    }
    std::vector<std::uint8_t> ram(0x100000, 0);
    // A stand-in front dashboard in the program image.
    const auto dash = pattern(320, 80, 9);
    const auto packed = rle(to_planes(dash, 320, 80));
    std::copy(packed.begin(), packed.end(), ram.begin() + kProgramImage + 0x100);
    put16(ram, kGameDs + 0x3AA1, 125);  // three digits
    put16(ram, kGameCs + 0x588F, 7);    // two, with a leading zero
    put16(ram, kGameDs + 0x2D45, 0);
    ram[kGameDs + 0x2D47] = 5;
    Substitution sub(Art::Mac, files);
    for (const auto& w : sub.warnings()) std::printf("  %s\n", w.c_str());
    sub.set_program_memory(ram.data());
    std::vector<std::uint8_t> frame(320 * 200, 3);
    std::copy(dash.begin(), dash.end(), frame.begin() + 320 * 120);
    Composite c;
    CHECK(sub.compose({frame.data(), 320, 200, &kEga}, c));
    CHECK(sub.found().size() == 1 && sub.found()[0].screen == Screen::Dash);
    // The dashboard, the hand, the two arcs, 3 + 2 digits and the shift light (off); no shifter, since
    // the DOS frame doesn't show one; nothing of the DOS dashboard on top.
    CHECK_EQ(c.layers.size(), std::size_t{1 + 1 + 2 + 5 + 1});
    for (int y = 120; y < 200; ++y)
        for (int x = 0; x < 320; ++x) CHECK(c.over[static_cast<std::size_t>(y) * 320 + x] == kTransparent);
    // Shifter shown: the DOS frame has something drawn bottom right.
    for (int y = 172; y < 198; ++y)
        for (int x = 236; x < 316; ++x) frame[static_cast<std::size_t>(y) * 320 + x] = static_cast<std::uint8_t>((dash[static_cast<std::size_t>(y - 120) * 320 + x] + 1) & 15);
    CHECK(sub.compose({frame.data(), 320, 200, &kEga}, c));
    CHECK_EQ(c.layers.size(), std::size_t{1 + 1 + 2 + 5 + 1 + 2});

    // The course map, course 2: the route and the box of course 2 and the button frame.
    const auto mappic = files.dos_file("MAPPIC.BIN");
    if (mappic.empty()) return;
    DosPicture map;
    CHECK(decode_dos_picture(mappic, 0, 640, 200, map));
    put16(ram, kGameDs + 0xFD10, 2);
    CHECK(sub.compose({map.pixels.data(), 640, 200, &kEga}, c));
    CHECK(!sub.found().empty() && sub.found()[0].screen == Screen::CourseMap);
    CHECK_EQ(c.layers.size(), std::size_t{4});
    if (c.layers.size() == 4) {
        CHECK_EQ(c.layers[1].image->width, 337);  // course 2's route, PICT 4358
        CHECK_EQ(c.layers[2].image->width, 193);  // its box (no font in this memory: without text)
    }
}
