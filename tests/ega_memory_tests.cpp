// EGA CPU data path: write modes 0/1/2, set/reset, rotate, logical functions, bit mask, map mask,
// read modes 0/1, latches and the memory map.

#include <array>
#include <cstdint>

#include "host/ega.h"
#include "test.h"

using vette::host::Ega;

namespace {

void gc(Ega& e, int index, int value) {
    e.out8(0x3CE, static_cast<uint8_t>(index));
    e.out8(0x3CF, static_cast<uint8_t>(value));
}

void map_mask(Ega& e, int mask) {
    e.out8(0x3C4, 0x02);
    e.out8(0x3C5, static_cast<uint8_t>(mask));
}

// Writes each plane's byte at `offset` directly (write mode 0, no set/reset, replace, bit mask FFh),
// then restores map mask 0Fh. Leaves the other graphics registers in that plain state.
void poke(Ega& e, uint32_t offset, int p0, int p1, int p2, int p3) {
    gc(e, 0x01, 0x00);
    gc(e, 0x03, 0x00);
    gc(e, 0x05, 0x00);
    gc(e, 0x08, 0xFF);
    const int bytes[4] = {p0, p1, p2, p3};
    for (int p = 0; p < 4; ++p) {
        map_mask(e, 1 << p);
        e.vram_write(offset, static_cast<uint8_t>(bytes[p]));
    }
    map_mask(e, 0x0F);
}

// Reads the four planes at `offset` with read mode 0. Sets write mode 0 and loads the latches.
std::array<uint8_t, 4> peek(Ega& e, uint32_t offset) {
    gc(e, 0x05, 0x00);
    std::array<uint8_t, 4> out{};
    for (int p = 0; p < 4; ++p) {
        gc(e, 0x04, p);
        out[static_cast<size_t>(p)] = e.vram_read(offset);
    }
    gc(e, 0x04, 0x00);
    return out;
}

void check_planes(Ega& e, uint32_t offset, int p0, int p1, int p2, int p3, int line) {
    const std::array<uint8_t, 4> got = peek(e, offset);
    const int want[4] = {p0, p1, p2, p3};
    for (size_t p = 0; p < 4; ++p) {
        if (got[p] != want[p]) {
            char msg[128];
            std::snprintf(msg, sizeof msg, "offset %04X plane %zu: got %02X, want %02X", offset, p,
                          got[p], want[p]);
            vette::test::fail(__FILE__, line, msg);
        }
    }
}

#define CHECK_PLANES(e, off, p0, p1, p2, p3) check_planes(e, off, p0, p1, p2, p3, __LINE__)

} // namespace

TEST(ega_handles_its_ports_only) {
    for (uint16_t port = 0x3C0; port <= 0x3CF; ++port) {
        CHECK(Ega::handles(port));
    }
    CHECK(Ega::handles(0x3D4));
    CHECK(Ega::handles(0x3D5));
    CHECK(Ega::handles(0x3DA));
    CHECK(!Ega::handles(0x3BF));
    CHECK(!Ega::handles(0x3D0));
    CHECK(!Ega::handles(0x3D6));
    CHECK(!Ega::handles(0x3D9));
    CHECK(!Ega::handles(0x3DB));
    CHECK(!Ega::handles(0x3B4));
    CHECK(!Ega::handles(0x3BA));
    CHECK(!Ega::handles(0x0060));
}

TEST(ega_power_on_reports_text_mode_with_working_planes) {
    Ega e;
    CHECK_EQ(e.mode(), 0x03);
    e.vram_write(0x1234, 0x5A);
    CHECK_PLANES(e, 0x1234, 0x5A, 0x5A, 0x5A, 0x5A);
}

TEST(ega_mode_0e_bios_state_writes_cpu_byte_to_all_planes) {
    Ega e;
    e.set_mode(0x0E);
    CHECK_EQ(e.mode(), 0x0E);
    e.vram_write(0x0000, 0xA5);
    CHECK_PLANES(e, 0x0000, 0xA5, 0xA5, 0xA5, 0xA5);
    // Offsets wrap at 64 KB.
    e.vram_write(0x1FFFF, 0x77);
    CHECK_PLANES(e, 0xFFFF, 0x77, 0x77, 0x77, 0x77);
}

