#include "host/speaker.h"

#include <algorithm>
#include <cmath>

namespace vette::host {

void Speaker::update(uint64_t now, bool data_enable, const Pit& pit) {
    const Pit::Channel& c = pit.channel(2);
    State s;
    s.t = now;
    s.data = data_enable;
    s.gate = c.gate;
    s.mode = c.mode;
    s.reload = c.reload;
    s.start = c.start;
    pending_.push_back(s);
}

// PIT clocks within [from, to) during which the speaker input is high, for one constant state.
double Speaker::high_time(const State& s, uint64_t from, uint64_t to) {
    if (!s.data) {
        return 0;
    }
    if (!s.gate || s.mode != 3) {
        return static_cast<double>(to - from);  // channel 2 output held high
    }
    const uint64_t period = s.reload;
    const uint64_t high = (period + 1) / 2;
    auto cumulative = [&](uint64_t t) {  // high clocks in [start, t)
        const uint64_t e = t - s.start;
        return (e / period) * high + std::min(e % period, high);
    };
    return static_cast<double>(cumulative(to) - cumulative(from));
}

void Speaker::render(uint64_t to, int sample_rate, std::vector<int16_t>& out) {
    const auto rate = static_cast<uint64_t>(sample_rate);
    if (sample_rate != rate_) {  // first call or rate change: continue from where rendering stopped
        rate_ = sample_rate;
        samples_ = (rendered_to_ * rate + kPitHz - 1) / kPitHz;
    }
    constexpr double kAmplitude = 0.25 * 32767.0;
    constexpr double kHighPass = 0.995;  // ~38 Hz corner at 48 kHz: removes DC from held levels

    size_t next = 0;
    for (;;) {
        const uint64_t a = samples_ * kPitHz / rate;
        const uint64_t b = (samples_ + 1) * kPitHz / rate;
        if (b > to) {
            break;
        }
        double high = 0;
        uint64_t t = a;
        while (t < b) {
            while (next < pending_.size() && pending_[next].t <= t) {
                current_ = pending_[next++];
            }
            const uint64_t seg_end = next < pending_.size() ? std::min(b, pending_[next].t) : b;
            high += high_time(current_, t, seg_end);
            t = seg_end;
        }
        const double level = high / static_cast<double>(b - a) * 2.0 - 1.0;
        const double filtered = level - hp_prev_in_ + kHighPass * hp_prev_out_;
        hp_prev_in_ = level;
        hp_prev_out_ = filtered;
        out.push_back(static_cast<int16_t>(std::lround(std::clamp(filtered, -1.0, 1.0) * kAmplitude)));
        ++samples_;
        rendered_to_ = b;
    }
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(next));
}

} // namespace vette::host
