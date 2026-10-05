#include "sound/pc98_backend.h"

#include <cstddef>
#include <optional>
#include <utility>

#include "game/sound_events.h"

namespace vette::sound {
namespace {

std::optional<Pc98Song> song_for_id(std::string_view id) {
    const auto sfx = game::sfx_from_name(id);
    return sfx ? Pc98Sound::song_for(*sfx) : std::nullopt;
}

}  // namespace

Pc98Backend::Pc98Backend(host::Machine& machine, std::unique_ptr<Pc98Sound> sound)
    : sound_(std::move(sound)), cues_(machine) {}

bool Pc98Backend::covers(std::string_view id) const { return song_for_id(id).has_value(); }

bool Pc98Backend::playing(std::string_view id) const {
    const auto song = song_for_id(id);
    return song && sound_->playing() && sound_->current() == song;
}

void Pc98Backend::render(float* out, int frames) {
    cues_.apply(*sound_);
    buf_.resize(static_cast<size_t>(frames));
    sound_->render(buf_.data(), frames);
    for (int i = 0; i < frames; ++i) {
        out[i] += buf_[static_cast<size_t>(i)];
    }
}

}  // namespace vette::sound
