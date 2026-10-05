#include "sound/adlib_sfx.h"

#include <algorithm>
#include <cmath>

namespace vette::sound {
namespace {

constexpr int kEngineChannel = 0;
constexpr double kBlockSeconds = 0.001;  // programs and sweeps advance in 1 ms steps

double semitones(double hz, float transpose) { return hz * std::exp2(transpose / 12.0); }

}  // namespace

AdlibSfx::AdlibSfx(int output_rate)
    : chip_(FmChip::Type::Ym3812, output_rate), output_rate_(output_rate), bank_(SfxBank::defaults()),
      voices_(kOplChannels) {
    load_patch(chip_, kEngineChannel, bank_.engine.patch, volume_to_level(bank_.engine.volume));
}

void AdlibSfx::set_bank(const SfxBank& bank) {
    bank_ = bank;
    load_patch(chip_, kEngineChannel, bank_.engine.patch, volume_to_level(bank_.engine.volume));
    if (engine_on_) {
        const bool on = engine_on_;
        engine_on_ = false;
        engine(on, engine_hz_);
    }
}

double AdlibSfx::note(const Voice& v, double hz) const { return semitones(hz, v.setting.transpose); }

// Key-on edges are seen only between samples, so a restart releases the note now and the next block
// strikes it again (see advance()).
void AdlibSfx::key(int channel, Voice& v, double hz, bool on, bool restart) {
    if (on && restart && v.keyed) {
        set_frequency(chip_, channel, hz, false);
        v.keyed = false;
        v.step_left = std::max(v.step_left, kBlockSeconds);
        return;  // advance() keys it on again on the next block, as the voice is active and unkeyed
    }
    set_frequency(chip_, channel, hz, on);
    v.keyed = on;
}

void AdlibSfx::start(std::string_view name, const SpeakerProgram* original) {
    const SfxVoice& setting = bank_.sound(name);
    stop(name);
    if (!setting.enabled) {
        return;
    }
    // A free channel, or the oldest sound's.
    int channel = -1;
    for (int c = 1; c < kOplChannels; ++c) {
        if (!voices_[static_cast<size_t>(c)].active && (channel < 0 || !voices_[static_cast<size_t>(c)].keyed)) {
            channel = c;
        }
    }
    if (channel < 0) {
        channel = 1;
        for (int c = 2; c < kOplChannels; ++c) {
            if (voices_[static_cast<size_t>(c)].started < voices_[static_cast<size_t>(channel)].started) {
                channel = c;
            }
        }
    }
    Voice& v = voices_[static_cast<size_t>(channel)];
    v = Voice{};
    v.name = std::string(name);
    v.setting = setting;
    v.active = true;
    v.started = ++starts_;
    if (setting.pitch == SfxVoice::Pitch::Original && original && !original->steps.empty()) {
        v.program = *original;
    } else if (setting.pitch == SfxVoice::Pitch::Original) {
        v.program.steps = {{1, setting.hz}};  // no program known: hold the bank's note
        v.program.loop_to = 0;
    }
    load_patch(chip_, channel, setting.patch, volume_to_level(setting.volume));

    if (setting.pitch == SfxVoice::Pitch::Original) {
        const SpeakerProgram::Step& first = v.program.steps.front();
        v.step_left = first.ticks / SpeakerProgram::kTickHz;
        key(channel, v, note(v, first.hz > 0 ? first.hz : setting.hz), first.hz > 0, false);
    } else {
        key(channel, v, note(v, setting.hz), true, false);
    }
}

void AdlibSfx::stop(std::string_view name) {
    for (int c = 1; c < kOplChannels; ++c) {
        Voice& v = voices_[static_cast<size_t>(c)];
        if (v.active && v.name == name) {
            set_frequency(chip_, c, 0, false);  // release; the envelope dies away on its own
            v.active = false;
            v.keyed = false;
        }
    }
}

void AdlibSfx::stop_all() {
    for (int c = 1; c < kOplChannels; ++c) {
        if (voices_[static_cast<size_t>(c)].active) {
            stop(voices_[static_cast<size_t>(c)].name);
        }
    }
}

bool AdlibSfx::playing(std::string_view name) const {
    return std::any_of(voices_.begin() + 1, voices_.end(), [&](const Voice& v) { return v.active && v.name == name; });
}

void AdlibSfx::engine(bool on, float speaker_hz) {
    on = on && bank_.engine.enabled && speaker_hz > 0;
    const double hz = semitones(speaker_hz * bank_.engine.ratio, bank_.engine.transpose);
    if (on) {
        set_frequency(chip_, kEngineChannel, hz, true);  // glides while held
    } else if (engine_on_) {
        set_frequency(chip_, kEngineChannel, semitones(engine_hz_ * bank_.engine.ratio, bank_.engine.transpose), false);
    }
    engine_on_ = on;
    engine_hz_ = speaker_hz;
}

void AdlibSfx::advance(double seconds) {
    for (int c = 1; c < kOplChannels; ++c) {
        Voice& v = voices_[static_cast<size_t>(c)];
        if (!v.active) {
            continue;
        }
        v.elapsed += seconds;
        const SfxVoice& s = v.setting;
        if (s.pitch == SfxVoice::Pitch::Original) {
            v.step_left -= seconds;
            const SpeakerProgram::Step* step = &v.program.steps[v.step];
            bool changed = false;
            while (v.step_left <= 0) {
                if (++v.step >= v.program.steps.size()) {
                    if (v.program.loop_to < 0 || static_cast<size_t>(v.program.loop_to) >= v.program.steps.size()) {
                        stop(v.name);
                        break;
                    }
                    v.step = static_cast<size_t>(v.program.loop_to);
                }
                step = &v.program.steps[v.step];
                v.step_left += std::max(step->ticks, 1) / SpeakerProgram::kTickHz;
                changed = true;
            }
            if (!v.active) {
                continue;
            }
            if (changed || (!v.keyed && step->hz > 0)) {
                key(c, v, note(v, step->hz > 0 ? step->hz : s.hz), step->hz > 0, changed && s.retrigger);
            }
            continue;
        }
        const double length = s.time_ms / 1000.0;
        if (length > 0 && v.elapsed >= length) {
            stop(v.name);
            continue;
        }
        if (s.pitch == SfxVoice::Pitch::Sweep && length > 0) {
            const double t = std::min(1.0, v.elapsed / length);
            key(c, v, note(v, s.hz * std::pow(s.to_hz / s.hz, t)), true, false);
        } else if (!v.keyed) {
            key(c, v, note(v, s.hz), true, false);
        }
    }
}

void AdlibSfx::render(float* out, int frames) {
    const int block = std::max(1, static_cast<int>(output_rate_ * kBlockSeconds));
    for (int done = 0; done < frames;) {
        const int n = std::min(block, frames - done);
        advance(static_cast<double>(n) / output_rate_);
        chip_.render(out + done, n);
        done += n;
    }
}

}  // namespace vette::sound
