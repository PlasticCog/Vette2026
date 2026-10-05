// Graphics option: recognising DOS pictures in a frame, the composite (art, DOS pixels on top,
// remapped regions), the reference renderer, and the player's real art sets when present.

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "graphics/art_files.h"
#include "graphics/composite.h"
#include "graphics/dos_art.h"
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

// The DOS games's RLE: bytes from C0h up need a run prefix.
std::vector<std::uint8_t> rle(const std::vector<std::uint8_t>& in) {
    std::vector<std::uint8_t> out;
    for (const std::uint8_t b : in) {
        if (b >= 0xC0) out.push_back(0xC1);
        out.push_back(b);
    }
    return out;
}

// A busy picture: every colour, in diagonal bands.
std::vector<std::uint8_t> pattern(int w, int h, int salt) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) px[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint8_t>((x / 3 + y / 2 + salt) % 16);
    return px;
}

// A v2 PICT of one colour (an 8-bit pixmap), as the Mac art.
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
    u16(0x0098); u16(0x8000 | w); rect(0, 0, h, w);
    u16(0); u16(0); u32(0); u32(0x00480000); u32(0x00480000); u16(0); u16(8); u16(1); u16(8); u32(0); u32(0); u32(0);
    u32(0); u16(0); u16(0);  // one colour
    u16(0); u16(static_cast<int>((rgb >> 16 & 255) * 257)); u16(static_cast<int>((rgb >> 8 & 255) * 257)); u16(static_cast<int>((rgb & 255) * 257));
    rect(0, 0, h, w); rect(0, 0, h, w); u16(0);
    for (int y = 0; y < h; ++y) { u8(2); u8(1 - w); u8(0); }  // a run of w zeros
    if (v.size() & 1) u8(0);
    u16(0x00FF);
    return v;
}

struct Fake {
    std::vector<std::uint8_t> title = pattern(640, 200, 0);
    std::vector<std::uint8_t> crash = pattern(176, 128, 7);
    ArtFiles files() const {
        ArtFiles f;
        const auto title_bin = rle(to_planes(title, 640, 200));
        const auto crash_bin = rle(to_planes(crash, 176, 128));
        f.dos_file = [=](const std::string& name) {
            return name == "TITLE.BIN" ? title_bin : name == "CRASH0.BIN" ? crash_bin : std::vector<std::uint8_t>{};
        };
        f.pc98_file = [](const std::string& name) {
            if (name == "TITLE.PIC") return to_planes(std::vector<std::uint8_t>(640 * 200, 3), 640, 200);
            if (name == "CRASH0.PIC") return to_planes(std::vector<std::uint8_t>(352 * 128, 5), 352, 128);
            return std::vector<std::uint8_t>{};
        };
        f.mac_pict = [](std::int16_t id) { return id == 24592 ? solid_pict(64, 40, 0x336699) : std::vector<std::uint8_t>{}; };
        return f;
    }
};

fs::path repo_root() { return fs::path(__FILE__).parent_path().parent_path(); }

}  // namespace

TEST(graphics_dos_pictures) {
    const auto px = pattern(16, 3, 1);
    std::vector<std::uint8_t> file = {0x12, 0x34};  // EGAPIC.BIN-style size word
    const auto packed = rle(to_planes(px, 16, 3));
    file.insert(file.end(), packed.begin(), packed.end());
    DosPicture pic;
    CHECK(decode_dos_picture(file, 2, 16, 3, pic));
    CHECK(pic.pixels == px);
    CHECK(!decode_dos_picture(std::vector<std::uint8_t>(file.begin(), file.end() - 5), 2, 16, 3, pic));
    // The dashboard is packed into the program image at 100h.
    std::vector<std::uint8_t> image(0x100, 0);
    const auto dash = pattern(kDashWidth, kDashHeight, 2);
    const auto dash_packed = rle(to_planes(dash, kDashWidth, kDashHeight));
    image.insert(image.end(), dash_packed.begin(), dash_packed.end());
    CHECK(decode_dos_dash(image, pic));
    CHECK(pic.pixels == dash);
}

