#pragma once
// The AdLib sound bank: for each of the game's sounds, the OPL2 instrument it's played with and how
// its pitch moves, plus the engine's voice. Stored as adlib.ini in the settings folder, edited with
// the sound editor (vette_sfx), read by the game. Sounds are named by the DOS sound list's stable ids
// (game/sound_events.h, sfx_name()).

#include <map>
#include <string>
#include <string_view>

#include "sound/opl_patch.h"

namespace vette::sound {

struct SfxVoice {
    enum class Pitch {
        Original,  // the DOS sound's own notes and timing (its PC-speaker program), on this instrument
        Fixed,     // one note, `hz`
        Sweep,     // a glide from `hz` to `to_hz`
    };
    bool enabled = true;  // off: the sound is silent with AdLib
    OplPatch patch;
    Pitch pitch = Pitch::Original;
    float transpose = 0;     // semitones (the PC speaker's notes can sit uncomfortably high)
    float hz = 440;          // Fixed, Sweep: the (starting) note
    float to_hz = 220;       // Sweep: the final note
    float time_ms = 0;       // Fixed, Sweep: length, then release (0: until the game stops the sound)
    bool retrigger = false;  // Original: restart the envelope on every note (else glide between them)
    int volume = 100;        // percent

    bool operator==(const SfxVoice&) const = default;
};

struct EngineVoice {
    bool enabled = true;
    OplPatch patch;
    float ratio = 1;      // the voice's pitch as a multiple of the original engine note
    float transpose = 0;  // semitones on top
    int volume = 70;      // percent

    bool operator==(const EngineVoice&) const = default;
};

struct SfxBank {
    EngineVoice engine;
    std::map<std::string, SfxVoice, std::less<>> sounds;
    SfxVoice fallback;  // for sounds the bank doesn't list

    const SfxVoice& sound(std::string_view name) const;

    // The built-in bank, the starting point for players' own.
    static SfxBank defaults();

    std::string serialize() const;
    // Unknown keys, sections and bad values are ignored; missing values keep the defaults().
    static SfxBank parse(std::string_view text);

    bool operator==(const SfxBank&) const = default;
};

// Volume percent as OPL2 attenuation steps (0.75 dB each), 0..63.
int volume_to_level(int percent);

}  // namespace vette::sound
