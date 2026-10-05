// QuickDraw PICT decoder: synthetic pictures for each feature, malformed input, and the real Mac
// Color VETTE! pictures when the player's files are present.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "assets/mac_files.h"
#include "assets/pict.h"
#include "test.h"

namespace fs = std::filesystem;
using vette::assets::decode_pict;
using vette::assets::Pict;
using vette::assets::unpack_bits;

namespace {

struct Builder {
    std::vector<std::uint8_t> v;
    Builder& u8(int b) {
        v.push_back(static_cast<std::uint8_t>(b));
        return *this;
    }
    Builder& u16(int w) { return u8(w >> 8).u8(w); }
    Builder& u32(std::uint32_t d) { return u16(static_cast<int>(d >> 16)).u16(static_cast<int>(d & 0xFFFF)); }
    Builder& rect(int top, int left, int bottom, int right) { return u16(top).u16(left).u16(bottom).u16(right); }
    Builder& pad() {
        if (v.size() & 1) u8(0);
        return *this;
    }
};

// A version 2 picture holding one 8-bit PackBitsRect (rows stored as literal runs).
std::vector<std::uint8_t> pict_v2_8bit(int w, int h, const std::vector<std::uint8_t>& px,
                                       const std::vector<std::uint32_t>& clut, int mode = 0) {
    Builder b;
    b.u16(0).rect(0, 0, h, w);
    b.u16(0x0011).u16(0x02FF);
    b.u16(0x0C00).u32(0xFFFE0000).u32(0x00480000).u32(0x00480000).rect(0, 0, h, w).u32(0);
    b.u16(0x001E);
    b.u16(0x0001).u16(10).rect(0, 0, h, w);
    const int row_bytes = std::max(w, 8);
    b.u16(0x0098).u16(0x8000 | row_bytes).rect(0, 0, h, w);
    b.u16(0).u16(0).u32(0).u32(0x00480000).u32(0x00480000).u16(0).u16(8).u16(1).u16(8).u32(0).u32(0).u32(0);
    b.u32(0).u16(0).u16(static_cast<int>(clut.size()) - 1);
    for (std::size_t i = 0; i < clut.size(); ++i) {
        const std::uint32_t c = clut[i];
        b.u16(static_cast<int>(i)).u16(static_cast<int>((c >> 16 & 255) * 257)).u16(static_cast<int>((c >> 8 & 255) * 257)).u16(static_cast<int>((c & 255) * 257));
    }
    b.rect(0, 0, h, w).rect(0, 0, h, w).u16(mode);
    for (int y = 0; y < h; ++y) {
        b.u8(row_bytes + 1).u8(row_bytes - 1);  // count, then one literal run of the whole row
        for (int x = 0; x < row_bytes; ++x) b.u8(x < w ? px[static_cast<std::size_t>(y) * w + x] : 0);
    }
    b.pad().u16(0x00FF);
    return b.v;
}

fs::path repo_root() { return fs::path(__FILE__).parent_path().parent_path(); }

}  // namespace

TEST(assets_pict_unpack_bits) {
    std::uint8_t out[16] = {};
    const std::vector<std::uint8_t> in = {0x02, 1, 2, 3, 0xFE, 9, 0x80, 0x00, 7};
    CHECK_EQ(unpack_bits(in, out, 7), 9L);
    const std::uint8_t want[7] = {1, 2, 3, 9, 9, 9, 7};
    for (int i = 0; i < 7; ++i) CHECK_EQ(out[i], want[i]);
    CHECK_EQ(unpack_bits(std::vector<std::uint8_t>{0x05, 1, 2}, out, 6), -1L);  // literal run cut short
    CHECK_EQ(unpack_bits(std::vector<std::uint8_t>{0xFD}, out, 4), -1L);        // repeat without its byte
    const std::vector<std::uint8_t> words = {0xFF, 0xAB, 0xCD};                // 16-bit pixmap variant
    CHECK_EQ(unpack_bits(words, out, 4, 2), 3L);
    CHECK_EQ(out[2], 0xAB);
    CHECK_EQ(out[3], 0xCD);
}

TEST(assets_pict_v2_indexed_pixmap) {
    const int w = 8, h = 2;
    std::vector<std::uint8_t> px = {1, 1, 1, 1, 2, 2, 2, 2, 0, 1, 0, 1, 0, 2, 0, 2};
    Pict p;
    std::string err;
    CHECK(decode_pict(pict_v2_8bit(w, h, px, {0xFFFFFF, 0xFF0000, 0x0000FF}), p, &err));
    CHECK_EQ(p.version, 2);
    CHECK_EQ(p.width, 8);
    CHECK_EQ(p.height, 2);
    CHECK_EQ(p.pixels[0], 0xFFFF0000u);
    CHECK_EQ(p.pixels[4], 0xFF0000FFu);
    CHECK_EQ(p.pixels[8], 0xFFFFFFFFu);
    CHECK_EQ(p.pixels[13], 0xFF0000FFu);
    CHECK_EQ(p.skipped, 0);
}

TEST(assets_pict_srcor_leaves_white_undrawn) {
    std::vector<std::uint8_t> px(16, 0);
    px[3] = 1;
    Pict p;
    CHECK(decode_pict(pict_v2_8bit(8, 2, px, {0xFFFFFF, 0x000000}, 1), p));
    CHECK_EQ(p.pixels[3], 0xFF000000u);
    CHECK_EQ(p.pixels[0] >> 24, 0u);  // white under srcOr draws nothing
}

