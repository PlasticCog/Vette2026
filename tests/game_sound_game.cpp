// SoundEvents against the real game: a scripted session (title, garage rev, race, missed shifts, a
// crash into traffic) must produce the expected events, the native engine-note model must match the
// original's at every step, and the observer must not change the run. Skipped when the game files are
// missing (Game/VETTE.EXE, or the folder in VETTE_GAME_DIR).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "game/options.h"
#include "game/sound_events.h"
#include "game/x86.h"
#include "host/machine.h"
#include "test.h"

namespace fs = std::filesystem;
using vette::game::Sfx;
using vette::game::SoundEvent;
using vette::game::SoundEvents;
using vette::host::Cpu;
using vette::host::Machine;

namespace {

constexpr uint16_t kCode = vette::game::emu_seg(0x3009);

fs::path game_dir() {
#ifdef _MSC_VER
    char* value = nullptr;
    size_t len = 0;
    std::string env;
    if (_dupenv_s(&value, &len, "VETTE_GAME_DIR") == 0 && value) env = value;
    std::free(value);
#else
    const char* value = std::getenv("VETTE_GAME_DIR");
    const std::string env = value ? value : "";
#endif
    if (!env.empty()) {
        return env;
    }
    return fs::path(__FILE__).parent_path().parent_path() / "Game";
}

std::unique_ptr<Machine> boot(const fs::path& dir) {
    vette::host::MachineConfig config;
    config.game_dir = dir;
    config.save_dir = fs::temp_directory_path() / "vette2026_sound_test";
    config.start_time = vette::host::RealTime{1989, 10, 23, 12, 0, 0, 0};
    auto m = std::make_unique<Machine>(config);
    std::string error;
    if (!m->boot(error)) {
        std::printf("  boot failed: %s\n", error.c_str());
        return nullptr;
    }
    vette::game::install_skip_manual_check(m->cpu());
    return m;
}

struct Key {
    double at;
    uint8_t scancode;
};

// Title (skipped with Space), garage: rev the engine (Space), then the menus to course 1 at TRAINEE.
// In the race: automatic gearbox and 1st gear, full throttle, right onto the Great Highway, reverse
// while moving (grind), 1st gear at speed (grind), then a left turn into traffic.
std::vector<Key> script() {
    std::vector<Key> keys;
    const auto tap = [&](double t, uint8_t sc) {
        keys.push_back({t, sc});
        keys.push_back({t + 0.1, static_cast<uint8_t>(sc | 0x80)});
    };
    const auto hold = [&](double a, double b, uint8_t sc) {
        keys.push_back({a, sc});
        keys.push_back({b, static_cast<uint8_t>(sc | 0x80)});
    };
    tap(13, 0x39);
    tap(15, 0x39);
    for (const double t : {17.0, 21.0, 25.0, 30.0}) {
        tap(t, 0x1C);
    }
    tap(37, 0x1E);
    tap(37.3, 0x02);
    hold(37.5, 45, 0x48);
    hold(38.5, 39.3, 0x4D);
    tap(41, 0x13);
    tap(43.5, 0x02);
    hold(44, 46, 0x4B);
    std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.at < b.at; });
    return keys;
}

// Runs the script for `seconds`; calls `each_ms` after every emulated millisecond.
template <typename F>
void run(Machine& m, double seconds, F each_ms) {
    const auto keys = script();
    size_t next = 0;
    for (int ms = 0; ms < static_cast<int>(seconds * 1000) && !m.stopped(); ++ms) {
        while (next < keys.size() && keys[next].at * 1000 <= ms) {
            m.key(keys[next++].scancode);
        }
        m.run_for(1'000'000);
        each_ms();
    }
}

} // namespace

