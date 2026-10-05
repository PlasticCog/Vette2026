// Graphics option, the draw tracker: what the game draws over its pictures, followed through its
// own drawing routines (no game files needed: each routine's entry is a HLT the watch fires on).

#include <cstddef>
#include <cstdint>
#include <vector>

#include "graphics/draw_tracker.h"
#include "graphics/game_state.h"
#include "host/machine.h"
#include "test.h"

using namespace vette::graphics;
using namespace vette::host;

namespace {

constexpr int kW = 640, kH = 200;

struct Rig {
    Machine machine{[] {
        MachineConfig config;
        config.game_dir = "no-game-here";
        return config;
    }()};
    Memory& mem = machine.memory();
    Cpu& cpu = machine.cpu();
    DrawTracker tracker{machine};

    Rig() {
        cs16(0x8DAB, 0xA000);  // draw page
        cs16(0x8DA9, 0xA000);  // display page
        mem.write8(kGameCs + 0x926B, 0x28);  // 640 mode: 80-byte rows
        cpu.regs.s[SS] = 0x9000;
        cpu.regs.r[SP] = 0x0100;
    }
    void cs16(std::uint16_t off, std::uint16_t v) {
        mem.write8(kGameCs + off, static_cast<std::uint8_t>(v));
        mem.write8(kGameCs + off + 1u, static_cast<std::uint8_t>(v >> 8));
    }
    // Enters the routine at cs:entry (a HLT stands in for it).
    void call(std::uint16_t entry) {
        mem.write8(kGameCs + entry, 0xF4);
        const Registers args = cpu.regs;
        cpu.reset();  // out of the last call's HLT
        cpu.regs = args;
        cpu.regs.s[CS] = 0x4009;
        cpu.regs.ip = entry;
        cpu.run(50);
    }
};

}  // namespace

TEST(draw_tracker_text_is_exact) {
    Rig rig;
    // 'A' in the 8x10 font at 224A:F3DE: a ring, then a filled row.
    const std::uint8_t glyph[10] = {0x18, 0x24, 0x42, 0x42, 0x7E, 0x42, 0x42, 0x42, 0x00, 0x00};
    for (int k = 0; k < 10; ++k) rig.mem.write8(Cpu::linear(0x224A, static_cast<std::uint16_t>(0xF3DE + 0x21 * 10 + k)), glyph[k]);
    rig.mem.write8(Cpu::linear(0x224A, 0x0100), 'A');
    rig.mem.write8(Cpu::linear(0x224A, 0x0101), ' ');
    rig.mem.write8(Cpu::linear(0x224A, 0x0102), 0);
    rig.cpu.regs.s[DS] = 0x224A;
    rig.cpu.regs.r[SI] = 0x0100;
    rig.cpu.regs.r[DI] = 2 * 80 + 5;  // row 2, x 40
    rig.cpu.regs.set_hi(AX, 12);
    rig.call(0x88EB);

    // The frame shows the letter in colour 12, except one pixel something else drew over.
    std::vector<std::uint8_t> frame(static_cast<std::size_t>(kW) * kH, 12);  // the picture: colour 12 too
    const auto at = [](int x, int y) { return static_cast<std::size_t>(y) * kW + x; };
    frame[at(40 + 3, 2)] = 1;
    std::vector<std::uint8_t> px, cells;
    CHECK(rig.tracker.overlay(frame.data(), kW, kH, px, cells));
    int kept = 0, wrong = 0;
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 8; ++x) {
            const bool on = (glyph[y] >> (7 - x)) & 1;
            const std::uint8_t v = px[at(40 + x, 2 + y)];
            if (on && !(x == 3 && y == 0)) {
                kept += v == 12;
                wrong += v != 12;
            } else {
                wrong += v != DrawTracker::kNone;
            }
        }
    CHECK_EQ(kept, 19);  // every letter pixel the frame still shows, though the picture is that colour
    CHECK_EQ(wrong, 0);
    CHECK_EQ(px[at(39, 4)], DrawTracker::kNone);
    // Cells: the letter's and the space's, row from 1 and height 10.
    CHECK_EQ(cells[at(40, 2)], std::uint8_t{1 | 9 << 4});
    CHECK_EQ(cells[at(55, 11)], std::uint8_t{10 | 9 << 4});
    CHECK_EQ(cells[at(56, 2)], std::uint8_t{0});
    CHECK_EQ(cells[at(40, 12)], std::uint8_t{0});

    // Unpacking a picture over it: nothing is left.
    rig.cpu.regs.r[AX] = 0xA000;
    rig.cpu.regs.r[DI] = 0;
    rig.cpu.regs.set_hi(BX, 80);
    rig.cpu.regs.r[BP] = 200;
    rig.call(0x8666);
    CHECK(!rig.tracker.overlay(frame.data(), kW, kH, px, cells));
}

