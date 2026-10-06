// The DOS sound list and the decoding of the original's PC-speaker driver (game/sound_events.h), on
// synthetic data. The run against the real game is in game_sound_game.cpp.

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "game/sound_events.h"
#include "test.h"

using vette::game::decode_sequence;
using vette::game::divisor_hz;
using vette::game::engine_pitch_tick;
using vette::game::engine_slide;
using vette::game::NoiseParams;
using vette::game::noise_pulses;
using vette::game::Sfx;
using vette::game::sfx_from_name;
using vette::game::sfx_name;

namespace {

// A code segment with driver sequences written at chosen offsets.
struct Code {
    std::map<uint16_t, uint16_t> words;
    void seq(uint16_t at, std::vector<uint16_t> ws) {
        for (const uint16_t w : ws) {
            words[at] = w;
            at = static_cast<uint16_t>(at + 2);
        }
    }
    uint16_t operator()(uint16_t off) const {
        const auto it = words.find(off);
        return it == words.end() ? 0x1234 : it->second;  // junk where nothing was written
    }
};

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

} // namespace

TEST(game_sound_names_round_trip) {
    std::set<std::string> seen;
    for (int i = 0; i < static_cast<int>(Sfx::Count); ++i) {
        const auto sfx = static_cast<Sfx>(i);
        const char* name = sfx_name(sfx);
        CHECK(name != nullptr);
        if (!name) {
            continue;
        }
        const std::string s = name;
        CHECK(!s.empty());
        for (const char c : s) {
            CHECK(std::islower(static_cast<unsigned char>(c)) || c == '_');
        }
        CHECK(seen.insert(s).second);  // unique
        const auto back = sfx_from_name(s);
        CHECK(back.has_value());
        CHECK(back && *back == sfx);
    }
    CHECK(sfx_name(Sfx::Count) == nullptr);
    CHECK(!sfx_from_name("").has_value());
    CHECK(!sfx_from_name("Engine").has_value());
    CHECK(!sfx_from_name("engine ").has_value());
    CHECK(!sfx_from_name("klaxon").has_value());
    CHECK_EQ(std::string(sfx_name(Sfx::Engine)), std::string("engine"));
    CHECK_EQ(std::string(sfx_name(Sfx::HitPedestrian)), std::string("hit_pedestrian"));
}

TEST(game_sound_divisor_hz) {
    CHECK(near(divisor_hz(0x1000), 1193181.8 / 4096, 1e-3));
    CHECK(near(divisor_hz(1), 1193181.8, 0.5));
    CHECK(near(divisor_hz(0), 1193181.8 / 65536, 1e-4));  // 0 counts as 65536 on the PIT
}

TEST(game_sound_decode_one_shot) {
    Code code;
    code.seq(0x100, {10, 0x1000, 5, 0xFFFF, 20, 0x0800, 0xFFFF});
    const auto p = decode_sequence(code, 0x100);
    CHECK_EQ(p.steps.size(), size_t{3});
    CHECK_EQ(p.loop_to, -1);
    CHECK(p.pulses.empty());
    if (p.steps.size() == 3) {
        CHECK_EQ(p.steps[0].ticks, uint16_t{10});
        CHECK(near(p.steps[0].hz, divisor_hz(0x1000), 1e-3));
        CHECK_EQ(p.steps[1].ticks, uint16_t{5});
        CHECK(p.steps[1].hz == 0.0f);  // FFFFh: rest
        CHECK_EQ(p.steps[2].ticks, uint16_t{20});
    }
}

TEST(game_sound_decode_loop) {
    // Three entries, then a jump back by 8 bytes: to the second entry. The jump entry costs the driver a
    // tick, during which the last note goes on.
    Code code;
    code.seq(0x200, {3, 0x0900, 2, 0x0A00, 4, 0x0B00, 0, 8});
    const auto p = decode_sequence(code, 0x200);
    CHECK_EQ(p.steps.size(), size_t{3});
    CHECK_EQ(p.loop_to, 1);
    if (p.steps.size() == 3) {
        CHECK_EQ(p.steps[0].ticks, uint16_t{3});
        CHECK_EQ(p.steps[2].ticks, uint16_t{5});
    }

    // Back to the start: the whole sequence repeats.
    Code whole;
    whole.seq(0x300, {28, 0x0700, 28, 0x0900, 0, 8});
    const auto q = decode_sequence(whole, 0x300);
    CHECK_EQ(q.loop_to, 0);
    CHECK_EQ(q.steps.size(), size_t{2});

    // A jump that doesn't land on an entry of this sequence: treated as the end.
    Code odd;
    odd.seq(0x400, {7, 0x0700, 0, 2});
    const auto r = decode_sequence(odd, 0x400);
    CHECK_EQ(r.loop_to, -1);
    CHECK_EQ(r.steps.size(), size_t{1});
    if (!r.steps.empty()) {
        CHECK_EQ(r.steps[0].ticks, uint16_t{7});
    }
}