TEST(game_sound_real_game_session) {
    const fs::path dir = game_dir();
    if (!fs::exists(dir / "VETTE.EXE")) {
        std::printf("  SKIPPED: no VETTE.EXE in %s\n", dir.string().c_str());
        return;
    }
    auto m = boot(dir);
    CHECK(m != nullptr);
    if (!m) {
        return;
    }
    SoundEvents sound(*m);

    // The native engine-note model, checked at every call of the original's two routines.
    int slide_checks = 0, tick_checks = 0, slide_bad = 0, tick_bad = 0;
    uint16_t want_slide = 0, want_last = 0, want_div = 0;
    Cpu& cpu = m->cpu();
    vette::host::Memory& mem = m->memory();
    using vette::game::rd16;
    const auto w1 = cpu.add_watch(Cpu::linear(kCode, 0x93EA), [&](Cpu&) {  // snd_engine_set_target
        want_last = rd16(mem, kCode, 0x5891);
        want_slide = vette::game::engine_slide(want_last, rd16(mem, kCode, 0x9273), rd16(mem, kCode, 0x92BA));
    });
    const auto w2 = cpu.add_watch(Cpu::linear(kCode, 0x9421), [&](Cpu&) {  // ... slide and revs stored
        ++slide_checks;
        slide_bad += rd16(mem, kCode, 0x92BA) != want_slide || rd16(mem, kCode, 0x9273) != want_last;
    });
    const auto w3 = cpu.add_watch(Cpu::linear(kCode, 0x9428), [&](Cpu&) {  // snd_engine_pitch_tick
        want_div = vette::game::engine_pitch_tick(rd16(mem, kCode, 0x92A8), rd16(mem, kCode, 0x92BA),
                                                  rd16(mem, kCode, 0x5891));
    });
    const auto w4 = cpu.add_watch(Cpu::linear(kCode, 0x946F), [&](Cpu&) {  // ... its RET
        ++tick_checks;
        tick_bad += rd16(mem, kCode, 0x92A8) != want_div;
    });

    std::vector<SoundEvent> events;
    bool engine_seen_running = false, engine_pitch_ok = true;
    run(*m, 50, [&] {
        sound.take(events);
        const auto e = sound.engine();
        if (e.running) {
            engine_seen_running = true;
            engine_pitch_ok = engine_pitch_ok && e.pitch_hz > 15 && e.pitch_hz < 260 && e.rpm >= e.idle_rpm &&
                              e.rpm <= e.redline_rpm && (!e.on || e.speaker_hz > 15);
        }
    });
    for (const auto id : {w1, w2, w3, w4}) {
        cpu.remove_watch(id);
    }

    CHECK(slide_checks > 100);
    CHECK(tick_checks > 1000);
    CHECK_EQ(slide_bad, 0);
    CHECK_EQ(tick_bad, 0);
    CHECK(engine_seen_running);
    CHECK(engine_pitch_ok);
    CHECK(sound.enabled());

    // Events: in time order, each stop after a start of the same sound, one tone at a time.
    std::vector<int> starts(static_cast<size_t>(Sfx::Count));
    std::vector<bool> on(static_cast<size_t>(Sfx::Count));
    uint64_t last_t = 0;
    int tones_on = 0;
    const auto is_tone = [](Sfx s) { return s <= Sfx::WinTune; };
    for (const auto& e : events) {
        const auto i = static_cast<size_t>(e.sfx);
        CHECK(e.t_ns >= last_t);
        last_t = e.t_ns;
        CHECK(e.start != on[i]);
        on[i] = e.start;
        starts[i] += e.start ? 1 : 0;
        if (is_tone(e.sfx)) {
            tones_on += e.start ? 1 : -1;
            CHECK(tones_on <= 1);
        }
    }
    CHECK_EQ(starts[static_cast<size_t>(Sfx::TitleTune)], 1);
    CHECK_EQ(starts[static_cast<size_t>(Sfx::GarageRev)], 1);
    CHECK(starts[static_cast<size_t>(Sfx::Engine)] >= 1);
    CHECK_EQ(starts[static_cast<size_t>(Sfx::GearGrind)], 2);
    CHECK(starts[static_cast<size_t>(Sfx::CrashCar)] >= 1);
    CHECK_EQ(starts[static_cast<size_t>(Sfx::Siren)], 0);  // TRAINEE: no police

    // The title tune plays out: 9 notes, 640 ticks.
    for (size_t i = 0; i + 1 < events.size(); ++i) {
        if (events[i].sfx == Sfx::TitleTune && events[i].start) {
            const double ticks = static_cast<double>(events[i + 1].t_ns - events[i].t_ns) * 1e-9 *
                                 vette::game::kSoundTickHz;
            CHECK(events[i + 1].sfx == Sfx::TitleTune && !events[i + 1].start);
            CHECK(ticks > 639 && ticks < 642);
        }
    }

    // Programs, read from the running game.
    const auto title = sound.program(Sfx::TitleTune);
    CHECK_EQ(title.steps.size(), size_t{9});
    CHECK_EQ(title.loop_to, -1);
    unsigned title_ticks = 0;
    for (const auto& s : title.steps) {
        title_ticks += s.ticks;
    }
    CHECK_EQ(title_ticks, 640u);
    const auto siren = sound.program(Sfx::Siren);
    CHECK_EQ(siren.steps.size(), size_t{2});
    CHECK_EQ(siren.loop_to, 0);
    const auto skid = sound.program(Sfx::Skid);
    CHECK_EQ(skid.steps.size(), size_t{4});
    CHECK_EQ(skid.loop_to, 0);
    const auto win = sound.program(Sfx::WinTune);
    CHECK(win.steps.size() > 4);
    CHECK_EQ(win.loop_to, 0);
    const auto engine = sound.program(Sfx::Engine);
    CHECK_EQ(engine.steps.size(), size_t{1});
    CHECK_EQ(engine.loop_to, 0);
    const auto rev = sound.program(Sfx::GarageRev);
    CHECK_EQ(rev.steps.size(), size_t{1});
    CHECK(!rev.steps.empty() && rev.steps[0].hz > 200 && rev.steps[0].hz < 300);
    for (const Sfx s : {Sfx::Crash, Sfx::CrashCar, Sfx::CrashRail, Sfx::HitPedestrian}) {
        const auto p = sound.program(s);
        CHECK(p.steps.empty());
        CHECK_EQ(p.pulses.size(), size_t{26});
    }
    CHECK_EQ(sound.program(Sfx::GearGrind).pulses.size(), size_t{41});
}

TEST(game_sound_observer_changes_nothing) {
    const fs::path dir = game_dir();
    if (!fs::exists(dir / "VETTE.EXE")) {
        std::printf("  SKIPPED: no VETTE.EXE in %s\n", dir.string().c_str());
        return;
    }
    auto plain = boot(dir);
    auto watched = boot(dir);
    CHECK(plain && watched);
    if (!plain || !watched) {
        return;
    }
    SoundEvents sound(*watched);
    std::vector<SoundEvent> events;
    run(*plain, 46, [] {});
    run(*watched, 46, [&] {
        sound.take(events);
        (void)sound.engine();
        (void)sound.requested(Sfx::Skid);
    });
    CHECK(!events.empty());
    CHECK_EQ(plain->cpu().total_cycles(), watched->cpu().total_cycles());
    CHECK(std::memcmp(plain->memory().ram(), watched->memory().ram(), vette::host::Memory::kSize) == 0);
    std::vector<int16_t> a, b;
    plain->take_audio(a);
    watched->take_audio(b);
    CHECK(a == b);
}