TEST(assets_pict_v1_bitmap_text_and_shapes) {
    Builder b;
    b.u16(0).rect(10, 20, 14, 36);  // frame 16x4 at (20,10)
    b.u8(0x11).u8(0x01);
    b.u8(0x90).u16(2).rect(10, 20, 12, 36);  // BitsRect, rowBytes 2: unpacked rows
    b.rect(10, 20, 12, 36).rect(10, 20, 12, 36).u16(1);  // srcOr
    b.u8(0xF0).u8(0x0F).u8(0x00).u8(0x81);
    b.u8(0x28).u16(13).u16(22).u8(2).u8('H').u8('i');  // LongText at (22,13)
    b.u8(0x09);
    for (int i = 0; i < 8; ++i) b.u8(0xFF);  // pen pattern: black
    b.u8(0x31).rect(13, 20, 14, 24);       // paintRect: 4x1 at (20,13)
    b.u8(0xFF);
    Pict p;
    std::string err;
    CHECK(decode_pict(b.v, p, &err));
    CHECK_EQ(p.version, 1);
    CHECK_EQ(p.width, 16);
    CHECK_EQ(p.frame_left, 20);
    CHECK_EQ(p.pixels[0], 0xFF000000u);       // set bit: black
    CHECK_EQ(p.pixels[4] >> 24, 0u);          // clear bit under srcOr: untouched
    CHECK_EQ(p.pixels[12], 0xFF000000u);
    CHECK_EQ(p.pixels[16 + 15], 0xFF000000u);  // second row, last bit
    CHECK_EQ(p.pixels[3 * 16 + 2], 0xFF000000u);  // the painted rectangle
    CHECK_EQ(p.pixels[3 * 16 + 5] >> 24, 0u);
    CHECK_EQ(p.texts.size(), std::size_t{1});
    if (!p.texts.empty()) {
        CHECK(p.texts[0].text == "Hi");
        CHECK_EQ(p.texts[0].x, 2);
        CHECK_EQ(p.texts[0].y, 3);
    }
}

TEST(assets_pict_file_header_is_skipped) {
    std::vector<std::uint8_t> file(512, 0);
    const auto pict = pict_v2_8bit(8, 1, std::vector<std::uint8_t>(8, 1), {0xFFFFFF, 0x00FF00});
    file.insert(file.end(), pict.begin(), pict.end());
    Pict p;
    CHECK(decode_pict(file, p));
    CHECK_EQ(p.pixels[7], 0xFF00FF00u);
}

TEST(assets_pict_malformed_input_fails_cleanly) {
    const auto good = pict_v2_8bit(8, 2, std::vector<std::uint8_t>(16, 1), {0xFFFFFF, 0x123456});
    Pict p;
    CHECK(decode_pict(good, p));
    // Every truncation fails (there is no OpEndPic) without reading past the end.
    for (std::size_t n = 0; n + 1 < good.size(); ++n)
        CHECK(!decode_pict(std::span<const std::uint8_t>(good.data(), n), p));
    // Byte corruption anywhere: decoding must stay in bounds (success or failure doesn't matter).
    std::uint32_t seed = 12345;
    for (int round = 0; round < 2000; ++round) {
        auto bad = good;
        for (int k = 0; k < 3; ++k) {
            seed = seed * 1664525u + 1013904223u;
            bad[(seed >> 8) % bad.size()] = static_cast<std::uint8_t>(seed >> 24);
        }
        decode_pict(bad, p);
    }
    // A huge frame is refused rather than allocated.
    Builder b;
    b.u16(0).rect(0, 0, 30000, 30000).u16(0x0011).u16(0x02FF).u16(0x00FF);
    std::string err;
    CHECK(!decode_pict(b.v, p, &err));
    CHECK(!err.empty());
    CHECK(!decode_pict(std::vector<std::uint8_t>{1, 2, 3}, p));
}

TEST(assets_pict_real_color_vette) {
    // The player's Mac files (Game/Mac/, any form), or the extracted copy in the repository.
    std::optional<vette::assets::ResourceFork> fork;
    std::error_code ec;
    const fs::path extracted = repo_root() / "Vette_Mac_EN" / "extracted" / "VETTE! Folder" / "(Folder) Color VETTE!" /
                               "Color VETTE!.rsrc";
    if (fs::exists(repo_root() / "Game" / "Mac", ec)) {
        const auto mac = vette::assets::MacFiles::open(repo_root() / "Game" / "Mac");
        if (const auto* app = mac.find("Color VETTE!")) fork = mac.resources(*app);
    }
    if (!fork && fs::exists(extracted, ec)) {
        fork.emplace();
        std::ifstream f(extracted, std::ios::binary);
        if (!fork->parse(std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {}))) fork.reset();
    }
    if (!fork) {
        std::printf("  (skipped: no Mac Color VETTE! files)\n");
        return;
    }
    const auto picts = fork->of_type(vette::assets::fourcc("PICT"));
    CHECK_EQ(picts.size(), std::size_t{192});
    int decoded = 0;
    for (const auto* r : picts) {
        Pict p;
        std::string err;
        if (decode_pict(fork->data(*r), p, &err)) ++decoded;
        else std::printf("  PICT %d: %s\n", r->id, err.c_str());
    }
    CHECK_EQ(decoded, 192);
    Pict title;
    CHECK(decode_pict(fork->get(vette::assets::fourcc("PICT"), 24592), title));
    CHECK_EQ(title.width, 512);
    CHECK_EQ(title.height, 323);
    CHECK_EQ(title.pixels[0] >> 24, 0xFFu);  // fully drawn
}