TEST(ega_mode_set_restores_graphics_registers) {
    Ega e;
    e.set_mode(0x0E);
    gc(e, 0x05, 0x02);
    gc(e, 0x08, 0x00);
    gc(e, 0x01, 0x0F);
    gc(e, 0x03, 0x1B);
    map_mask(e, 0x01);
    e.set_mode(0x0E);
    e.vram_write(0x0010, 0xC3);
    CHECK_PLANES(e, 0x0010, 0xC3, 0xC3, 0xC3, 0xC3);
}

TEST(ega_write_mode0_map_mask_selects_planes) {
    Ega e;
    e.set_mode(0x0E);
    map_mask(e, 0x05);
    e.vram_write(0x0100, 0x3C);
    CHECK_PLANES(e, 0x0100, 0x3C, 0x00, 0x3C, 0x00);
    map_mask(e, 0x0A);
    e.vram_write(0x0100, 0xC3);
    CHECK_PLANES(e, 0x0100, 0x3C, 0xC3, 0x3C, 0xC3);
    map_mask(e, 0x00);
    e.vram_write(0x0100, 0xFF);
    CHECK_PLANES(e, 0x0100, 0x3C, 0xC3, 0x3C, 0xC3);
    // Only the low four bits of the map mask matter.
    map_mask(e, 0xF4);
    e.vram_write(0x0100, 0x99);
    CHECK_PLANES(e, 0x0100, 0x3C, 0xC3, 0x99, 0xC3);
}

TEST(ega_write_mode0_set_reset_and_enable) {
    Ega e;
    e.set_mode(0x0E);
    // All planes from set/reset: color 5 = planes 0 and 2 set, 1 and 3 clear. CPU data ignored.
    gc(e, 0x00, 0x05);
    gc(e, 0x01, 0x0F);
    e.vram_write(0x0200, 0x5A);
    CHECK_PLANES(e, 0x0200, 0xFF, 0x00, 0xFF, 0x00);
    // Planes 0 and 1 from set/reset, planes 2 and 3 take the CPU byte.
    gc(e, 0x00, 0x05);
    gc(e, 0x01, 0x03);
    e.vram_write(0x0201, 0x5A);
    CHECK_PLANES(e, 0x0201, 0xFF, 0x00, 0x5A, 0x5A);
    // Set/reset bits without their enable bits have no effect.
    gc(e, 0x00, 0x0F);
    gc(e, 0x01, 0x00);
    e.vram_write(0x0202, 0x81);
    CHECK_PLANES(e, 0x0202, 0x81, 0x81, 0x81, 0x81);
    // Upper bits of both registers are ignored.
    gc(e, 0x00, 0xF6);
    gc(e, 0x01, 0xFF);
    e.vram_write(0x0203, 0x00);
    CHECK_PLANES(e, 0x0203, 0x00, 0xFF, 0xFF, 0x00);
}

TEST(ega_write_mode0_rotates_cpu_data) {
    Ega e;
    e.set_mode(0x0E);
    gc(e, 0x03, 0x01);
    e.vram_write(0x0300, 0x01);
    CHECK_PLANES(e, 0x0300, 0x80, 0x80, 0x80, 0x80);
    gc(e, 0x03, 0x03);
    e.vram_write(0x0301, 0x81);
    CHECK_PLANES(e, 0x0301, 0x30, 0x30, 0x30, 0x30);
    gc(e, 0x03, 0x07);
    e.vram_write(0x0302, 0x01);
    CHECK_PLANES(e, 0x0302, 0x02, 0x02, 0x02, 0x02);
    // Planes fed by set/reset are not rotated data: plane 0 from set/reset (0), the rest rotated.
    gc(e, 0x03, 0x04);
    gc(e, 0x00, 0x00);
    gc(e, 0x01, 0x01);
    e.vram_write(0x0303, 0x0F);
    CHECK_PLANES(e, 0x0303, 0x00, 0xF0, 0xF0, 0xF0);
}

