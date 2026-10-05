#pragma once
// The game's sound, put together from the sources the player chose: the sound effects from the
// emulated PC speaker or a replacement (AdLib, the Mac's samples), the music (the title and winner
// tunes) from the same, from the PC-98's FM songs, or not at all. The DOS game's sound events
// (game/sound_events.h) drive the replacements, each on its own sample within a stretch of emulated
// time, and gate the speaker so it's heard only for what it still plays. Unlike the original's single
// voice, the engine keeps running under a skid or the siren: what the race asks for is played, not
// just what won the speaker. The moments the DOS game passes in silence (countdown, helicopter view,
// horn, ...: game::SfxKind Cue and Held) play only on a replacement; in the helicopter view its rotor
// replaces the engine.

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "game/sound_events.h"
#include "sound/sfx_backend.h"

namespace vette::sound {

class GameAudio {
public:
    struct Sources {
        bool effects_off = false;
        SfxBackend* effects = nullptr;   // null: the PC speaker plays them
        bool music_off = false;
        SfxBackend* music = nullptr;     // null: the tunes go with the effects
        SfxBackend* fallback = nullptr;  // plays what `effects` doesn't have (may be null)
    };
    // The backends outlive this.
    GameAudio(host::Machine& machine, int rate, const Sources& sources);

    // The sound for the emulated time [t0_ns, t0_ns + speaker.size() / rate), mono, written to `out`.
    // `speaker`: the emulated PC speaker's samples for the same stretch. Call after each run_for.
    void render(uint64_t t0_ns, const std::vector<int16_t>& speaker, std::vector<float>& out);

    // The original's PC-speaker program for a sound (empty until the game has unpacked itself).
    const SpeakerProgram* original(game::Sfx sfx);

private:
    // Where a sound plays: on the speaker, on a backend, or nowhere.
    struct Route {
        bool speaker = false;
        SfxBackend* backend = nullptr;
    };
    Route route(game::Sfx sfx) const;
    void apply(const game::SoundEvent& e);
    void continuous();
    bool speaker_open() const;  // the speaker's current sound is one it plays itself
    void mix(const std::vector<int16_t>& speaker, std::vector<float>& out, int from, int to);

    game::SoundEvents events_;
    int rate_;
    Sources src_;
    std::vector<SfxBackend*> backends_;  // each once
    std::array<std::optional<SpeakerProgram>, static_cast<size_t>(game::Sfx::Count)> programs_;
    std::vector<game::SoundEvent> pending_;
    std::optional<game::Sfx> speaker_tone_, speaker_noise_;  // what the speaker plays now
    bool muted_ = false;
};

}  // namespace vette::sound
