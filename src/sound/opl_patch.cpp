#include "sound/opl_patch.h"

#include <algorithm>
#include <cmath>

#include "sound/fm_chip.h"

namespace vette::sound {
namespace {

// Register offsets of each channel's modulator; its carrier is 3 further on.
constexpr uint8_t kModulatorSlot[kOplChannels] = {0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x11, 0x12};

// The F-number base: OPL2's sample rate at the AdLib's 3.58 MHz clock.
constexpr double kOplRate = 3'579'545.0 / 72.0;

void load_operator(FmChip& chip, uint8_t slot, const OplOperator& op, int level) {
    chip.write(static_cast<uint8_t>(0x20 + slot),
               static_cast<uint8_t>((op.tremolo ? 0x80 : 0) | (op.vibrato ? 0x40 : 0) | (op.sustained ? 0x20 : 0) |
                                    (op.scale_rate ? 0x10 : 0) | (op.multiple & 0x0F)));
    chip.write(static_cast<uint8_t>(0x40 + slot),
               static_cast<uint8_t>((op.scale_level & 3) << 6 | std::clamp(level, 0, 63)));
    chip.write(static_cast<uint8_t>(0x60 + slot), static_cast<uint8_t>((op.attack & 0x0F) << 4 | (op.decay & 0x0F)));
    chip.write(static_cast<uint8_t>(0x80 + slot), static_cast<uint8_t>((op.sustain & 0x0F) << 4 | (op.release & 0x0F)));
    chip.write(static_cast<uint8_t>(0xE0 + slot), static_cast<uint8_t>(op.wave & 3));
}

}  // namespace

void load_patch(FmChip& chip, int channel, const OplPatch& patch, int extra_level) {
    const uint8_t slot = kModulatorSlot[channel];
    chip.write(0x01, 0x20);  // enable the waveform selects
    // The modulator is heard only in additive mode; otherwise its level sets the timbre, not the volume.
    load_operator(chip, slot, patch.modulator, patch.modulator.level + (patch.additive ? extra_level : 0));
    load_operator(chip, static_cast<uint8_t>(slot + 3), patch.carrier, patch.carrier.level + extra_level);
    chip.write(static_cast<uint8_t>(0xC0 + channel),
               static_cast<uint8_t>((patch.feedback & 7) << 1 | (patch.additive ? 1 : 0)));
}

OplPitch opl_pitch(double hz) {
    hz = std::clamp(hz, 0.0, 6000.0);
    for (int block = 0; block < 8; ++block) {
        const double fnum = hz * std::pow(2.0, 20 - block) / kOplRate;
        if (fnum < 1023.5) {
            return {block, static_cast<int>(std::lround(fnum))};
        }
    }
    return {7, 1023};
}

void set_frequency(FmChip& chip, int channel, double hz, bool key_on) {
    const OplPitch p = opl_pitch(hz);
    chip.write(static_cast<uint8_t>(0xA0 + channel), static_cast<uint8_t>(p.fnum & 0xFF));
    chip.write(static_cast<uint8_t>(0xB0 + channel),
               static_cast<uint8_t>((key_on ? 0x20 : 0) | p.block << 2 | (p.fnum >> 8 & 3)));
}

}  // namespace vette::sound