TEST(graphics_pc98_title_with_dos_text_on_top) {
    const Fake fake;
    Substitution sub(Art::Pc98, fake.files());
    CHECK(!sub.available().empty());
    CHECK(!sub.warnings().empty());  // the screens whose files are missing
    auto frame = fake.title;
    for (int y = 50; y < 58; ++y)
        for (int x = 100; x < 110; ++x) frame[static_cast<std::size_t>(y) * 640 + x] = 15;  // text the game drew
    const FrameView view{frame.data(), 640, 200, &kEga};
    Composite c;
    CHECK(sub.compose(view, c));
    CHECK(sub.found().size() == 1 && sub.found()[0].screen == Screen::Title);
    CHECK(c.base.empty());
    CHECK_EQ(c.layers.size(), std::size_t{1});
    CHECK(c.layers[0].dst.x == 0 && c.layers[0].dst.w == 640 && c.layers[0].dst.h == 200);  // PC-98: same layout
    CHECK_EQ(c.layers[0].image->at(0, 0), 0xFF00BBBBu);  // index 3 in the PC-98 palette
    // Only the changed pixels are kept, where the DOS picture didn't already have colour 15.
    int kept = 0, wrong = 0;
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 640; ++x) {
            const auto v = c.over[static_cast<std::size_t>(y) * 640 + x];
            const bool text = x >= 100 && x < 110 && y >= 50 && y < 58 && fake.title[static_cast<std::size_t>(y) * 640 + x] != 15;
            kept += v != kTransparent;
            wrong += (v != kTransparent) != text;
        }
    CHECK(kept > 0);
    CHECK_EQ(wrong, 0);
    CHECK_EQ(c.palette[7], 0xBBBBBBu);  // EGA colours become the PC-98's
}

TEST(graphics_unrecognised_frames) {
    const Fake fake;
    std::vector<std::uint8_t> frame(640 * 200, 5);
    const FrameView view{frame.data(), 640, 200, &kEga};
    Composite c;
    Substitution pc98(Art::Pc98, fake.files());
    CHECK(pc98.compose(view, c));  // nothing recognised, but PC-98 colours
    CHECK(c.layers.empty());
    CHECK_EQ(c.base.size(), frame.size());
    Substitution mac(Art::Mac, fake.files());
    CHECK(!mac.compose(view, c));
    Substitution dos(Art::Dos, fake.files());
    CHECK(!dos.compose(view, c));
    CHECK(dos.available().empty());
}

TEST(graphics_mac_title_is_letterboxed) {
    const Fake fake;
    Substitution sub(Art::Mac, fake.files());
    const FrameView view{fake.title.data(), 640, 200, &kEga};
    Composite c;
    CHECK(sub.compose(view, c));
    CHECK_EQ(c.layers.size(), std::size_t{1});
    // 64x40 (1.6:1) inside a 4:3 picture: full width, 5/6 of the height, centred.
    const FRect d = c.layers[0].dst;
    CHECK(d.x == 0 && d.w == 640);
    CHECK(d.h > 166.5f && d.h < 166.8f && d.y > 16.5f && d.y < 16.8f);
    // The DOS copyright line (kept below the art) is the only DOS content left on top.
    CHECK(c.over[195 * 640 + 10] != kTransparent);
    CHECK(c.over[100 * 640 + 10] == kTransparent);
    // Reference render: the art's colour in the middle, the backdrop in the letterbox.
    const Image img = render(c, 320, 240);
    CHECK_EQ(img.at(160, 120), 0xFF336699u);
    CHECK_EQ(img.at(160, 2), 0xFF000000u);
}

TEST(graphics_hysteresis) {
    const Fake fake;
    Substitution sub(Art::Pc98, fake.files());
    auto frame = fake.title;
    const FrameView view{frame.data(), 640, 200, &kEga};
    Composite c;
    // 45% of the picture showing: not enough to recognise it...
    for (std::size_t i = 0; i < frame.size() * 55 / 100; ++i) frame[i] = static_cast<std::uint8_t>((fake.title[i] + 1) % 16);
    CHECK(sub.compose(view, c));
    CHECK(sub.found().empty());
    // ...but enough to keep it once recognised (a dissolve or a menu drawn over it).
    frame = fake.title;
    CHECK(sub.compose(view, c));
    CHECK(!sub.found().empty());
    for (std::size_t i = 0; i < frame.size() * 55 / 100; ++i) frame[i] = static_cast<std::uint8_t>((fake.title[i] + 1) % 16);
    CHECK(sub.compose(view, c));
    CHECK(!sub.found().empty());
}

