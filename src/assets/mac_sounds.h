#pragma once
// The Mac VETTE!'s digitized sounds: the 16 'INST' resources of VETTE!.Data (and any 'snd ' found),
// decoded to playable PCM with the rates, loops and uses the Mac game gives them. re/notes/08-mac.md
// has the formats, the driver and the evidence.
//
// The Mac game plays them through its own driver, resource 'BGAS' 128 in VETTE!.Data: a VBL task that
// mixes three channels into the 22254.5 Hz sound buffer of the Mac Plus/SE, writing each mixed sample
// twice, so everything plays against an 11127 Hz clock. Channel 0 steps through its sample in 16.16
// fixed point (the engine's pitch follows the revs); channels 1 and 2 step whole bytes (1 or 2).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "assets/mac_files.h"

namespace vette::assets {

inline constexpr double kMacSoundBufferHz = 15'667'200.0 / 704;  // one sample per scan line: 22254.5 Hz
inline constexpr double kMacTickHz = kMacSoundBufferHz / 370;     // 370 lines per frame: 60.15 Hz
inline constexpr double kMacMixHz = kMacSoundBufferHz / 2;        // the driver's mix rate: 11127.3 Hz

struct MacSound {
    std::uint32_t type = 0;  // 'INST' or 'snd '
    std::int16_t id = 0;
    std::string name;                   // the resource's name ("Engine", "horn", "Opening song")
    std::vector<std::int16_t> samples;  // mono; 8-bit sources as (byte - 128) << 8
    int bits = 8;                       // the stored sample size
    double rate = kMacMixHz;            // Hz the Mac game plays it at (the engine: at a step of 1.0)
    double native_rate = kMacMixHz;     // Hz it was recorded at, from the header
    // The note at which the sample plays at native_rate. INST: the driver's note numbers, where 37
    // means kMacMixHz (25 marks a 22 kHz recording); 'snd ': MIDI (60 = middle C).
    int base_note = 37;
    std::size_t loop_start = 0, loop_end = 0;  // loops over [loop_start, loop_end) when loop_end > loop_start
    // INST only: false when the 8 header bytes are leftover sample data (4 of VETTE!'s 16). The driver
    // skips them anyway; such sounds have no loop and play at kMacMixHz.
    bool header_valid = true;

    bool loops() const { return loop_end > loop_start; }
    double seconds() const { return rate > 0 ? static_cast<double>(samples.size()) / rate : 0; }
};

// One INST resource: an 8-byte header (loop start, loop end, base note, flags, sample count; all
// big-endian words/bytes) and unsigned 8-bit samples. nullopt (with `error`) if it's too short or
// HCOM-compressed (the driver can expand HCOM; VETTE!'s sounds don't use it).
std::optional<MacSound> decode_inst(std::span<const std::uint8_t> inst, std::int16_t id, std::string_view name,
                                    std::string* error = nullptr);
// A 'snd ' resource (format 1 or 2, standard or extended sound header; compressed ones are refused).
std::optional<MacSound> decode_snd(std::span<const std::uint8_t> snd, std::int16_t id, std::string_view name,
                                   std::string* error = nullptr);

// Every INST and 'snd ' of a resource fork, in resource order. Each INST's `rate` is set from its use
// in the game (mac_sound_uses), else its native rate.
std::vector<MacSound> decode_mac_sounds(const ResourceFork& fork, std::vector<std::string>* problems = nullptr);

// VETTE!.Data's resource fork (the B&W and Color folders each hold an identical copy), or failing that
// the first file whose resource fork has INST resources.
std::optional<ResourceFork> find_vette_data(const MacFiles& files);

// How the Mac game uses a sound: one entry per call site group (CODE resources of Color VETTE! 1.02).
struct MacSoundUse {
    const char* sound;    // the name the game loads it by; the resource names match ignoring case
    const char* when;     // what makes the game play it
    int channel;          // the driver channel: 0 pitched, 1 and 2 whole-byte steps
    double rate;          // Hz; the race engine's is its idle rate (it follows mac_engine_rate)
    double max_seconds;   // the game stops it after this long; 0 = held until the game stops it
    const char* dos_sfx;  // the DOS sound this use stands in for (game/sound_events.h sfx_name), or null
};
std::span<const MacSoundUse> mac_sound_uses();
const MacSoundUse* mac_sound_use(std::string_view sound);       // its first use, ignoring case
const MacSoundUse* mac_sound_for_dos(std::string_view dos_sfx);  // the Mac sound for a DOS sound

// The race engine: the Mac's Calc_RPM pitches the "engine" sample (on channel 0) each frame to
// step = 15000 + 10 * rpm (16.16), at least 27000 (idle, up to 1200 rpm) and at most 85000 (from 7000
// rpm). Returns the sample rate that gives, 4584 Hz at idle to 14432 Hz.
double mac_engine_rate(double rpm);

// The driver's mix of its 3 channels at the game's volume (300): each channel's offset from 128 is
// scaled by 100/128 and the sum clipped to 8 bits. As a gain on a channel's own full scale:
inline constexpr double kMacChannelGain = 100.0 / 128;

}  // namespace vette::assets