TEST(game_sound_decode_edges) {
    Code empty;
    empty.seq(0x10, {0xFFFF, 0xFFFF});
    const auto p = decode_sequence(empty, 0x10);
    CHECK(p.steps.empty());
    CHECK_EQ(p.loop_to, -1);

    // No terminator: decoding gives up after 256 entries instead of running through memory.
    const auto runaway = decode_sequence([](uint16_t) { return uint16_t{1}; }, 0);
    CHECK_EQ(runaway.steps.size(), size_t{256});
    CHECK_EQ(runaway.loop_to, -1);
}

TEST(game_sound_engine_slide) {
    // Revs up: the divisor falls (pitch rises), rate 5.
    CHECK_EQ(engine_slide(30, 29, 0), uint16_t{0x8005});
    CHECK_EQ(engine_slide(70, 10, 0x40), uint16_t{0x8005});
    // Steady at 2000 rpm or more: hold.
    CHECK_EQ(engine_slide(20, 20, 0x8005), uint16_t{0});
    CHECK_EQ(engine_slide(60, 60, 0x40), uint16_t{0});
    // Steady below 2000 rpm, or revs down up to 5300 rpm: fall at 40h per tick.
    CHECK_EQ(engine_slide(11, 11, 0), uint16_t{0x40});
    CHECK_EQ(engine_slide(19, 19, 0x8005), uint16_t{0x40});
    CHECK_EQ(engine_slide(40, 45, 0x8005), uint16_t{0x40});
    CHECK_EQ(engine_slide(53, 54, 0), uint16_t{0x40});
    // Revs down above 5300 rpm: the slide stays as it was.
    CHECK_EQ(engine_slide(54, 55, 0x8005), uint16_t{0x8005});
    CHECK_EQ(engine_slide(70, 72, 0), uint16_t{0});
    CHECK_EQ(engine_slide(70, 72, 0x40), uint16_t{0x40});
}

TEST(game_sound_engine_pitch_tick) {
    // Rising: the divisor drops by rate * (85 - revs).
    CHECK_EQ(engine_pitch_tick(0x8000, 0x8005, 11), uint16_t{0x8000 - 5 * 74});
    CHECK_EQ(engine_pitch_tick(0x2000, 0x8005, 60), uint16_t{0x2000 - 5 * 25});
    // Top limits: 1400h, or 1500h up to 5300 rpm.
    CHECK_EQ(engine_pitch_tick(0x13FF, 0x8005, 60), uint16_t{0x13FF});
    CHECK_EQ(engine_pitch_tick(0x1450, 0x8005, 60), uint16_t{0x1450 - 125});
    CHECK_EQ(engine_pitch_tick(0x14FF, 0x8005, 53), uint16_t{0x14FF});
    CHECK_EQ(engine_pitch_tick(0x1500, 0x8005, 53), uint16_t{0x1500 - 5 * 32});
    // Falling: up by the rate until past B000h; a divisor already above it stays.
    CHECK_EQ(engine_pitch_tick(0x9000, 0x40, 11), uint16_t{0x9040});
    CHECK_EQ(engine_pitch_tick(0xB000, 0x40, 11), uint16_t{0xB040});
    CHECK_EQ(engine_pitch_tick(0xB001, 0x40, 11), uint16_t{0xB001});
    CHECK_EQ(engine_pitch_tick(0xDF00, 0x40, 11), uint16_t{0xDF00});
    // Hold.
    CHECK_EQ(engine_pitch_tick(0x3000, 0, 40), uint16_t{0x3000});
    // The multiply is 16-bit: revs above 85 would wrap (not reachable, redline is at most 72).
    CHECK_EQ(engine_pitch_tick(0x8000, 0x8005, 86), uint16_t{0x8000 + 5});
}

