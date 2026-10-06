// The moments the DOS game passes in silence (game::SfxKind Cue and Held): each has a Mac sound and an
// AdLib instrument, the held ones play until stopped and the cues end on their own.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "assets/mac_sounds.h"
#include "game/sound_events.h"
#include "sound/adlib_sfx.h"
#include "sound/sfx_bank.h"
#include "test.h"

using vette::game::Sfx;
using vette::game::sfx_kind;
using vette::game::sfx_name;
using vette::game::SfxKind;

namespace {

float peak(vette::sound::AdlibSfx& adlib, double seconds) {
    std::vector<float> buf(static_cast<size_t>(48000 * seconds));
    adlib.render(buf.data(), static_cast<int>(buf.size()));
    float p = 0;
    for (const float x : buf) {
        p = std::max(p, std::fabs(x));
    }
    return p;
}

std::vector<Sfx> silent_moments() {
    std::vector<Sfx> out;
    for (int i = 0; i < static_cast<int>(Sfx::Count); ++i) {
        const auto s = static_cast<Sfx>(i);
        if (sfx_kind(s) == SfxKind::Cue || sfx_kind(s) == SfxKind::Held) {
            out.push_back(s);
        }
    }
    return out;
}

}  // namespace

TEST(sound_silent_moments_mac_sounds) {
    struct Want {
        Sfx sfx;
        const char* mac;
    };
    const Want want[] = {
        {Sfx::Horn, "horn"},           {Sfx::Helicopter, "heli"},       {Sfx::CountdownBeep, "beep1"},
        {Sfx::CountdownGo, "beep2"},   {Sfx::Splash, "splash"},         {Sfx::Thud, "thud"},
        {Sfx::PulledOver, "joel"},     {Sfx::IntroCableCar, "cable car bell"}, {Sfx::IntroCar, "mic"},
        {Sfx::IntroLogo, "signature"},       {Sfx::ServiceStation, "cable car bell"},
    };
    CHECK_EQ(silent_moments().size(), std::size(want));
    for (const Want& w : want) {
        const auto* use = vette::assets::mac_sound_for_dos(sfx_name(w.sfx));
        CHECK(use != nullptr);
        if (use) {
            CHECK_EQ(std::string(use->sound), std::string(w.mac));
        }
    }
    // Held: looped until the game lets go; the title's bell is the intro's (2 s on channel 1).
    CHECK(vette::assets::mac_sound_for_dos("horn")->max_seconds == 0);
    CHECK(vette::assets::mac_sound_for_dos("helicopter")->max_seconds == 0);
    const auto* bell = vette::assets::mac_sound_for_dos("intro_cable_car");
    CHECK(bell && bell->channel == 1 && std::fabs(bell->max_seconds - 2.0) < 0.01);
    // The race's use of the same bell: the service station's driveway (channel 2, 3 s).
    const auto* station = vette::assets::mac_sound_for_dos("service_station");
    CHECK(station && station->channel == 2 && std::fabs(station->max_seconds - 3.0) < 0.01);
}

TEST(sound_silent_moments_adlib) {
    const auto bank = vette::sound::SfxBank::defaults();
    for (const Sfx s : silent_moments()) {
        const char* id = sfx_name(s);
        CHECK(bank.sounds.count(id) == 1);
        vette::sound::AdlibSfx adlib(48000);
        adlib.start(id, nullptr);
        CHECK(adlib.playing(id));
        CHECK(peak(adlib, 0.15) > 0.02f);
        peak(adlib, 2.5);
        // Held sounds play on until stopped; cues end by themselves.
        CHECK(adlib.playing(id) == (sfx_kind(s) == SfxKind::Held));
        adlib.stop(id);
        peak(adlib, 2.0);
        CHECK(peak(adlib, 0.05) < 0.001f);
    }
    // The serialized bank keeps them.
    const auto again = vette::sound::SfxBank::parse(bank.serialize());
    CHECK(again == bank);
}

TEST(sound_silent_moments_helicopter_chops) {
    // The rotor: the level rises and falls about 13 times a second.
    vette::sound::AdlibSfx adlib(48000);
    adlib.start("helicopter", nullptr);
    peak(adlib, 0.2);
    std::vector<float> buf(48000);
    adlib.render(buf.data(), static_cast<int>(buf.size()));
    std::vector<float> env;  // 5 ms windows
    for (size_t i = 0; i + 240 <= buf.size(); i += 240) {
        float p = 0;
        for (size_t j = i; j < i + 240; ++j) {
            p = std::max(p, std::fabs(buf[j]));
        }
        env.push_back(p);
    }
    const float hi = *std::max_element(env.begin(), env.end());
    int rises = 0;
    bool low = false;
    for (const float e : env) {
        if (e < 0.5f * hi) {
            low = true;
        } else if (low && e > 0.8f * hi) {
            ++rises;
            low = false;
        }
    }
    CHECK(rises >= 8 && rises <= 20);
}

TEST(sound_silent_moments_horn_pitch) {
    // A car's two horns: the strongest partials at 416 and 520 Hz, a major third apart.
    vette::sound::AdlibSfx adlib(48000);
    adlib.start("horn", nullptr);
    peak(adlib, 0.1);
    std::vector<float> buf(48000 / 2);
    adlib.render(buf.data(), static_cast<int>(buf.size()));
    const auto level = [&](double hz) {  // Goertzel
        const double w = 2 * 3.141592653589793 * hz / 48000, k = 2 * std::cos(w);
        double s1 = 0, s2 = 0;
        for (const float x : buf) {
            const double s0 = x + k * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        return std::sqrt(s1 * s1 + s2 * s2 - k * s1 * s2);
    };
    const double low = std::min(level(416), level(520));
    for (const double other : {208.0, 260.0, 832.0, 1040.0, 300.0, 700.0}) {
        CHECK(low > 2 * level(other));
    }
}
