#include "sound/game_audio.h"

#include <algorithm>
#include <cstddef>

namespace vette::sound {
namespace {

using game::Sfx;

// Played while the race asks for them (SoundEvents::requested), whatever has the speaker; the horn and
// the helicopter have no speaker sound at all.
constexpr Sfx kContinuous[] = {Sfx::Skid, Sfx::Siren, Sfx::Horn, Sfx::Helicopter};

bool is_continuous(Sfx s) {
    return s == Sfx::Engine || s == Sfx::Skid || s == Sfx::Siren || game::sfx_kind(s) == game::SfxKind::Held;
}
bool is_music(Sfx s) { return s == Sfx::TitleTune || s == Sfx::WinTune; }
// The noise routines click the speaker directly, over whatever tone program is running.
bool is_noise(Sfx s) { return game::sfx_kind(s) == game::SfxKind::Noise; }
// What the speaker itself plays: the rest (the silent moments) only ever reaches a replacement.
bool on_speaker(Sfx s) { return game::sfx_kind(s) == game::SfxKind::Tone || is_noise(s); }

}  // namespace

GameAudio::GameAudio(host::Machine& machine, int rate, const Sources& sources)
    : events_(machine), rate_(rate), src_(sources) {
    for (SfxBackend* b : {src_.effects, src_.music, src_.fallback}) {
        if (b && std::find(backends_.begin(), backends_.end(), b) == backends_.end())
            backends_.push_back(b);
    }
}

const SpeakerProgram* GameAudio::original(Sfx sfx) {
    std::optional<SpeakerProgram>& p = programs_[static_cast<size_t>(sfx)];
    if (!p) {
        const game::SoundEvents::Program prog = events_.program(sfx);
        if (prog.steps.empty() && prog.pulses.empty())
            return nullptr;  // not readable yet
        SpeakerProgram sp;
        for (const auto& s : prog.steps)
            sp.steps.push_back({s.ticks, s.hz});
        sp.loop_to = prog.loop_to;
        p = std::move(sp);
    }
    return p->steps.empty() ? nullptr : &*p;  // noise sounds (clicks) have no notes to follow
}

GameAudio::Route GameAudio::route(Sfx sfx) const {
    const char* id = game::sfx_name(sfx);
    if (is_music(sfx)) {
        if (src_.music_off)
            return {};
        if (src_.music && src_.music->covers(id))
            return {false, src_.music};
        if (src_.effects_off)
            return {true, nullptr};  // the original tunes, though the effects are off
    }
    if (src_.effects_off)
        return {};
    if (!src_.effects)
        return {true, nullptr};
    if (src_.effects->covers(id))
        return {false, src_.effects};
    if (src_.fallback && src_.fallback->covers(id))
        return {false, src_.fallback};
    return {true, nullptr};
}

bool GameAudio::speaker_open() const {
    // A noise burst takes over the speaker while it runs; otherwise it's the tone program, or silence.
    const std::optional<Sfx> now = speaker_noise_ ? speaker_noise_ : speaker_tone_;
    return !now || route(*now).speaker;
}

void GameAudio::apply(const game::SoundEvent& e) {
    // Track what the speaker plays, for the gate.
    if (on_speaker(e.sfx)) {
        std::optional<Sfx>& slot = is_noise(e.sfx) ? speaker_noise_ : speaker_tone_;
        if (e.start)
            slot = e.sfx;
        else if (slot == e.sfx)
            slot.reset();
    }

    if (is_continuous(e.sfx) || muted_)
        return;  // see continuous()
    const Route r = route(e.sfx);
    if (!r.backend)
        return;
    const char* id = game::sfx_name(e.sfx);
    if (e.start)
        r.backend->start(id, original(e.sfx));
    else if (original(e.sfx))
        r.backend->stop(id);  // a tune cut off; noise sounds play out their own length (cues have no stop)
}

void GameAudio::continuous() {
    const game::EngineSound e = events_.engine();
    if (SfxBackend* b = route(Sfx::Engine).backend) {
        // In the helicopter view the rotor replaces the engine, as on the Mac, where the device has both.
        const bool heli = events_.requested(Sfx::Helicopter) && route(Sfx::Helicopter).backend == b;
        b->engine({e.running && !muted_ && !heli, e.pitch_hz, e.rpm, e.throttle, e.gear});
    }
    for (const Sfx s : kContinuous) {
        SfxBackend* b = route(s).backend;
        if (!b)
            continue;
        const char* id = game::sfx_name(s);
        const bool want = events_.requested(s) && !muted_;
        if (want && !b->playing(id))
            b->start(id, original(s));
        else if (!want && b->playing(id))
            b->stop(id);
    }
}

void GameAudio::mix(const std::vector<int16_t>& speaker, std::vector<float>& out, int from, int to) {
    if (to <= from)
        return;
    if (speaker_open()) {
        for (int i = from; i < to; ++i)
            out[static_cast<size_t>(i)] += speaker[static_cast<size_t>(i)] / 32768.0f;
    }
    for (SfxBackend* b : backends_)
        b->render(out.data() + from, to - from);
    if (muted_)  // the game's sound switch: the speaker is silent already; the PC-98's songs ignore it
        std::fill(out.begin() + from, out.begin() + to, 0.0f);
}

void GameAudio::render(uint64_t t0_ns, const std::vector<int16_t>& speaker, std::vector<float>& out) {
    const int frames = static_cast<int>(speaker.size());
    out.assign(speaker.size(), 0.0f);
    // The game's own sound switch (its S key) silences the replacements too.
    const bool muted = !events_.enabled();
    if (muted && !muted_) {
        for (int i = 0; i < static_cast<int>(Sfx::Count); ++i) {
            if (SfxBackend* b = route(static_cast<Sfx>(i)).backend)
                b->stop(game::sfx_name(static_cast<Sfx>(i)));
        }
    }
    muted_ = muted;

    pending_.clear();
    events_.take(pending_);
    continuous();
    // Each event lands on its own sample: mix up to it, apply it, carry on.
    int done = 0;
    for (const game::SoundEvent& e : pending_) {
        const double offset = static_cast<double>(e.t_ns > t0_ns ? e.t_ns - t0_ns : 0) * rate_ / 1e9;
        const int at = std::clamp(static_cast<int>(offset), done, frames);
        mix(speaker, out, done, at);
        done = at;
        apply(e);
    }
    mix(speaker, out, done, frames);
}

}  // namespace vette::sound
