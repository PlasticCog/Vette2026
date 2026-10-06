// SoundEvents against the real game: a scripted session (title, garage rev, race, missed shifts, a
// crash into traffic, then the horn and the helicopter view) must produce the expected events, the
// native engine-note model must match the original's at every step, and the observer must not change
// the run. Skipped when the game files are
// missing (Game/VETTE.EXE, or the folder in VETTE_GAME_DIR).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
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
    hold(49.3, 49.8, vette::game::kHornScancode);
    tap(50.0, 0x3E);  // F4: helicopter view
    tap(51.0, 0x3C);  // F2: ahead
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
    bool engine_seen_running = false, engine_pitch_ok = true, horn_requested = false, horn_late = false;
    run(*m, 52, [&] {
        sound.take(events);
        const double t = static_cast<double>(m->emulated_ns()) / 1e9;
        if (t > 49.4 && t < 49.7) {
            horn_requested = horn_requested || sound.requested(Sfx::Horn);
        } else if (t > 49.9) {
            horn_late = horn_late || sound.requested(Sfx::Horn);
        }
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

    // Events: in time order, each stop after a start of the same sound, one tone at a time; cues start
    // only.
    std::vector<int> starts(static_cast<size_t>(Sfx::Count));
    std::vector<bool> on(static_cast<size_t>(Sfx::Count));
    uint64_t last_t = 0;
    int tones_on = 0;
    using vette::game::SfxKind;
    for (const auto& e : events) {
        const auto i = static_cast<size_t>(e.sfx);
        CHECK(e.t_ns >= last_t);
        last_t = e.t_ns;
        starts[i] += e.start ? 1 : 0;
        if (vette::game::sfx_kind(e.sfx) == SfxKind::Cue) {
            CHECK(e.start);
            continue;
        }
        CHECK(e.start != on[i]);
        on[i] = e.start;
        if (vette::game::sfx_kind(e.sfx) == SfxKind::Tone) {
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
    // The silent moments: the title's three, the start lights, the horn and the helicopter view.
    for (const Sfx s : {Sfx::IntroCableCar, Sfx::IntroCar, Sfx::IntroLogo, Sfx::CountdownGo, Sfx::Horn,
                        Sfx::Helicopter}) {
        CHECK_EQ(starts[static_cast<size_t>(s)], 1);
    }
    CHECK_EQ(starts[static_cast<size_t>(Sfx::CountdownBeep)], 2);
    CHECK(!on[static_cast<size_t>(Sfx::Horn)] && !on[static_cast<size_t>(Sfx::Helicopter)]);  // both ended
    CHECK(horn_requested && !horn_late);
    CHECK_EQ(starts[static_cast<size_t>(Sfx::Splash)] + starts[static_cast<size_t>(Sfx::PulledOver)], 0);
    // In order: the title's cable car, car and logo; the lights 2 s and 1 s apart, after the engine starts.
    std::vector<uint64_t> at(static_cast<size_t>(Sfx::Count));
    std::vector<uint64_t> beeps;
    for (const auto& e : events) {
        if (e.start && !at[static_cast<size_t>(e.sfx)]) {
            at[static_cast<size_t>(e.sfx)] = e.t_ns;
        }
        if (e.sfx == Sfx::CountdownBeep) {
            beeps.push_back(e.t_ns);
        }
    }
    const auto when = [&](Sfx s) { return static_cast<double>(at[static_cast<size_t>(s)]) / 1e9; };
    CHECK(when(Sfx::TitleTune) <= when(Sfx::IntroCableCar) && when(Sfx::IntroCableCar) < when(Sfx::IntroCar) &&
          when(Sfx::IntroCar) < when(Sfx::IntroLogo));
    CHECK(when(Sfx::Engine) <= when(Sfx::CountdownBeep));
    if (beeps.size() == 2) {
        const double gap = static_cast<double>(beeps[1] - beeps[0]) / 1e9;
        const double go = when(Sfx::CountdownGo) - static_cast<double>(beeps[1]) / 1e9;
        CHECK(gap > 1.8 && gap < 2.3);
        CHECK(go > 0.8 && go < 1.3);
    }
    CHECK(when(Sfx::Horn) > 49.2 && when(Sfx::Horn) < 49.4);
    CHECK(when(Sfx::Helicopter) > 49.9 && when(Sfx::Helicopter) < 50.4);

    // The title tune plays out: 9 notes, 640 ticks.
    for (size_t i = 0; i < events.size(); ++i) {
        if (events[i].sfx == Sfx::TitleTune && events[i].start) {
            size_t j = i + 1;
            while (j < events.size() && events[j].sfx != Sfx::TitleTune) {
                ++j;
            }
            CHECK(j < events.size() && !events[j].start);
            if (j < events.size()) {
                const double ticks = static_cast<double>(events[j].t_ns - events[i].t_ns) * 1e-9 *
                                     vette::game::kSoundTickHz;
                CHECK(ticks > 639 && ticks < 642);
            }
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
    // The countdown: the DOS game's two unused beeps.
    const auto beep = sound.program(Sfx::CountdownBeep), go = sound.program(Sfx::CountdownGo);
    CHECK(beep.steps.size() == 1 && beep.loop_to == -1 && go.steps.size() == 1 && go.loop_to == -1);
    if (beep.steps.size() == 1 && go.steps.size() == 1) {
        CHECK(beep.steps[0].ticks == 20 && std::fabs(beep.steps[0].hz - 659.6f) < 0.5f);
        CHECK(go.steps[0].ticks == 10 && std::fabs(go.steps[0].hz - 880.6f) < 0.5f);
    }
    for (const Sfx s : {Sfx::Horn, Sfx::Helicopter, Sfx::Splash, Sfx::Thud, Sfx::PulledOver, Sfx::IntroLogo}) {
        const auto p = sound.program(s);
        CHECK(p.steps.empty() && p.pulses.empty());
    }
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

TEST(game_sound_service_station) {
    // The service station at 38th Avenue and Santiago, cell (6, 13) in the start's big tile: the car is
    // put in front of its driveway during the countdown, drives through it, and is put back once to
    // drive through again. One bell each time, as the Mac rings it.
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
    vette::host::Memory& mem = m->memory();
    using vette::game::kDataSeg;
    const auto place = [&] {
        vette::game::wr16(mem, kDataSeg, 0x2D35, 6 * 0x800 + 200);    // x: the cell's south part
        vette::game::wr16(mem, kDataSeg, 0x2D37, 13 * 0x800 + 1650);  // y: on the driveway's line
        vette::game::wr16(mem, kDataSeg, 0x2D3B, 0);                  // heading north
    };
    struct Key {
        double at;
        uint8_t sc;
    };
    const Key keys[] = {{13, 0x39}, {13.1, 0xB9}, {17, 0x1C}, {17.1, 0x9C}, {21, 0x1C}, {21.1, 0x9C},
                        {25, 0x1C}, {25.1, 0x9C}, {30, 0x1C}, {30.1, 0x9C}, {37.3, 0x02}, {37.4, 0x82},
                        {37.5, 0x48}, {47, 0xC8}};
    size_t next = 0;
    bool placed = false, replaced = false;
    std::vector<SoundEvent> events;
    for (int ms = 0; ms < 47000 && !m->stopped(); ++ms) {
        while (next < std::size(keys) && keys[next].at * 1000 <= ms) {
            m->key(keys[next++].sc);
        }
        if (!placed && ms >= 32000) {
            place();
            placed = true;
        }
        if (!replaced && ms >= 43000) {
            place();  // back in front of the driveway, still rolling
            replaced = true;
        }
        m->run_for(1'000'000);
        sound.take(events);
    }
    std::vector<double> bells;
    for (const auto& e : events) {
        if (e.sfx == Sfx::ServiceStation) {
            CHECK(e.start);
            bells.push_back(static_cast<double>(e.t_ns) / 1e9);
        }
    }
    CHECK_EQ(bells.size(), size_t{2});
    if (bells.size() == 2) {
        CHECK(bells[0] > 38 && bells[0] < 43);
        CHECK(bells[1] > 43 && bells[1] < 47);
    }
}
