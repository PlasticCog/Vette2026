#pragma once
// The game's sound from a replacement source instead of the PC speaker. The DOS game's sound events
// (game/sound_events.h) drive the chosen backend, timed to the sample within each stretch of
// emulated time; sounds it doesn't have go to a fallback. Unlike the original's single voice, the
// engine keeps running under a skid or the siren: what the race asks for is played, not just what
// won the speaker.

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "game/sound_events.h"
#include "sound/sfx_backend.h"

namespace vette::sound {

class GameAudio {
public:
    // `primary` plays the sounds it covers, `fallback` (may be null) the others. Both outlive this.
    GameAudio(host::Machine& machine, int rate, SfxBackend& primary, SfxBackend* fallback);

    // The sound for the emulated time [t0_ns, t0_ns + frames / rate), mono, written to `out`. Call
    // after each Machine::run_for with the time it started at.
    void render(uint64_t t0_ns, int frames, std::vector<float>& out);

    // The original's PC-speaker program for a sound (empty until the game has unpacked itself).
    const SpeakerProgram* original(game::Sfx sfx);

private:
    SfxBackend* route(game::Sfx sfx);
    void apply(const game::SoundEvent& e);
    void continuous();

    game::SoundEvents events_;
    int rate_;
    SfxBackend& primary_;
    SfxBackend* fallback_;
    std::array<std::optional<SpeakerProgram>, static_cast<size_t>(game::Sfx::Count)> programs_;
    std::vector<game::SoundEvent> pending_;
    bool muted_ = false;
};

}  // namespace vette::sound