TEST(ega_write_mode0_logical_functions_use_latches) {
    Ega e;
    e.set_mode(0x0E);
    struct Case {
        int function;  // data rotate register bits 3-4
        int p0, p1, p2, p3;
    };
    // Latches F0 CC AA 0F, CPU byte 3C.
    const Case cases[] = {
        {0x00, 0x3C, 0x3C, 0x3C, 0x3C},  // replace
        {0x08, 0x30, 0x0C, 0x28, 0x0C},  // AND
        {0x10, 0xFC, 0xFC, 0xBE, 0x3F},  // OR
        {0x18, 0xCC, 0xF0, 0x96, 0x33},  // XOR
    };
    for (const Case& c : cases) {
        poke(e, 0x0400, 0xF0, 0xCC, 0xAA, 0x0F);
        e.vram_read(0x0400);  // load the latches
        gc(e, 0x03, c.function);
        e.vram_write(0x0400, 0x3C);
        CHECK_PLANES(e, 0x0400, c.p0, c.p1, c.p2, c.p3);
    }
    // The function combines with the latches, not with the destination's contents.
    poke(e, 0x0401, 0x0F, 0x0F, 0x0F, 0x0F);
    poke(e, 0x0402, 0x00, 0x00, 0x00, 0x00);
    e.vram_read(0x0401);
    gc(e, 0x03, 0x10);  // OR
    e.vram_write(0x0402, 0x30);
    CHECK_PLANES(e, 0x0402, 0x3F, 0x3F, 0x3F, 0x3F);
    // With set/reset: XOR a color into the latched pixels.
    poke(e, 0x0403, 0xFF, 0x00, 0xFF, 0x00);
    e.vram_read(0x0403);
    gc(e, 0x00, 0x03);
    gc(e, 0x01, 0x0F);
    gc(e, 0x03, 0x18);
    e.vram_write(0x0403, 0x00);
    CHECK_PLANES(e, 0x0403, 0x00, 0xFF, 0xFF, 0x00);
}

TEST(ega_write_mode0_bit_mask_takes_unmasked_bits_from_latches) {
    Ega e;
    e.set_mode(0x0E);
    poke(e, 0x0500, 0x0F, 0x0F, 0x0F, 0x0F);
    poke(e, 0x0501, 0xFF, 0x00, 0xFF, 0x00);
    e.vram_read(0x0501);  // latches FF 00 FF 00
    gc(e, 0x08, 0xF0);
    e.vram_write(0x0500, 0xAA);
    CHECK_PLANES(e, 0x0500, 0xAF, 0xA0, 0xAF, 0xA0);
    // Bit mask 0: the latches are written unchanged.
    e.vram_read(0x0501);
    gc(e, 0x08, 0x00);
    e.vram_write(0x0500, 0x55);
    CHECK_PLANES(e, 0x0500, 0xFF, 0x00, 0xFF, 0x00);
    // The bit mask applies after the logical function and set/reset; the map mask applies last.
    poke(e, 0x0502, 0x00, 0x00, 0x00, 0x00);
    e.vram_read(0x0502);
    gc(e, 0x00, 0x0F);
    gc(e, 0x01, 0x0F);
    gc(e, 0x03, 0x10);
    gc(e, 0x08, 0x18);
    map_mask(e, 0x06);
    e.vram_write(0x0502, 0x00);
    CHECK_PLANES(e, 0x0502, 0x00, 0x18, 0x18, 0x00);
}

TEST(ega_write_mode1_copies_latches) {
    Ega e;
    e.set_mode(0x0E);
    poke(e, 0x0600, 0x12, 0x34, 0x56, 0x78);
    gc(e, 0x05, 0x01);
    // None of these affect write mode 1.
    gc(e, 0x00, 0x0F);
    gc(e, 0x01, 0x0F);
    gc(e, 0x03, 0x1D);
    gc(e, 0x08, 0x00);
    e.vram_read(0x0600);
    e.vram_write(0x0700, 0xFF);
    CHECK_PLANES(e, 0x0700, 0x12, 0x34, 0x56, 0x78);
    // The map mask still applies.
    gc(e, 0x05, 0x01);
    map_mask(e, 0x05);
    e.vram_read(0x0600);
    e.vram_write(0x0701, 0x00);
    CHECK_PLANES(e, 0x0701, 0x12, 0x00, 0x56, 0x00);
}

