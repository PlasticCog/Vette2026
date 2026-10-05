#pragma once
// An AdLib (YM3812 / OPL2) instrument: two operators, a modulator and a carrier, as the chip's
// registers hold them, and how to load one into a channel and play a frequency on it.

#include <cstdint>

namespace vette::sound {

class FmChip;

struct OplOperator {
    bool tremolo = false;      // AM: amplitude vibrato
    bool vibrato = false;      // VIB: frequency vibrato
    bool sustained = true;     // EG-TYP: hold at the sustain level while the key is down
    bool scale_rate = false;   // KSR: faster envelopes at higher pitches
    uint8_t multiple = 1;      // MULT 0..15: frequency multiplier (0 = x0.5)
    uint8_t scale_level = 0;   // KSL 0..3: quieter at higher pitches
    uint8_t level = 0;         // TL 0..63: attenuation, 0.75 dB per step (0 = loudest)
    uint8_t attack = 15;       // AR 0..15 (15 = fastest)
    uint8_t decay = 0;         // DR 0..15
    uint8_t sustain = 0;       // SL 0..15: 3 dB per step below the peak (0 = no drop)
    uint8_t release = 7;       // RR 0..15
    uint8_t wave = 0;          // WS 0..3: sine, half sine, absolute sine, quarter sine

    bool operator==(const OplOperator&) const = default;
};

struct OplPatch {
    OplOperator modulator;
    OplOperator carrier;
    uint8_t feedback = 0;   // FB 0..7: the modulator feeding itself
    bool additive = false;  // CON: both operators heard (else the modulator modulates the carrier)

    bool operator==(const OplPatch&) const = default;
};

// The chip's 9 melodic channels.
constexpr int kOplChannels = 9;

// Writes the patch to `channel`; `extra_level` (0..63) attenuates the audible operators further
// (a volume control that keeps the timbre).
void load_patch(FmChip& chip, int channel, const OplPatch& patch, int extra_level = 0);

// Sets the channel's frequency (Hz, clamped to what OPL2 can play) and its key on or off. Changing
// the frequency with the key held glides without restarting the envelope.
void set_frequency(FmChip& chip, int channel, double hz, bool key_on);

// The block and F-number OPL2 plays `hz` with (F-number = hz * 2^(20 - block) / 49716 Hz).
struct OplPitch {
    int block = 0;   // 0..7
    int fnum = 0;    // 0..1023
};
OplPitch opl_pitch(double hz);

}  // namespace vette::sound