TEST(graphics_inset_found_where_drawn) {
    const Fake fake;
    Substitution sub(Art::Pc98, fake.files());
    std::vector<std::uint8_t> frame(320 * 200, 9);
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 176; ++x)
            frame[static_cast<std::size_t>(40 + y) * 320 + 64 + x] = fake.crash[static_cast<std::size_t>(y) * 176 + x];
    const FrameView view{frame.data(), 320, 200, &kEga};
    Composite c;
    CHECK(sub.compose(view, c));
    CHECK_EQ(sub.found().size(), std::size_t{1});
    if (!sub.found().empty()) {
        CHECK(sub.found()[0].screen == Screen::Crash0);
        CHECK_EQ(sub.found()[0].rect.x, 64);
        CHECK_EQ(sub.found()[0].rect.y, 40);
    }
    // The rest of the frame stays (the race view), the inset's area is the art's.
    CHECK_EQ(c.base.size(), frame.size());
    CHECK_EQ(c.base[0], 9);
    CHECK_EQ(c.base[static_cast<std::size_t>(100) * 320 + 100], kTransparent);
    CHECK(c.layers.size() == 1 && c.layers[0].dst.x == 64 && c.layers[0].dst.w == 176);
}

TEST(graphics_render_layers_and_pieces) {
    Image art;
    art.width = 2;
    art.height = 2;
    art.pixels.assign(4, 0xFF102030);
    Composite c;
    c.frame_w = 4;
    c.frame_h = 4;
    c.palette = kEga;
    c.layers.push_back({&art, {0, 0, 2, 2}, {0, 0, 4, 4}});
    c.over.assign(16, kTransparent);
    c.over[0] = 15;
    c.moved.assign(16, kTransparent);
    c.moved[5] = 12;  // frame pixel (1,1), drawn at (3,3)
    c.pieces.push_back({{1, 1, 1, 1}, {3, 3, 1, 1}});
    const Image img = render(c, 8, 8);
    CHECK_EQ(img.at(0, 0), 0xFFFFFFFFu);   // over
    CHECK_EQ(img.at(4, 4), 0xFF102030u);   // art
    CHECK_EQ(img.at(7, 7), 0xFFFF5555u);   // the moved piece
    CHECK_EQ(img.at(2, 2), 0xFF102030u);   // its source isn't drawn in place
}

TEST(graphics_art_files_ignore_case) {
    const fs::path dir = fs::temp_directory_path() / "vette_graphics_test";
    fs::create_directories(dir);
    {
        std::ofstream(dir / "Title.Bin", std::ios::binary) << "abc";
    }
    const auto files = ArtFiles::from_folders(dir, dir, dir / "missing.rsrc");
    CHECK_EQ(files.dos_file("TITLE.BIN").size(), std::size_t{3});
    CHECK(files.pc98_file("NONE.PIC").empty());
    CHECK(files.mac_pict(24592).empty());
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(graphics_real_art_sets) {
    const fs::path game = repo_root() / "Game";
    std::error_code ec;
    if (!fs::exists(game / "TITLE.BIN", ec)) {
        std::printf("  (skipped: no DOS game files)\n");
        return;
    }
    const auto files = ArtFiles::from_game_dir(game);
    std::vector<std::uint8_t> title_bin;
    {
        std::ifstream f(game / "TITLE.BIN", std::ios::binary);
        title_bin.assign(std::istreambuf_iterator<char>(f), {});
    }
    DosPicture title;
    CHECK(decode_dos_picture(title_bin, 0, 640, 200, title));
    for (const Art art : {Art::Pc98, Art::Mac}) {
        Substitution sub(art, files);
        if (sub.available().empty()) {
            std::printf("  (%s art not present)\n", art_name(art));
            continue;
        }
        for (const auto& w : sub.warnings()) std::printf("  %s: %s\n", art_name(art), w.c_str());
        CHECK_EQ(sub.available().size(), static_cast<std::size_t>(Screen::Count) - 1);
        CHECK(sub.warnings().empty());
        const FrameView view{title.pixels.data(), 640, 200, &kEga};
        Composite c;
        CHECK(sub.compose(view, c));
        CHECK(!sub.found().empty() && sub.found()[0].screen == Screen::Title && sub.found()[0].match == 1.0f);
    }
}