TEST(ega_write_mode1_block_copy_like_vette) {
    // VETTE copies screen blocks with write mode 1 and REP MOVSB (read source, write destination).
    Ega e;
    e.set_mode(0x0E);
    for (uint32_t i = 0; i < 40; ++i) {
        const auto v = static_cast<int>(i);
        poke(e, 0x0800 + i, v, v * 3, 0xFF - v, v ^ 0x5A);
    }
    gc(e, 0x05, 0x01);
    for (uint32_t i = 0; i < 40; ++i) {
        e.vram_write(0x4800 + i, e.vram_read(0x0800 + i));
    }
    gc(e, 0x05, 0x00);
    for (uint32_t i = 0; i < 40; ++i) {
        const auto v = static_cast<int>(i);
        CHECK_PLANES(e, 0x4800 + i, v, (v * 3) & 0xFF, 0xFF - v, v ^ 0x5A);
    }
}

TEST(ega_write_mode2_expands_color_bits) {
    Ega e;
    e.set_mode(0x0E);
    gc(e, 0x05, 0x02);
    e.vram_write(0x0900, 0x0D);
    CHECK_PLANES(e, 0x0900, 0xFF, 0x00, 0xFF, 0xFF);
    // Bits 4-7 are ignored.
    gc(e, 0x05, 0x02);
    e.vram_write(0x0901, 0xF2);
    CHECK_PLANES(e, 0x0901, 0x00, 0xFF, 0x00, 0x00);
    // The rotate count and set/reset don't apply in write mode 2.
    gc(e, 0x05, 0x02);
    gc(e, 0x03, 0x03);
    gc(e, 0x00, 0x0F);
    gc(e, 0x01, 0x0F);
    e.vram_write(0x0902, 0x01);
    CHECK_PLANES(e, 0x0902, 0xFF, 0x00, 0x00, 0x00);
    // Bit mask with the latches.
    poke(e, 0x0903, 0x0F, 0x0F, 0x0F, 0x0F);
    e.vram_read(0x0903);
    gc(e, 0x05, 0x02);
    gc(e, 0x08, 0xC0);
    e.vram_write(0x0903, 0x05);
    CHECK_PLANES(e, 0x0903, 0xCF, 0x0F, 0xCF, 0x0F);
    // Logical functions.
    poke(e, 0x0904, 0xF0, 0xF0, 0xF0, 0xF0);
    e.vram_read(0x0904);
    gc(e, 0x05, 0x02);
    gc(e, 0x03, 0x08);  // AND
    e.vram_write(0x0904, 0x03);
    CHECK_PLANES(e, 0x0904, 0xF0, 0xF0, 0x00, 0x00);
    poke(e, 0x0905, 0xF0, 0xF0, 0xF0, 0xF0);
    e.vram_read(0x0905);
    gc(e, 0x05, 0x02);
    gc(e, 0x03, 0x10);  // OR
    e.vram_write(0x0905, 0x01);
    CHECK_PLANES(e, 0x0905, 0xFF, 0xF0, 0xF0, 0xF0);
    poke(e, 0x0906, 0xF0, 0xF0, 0xF0, 0xF0);
    e.vram_read(0x0906);
    gc(e, 0x05, 0x02);
    gc(e, 0x03, 0x18);  // XOR
    e.vram_write(0x0906, 0x06);
    CHECK_PLANES(e, 0x0906, 0xF0, 0x0F, 0x0F, 0xF0);
    // Map mask.
    poke(e, 0x0907, 0x00, 0x00, 0x00, 0x00);
    gc(e, 0x05, 0x02);
    map_mask(e, 0x01);
    e.vram_write(0x0907, 0x0F);
    CHECK_PLANES(e, 0x0907, 0xFF, 0x00, 0x00, 0x00);
}

TEST(ega_read_mode0_returns_selected_plane) {
    Ega e;
    e.set_mode(0x0E);
    poke(e, 0x0A00, 0x11, 0x22, 0x33, 0x44);
    const uint8_t want[4] = {0x11, 0x22, 0x33, 0x44};
    for (int p = 0; p < 4; ++p) {
        gc(e, 0x04, p);
        CHECK_EQ(e.vram_read(0x0A00), want[p]);
    }
    gc(e, 0x04, 0xFE);  // only bits 0-1 select the plane
    CHECK_EQ(e.vram_read(0x0A00), 0x33);
}

