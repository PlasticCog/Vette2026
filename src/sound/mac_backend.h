#pragma once
// The Mac version's digitized sounds in place of the DOS game's: each DOS sound plays the sample the
// Mac game uses for it (assets/mac_sounds.h), at the Mac's rate, looping and length; the engine loop is
// pitched by the revs as the Mac's Calc_RPM does. The Mac mixed three channels; this mixes as many as
// are playing.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "assets/mac_sounds.h"
#include "sound/sfx_backend.h"

namespace vette::sound {

class MacBackend final : public SfxBackend {
public:
    MacBackend(std::vector<assets::MacSound> sounds, int rate);

    void start(std::string_view id, const SpeakerProgram* original) override;
    void stop(std::string_view id) override;
    bool playing(std::string_view id) const override;
    bool covers(std::string_view id) const override;
    void engine(const Engine& e) override;
    void render(float* out, int frames) override;

private:
    struct Voice {
        std::string id;
        const assets::MacSound* sound = nullptr;
        double pos = 0;      // in samples
        double step = 1;     // samples per output sample
        double left = 0;     // seconds until the game would stop it (0: no limit)
        bool hold = false;   // loops until stopped
        float gain = 0;      // current level, ramped to `target` (no clicks)
        float target = 1;
    };

    const assets::MacSound* find(std::string_view name) const;
    float next(Voice& v);  // one output sample; false-ish voices end themselves

    std::vector<assets::MacSound> sounds_;
    int rate_;
    std::vector<Voice> voices_;
    std::optional<Voice> engine_;
};

}  // namespace vette::sound
