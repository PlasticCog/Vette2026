// PIC, PIT and PC speaker: the devices behind VETTE's 291 Hz timer and its sound.

#include <cmath>
#include <cstddef>
#include <vector>

#include "host/pic.h"
#include "host/pit.h"
#include "host/speaker.h"
#include "test.h"

using namespace vette::host;

TEST(pic_priority_and_eoi) {
    Pic pic;
    pic.raise(1);
    pic.raise(0);
    CHECK(pic.irq_pending());
    CHECK_EQ(pic.irq_acknowledge(), 0x08);  // IRQ0 first
    CHECK(!pic.irq_pending());              // IRQ1 waits while IRQ0 is in service
    pic.out8(0x20, 0x20);                   // non-specific EOI
    CHECK(pic.irq_pending());
    CHECK_EQ(pic.irq_acknowledge(), 0x09);
    pic.out8(0x20, 0x20);
    CHECK(!pic.irq_pending());
}

TEST(pic_mask_and_remap) {
    Pic pic;
    pic.out8(0x21, 0x01);  // mask IRQ0
    pic.raise(0);
    CHECK(!pic.irq_pending());
    pic.out8(0x21, 0x00);
    CHECK(pic.irq_pending());
    pic.lower(0);
    pic.out8(0x20, 0x11);  // ICW1 (ICW4 follows)
    pic.out8(0x21, 0x50);  // ICW2: base 50h
    pic.out8(0x21, 0x01);  // ICW4
    pic.raise(3);
    CHECK_EQ(pic.irq_acknowledge(), 0x53);
}

TEST(pit_vette_rate) {
    Pit pit;
    // What VETTE's timer_init does: mode 3, divisor 0x1000.
    pit.out8(0x43, 0x36, 100);
    pit.out8(0x40, 0x00, 100);
    pit.out8(0x40, 0x10, 100);
    CHECK_EQ(pit.next_irq0_after(100), 100u + 0x1000);
    CHECK_EQ(pit.next_irq0_after(100 + 0x1000), 100u + 0x2000);
    CHECK_EQ(pit.next_irq0_after(100 + 0x1000 - 1), 100u + 0x1000);
    // 291.27 Hz: about 291 interrupts per emulated second.
    uint64_t t = 100, n = 0;
    while ((t = pit.next_irq0_after(t)) <= 100 + kPitHz) {
        ++n;
    }
    CHECK_EQ(n, 291u);
}

TEST(pit_latch_and_mode3_count) {
    Pit pit;
    pit.out8(0x43, 0x36, 0);
    pit.out8(0x40, 0x00, 0);
    pit.out8(0x40, 0x10, 0);  // reload 4096
    pit.out8(0x43, 0x00, 10);  // latch channel 0 at t=10
    const uint8_t lo = pit.in8(0x40, 500);
    const uint8_t hi = pit.in8(0x40, 500);
    CHECK_EQ(lo | (hi << 8), 4096 - 2 * 10);  // mode 3 counts down by 2
    CHECK(pit.output(0, 0));
    CHECK(!pit.output(0, 2048));  // second half of the period is low
}

TEST(pit_gate2) {
    Pit pit;
    pit.out8(0x43, 0xB6, 0);  // channel 2, mode 3 (VETTE's speaker setup)
    pit.out8(0x42, 0x00, 0);
    pit.out8(0x42, 0x01, 0);  // reload 256
    CHECK(pit.output(2, 50));  // gate low: output held high
    pit.set_gate2(true, 1000);
    CHECK(pit.output(2, 1000));
    CHECK(!pit.output(2, 1000 + 200));
}

namespace {
int zero_crossings(const std::vector<int16_t>& s, size_t from) {
    int n = 0;
    for (size_t i = from + 1; i < s.size(); ++i) {
        if ((s[i - 1] < 0) != (s[i] < 0)) {
            ++n;
        }
    }
    return n;
}
} // namespace

TEST(speaker_tone_frequency) {
    Pit pit;
    Speaker spk;
    const uint16_t divisor = static_cast<uint16_t>(kPitHz / 1000);  // ~1 kHz
    pit.out8(0x43, 0xB6, 0);
    pit.out8(0x42, static_cast<uint8_t>(divisor), 0);
    pit.out8(0x42, static_cast<uint8_t>(divisor >> 8), 0);
    pit.set_gate2(true, 0);
    spk.update(0, true, pit);
    std::vector<int16_t> out;
    spk.render(kPitHz, 48000, out);  // one second
    CHECK(out.size() >= 47999 && out.size() <= 48000);
    const int crossings = zero_crossings(out, 4800);  // skip the high-pass settling
    CHECK(std::abs(crossings - 2 * 900) <= 4);         // 900 ms of a 1 kHz square: 1800 crossings
}

TEST(speaker_silence_and_continuity) {
    Pit pit;
    Speaker spk;
    std::vector<int16_t> out;
    spk.render(kPitHz / 10, 48000, out);
    for (int16_t v : out) {
        CHECK_EQ(v, 0);
    }
    // Rendering in pieces gives the same sample count as rendering at once.
    Speaker a, b;
    std::vector<int16_t> one, pieces;
    a.render(kPitHz, 48000, one);
    for (int i = 1; i <= 60; ++i) {
        b.render(kPitHz * static_cast<uint64_t>(i) / 60, 48000, pieces);
    }
    CHECK_EQ(one.size(), pieces.size());
}