TEST(ega_read_mode1_color_compare_and_dont_care) {
    Ega e;
    e.set_mode(0x0E);
    // Pixel colors from the left: 7 3 5 1 6 2 4 0.
    poke(e, 0x0B00, 0xF0, 0xCC, 0xAA, 0x00);
    gc(e, 0x05, 0x08);
    gc(e, 0x07, 0x0F);
    const uint8_t want_for_color[8] = {0x01, 0x10, 0x04, 0x40, 0x02, 0x20, 0x08, 0x80};
    for (int color = 0; color < 8; ++color) {
        gc(e, 0x02, color);
        CHECK_EQ(e.vram_read(0x0B00), want_for_color[color]);
    }
    gc(e, 0x02, 0x08);
    CHECK_EQ(e.vram_read(0x0B00), 0x00);
    // Don't care: only planes whose bit is 1 take part.
    gc(e, 0x02, 0x01);
    gc(e, 0x07, 0x01);
    CHECK_EQ(e.vram_read(0x0B00), 0xF0);
    gc(e, 0x02, 0x03);
    gc(e, 0x07, 0x0B);
    CHECK_EQ(e.vram_read(0x0B00), 0xC0);
    gc(e, 0x02, 0x0F);
    gc(e, 0x07, 0x00);
    CHECK_EQ(e.vram_read(0x0B00), 0xFF);
    gc(e, 0x02, 0x08);
    gc(e, 0x07, 0x08);
    CHECK_EQ(e.vram_read(0x0B00), 0x00);
    gc(e, 0x02, 0x00);
    CHECK_EQ(e.vram_read(0x0B00), 0xFF);
}

TEST(ega_reads_load_all_four_latches) {
    Ega e;
    e.set_mode(0x0E);
    poke(e, 0x0C00, 0xDE, 0xAD, 0xBE, 0xEF);
    poke(e, 0x0C01, 0x01, 0x02, 0x03, 0x04);
    // Read map select doesn't limit what is latched.
    gc(e, 0x04, 0x02);
    CHECK_EQ(e.vram_read(0x0C00), 0xBE);
    gc(e, 0x05, 0x01);
    e.vram_write(0x0C10, 0x00);
    CHECK_PLANES(e, 0x0C10, 0xDE, 0xAD, 0xBE, 0xEF);
    // Read mode 1 loads the latches too.
    gc(e, 0x05, 0x08);
    e.vram_read(0x0C01);
    gc(e, 0x05, 0x01);
    e.vram_write(0x0C11, 0x00);
    CHECK_PLANES(e, 0x0C11, 0x01, 0x02, 0x03, 0x04);
    // Writes don't load the latches.
    e.vram_read(0x0C00);
    gc(e, 0x05, 0x00);
    e.vram_write(0x0C12, 0x99);
    gc(e, 0x05, 0x01);
    e.vram_write(0x0C13, 0x00);
    CHECK_PLANES(e, 0x0C13, 0xDE, 0xAD, 0xBE, 0xEF);
}

TEST(ega_memory_map_select_controls_a000_decoding) {
    Ega e;
    e.set_mode(0x0E);
    e.vram_write(0x0D00, 0x42);
    gc(e, 0x06, 0x0D);  // B8000h-BFFFFh: A000 is not decoded
    CHECK_EQ(e.vram_read(0x0D00), 0xFF);
    e.vram_write(0x0D00, 0x00);
    gc(e, 0x06, 0x09);  // B0000h-B7FFFh
    CHECK_EQ(e.vram_read(0x0D00), 0xFF);
    gc(e, 0x06, 0x01);  // A0000h-BFFFFh
    CHECK_EQ(e.vram_read(0x0D00), 0x42);
    gc(e, 0x06, 0x05);  // A0000h-AFFFFh
    CHECK_PLANES(e, 0x0D00, 0x42, 0x42, 0x42, 0x42);
}

