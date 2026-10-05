#pragma once
// The game's sounds on an emulated AdLib: each sound is played with its SfxBank instrument, on the
// original's notes (its PC-speaker program) or the bank's own pitch, and the engine note follows the
// game's. Channel 0 is the engine's; sounds share the other eight (a new sound takes the quietest
// or oldest when all are busy).

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "sound/fm_chip.h"
#include "sound/sfx_bank.h"

namespace vette::sound {

// A DOS sound's PC-speaker program: notes of `ticks` driver ticks (72.8 Hz), 0 Hz = a rest. After the
// last step it jumps to `loop_to`, or ends if that's -1.
struct SpeakerProgram {
    static constexpr double kTickHz = 1193182.0 / 0x1000 / 4;  // the PIT's 291 Hz interrupt, every 4th
    struct Step {
        int ticks = 1;
        float hz = 0;
    };
    std::vector<Step> steps;
    int loop_to = -1;
};

class AdlibSfx {
public:
    explicit AdlibSfx(int output_rate);

    void set_bank(const SfxBank& bank);
    const SfxBank& bank() const { return bank_; }

    // Starts `name` (restarting it if it's playing). `original` is its DOS program, for Pitch::Original
    // (no program: the bank's `hz` is held instead).
    void start(std::string_view name, const SpeakerProgram* original);
    void stop(std::string_view name);  // releases it
    void stop_all();
    bool playing(std::string_view name) const;

    // The engine note: on or off, and the original's pitch.
    void engine(bool on, float speaker_hz);

    // Mono samples at the output rate, written to `out`.
    void render(float* out, int frames);

private:
    struct Voice {
        std::string name;
        SfxVoice setting;
        SpeakerProgram program;
        bool active = false;    // sounding (key on, or releasing after a note-off we track)
        bool keyed = false;
        size_t step = 0;
        double step_left = 0;   // seconds left in the current step
        double elapsed = 0;     // seconds since start
        uint64_t started = 0;   // start order, for stealing
    };

    void advance(double seconds);  // moves every voice's program on
    void key(int channel, Voice& v, double hz, bool on, bool restart);
    double note(const Voice& v, double hz) const;

    FmChip chip_;
    int output_rate_;
    SfxBank bank_;
    std::vector<Voice> voices_;  // index = channel; channel 0 unused here (the engine)
    uint64_t starts_ = 0;
    bool engine_on_ = false;
    float engine_hz_ = 0;
    std::vector<float> block_;
};

}  // namespace vette::sound
