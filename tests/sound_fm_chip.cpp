// The FM chips through ymfm: a note sounds, silence is silent, timers raise their flags.

#include <cmath>
#include <vector>

#include "sound/fm_chip.h"
#include "test.h"

using vette::sound::FmChip;

namespace {

float peak(FmChip& chip, int frames) {
    std::vector<float> buf(static_cast<size_t>(frames));
    chip.render(buf.data(), frames);
    float p = 0;
    for (const float v : buf) {
        p = std::max(p, std::fabs(v));
    }
    return p;
}

}  // namespace

TEST(fm_chip_opl2_note) {
    FmChip chip(FmChip::Type::Ym3812, 48000);
    CHECK(chip.chip_rate() > 49000 && chip.chip_rate() < 50500);  // 3.58 MHz / 72
    CHECK(peak(chip, 4800) == 0.0f);
    // Channel 0: modulator (op 0) and carrier (op 3) audible, fast attack, sustained; A-440.
    chip.write(0x20, 0x01);
    chip.write(0x23, 0x01);
    chip.write(0x40, 0x10);
    chip.write(0x43, 0x00);
    chip.write(0x60, 0xF0);
    chip.write(0x63, 0xF0);
    chip.write(0x80, 0x77);
    chip.write(0x83, 0x77);
    chip.write(0xA0, 0x41);
    chip.write(0xB0, 0x32);  // key on, block 4
    CHECK(peak(chip, 4800) > 0.05f);
}

TEST(fm_chip_ym2203_ssg_tone) {
    FmChip chip(FmChip::Type::Ym2203, 48000);
    CHECK(peak(chip, 4800) == 0.0f);
    chip.write(0x00, 0xFE);  // SSG channel A period
    chip.write(0x01, 0x00);
    chip.write(0x07, 0x3E);  // tone A on, everything else off
    chip.write(0x08, 0x0F);  // channel A volume
    CHECK(peak(chip, 4800) > 0.01f);
}

TEST(fm_chip_opl2_timer_flag) {
    FmChip chip(FmChip::Type::Ym3812, 48000);
    chip.write(0x02, 0xF0);  // timer 1: 16 x 80 us
    chip.write(0x04, 0x01);  // start timer 1
    CHECK((chip.status() & 0x40) == 0);
    std::vector<float> buf(480);  // 10 ms
    chip.render(buf.data(), static_cast<int>(buf.size()));
    CHECK((chip.status() & 0xC0) == 0xC0);  // IRQ + timer 1 flags
}
