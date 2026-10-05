#include "sound/game_audio.h"

#include <algorithm>
#include <cstddef>

namespace vette::sound {
namespace {

using game::Sfx;

// Played while the race asks for them (SoundEvents::requested), whatever has the speaker.
constexpr Sfx kContinuous[] = {Sfx::Skid, Sfx::Siren};

bool is_continuous(Sfx s) { return s == Sfx::Engine || s == Sfx::Skid || s == Sfx::Siren; }

}  // namespace

GameAudio::GameAudio(host::Machine& machine, int rate, SfxBackend& primary, SfxBackend* fallback)
    : events_(machine), rate_(rate), primary_(primary), fallback_(fallback) {}

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

SfxBackend* GameAudio::route(Sfx sfx) {
    const char* id = game::sfx_name(sfx);
    if (primary_.covers(id))
        return &primary_;
    return fallback_ && fallback_->covers(id) ? fallback_ : nullptr;
}

void GameAudio::apply(const game::SoundEvent& e) {
    if (is_continuous(e.sfx))
        return;  // see continuous()
    SfxBackend* b = route(e.sfx);
    if (!b)
        return;
    const char* id = game::sfx_name(e.sfx);
    if (e.start) {
        b->start(id, original(e.sfx));
    } else if (original(e.sfx)) {
        b->stop(id);  // a tune cut off; noise sounds play out their own length
    }
}

void GameAudio::continuous() {
    const game::EngineSound e = events_.engine();
    if (SfxBackend* b = route(Sfx::Engine))
        b->engine({e.running && !muted_, e.pitch_hz, e.rpm, e.throttle, e.gear});
    for (const Sfx s : kContinuous) {
        SfxBackend* b = route(s);
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

void GameAudio::render(uint64_t t0_ns, int frames, std::vector<float>& out) {
    out.assign(static_cast<size_t>(frames), 0.0f);
    // The game's own sound switch (its S key) silences the replacement too.
    const bool muted = !events_.enabled();
    if (muted && !muted_) {
        for (int i = 0; i < static_cast<int>(Sfx::Count); ++i) {
            if (SfxBackend* b = route(static_cast<Sfx>(i)))
                b->stop(game::sfx_name(static_cast<Sfx>(i)));
        }
    }
    muted_ = muted;

    pending_.clear();
    events_.take(pending_);
    continuous();
    // Each event lands on its own sample: render up to it, apply it, carry on.
    int done = 0;
    for (const game::SoundEvent& e : pending_) {
        const double offset = static_cast<double>(e.t_ns > t0_ns ? e.t_ns - t0_ns : 0) * rate_ / 1e9;
        const int at = std::clamp(static_cast<int>(offset), done, frames);
        if (at > done) {
            primary_.render(out.data() + done, at - done);
            if (fallback_)
                fallback_->render(out.data() + done, at - done);
            done = at;
        }
        if (!muted_)
            apply(e);
    }
    if (frames > done) {
        primary_.render(out.data() + done, frames - done);
        if (fallback_)
            fallback_->render(out.data() + done, frames - done);
    }
}

}  // namespace vette::sound
