#pragma once
// A replacement for the DOS game's PC-speaker sound: AdLib FM, the PC-98's YM2203, the Mac's samples.
// The game's sounds arrive by name (game/sound_events.h, sfx_name()); the engine note continuously.

#include <string_view>

#include "sound/adlib_sfx.h"

namespace vette::sound {

class SfxBackend {
public:
    virtual ~SfxBackend() = default;

    // Starts a sound (restarting it if it's playing). `original`: its PC-speaker program, if it has one.
    virtual void start(std::string_view id, const SpeakerProgram* original) = 0;
    // The game stopped it, or cut it off.
    virtual void stop(std::string_view id) = 0;
    virtual bool playing(std::string_view id) const = 0;
    // False for a sound this backend has nothing for (another plays it instead).
    virtual bool covers(std::string_view id) const = 0;

    struct Engine {
        bool on = false;
        float hz = 0;   // the original's engine note
        float rpm = 0;  // the revs
        bool throttle = false;
        int gear = 0;   // 0 neutral, -1 reverse
    };
    virtual void engine(const Engine& e) = 0;

    // Mono samples at the output rate, added to `out`.
    virtual void render(float* out, int frames) = 0;
};

// AdLib: the sound bank's instruments on an emulated YM3812.
class AdlibBackend final : public SfxBackend {
public:
    explicit AdlibBackend(int rate) : adlib_(rate) {}
    AdlibSfx& adlib() { return adlib_; }

    void start(std::string_view id, const SpeakerProgram* original) override { adlib_.start(id, original); }
    void stop(std::string_view id) override { adlib_.stop(id); }
    bool playing(std::string_view id) const override { return adlib_.playing(id); }
    bool covers(std::string_view) const override { return true; }  // the bank's fallback plays the rest
    void engine(const Engine& e) override { adlib_.engine(e.on, e.hz); }
    void render(float* out, int frames) override;

private:
    AdlibSfx adlib_;
    std::vector<float> buf_;
};

}  // namespace vette::sound
