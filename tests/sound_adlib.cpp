// The AdLib sound bank (adlib.ini) and its player.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "sound/adlib_sfx.h"
#include "sound/sfx_bank.h"
#include "test.h"

using vette::sound::AdlibSfx;
using vette::sound::SfxBank;
using vette::sound::SfxVoice;
using vette::sound::SpeakerProgram;

namespace {

float peak(AdlibSfx& s, double seconds) {
    std::vector<float> buf(static_cast<size_t>(48000 * seconds));
    s.render(buf.data(), static_cast<int>(buf.size()));
    float p = 0;
    for (const float v : buf) p = std::max(p, std::fabs(v));
    return p;
}

}  // namespace

TEST(sfx_bank_volume_levels) {
    CHECK_EQ(vette::sound::volume_to_level(100), 0);
    CHECK_EQ(vette::sound::volume_to_level(50), 8);  // -6 dB
    CHECK_EQ(vette::sound::volume_to_level(0), 63);
}

TEST(sfx_bank_round_trip) {
    SfxBank bank = SfxBank::defaults();
    SfxVoice crash = bank.fallback;
    crash.pitch = SfxVoice::Pitch::Sweep;
    crash.hz = 880;
    crash.to_hz = 55;
    crash.time_ms = 400;
    crash.volume = 80;
    crash.transpose = -12;
    crash.patch.feedback = 7;
    crash.patch.modulator.wave = 3;
    crash.patch.carrier.sustained = false;
    bank.sounds["crash"] = crash;
    bank.sounds["horn"].retrigger = true;
    bank.engine.ratio = 0.25f;
    bank.engine.enabled = false;
    CHECK(SfxBank::parse(bank.serialize()) == bank);
}

TEST(sfx_bank_parse_is_tolerant) {
    const SfxBank bank = SfxBank::parse(
        "# a comment\n[engine]\nvolume = 250\nnonsense\nwhat = 3\n[siren]\npitch = wobbly\nmod.level = 99\n"
        "car.wave = 2\r\n");
    CHECK_EQ(bank.engine.volume, 100);  // clamped
    CHECK(bank.sounds.contains("siren"));
    const SfxVoice& siren = bank.sound("siren");
    CHECK(siren.pitch == SfxVoice::Pitch::Original);  // bad value: the default stays
    CHECK_EQ(int{siren.patch.modulator.level}, 63);
    CHECK_EQ(int{siren.patch.carrier.wave}, 2);
    CHECK(&bank.sound("unknown") == &bank.fallback);
}

TEST(adlib_plays_and_ends_a_program) {
    AdlibSfx adlib(48000);
    SpeakerProgram beep;
    beep.steps = {{4, 440}, {2, 0}, {4, 660}};  // about 55 + 27 + 55 ms
    CHECK(peak(adlib, 0.05) == 0.0f);
    adlib.start("beep", &beep);
    CHECK(adlib.playing("beep"));
    CHECK(peak(adlib, 0.05) > 0.05f);
    peak(adlib, 0.2);
    CHECK(!adlib.playing("beep"));  // ended
    peak(adlib, 1.0);               // release
    CHECK(peak(adlib, 0.05) < 0.001f);
}

TEST(adlib_loops_until_stopped) {
    AdlibSfx adlib(48000);
    SpeakerProgram siren;
    siren.steps = {{3, 600}, {3, 800}};
    siren.loop_to = 0;
    adlib.start("siren", &siren);
    peak(adlib, 1.0);
    CHECK(adlib.playing("siren"));
    adlib.stop("siren");
    CHECK(!adlib.playing("siren"));
    peak(adlib, 1.0);
    CHECK(peak(adlib, 0.05) < 0.001f);
}

TEST(adlib_engine_follows_the_note) {
    AdlibSfx adlib(48000);
    adlib.engine(true, 120);
    CHECK(peak(adlib, 0.1) > 0.02f);
    adlib.engine(false, 0);
    peak(adlib, 1.5);
    CHECK(peak(adlib, 0.05) < 0.001f);
}

TEST(adlib_sweep_and_fixed_lengths) {
    AdlibSfx adlib(48000);
    SfxBank bank = SfxBank::defaults();
    SfxVoice& tick = bank.sounds["tick"];
    tick = bank.fallback;
    tick.pitch = SfxVoice::Pitch::Fixed;
    tick.time_ms = 30;
    adlib.set_bank(bank);
    adlib.start("tick", nullptr);
    CHECK(peak(adlib, 0.02) > 0.05f);
    peak(adlib, 0.02);
    CHECK(!adlib.playing("tick"));
}
