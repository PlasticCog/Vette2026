#pragma once
// The PC-98 version's sound as an SfxBackend for GameAudio: its FM songs, played where the PC-98
// played them (Pc98MusicCues), in place of the DOS title and winner tunes, plus the menu and loser
// songs where DOS is silent. It covers only those two tunes: the PC-98's in-game sounds were its
// beeper, with the DOS speaker's programs, so the fallback backend plays everything else.

#include <memory>
#include <string_view>
#include <vector>

#include "sound/pc98_cues.h"
#include "sound/pc98_sound.h"
#include "sound/sfx_backend.h"

namespace vette::sound {

class Pc98Backend final : public SfxBackend {
public:
    Pc98Backend(host::Machine& machine, std::unique_ptr<Pc98Sound> sound);

    // The cues start and stop the songs (the DOS tunes are shorter than the songs that replace them).
    void start(std::string_view, const SpeakerProgram*) override {}
    void stop(std::string_view) override {}
    bool playing(std::string_view id) const override;
    bool covers(std::string_view id) const override;
    void engine(const Engine&) override {}
    void render(float* out, int frames) override;  // adds

    Pc98Sound& sound() { return *sound_; }

private:
    std::unique_ptr<Pc98Sound> sound_;
    Pc98MusicCues cues_;
    std::vector<float> buf_;
};

}  // namespace vette::sound
