#pragma once
// PC speaker: port 61h bit 1 (data enable) ANDed with PIT channel 2's output. State changes are
// logged with PIT timestamps and integrated per audio sample, so tones are band-limited by a box
// filter and direct bit-banging (PWM sound) works too.

#include <cstdint>
#include <vector>

#include "host/pit.h"

namespace vette::host {

class Speaker {
public:
    // Call after any change to port 61h bits 0-1 or to channel 2's programming.
    void update(uint64_t now, bool data_enable, const Pit& pit);

    // Appends mono samples covering PIT time up to `to` (continuing from the previous call).
    void render(uint64_t to, int sample_rate, std::vector<int16_t>& out);

private:
    struct State {
        uint64_t t = 0;
        bool data = false;
        bool gate = false;
        uint8_t mode = 3;
        uint32_t reload = 65536;
        uint64_t start = 0;
    };

    static double high_time(const State& s, uint64_t from, uint64_t to);

    State current_;
    std::vector<State> pending_;   // changes not yet rendered, in time order
    uint64_t samples_ = 0;         // samples emitted since t=0 at the current rate
    uint64_t rendered_to_ = 0;     // PIT time covered by the samples emitted so far
    int rate_ = 0;
    double hp_prev_in_ = -1, hp_prev_out_ = 0;  // DC-blocking high-pass, starting at rest (low)
};

} // namespace vette::host