TEST(ega_mode_set_clears_memory_unless_asked_not_to) {
    Ega e;
    e.set_mode(0x0E);
    e.vram_write(0x0E00, 0x55);
    e.set_mode(0x0E, false);
    CHECK_PLANES(e, 0x0E00, 0x55, 0x55, 0x55, 0x55);
    e.set_mode(0x8E);  // INT 10h bit 7: keep memory
    CHECK_EQ(e.mode(), 0x0E);
    CHECK_PLANES(e, 0x0E00, 0x55, 0x55, 0x55, 0x55);
    e.set_mode(0x03);  // text modes are only recorded
    CHECK_EQ(e.mode(), 0x03);
    CHECK_PLANES(e, 0x0E00, 0x55, 0x55, 0x55, 0x55);
    e.set_mode(0x0D);
    CHECK_EQ(e.mode(), 0x0D);
    CHECK_PLANES(e, 0x0E00, 0x00, 0x00, 0x00, 0x00);
}

TEST(ega_write_only_registers_read_as_open_bus) {
    Ega e;
    e.set_mode(0x0E);
    e.out8(0x3CE, 0x08);
    CHECK_EQ(e.in8(0x3CF), 0xFF);
    CHECK_EQ(e.in8(0x3CE), 0xFF);
    e.out8(0x3C4, 0x02);
    CHECK_EQ(e.in8(0x3C5), 0xFF);
    CHECK_EQ(e.in8(0x3C0), 0xFF);
    CHECK_EQ(e.in8(0x3C1), 0xFF);
    CHECK_EQ(e.in8(0x3D4), 0xFF);
}

TEST(ega_polygon_fill_idiom_on_back_page) {
    // The fast span fill: set/reset holds the color, enable set/reset = 0Fh, the bit mask selects
    // the edge pixels and a read before each edge write loads the latches so that the pixels outside
    // the mask keep their colors.
    Ega e;
    e.set_mode(0x0E);
    const uint32_t page1 = 0x4000;
    const uint32_t row = page1 + 10 * 80;

    // Background: color 1 across row 10 of the back page.
    gc(e, 0x00, 0x01);
    gc(e, 0x01, 0x0F);
    gc(e, 0x08, 0xFF);
    for (uint32_t x = 0; x < 80; ++x) {
        e.vram_write(row + x, 0xFF);
    }

    // Span x = 13..50 in color 14.
    const int x0 = 13;
    const int x1 = 50;
    gc(e, 0x00, 14);
    const auto left = static_cast<uint32_t>(x0 / 8);
    const auto right = static_cast<uint32_t>(x1 / 8);
    gc(e, 0x08, 0xFF >> (x0 & 7));
    e.vram_read(row + left);
    e.vram_write(row + left, 0xFF);
    gc(e, 0x08, 0xFF);
    for (uint32_t b = left + 1; b < right; ++b) {
        e.vram_write(row + b, 0xFF);  // full bytes: no latch read needed
    }
    gc(e, 0x08, (0xFF << (7 - (x1 & 7))) & 0xFF);
    e.vram_read(row + right);
    e.vram_write(row + right, 0xFF);
    gc(e, 0x08, 0xFF);
    gc(e, 0x01, 0x00);

    // Flip to the back page and check the pixels.
    e.out8(0x3D4, 0x0C);
    e.out8(0x3D5, 0x40);
    e.out8(0x3D4, 0x0D);
    e.out8(0x3D5, 0x00);
    Ega::Frame f;
    e.render(f);
    CHECK_EQ(f.width, 640);
    CHECK_EQ(f.height, 200);
    int wrong = 0;
    for (int y = 0; y < 200; ++y) {
        for (int x = 0; x < 640; ++x) {
            int want = 0;
            if (y == 10) {
                want = (x >= x0 && x <= x1) ? 14 : 1;
            }
            if (f.pixels[static_cast<size_t>(y * 640 + x)] != want) {
                ++wrong;
            }
        }
    }
    CHECK_EQ(wrong, 0);
    CHECK_EQ(f.palette[14], 0xFFFF55u);
    CHECK_EQ(f.palette[1], 0x0000AAu);

    // The front page is untouched.
    e.out8(0x3D4, 0x0C);
    e.out8(0x3D5, 0x00);
    e.render(f);
    int lit = 0;
    for (uint8_t px : f.pixels) {
        if (px != 0) {
            ++lit;
        }
    }
    CHECK_EQ(lit, 0);
}