TEST(draw_tracker_fills_and_copies) {
    Rig rig;
    // A solid rectangle in colour 15 on the other page: 2 bytes x 3 rows at row 10, x 80.
    rig.cpu.regs.r[AX] = 0xA400;
    rig.cpu.regs.r[DI] = 10 * 80 + 10;
    rig.cpu.regs.r[BX] = 0x020F;
    rig.cpu.regs.r[BP] = 3;
    rig.call(0x88AF);
    std::vector<std::uint8_t> frame(static_cast<std::size_t>(kW) * kH, 15), px, cells;
    CHECK(!rig.tracker.overlay(frame.data(), kW, kH, px, cells));  // not on the page on display

    // Copied to the page on display (A000h), at row 50.
    rig.cpu.regs.r[AX] = 0xA400;
    rig.cpu.regs.r[SI] = 10 * 80 + 10;
    rig.cpu.regs.r[DX] = 0xA000;
    rig.cpu.regs.r[DI] = 50 * 80 + 10;
    rig.cpu.regs.set_hi(BX, 2);
    rig.cpu.regs.r[BP] = 3;
    rig.call(0x8820);
    CHECK(rig.tracker.overlay(frame.data(), kW, kH, px, cells));
    int n = 0;
    for (const std::uint8_t v : px) n += v == 15;
    CHECK_EQ(n, 16 * 3);
    CHECK_EQ(px[static_cast<std::size_t>(52) * kW + 95], std::uint8_t{15});
    CHECK_EQ(px[static_cast<std::size_t>(53) * kW + 95], DrawTracker::kNone);

    // The mode set clears everything.
    rig.call(0x8F00);
    CHECK(!rig.tracker.overlay(frame.data(), kW, kH, px, cells));
}

TEST(draw_tracker_sprite_mask) {
    Rig rig;
    // The mask pass of a 2-byte x 2-row sprite at row 0: mask bytes (1 = transparent) 0F F0 / 00 FF.
    const std::uint8_t mask[4] = {0x0F, 0xF0, 0x00, 0xFF};
    for (int i = 0; i < 4; ++i) rig.mem.write8(Cpu::linear(0x3000, static_cast<std::uint16_t>(0x200 + i)), mask[i]);
    rig.cpu.regs.s[DS] = 0x3000;
    rig.cpu.regs.r[SI] = 0x200;
    rig.cs16(0x7FB7, 0);       // destination
    rig.cs16(0x7FB5, 0);       // source skip at the start
    rig.cs16(0x7FA1, 0);       // left edge: whole first byte
    rig.cs16(0x7FA5, 15);      // right edge x
    rig.cs16(0x7FAB, 0);
    rig.cs16(0x7FBB, 2);       // rows
    rig.cs16(0x7FB9, 2);       // bytes
    rig.cs16(0x7FB3, 80 - 2);  // destination skip
    rig.cs16(0x7FB1, 0);       // source skip
    // Called from the mask pass (return address 854Fh).
    rig.mem.write8(Cpu::linear(0x9000, 0x0100), 0x4F);
    rig.mem.write8(Cpu::linear(0x9000, 0x0101), 0x85);
    rig.call(0x858F);
    std::vector<std::uint8_t> frame(static_cast<std::size_t>(kW) * kH, 7), px, cells;
    CHECK(rig.tracker.overlay(frame.data(), kW, kH, px, cells));
    int n = 0;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 16; ++x) {
            const bool written = !((mask[y * 2 + x / 8] >> (7 - x % 8)) & 1);
            n += (px[static_cast<std::size_t>(y) * kW + x] == DrawTracker::kSprite) == written;
        }
    CHECK_EQ(n, 32);

    // A colour plane pass (any other caller) adds nothing.
    Rig other;
    other.mem.write8(Cpu::linear(0x9000, 0x0100), 0x00);
    other.call(0x858F);
    CHECK(!other.tracker.overlay(frame.data(), kW, kH, px, cells));
}

TEST(draw_tracker_title_dissolve) {
    // The title's transition onto page A000h: until the dissolve brings a block in, it is the old
    // screen, kept as the frame shows it.
    Rig rig;
    rig.cpu.regs.r[AX] = 0xA000;
    rig.cpu.regs.r[BX] = 0xA400;
    rig.call(0x7FE7);
    std::vector<std::uint8_t> frame(static_cast<std::size_t>(kW) * kH, 0), px, cells;
    CHECK(rig.tracker.overlay(frame.data(), kW, kH, px, cells));
    CHECK_EQ(px[0], DrawTracker::kSprite);
    CHECK_EQ(px[static_cast<std::size_t>(199) * kW + 639], DrawTracker::kSprite);
    // One block in: byte 1 (x 8-15) of rows 0-4, from DS:SI to ES:SI.
    rig.cpu.regs.s[DS] = 0xA400;
    rig.cpu.regs.s[ES] = 0xA000;
    rig.cpu.regs.r[SI] = 1;
    rig.call(0x8285);
    CHECK(rig.tracker.overlay(frame.data(), kW, kH, px, cells));
    int in = 0;
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 24; ++x) in += px[static_cast<std::size_t>(y) * kW + x] != DrawTracker::kSprite;
    CHECK_EQ(in, 8 * 5);
    CHECK_EQ(px[static_cast<std::size_t>(4) * kW + 15], DrawTracker::kNone);
    CHECK_EQ(px[static_cast<std::size_t>(5) * kW + 15], DrawTracker::kSprite);
    // Its end (also when a key cuts it short): nothing is held back any more.
    rig.call(0x82B1);
    CHECK(!rig.tracker.overlay(frame.data(), kW, kH, px, cells));
}
