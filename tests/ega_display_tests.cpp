// EGA display side: attribute controller (flip-flop, palette, color plane enable), monitor color
// decode, CRTC start address/offset, render(), and the 3DAh/3C2h timing bits.

#include <array>
#include <cstdint>
#include <vector>

#include "host/ega.h"
#include "test.h"

using vette::host::Ega;

namespace {

constexpr std::array<uint32_t, 16> kCgaColors = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

// Line periods: 912 clocks of 14.31818 MHz (0Dh, 0Eh) and 744 clocks of 16.257 MHz (10h).
constexpr double kLineNs200 = 912.0 * 11.0e9 / 157.5e6;
constexpr double kLineNs350 = 744.0 * 1.0e9 / 16.257e6;

uint64_t at_line(double line, double line_ns = kLineNs200) {
    return static_cast<uint64_t>(line * line_ns);
}

void gc(Ega& e, int index, int value) {
    e.out8(0x3CE, static_cast<uint8_t>(index));
    e.out8(0x3CF, static_cast<uint8_t>(value));
}

void crtc(Ega& e, int index, int value) {
    e.out8(0x3D4, static_cast<uint8_t>(index));
    e.out8(0x3D5, static_cast<uint8_t>(value));
}

void set_start(Ega& e, uint16_t start) {
    // Low byte first, as VETTE does.
    crtc(e, 0x0D, start & 0xFF);
    crtc(e, 0x0C, start >> 8);
}

// Fills the byte at `offset` (8 pixels) with `color` via write mode 2, then back to write mode 0.
void fill_byte(Ega& e, uint32_t offset, int color) {
    gc(e, 0x08, 0xFF);
    gc(e, 0x05, 0x02);
    e.vram_write(offset, static_cast<uint8_t>(color));
    gc(e, 0x05, 0x00);
}

// Sets pixels in one byte (mask) to `color` via write mode 2 and the latches.
void set_pixels(Ega& e, uint32_t offset, int mask, int color) {
    e.vram_read(offset);
    gc(e, 0x05, 0x02);
    gc(e, 0x08, mask);
    e.vram_write(offset, static_cast<uint8_t>(color));
    gc(e, 0x08, 0xFF);
    gc(e, 0x05, 0x00);
}

uint8_t pixel(const Ega::Frame& f, int x, int y) {
    return f.pixels[static_cast<size_t>(y) * static_cast<size_t>(f.width) + static_cast<size_t>(x)];
}

bool all_pixels_zero(const Ega::Frame& f) {
    for (uint8_t px : f.pixels) {
        if (px != 0) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST(ega_default_200_line_palette_is_exact_cga_colors) {
    for (uint8_t mode : {uint8_t{0x0D}, uint8_t{0x0E}}) {
        Ega e;
        e.set_mode(mode);
        Ega::Frame f;
        e.render(f);
        for (size_t i = 0; i < 16; ++i) {
            CHECK_EQ(f.palette[i], kCgaColors[i]);
        }
        CHECK_EQ(f.palette[6], 0xAA5500u);  // brown, not dark yellow
    }
}

TEST(ega_200_line_decode_uses_rgb_and_bit4_only) {
    Ega e;
    e.set_mode(0x0E);
    e.set_palette_register(1, 0x3F);  // all six bits: white
    e.set_palette_register(2, 0x08);  // secondary blue only: ignored, black
    e.set_palette_register(3, 0x20);  // secondary red only: ignored, black
    e.set_palette_register(4, 0x2E);  // R G + ignored b r, no intensity: brown
    e.set_palette_register(5, 0x07);  // R G B: light gray, no brown fix
    e.set_palette_register(6, 0x16);  // R G + intensity: yellow
    e.set_palette_register(7, 0x10);  // intensity alone: dark gray
    e.set_palette_register(8, 0x19);  // B + intensity (+ ignored b): light blue
    e.set_palette_register(9, 0x0E);  // R G + ignored b: still brown
    Ega::Frame f;
    e.render(f);
    CHECK_EQ(f.palette[1], 0xFFFFFFu);
    CHECK_EQ(f.palette[2], 0x000000u);
    CHECK_EQ(f.palette[3], 0x000000u);
    CHECK_EQ(f.palette[4], 0xAA5500u);
    CHECK_EQ(f.palette[5], 0xAAAAAAu);
    CHECK_EQ(f.palette[6], 0xFFFF55u);
    CHECK_EQ(f.palette[7], 0x555555u);
    CHECK_EQ(f.palette[8], 0x5555FFu);
    CHECK_EQ(f.palette[9], 0xAA5500u);
}

TEST(ega_350_line_decode_uses_all_six_bits) {
    Ega e;
    e.set_mode(0x10);
    CHECK_EQ(e.mode(), 0x10);
    Ega::Frame f;
    e.render(f);
    CHECK_EQ(f.width, 640);
    CHECK_EQ(f.height, 350);
    CHECK_EQ(f.pixels.size(), size_t{640 * 350});
    for (size_t i = 0; i < 16; ++i) {
        CHECK_EQ(f.palette[i], kCgaColors[i]);  // the 10h default palette (00..05,14,07,38..3F)
    }
    e.set_palette_register(1, 0x06);  // R G: no brown fix in 350-line mode
    e.set_palette_register(2, 0x09);  // B + b
    e.set_palette_register(3, 0x2A);  // G + b + r
    e.set_palette_register(4, 0x24);  // R + r
    e.set_palette_register(5, 0x10);  // g
    e.set_palette_register(6, 0x3F);
    e.set_palette_register(7, 0x38);
    e.render(f);
    CHECK_EQ(f.palette[1], 0xAAAA00u);
    CHECK_EQ(f.palette[2], 0x0000FFu);
    CHECK_EQ(f.palette[3], 0x55AA55u);
    CHECK_EQ(f.palette[4], 0xFF0000u);
    CHECK_EQ(f.palette[5], 0x005500u);
    CHECK_EQ(f.palette[6], 0xFFFFFFu);
    CHECK_EQ(f.palette[7], 0x555555u);
}

TEST(ega_color_decode_follows_vertical_sync_polarity) {
    // The 5154 picks 200- or 350-line decoding from the vertical sync polarity (misc output bit 7).
    Ega e;
    e.set_mode(0x0E);
    e.set_palette_register(1, 0x09);
    Ega::Frame f;
    e.render(f);
    CHECK_EQ(f.palette[1], 0x0000AAu);
    e.out8(0x3C2, 0xA3);  // negative vertical sync
    e.render(f);
    CHECK_EQ(f.palette[1], 0x0000FFu);
    e.out8(0x3C2, 0x23);
    e.render(f);
    CHECK_EQ(f.palette[1], 0x0000AAu);
}

TEST(ega_attribute_flip_flop_and_3da_reset) {
    Ega e;
    e.set_mode(0x0E);
    Ega::Frame f;

    // Writes alternate index, data, index, data.
    e.in8(0x3DA);
    e.out8(0x3C0, 0x02);
    e.out8(0x3C0, 0x14);  // palette 2 = 14h
    e.out8(0x3C0, 0x03);
    e.out8(0x3C0, 0x15);  // palette 3 = 15h
    e.out8(0x3C0, 0x20);  // index with the palette address source set: display on
    e.render(f);
    CHECK_EQ(f.palette[2], 0xFF5555u);
    CHECK_EQ(f.palette[3], 0xFF55FFu);

    // A 3DAh read between index and data makes the next write an index again.
    e.in8(0x3DA);
    e.out8(0x3C0, 0x04);
    e.in8(0x3DA);
    e.out8(0x3C0, 0x05);
    e.out8(0x3C0, 0x3F);  // palette 5, not palette 4
    e.out8(0x3C0, 0x20);
    e.render(f);
    CHECK_EQ(f.palette[4], 0xAA0000u);
    CHECK_EQ(f.palette[5], 0xFFFFFFu);

    // After the 20h index the flip-flop waits for data: this byte is data for index 0 with the
    // palette address source set, so it changes nothing and the display stays on.
    e.out8(0x3C0, 0x01);
    e.render(f);
    CHECK_EQ(f.palette[0], 0x000000u);
    CHECK_EQ(f.palette[1], 0x0000AAu);

    // 3C1h writes reach the attribute controller too (out dx,ax to 3C0h).
    e.in8(0x3DA);
    e.out8(0x3C0, 0x07);
    e.out8(0x3C1, 0x01);
    e.out8(0x3C0, 0x20);
    e.render(f);
    CHECK_EQ(f.palette[7], 0x0000AAu);
}

TEST(ega_palette_is_locked_while_it_drives_the_display) {
    Ega e;
    e.set_mode(0x0E);
    e.in8(0x3DA);
    e.out8(0x3C0, 0x21);  // index 1, palette address source set
    e.out8(0x3C0, 0x3F);
    // Registers 10h-13h stay writable.
    e.out8(0x3C0, 0x32);
    e.out8(0x3C0, 0x03);
    fill_byte(e, 0, 0x0F);
    Ega::Frame f;
    e.render(f);
    CHECK_EQ(f.palette[1], 0x0000AAu);
    CHECK_EQ(pixel(f, 0, 0), 3);
}

TEST(ega_palette_address_source_off_blanks_to_overscan) {
    Ega e;
    e.set_mode(0x0E);
    fill_byte(e, 0, 9);
    e.set_border(0x04);
    e.in8(0x3DA);
    e.out8(0x3C0, 0x00);  // index 0, palette address source off
    Ega::Frame f;
    e.render(f);
    CHECK_EQ(f.width, 640);
    CHECK(all_pixels_zero(f));
    for (uint32_t rgb : f.palette) {
        CHECK_EQ(rgb, 0xAA0000u);
    }
    e.out8(0x3C0, 0x00);  // palette 0 = 0
    e.out8(0x3C0, 0x20);
    e.render(f);
    CHECK_EQ(pixel(f, 0, 0), 9);
    CHECK_EQ(f.palette[0], 0x000000u);
    CHECK_EQ(f.palette[9], 0x5555FFu);
}

TEST(ega_bios_palette_calls_leave_display_on_and_flip_flop_at_index) {
    Ega e;
    e.set_mode(0x0E);
    e.set_palette_register(15, 0x00);
    e.set_palette_register(0x14, 0x3F);  // no such register: ignored
    // The flip-flop is at index: these two writes set palette 1.
    e.out8(0x3C0, 0x01);
    e.out8(0x3C0, 0x04);
    e.out8(0x3C0, 0x20);
    Ega::Frame f;
    e.render(f);
    CHECK_EQ(f.palette[15], 0x000000u);
    CHECK_EQ(f.palette[1], 0xAA0000u);
    CHECK(f.palette[0] == 0x000000u);
    // INT 10h AX=1000h reaches registers 10h-13h as well.
    fill_byte(e, 0, 0x0F);
    e.set_palette_register(0x12, 0x01);
    e.render(f);
    CHECK_EQ(pixel(f, 0, 0), 1);
}

TEST(ega_color_plane_enable_masks_indices) {
    Ega e;
    e.set_mode(0x0E);
    fill_byte(e, 0, 0x0F);
    fill_byte(e, 1, 0x0A);
    e.in8(0x3DA);
    e.out8(0x3C0, 0x32);  // color plane enable, palette address source set
    e.out8(0x3C0, 0x05);
    Ega::Frame f;
    e.render(f);
    CHECK_EQ(pixel(f, 0, 0), 5);
    CHECK_EQ(pixel(f, 8, 0), 0);
    CHECK_EQ(f.palette[15], 0xFFFFFFu);  // the palette itself is unchanged
    e.out8(0x3C0, 0x32);
    e.out8(0x3C0, 0x0E);
    e.render(f);
    CHECK_EQ(pixel(f, 0, 0), 14);
    CHECK_EQ(pixel(f, 8, 0), 10);
}

TEST(ega_render_mode_0e_planar_layout) {
    Ega e;
    e.set_mode(0x0E);
    set_pixels(e, 0, 0x80, 1);       // (0, 0)
    set_pixels(e, 79, 0x01, 12);     // (639, 0)
    set_pixels(e, 80, 0x40, 7);      // (1, 1)
    set_pixels(e, 199 * 80 + 40, 0x10, 15);  // (323, 199)
    Ega::Frame f;
    e.render(f);
    CHECK_EQ(f.width, 640);
    CHECK_EQ(f.height, 200);
    CHECK_EQ(f.pixels.size(), size_t{640 * 200});
    CHECK_EQ(pixel(f, 0, 0), 1);
    CHECK_EQ(pixel(f, 1, 0), 0);
    CHECK_EQ(pixel(f, 638, 0), 0);
    CHECK_EQ(pixel(f, 639, 0), 12);
    CHECK_EQ(pixel(f, 0, 1), 0);
    CHECK_EQ(pixel(f, 1, 1), 7);
    CHECK_EQ(pixel(f, 323, 199), 15);
    int lit = 0;
    for (uint8_t px : f.pixels) {
        if (px != 0) {
            ++lit;
        }
    }
    CHECK_EQ(lit, 4);
}

TEST(ega_render_page_flip_via_start_address) {
    Ega e;
    e.set_mode(0x0E);
    fill_byte(e, 0x0000, 2);
    fill_byte(e, 0x4000, 4);
    fill_byte(e, 0x4000 + 80, 9);
    Ega::Frame f;
    e.render(f);
    CHECK_EQ(pixel(f, 0, 0), 2);
    CHECK_EQ(pixel(f, 0, 1), 0);
    set_start(e, 0x4000);
    e.render(f);
    CHECK_EQ(pixel(f, 0, 0), 4);
    CHECK_EQ(pixel(f, 7, 0), 4);
    CHECK_EQ(pixel(f, 8, 0), 0);
    CHECK_EQ(pixel(f, 0, 1), 9);
    set_start(e, 0x0000);
    e.render(f);
    CHECK_EQ(pixel(f, 0, 0), 2);
    // The start address is a byte offset and the display wraps at 64 KB.
    set_start(e, 0xFFFF);
    e.render(f);
    CHECK_EQ(pixel(f, 0, 0), 0);
    CHECK_EQ(pixel(f, 8, 0), 2);
}

TEST(ega_render_line_stride_from_offset_register) {
    Ega e;
    e.set_mode(0x0E);
    fill_byte(e, 80, 6);
    fill_byte(e, 82, 5);
    Ega::Frame f;
    e.render(f);
    CHECK_EQ(pixel(f, 0, 1), 6);
    crtc(e, 0x13, 0x29);  // 41 words = 82 bytes per line
    e.render(f);
    CHECK_EQ(pixel(f, 0, 1), 5);
    CHECK_EQ(pixel(f, 0, 0), 0);
}

TEST(ega_render_mode_0d_320x200) {
    Ega e;
    e.set_mode(0x0D);
    fill_byte(e, 40, 3);  // row 1 starts at byte 40
    fill_byte(e, 0x2000, 14);
    Ega::Frame f;
    e.render(f);
    CHECK_EQ(f.width, 320);
    CHECK_EQ(f.height, 200);
    CHECK_EQ(f.pixels.size(), size_t{320 * 200});
    CHECK_EQ(pixel(f, 0, 1), 3);
    CHECK_EQ(pixel(f, 7, 1), 3);
    CHECK_EQ(pixel(f, 8, 1), 0);
    CHECK_EQ(pixel(f, 0, 0), 0);
    set_start(e, 0x2000);  // VETTE's second 0Dh page (A200:0000)
    e.render(f);
    CHECK_EQ(pixel(f, 0, 0), 14);
    CHECK_EQ(f.palette[14], 0xFFFF55u);
}

TEST(ega_render_text_mode_is_empty) {
    Ega e;
    Ega::Frame f;
    f.width = 1;
    f.pixels.resize(4);
    e.render(f);  // power-on: mode 03h
    CHECK_EQ(f.width, 0);
    CHECK_EQ(f.height, 0);
    CHECK(f.pixels.empty());
    e.set_mode(0x0E);
    e.render(f);
    CHECK_EQ(f.width, 640);
    e.set_mode(0x03);
    e.render(f);
    CHECK_EQ(f.width, 0);
    CHECK(f.pixels.empty());
}

TEST(ega_crtc_registers_readable_only_where_the_ega_allows) {
    Ega e;
    e.set_mode(0x0E);
    crtc(e, 0x0C, 0x12);
    crtc(e, 0x0D, 0x34);
    crtc(e, 0x0E, 0x56);
    crtc(e, 0x0F, 0x78);
    const uint8_t want[4] = {0x12, 0x34, 0x56, 0x78};
    for (int i = 0; i < 4; ++i) {
        e.out8(0x3D4, static_cast<uint8_t>(0x0C + i));
        CHECK_EQ(e.in8(0x3D5), want[i]);
    }
    e.out8(0x3D4, 0x10);  // light pen high (no light pen)
    CHECK_EQ(e.in8(0x3D5), 0x00);
    e.out8(0x3D4, 0x13);
    CHECK_EQ(e.in8(0x3D5), 0xFF);
    e.out8(0x3D4, 0x00);
    CHECK_EQ(e.in8(0x3D5), 0xFF);
}

TEST(ega_3da_retrace_and_display_enable_follow_time) {
    uint64_t now = 0;
    Ega e;
    e.set_time_source([&now] { return now; });
    e.set_mode(0x0E);
    struct Sample {
        uint64_t t;
        uint8_t status;
    };
    const Sample samples[] = {
        {at_line(0.5), 0x04},           // active display (light pen switch bit 2 always set)
        {50'000, 0x05},                 // line 0, character 89: horizontal blank
        {at_line(100.2), 0x04},
        {at_line(199.5), 0x04},         // last displayed line
        {at_line(200.5), 0x05},         // vertical blank
        {at_line(223.5), 0x05},
        {at_line(224.5), 0x0D},         // vertical retrace: lines 224-226
        {at_line(226.5), 0x0D},
        {at_line(227.5), 0x05},
        {at_line(261.5), 0x05},
        {at_line(262.5), 0x04},         // next frame
        {at_line(262 + 224.5), 0x0D},
    };
    for (const Sample& s : samples) {
        now = s.t;
        CHECK_EQ(e.in8(0x3DA), s.status);
    }
}

TEST(ega_3da_retrace_runs_at_59_92_hz) {
    uint64_t now = 0;
    Ega e;
    e.set_time_source([&now] { return now; });
    e.set_mode(0x0E);
    std::vector<uint64_t> rises;
    int first_pulse = 0;
    bool was = false;
    for (now = 0; now < 40'000'000; now += 1000) {
        const bool retrace = (e.in8(0x3DA) & 0x08) != 0;
        if (retrace && !was) {
            rises.push_back(now);
        }
        if (retrace && rises.size() == 1) {
            ++first_pulse;
        }
        was = retrace;
    }
    CHECK_EQ(rises.size(), size_t{2});
    if (rises.size() == 2) {
        CHECK(rises[0] >= 14'267'000 && rises[0] <= 14'269'000);  // line 224
        const uint64_t period = rises[1] - rises[0];                // 262 * 912 / 14.31818 MHz
        CHECK(period >= 16'687'000 && period <= 16'690'000);
    }
    CHECK(first_pulse >= 190 && first_pulse <= 192);  // three lines, 191 us
}

TEST(ega_3da_timing_for_modes_0d_and_10h) {
    uint64_t now = 0;
    Ega e;
    e.set_time_source([&now] { return now; });

    e.set_mode(0x0D);  // 40 characters of 16 clocks displayed per 57; retrace lines 225-227
    now = at_line(0.5);
    CHECK_EQ(e.in8(0x3DA), 0x04);
    now = at_line(0.8);
    CHECK_EQ(e.in8(0x3DA), 0x05);
    now = at_line(224.5);
    CHECK_EQ(e.in8(0x3DA), 0x05);
    now = at_line(225.5);
    CHECK_EQ(e.in8(0x3DA), 0x0D);
    now = at_line(227.5);
    CHECK_EQ(e.in8(0x3DA), 0x0D);
    now = at_line(228.5);
    CHECK_EQ(e.in8(0x3DA), 0x05);

    e.set_mode(0x10);  // 350 displayed lines of 366, retrace lines 350-362
    now = at_line(349.5, kLineNs350);
    CHECK_EQ(e.in8(0x3DA), 0x04);
    now = at_line(350.5, kLineNs350);
    CHECK_EQ(e.in8(0x3DA), 0x0D);
    now = at_line(362.5, kLineNs350);
    CHECK_EQ(e.in8(0x3DA), 0x0D);
    now = at_line(363.5, kLineNs350);
    CHECK_EQ(e.in8(0x3DA), 0x05);
    now = at_line(366.5, kLineNs350);
    CHECK_EQ(e.in8(0x3DA), 0x04);
}

TEST(ega_start_address_latched_at_vertical_retrace) {
    uint64_t now = at_line(10.5);
    Ega e;
    e.set_time_source([&now] { return now; });
    e.set_mode(0x0E);
    fill_byte(e, 0x0000, 2);
    fill_byte(e, 0x4000, 4);
    Ega::Frame f;

    // Flip during display: the new page shows from the next retrace on.
    set_start(e, 0x4000);
    e.render(f);
    CHECK_EQ(pixel(f, 0, 0), 2);
    now = at_line(223.5);
    e.render(f);
    CHECK_EQ(pixel(f, 0, 0), 2);
    now = at_line(224.5);
    e.render(f);
    CHECK_EQ(pixel(f, 0, 0), 4);

    // VETTE's other idiom: wait for retrace, then write the start address. It was already loaded
    // at this retrace, so the flip happens one frame later.
    now = at_line(262 + 224.6);
    CHECK_EQ(e.in8(0x3DA) & 0x08, 0x08);
    set_start(e, 0x0000);
    e.render(f);
    CHECK_EQ(pixel(f, 0, 0), 4);
    now = at_line(2 * 262 + 100);
    e.render(f);
    CHECK_EQ(pixel(f, 0, 0), 4);
    now = at_line(2 * 262 + 224.5);
    e.render(f);
    CHECK_EQ(pixel(f, 0, 0), 2);

    // A mode set loads the start address at once.
    set_start(e, 0x4000);
    e.set_mode(0x0E, false);
    e.render(f);
    CHECK_EQ(pixel(f, 0, 0), 2);
}

TEST(ega_3c2_vertical_interrupt_and_switch_sense) {
    uint64_t now = at_line(10);
    Ega e;
    e.set_time_source([&now] { return now; });
    e.set_mode(0x0E);  // CRTC 11h = 23h: interrupt flip-flop held clear
    CHECK_EQ(e.in8(0x3C2) & 0x80, 0);
    crtc(e, 0x11, 0x33);  // stop clearing: the next retrace sets it
    now = at_line(100);
    CHECK_EQ(e.in8(0x3C2) & 0x80, 0);
    now = at_line(224.5);
    CHECK_EQ(e.in8(0x3C2) & 0x80, 0x80);
    now = at_line(300);
    CHECK_EQ(e.in8(0x3C2) & 0x80, 0x80);  // stays set until cleared
    crtc(e, 0x11, 0x23);
    CHECK_EQ(e.in8(0x3C2) & 0x80, 0);
    crtc(e, 0x11, 0x33);
    CHECK_EQ(e.in8(0x3C2) & 0x80, 0);
    now = at_line(262 + 224.5);
    CHECK_EQ(e.in8(0x3C2) & 0x80, 0x80);

    // Switch sense (bit 4) for switches 1001b, selected by the clock select bits.
    const int want[4] = {0x10, 0x00, 0x00, 0x10};
    for (int cs = 0; cs < 4; ++cs) {
        e.out8(0x3C2, static_cast<uint8_t>(0x23 | (cs << 2)));
        CHECK_EQ(e.in8(0x3C2) & 0x10, want[cs]);
    }
}
