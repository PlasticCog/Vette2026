#include "sound/mac_backend.h"

#include <algorithm>
#include <cctype>
#include <cstddef>

namespace vette::sound {
namespace {

constexpr float kRampPerSample = 1.0f / 480;  // 10 ms fades at 48 kHz, so starts and stops don't click

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

}  // namespace

MacBackend::MacBackend(std::vector<assets::MacSound> sounds, int rate) : sounds_(std::move(sounds)), rate_(rate) {}

const assets::MacSound* MacBackend::find(std::string_view name) const {
    for (const assets::MacSound& s : sounds_) {
        if (iequals(s.name, name) && !s.samples.empty())
            return &s;
    }
    return nullptr;
}

bool MacBackend::covers(std::string_view id) const {
    const assets::MacSoundUse* use = assets::mac_sound_for_dos(id);
    return use && find(use->sound);
}

void MacBackend::start(std::string_view id, const SpeakerProgram*) {
    const assets::MacSoundUse* use = assets::mac_sound_for_dos(id);
    const assets::MacSound* sound = use ? find(use->sound) : nullptr;
    if (!sound)
        return;
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [&](const Voice& v) { return v.id == id; }),
                  voices_.end());
    Voice v;
    v.id = std::string(id);
    v.sound = sound;
    v.step = use->rate / rate_;
    v.left = use->max_seconds;
    v.hold = sound->loops() && use->max_seconds == 0;  // a looped sound the game holds (siren, skid)
    v.gain = 1;
    voices_.push_back(v);
}

void MacBackend::stop(std::string_view id) {
    for (Voice& v : voices_) {
        if (v.id == id)
            v.target = 0;
    }
}

bool MacBackend::playing(std::string_view id) const {
    return std::any_of(voices_.begin(), voices_.end(), [&](const Voice& v) { return v.id == id && v.target > 0; });
}

void MacBackend::engine(const Engine& e) {
    if (e.on && !engine_) {
        if (const assets::MacSound* s = find("Engine")) {
            engine_ = Voice{"engine", s, 0, 1, 0, true, 0, 1};
        }
    }
    if (!engine_)
        return;
    engine_->target = e.on ? 1.0f : 0.0f;
    engine_->step = assets::mac_engine_rate(e.rpm) / rate_;
}

float MacBackend::next(Voice& v) {
    const assets::MacSound& s = *v.sound;
    const auto n = static_cast<double>(s.samples.size());
    if (v.pos >= n)
        return 0;
    // Linear interpolation between neighbouring samples.
    const auto i = static_cast<std::size_t>(v.pos);
    const double frac = v.pos - static_cast<double>(i);
    const double a = s.samples[i];
    const double b = i + 1 < s.samples.size() ? s.samples[i + 1] : a;
    const float out = static_cast<float>((a + (b - a) * frac) / 32768.0 * assets::kMacChannelGain) * v.gain;

    v.pos += v.step;
    if (v.hold && s.loops() && v.pos >= static_cast<double>(s.loop_end))
        v.pos -= static_cast<double>(s.loop_end - s.loop_start);
    else if (v.hold && !s.loops() && v.pos >= n)
        v.pos -= n;  // held without loop points: the whole sample, again
    if (v.gain < v.target)
        v.gain = std::min(v.target, v.gain + kRampPerSample);
    else if (v.gain > v.target)
        v.gain = std::max(v.target, v.gain - kRampPerSample);
    return out;
}

void MacBackend::render(float* out, int frames) {
    const double dt = 1.0 / rate_;
    for (Voice& v : voices_) {
        for (int i = 0; i < frames; ++i) {
            out[i] += next(v);
            if (v.left > 0) {
                v.left -= dt;
                if (v.left <= 0)
                    v.target = 0;  // the game's time limit for it
            }
        }
    }
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                 [](const Voice& v) {
                                     return v.pos >= static_cast<double>(v.sound->samples.size()) ||
                                            (v.target == 0 && v.gain == 0);
                                 }),
                  voices_.end());
    if (engine_) {
        for (int i = 0; i < frames; ++i)
            out[i] += next(*engine_);
        if (engine_->target == 0 && engine_->gain == 0)
            engine_.reset();
    }
}

}  // namespace vette::sound