TEST(game_sound_noise_pulses) {
    // Crash-like: a fixed off-time that grows by a step per click.
    NoiseParams crash;
    crash.clicks = 26;
    crash.mask = 0x7FF;
    crash.off_start = 3000;
    crash.off_step = 100;
    const double hz = 12e6;
    const auto a = noise_pulses(crash, hz, 5);
    const auto b = noise_pulses(crash, hz, 5);
    CHECK_EQ(a.size(), size_t{26});
    CHECK(a.size() == b.size());
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        CHECK(a[i].on_us == b[i].on_us && a[i].off_us == b[i].off_us);  // deterministic per seed
    }
    for (size_t i = 1; i < a.size(); ++i) {
        // Each off-wait is 100 LOOPs (10 cycles each) longer than the one before.
        CHECK(near(a[i].off_us - a[i - 1].off_us, 100 * 10 / 12.0, 0.01));
    }
    double total_ms = 0;
    for (const auto& p : a) {
        CHECK(p.on_us > 0 && p.on_us < 65536 * 10 / 12.0 + 10);
        total_ms += (p.on_us + p.off_us) / 1000;
    }
    CHECK(total_ms > 80 && total_ms < 160);  // about 0.1 s at 12 MHz

    // Grind-like: the off-time follows the on-time.
    NoiseParams grind;
    grind.clicks = 41;
    grind.mask = 0x3FF;
    grind.off_step = 10;
    grind.off_from_on = true;
    const auto g = noise_pulses(grind, hz, 9);
    CHECK_EQ(g.size(), size_t{41});
    for (const auto& p : g) {
        if (p.on_us < 1000) {  // not the 65536-LOOP case
            CHECK(near(p.off_us - p.on_us, (10 * 10 + 15 - 70) / 12.0, 0.01));
        }
    }
    CHECK(noise_pulses(NoiseParams{}, hz).empty());
}

TEST(game_sound_kinds) {
    using vette::game::sfx_kind;
    using vette::game::SfxKind;
    // The DOS game's own sounds come first: tones and noises; then the silent moments.
    for (const Sfx s : {Sfx::Engine, Sfx::GarageRev, Sfx::Skid, Sfx::Siren, Sfx::TitleTune, Sfx::WinTune}) {
        CHECK(sfx_kind(s) == SfxKind::Tone);
    }
    for (const Sfx s : {Sfx::Crash, Sfx::CrashCar, Sfx::CrashRail, Sfx::HitPedestrian, Sfx::GearGrind}) {
        CHECK(sfx_kind(s) == SfxKind::Noise);
    }
    CHECK(sfx_kind(Sfx::Horn) == SfxKind::Held && sfx_kind(Sfx::Helicopter) == SfxKind::Held);
    for (const Sfx s : {Sfx::CountdownBeep, Sfx::CountdownGo, Sfx::Splash, Sfx::Thud, Sfx::PulledOver,
                        Sfx::IntroCableCar, Sfx::IntroCar, Sfx::IntroLogo, Sfx::ServiceStation}) {
        CHECK(sfx_kind(s) == SfxKind::Cue);
    }
    // The ids stay where they were (sound banks and settings name them).
    static_assert(static_cast<int>(Sfx::GearGrind) == 10 && static_cast<int>(Sfx::Horn) == 11);
    static_assert(vette::game::kHornScancode == 0x2D);  // X
    CHECK_EQ(std::string(sfx_name(Sfx::Helicopter)), std::string("helicopter"));
    CHECK_EQ(std::string(sfx_name(Sfx::IntroLogo)), std::string("intro_logo"));
    CHECK_EQ(std::string(sfx_name(Sfx::ServiceStation)), std::string("service_station"));
    static_assert(static_cast<int>(Sfx::ServiceStation) == 21);
}

TEST(game_sound_thud) {
    using vette::game::CarMotion;
    using vette::game::is_thud;
    const CarMotion flat{0, 0, 600, false};
    // Onto an uphill (pitch 0 -> +7, the car's nose up) and off a downhill (-7 -> 0): the suspension
    // takes the jolt.
    CHECK(is_thud(flat, CarMotion{1, 7, 600, false}));
    CHECK(is_thud(CarMotion{100, -7, 600, false}, CarMotion{90, 0, 600, false}));
    // Over a crest or into a downhill the load comes off instead.
    CHECK(!is_thud(CarMotion{224, 7, 600, false}, CarMotion{224, 0, 600, false}));
    CHECK(!is_thud(flat, CarMotion{0, -7, 600, false}));
    // A step in height, either way.
    CHECK(is_thud(flat, CarMotion{40, 0, 600, false}));
    CHECK(is_thud(CarMotion{224, 0, 600, false}, CarMotion{0, 0, 600, false}));
    CHECK(!is_thud(flat, CarMotion{9, 0, 600, false}));  // a slope's climb in one frame
    // Too slow, or the highway (entering and leaving it moves the car).
    CHECK(!is_thud(flat, CarMotion{1, 7, 139, false}));
    CHECK(is_thud(flat, CarMotion{1, 7, 140, false}));
    CHECK(!is_thud(CarMotion{224, 0, 800, false}, CarMotion{7, 0, 800, true}));
    CHECK(!is_thud(CarMotion{7, 0, 800, true}, CarMotion{224, 0, 800, false}));
}
