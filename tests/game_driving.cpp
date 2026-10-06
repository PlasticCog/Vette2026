// Improved Driving and Lane Centering (game/driving.h): the lane finder on synthetic markings, and
// against the real game: with both options off the run is bit-identical to the original's; Improved
// Driving's slide builds smoothly where the original skid jumps; the car flies over the Great Highway's
// hill at speed and lands with a thud. The real-game tests are skipped when the game files are missing
// (Game/VETTE.EXE, or the folder in VETTE_GAME_DIR).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "game/driving.h"
#include "game/options.h"
#include "game/sound_events.h"
#include "game/x86.h"
#include "host/machine.h"
#include "test.h"

namespace fs = std::filesystem;
using vette::game::Driving;
using vette::game::DrivingTuning;
using vette::game::LaneLine;
using vette::game::LaneMap;
using vette::host::Cpu;
using vette::host::Machine;

namespace {

// A north-south road like the city's (strip 7270): white dashes 128 long with 128 gaps at y 64 and
// 192, the yellow centre line at 128; lanes 64 wide, centres 32, 96, 160, 224. Plus an east-west
// marking crossing it, which the finder must ignore.
LaneMap test_road() {
    std::vector<LaneLine> lines;
    for (int x = 0; x < 4096; x += 256) {
        lines.push_back({x, 64, x + 128, 64});
        lines.push_back({x + 128, 192, x, 192});  // either way round
    }
    lines.push_back({0, 128, 4096, 128});
    lines.push_back({1000, 0, 1000, 256});
    return LaneMap(std::move(lines));
}

bool near(double a, double b, double tol = 0.01) { return std::fabs(a - b) <= tol; }

} // namespace

TEST(driving_lane_map_lanes) {
    const LaneMap road = test_road();
    // Between two markings: their midpoint, + to the right (east when heading north).
    auto fix = road.find(2000, 96, 0);
    CHECK(fix.has_value());
    CHECK(fix && near(fix->offset, 0) && near(fix->direction, 0));
    fix = road.find(2000, 80, 0);
    CHECK(fix && near(fix->offset, 16));
    fix = road.find(2000, 110, 0);
    CHECK(fix && near(fix->offset, -14));
    // In a gap between dashes: the dashes ahead and behind count.
    fix = road.find(2200, 96, 0);
    CHECK(fix && near(fix->offset, 0));
    // An outer lane (one marking beside it): half a lane from it.
    fix = road.find(2000, 40, 0);
    CHECK(fix && near(fix->offset, -8));
    fix = road.find(2000, 230, 0);
    CHECK(fix && near(fix->offset, -6));
    // Southbound: right is west.
    fix = road.find(2000, 150, 180);
    CHECK(fix && near(fix->offset, -10) && near(fix->direction, 180));
    // Angled 10 degrees off: the lane's own direction.
    fix = road.find(2000, 96, 10);
    CHECK(fix && near(fix->direction, 0, 0.5));
    fix = road.find(2000, 96, 352);
    CHECK(fix && near(fix->direction, 0, 0.5));
    // Across the road, or too far off its direction, or off it: none.
    CHECK(!road.find(2000, 96, 90).has_value());
    CHECK(!road.find(2000, 96, 20).has_value());
    CHECK(!road.find(2000, 600, 0).has_value());
    CHECK(!road.find(9000, 96, 0).has_value());
}

TEST(driving_tuning_names) {
    DrivingTuning t;
    CHECK(vette::game::set_tuning(t, "drift_max", 25));
    CHECK_EQ(static_cast<int>(t.drift_max), 25);
    CHECK(vette::game::set_tuning(t, "lane_align_rate", 1.5f));
    CHECK(near(t.lane_align_rate, 1.5));
    for (const char* name : {"drift_onset", "drift_per_grip", "drift_build", "drift_relax", "drift_sound",
                             "drift_scrub", "gravity", "lift_off", "jump_min_speed", "hard_landing",
                             "air_pitch_rate", "air_pitch_max", "air_view_max", "lane_resume_delay",
                             "lane_centre_gain", "lane_centre_rate", "lane_centre_angle", "lane_centre_slack",
                             "lane_max_angle", "lane_min_speed"}) {
        CHECK(vette::game::set_tuning(t, name, 1));
    }
    CHECK(!vette::game::set_tuning(t, "grip", 1));
}

namespace {

// A car driven hands-off by the LaneKeeper alone along a straight marked road: the whole-degree
// heading it sets, and a sideways glide; it moves exactly where it points. Returns the heading turns
// and how many of them reversed the one before; `offset_at_end`: the lane centre's offset then.
struct KeeperRun {
    int turns = 0, reversals = 0, final_heading = 0;
    double offset_at_end = 0, max_glide = 0;
    double centred_at = -1;  // seconds: from here on within the slack (+1) of the centre
};
KeeperRun keeper_drive(double road_deg, double start_side, int start_heading, double seconds) {
    const double r = road_deg * 3.14159265358979323846 / 180;
    const double ux = std::cos(r), uy = std::sin(r), nx = -uy, ny = ux;  // along, right
    // Markings 64 apart across the road (dashes 128 long, 128 gaps; a solid centre line), from
    // (20000, 20000) along the road.
    std::vector<LaneLine> lines;
    for (const double side : {-64.0, 0.0, 64.0}) {
        for (double t = 0; t < 30000; t += 256) {
            const double len = side == 0 ? 256 : 128;
            lines.push_back({static_cast<int32_t>(std::lround(20000 + ux * t + nx * side)),
                             static_cast<int32_t>(std::lround(20000 + uy * t + ny * side)),
                             static_cast<int32_t>(std::lround(20000 + ux * (t + len) + nx * side)),
                             static_cast<int32_t>(std::lround(20000 + uy * (t + len) + ny * side))});
        }
    }
    const LaneMap road(std::move(lines));
    const DrivingTuning tuning;
    vette::game::LaneKeeper keeper(tuning);
    // In the lane between the markings at 0 and +64 (centre +32), `start_side` off it.
    double x = 20000 + ux * 500 + nx * (32 + start_side), y = 20000 + uy * 500 + ny * (32 + start_side);
    int heading = start_heading, last_turn = 0;
    const double speed = 400, dt = 1.0 / 15;
    KeeperRun run;
    for (int frame = 0; frame < static_cast<int>(seconds / dt); ++frame) {
        const auto fix = road.find(x, y, heading, tuning.lane_max_angle);
        const auto step = keeper.update(fix, heading, speed, dt);
        if (step.turn != 0) {
            ++run.turns;
            run.reversals += last_turn != 0 && step.turn != last_turn;
            last_turn = step.turn;
            heading = (heading + step.turn + 360) % 360;
        }
        const double h = heading * 3.14159265358979323846 / 180;
        const double dir = fix ? fix->direction * 3.14159265358979323846 / 180 : 0;
        x += speed * dt * std::cos(h) - step.glide * dt * std::sin(dir);
        y += speed * dt * std::sin(h) + step.glide * dt * std::cos(dir);
        run.max_glide = std::max(run.max_glide, std::fabs(step.glide));
        const double off = fix ? fix->offset : 99;
        if (std::fabs(off) <= tuning.lane_centre_slack + 1) {
            if (run.centred_at < 0) run.centred_at = frame * dt;
        } else {
            run.centred_at = -1;
        }
        run.offset_at_end = off;
    }
    run.final_heading = heading;
    return run;
}

} // namespace

// Lane Centering's steering: the heading only lines up with the lane, a degree at a time and never
// back; the centring is a sideways glide. On an axis road, a car parallel to its lane but 20 units off
// glides over without a single turn; on a diagonal at 153.4 degrees (between whole degrees) the heading
// settles on 153 without flipping to 154 and back, and the glide takes up the drift.
TEST(driving_lane_keeper_smooth) {
    const DrivingTuning tuning;
    auto run = keeper_drive(0, -20, 0, 10);
    CHECK_EQ(run.turns, 0);
    CHECK(run.centred_at >= 0 && run.centred_at < 5);
    CHECK(run.max_glide <= tuning.lane_centre_rate + 1e-9);
    run = keeper_drive(0, 20, 356, 10);
    CHECK_EQ(run.turns, 4);
    CHECK_EQ(run.reversals, 0);
    CHECK_EQ(run.final_heading, 0);
    CHECK(run.centred_at >= 0 && run.centred_at < 6);
    run = keeper_drive(153.43494882, 15, 150, 30);  // atan2(1, -2): the 7716/7734 strips
    std::printf("  153.4-degree road: %d turns, %d reversals, heading %d, %.1f units off the centre\n", run.turns,
                run.reversals, run.final_heading, run.offset_at_end);
    CHECK_EQ(run.turns, 3);
    CHECK_EQ(run.reversals, 0);
    CHECK_EQ(run.final_heading, 153);
    CHECK(std::fabs(run.offset_at_end) < 8);
}

namespace {

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
    config.save_dir = fs::temp_directory_path() / "vette2026_driving_test";
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

struct Input {
    double at;
    uint8_t scancode = 0;            // a key, or
    uint16_t poke = 0, value = 0;    // a DS word to write
};

// Course 1 at TRAINEE, automatic gearbox, full throttle, right onto the Great Highway northbound, then
// placed in its right lane heading due north (41 s). `bend`: a fast S-bend at 66 s (into a crash).
std::vector<Input> north_drive(double until, bool bend) {
    std::vector<Input> in;
    const auto tap = [&](double t, uint8_t sc) {
        in.push_back({t, sc});
        in.push_back({t + 0.1, static_cast<uint8_t>(sc | 0x80)});
    };
    const auto hold = [&](double a, double b, uint8_t sc) {
        in.push_back({a, sc});
        in.push_back({b, static_cast<uint8_t>(sc | 0x80)});
    };
    tap(13, 0x39);
    for (const double t : {17.0, 21.0, 25.0, 30.0}) {
        tap(t, 0x1C);
    }
    tap(37.3, 0x02);
    hold(37.5, until, 0x48);
    hold(38.5, 39.6, 0x4D);
    in.push_back({41, 0, 0x2D37, 4320});  // y: the lane between the markings at 4288 and the kerb at 4352
    in.push_back({41, 0, 0x2D3B, 0});     // heading north
    if (bend) {
        hold(66, 67.2, 0x4D);
        hold(67.6, 68.8, 0x4B);
    }
    std::stable_sort(in.begin(), in.end(), [](const Input& a, const Input& b) { return a.at < b.at; });
    return in;
}

template <typename F>
void run(Machine& m, const std::vector<Input>& in, double seconds, F each_ms) {
    size_t next = 0;
    for (int ms = 0; ms < static_cast<int>(seconds * 1000) && !m.stopped(); ++ms) {
        for (; next < in.size() && in[next].at * 1000 <= ms; ++next) {
            if (in[next].scancode) {
                m.key(in[next].scancode);
            } else {
                vette::game::wr16(m.memory(), vette::game::kDataSeg, in[next].poke, in[next].value);
            }
        }
        m.run_for(1'000'000);
        each_ms();
    }
}

// The player's car after each player step (3009:020D): emulated time, facing, travel direction, skid.
struct Frame {
    double t;
    int facing, travel, slip;
    bool skid;
};
Cpu::WatchId record_frames(Machine& m, std::vector<Frame>& out) {
    return m.cpu().add_watch(Cpu::linear(vette::game::emu_seg(0x3009), 0x020D), [&m, &out](Cpu&) {
        const auto w = [&m](uint16_t off) { return vette::game::rd16(m.memory(), vette::game::kDataSeg, off); };
        const int facing = w(0x2D3B), travel = w(0x2C45);
        out.push_back({static_cast<double>(m.emulated_ns()) / 1e9, facing, travel,
                       ((facing - travel) % 360 + 540) % 360 - 180, w(0x2C4B) != 0});
    });
}

bool have_game(const fs::path& dir) {
    if (!fs::exists(dir / "VETTE.EXE")) {
        std::printf("  SKIPPED: no VETTE.EXE in %s\n", dir.string().c_str());
        return false;
    }
    return true;
}

} // namespace

// "Prove the game is untouched with them off": the same session (a fast S-bend that skids and crashes)
// with no driving layer and with one whose options are both off ends in identical machines.
TEST(driving_off_is_bit_identical) {
    const fs::path dir = game_dir();
    if (!have_game(dir)) {
        return;
    }
    const auto in = north_drive(70, true);
    auto a = boot(dir);
    auto b = boot(dir);
    CHECK(a && b);
    if (!a || !b) {
        return;
    }
    std::vector<Frame> fa, fb;
    record_frames(*a, fa);
    record_frames(*b, fb);
    {
        Driving off(*b, Driving::Options{false, false});
        run(*a, in, 70, [] {});
        run(*b, in, 70, [] {});
    }
    CHECK_EQ(a->cpu().total_cycles(), b->cpu().total_cycles());
    CHECK(a->cpu().regs.r == b->cpu().regs.r && a->cpu().regs.s == b->cpu().regs.s && a->cpu().regs.ip == b->cpu().regs.ip);
    CHECK(std::memcmp(a->memory().ram(), b->memory().ram(), vette::host::Memory::kSize) == 0);
    CHECK_EQ(fa.size(), fb.size());
    // The session did reach the original's skid (and its slip jumping by many degrees a frame).
    int max_slip = 0, max_jump = 0;
    for (size_t i = 1; i < fa.size(); ++i) {
        if (fa[i].t > 66 && fa[i].t < 67.1) {
            max_slip = std::max(max_slip, std::abs(fa[i].slip));
            max_jump = std::max(max_jump, std::abs(fa[i].slip - fa[i - 1].slip));
        }
    }
    CHECK(max_slip >= 25);
    CHECK(max_jump >= 15);
}

// The same S-bend with Improved Driving: the slip angle builds a few degrees a frame up to the cap, and
// the skid sound only comes with a big slide.
TEST(driving_improved_drift) {
    const fs::path dir = game_dir();
    if (!have_game(dir)) {
        return;
    }
    auto m = boot(dir);
    CHECK(m != nullptr);
    if (!m) {
        return;
    }
    const DrivingTuning tuning;
    Driving driving(*m, Driving::Options{true, false}, tuning);
    struct Sample {
        double t, slip;
        bool skid;
        int fr;
    };
    std::vector<Sample> s;
    driving.on_frame = [&s](const Driving::Telemetry& t) {
        s.push_back({static_cast<double>(t.t_ns) / 1e9, t.slip, t.skid, t.frame_rate});
    };
    run(*m, north_drive(70, true), 70, [] {});
    double max_slip = 0, max_step = 0;
    bool skid_ok = true;
    for (size_t i = 1; i < s.size(); ++i) {
        if (s[i].t > 66 && s[i].t < 67.1) {
            max_slip = std::max(max_slip, std::fabs(s[i].slip));
            max_step = std::max(max_step, std::fabs(s[i].slip - s[i - 1].slip) * s[i].fr);
            skid_ok = skid_ok && s[i].skid == (std::fabs(s[i].slip) >= tuning.drift_sound);
        }
    }
    std::printf("  improved drift: max slip %.1f deg, fastest change %.1f deg/s\n", max_slip, max_step);
    CHECK(max_slip >= 8);
    CHECK(max_slip <= tuning.drift_max);
    CHECK(max_step <= tuning.drift_build + 0.01);
    CHECK(skid_ok);
}

// Over the hill on the Great Highway (a 7-degree ramp up to cell (23,2), its crest at x ~ 46900) at
// full speed: the car leaves the ground, flies on a ballistic path with its nose following it, lands,
// and the landing is a thud; the original's own thud test stays quiet in the air.
TEST(driving_improved_jump) {
    const fs::path dir = game_dir();
    if (!have_game(dir)) {
        return;
    }
    auto m = boot(dir);
    CHECK(m != nullptr);
    if (!m) {
        return;
    }
    vette::game::SoundEvents sound(*m);
    Driving driving(*m, Driving::Options{true, false});
    driving.on_hard_landing = [&sound](float) { sound.report(vette::game::Sfx::Thud); };
    sound.thud_hold = [&driving] { return driving.flying(); };
    double max_air = 0, max_height = 0, takeoff = 0, landing = 0, min_pitch = 0;
    int pitch_steps = 0;
    int last_pitch = 0;
    driving.on_frame = [&](const Driving::Telemetry& t) {
        const double now = static_cast<double>(t.t_ns) / 1e9;
        if (t.airborne) {
            if (takeoff == 0) takeoff = now;
            max_air = std::max(max_air, t.air_time);
            max_height = std::max(max_height, t.z - t.ground_z);
            min_pitch = std::min<double>(min_pitch, t.pitch);
            pitch_steps = std::max(pitch_steps, std::abs(t.pitch - last_pitch));
        } else if (takeoff != 0 && landing == 0) {
            landing = now;
        }
        last_pitch = t.pitch;
    };
    std::vector<vette::game::SoundEvent> events;
    run(*m, north_drive(111, false), 111, [&] { sound.take(events); });
    const auto& tm = driving.telemetry();
    std::printf("  jump at %.2f s: %.2f s in the air, %.1f units up, nose down to %.0f deg; %d landing(s), %d hard\n",
                takeoff, max_air, max_height, min_pitch, tm.landings, tm.hard_landings);
    CHECK_EQ(tm.jumps, 1);
    CHECK(takeoff > 108 && takeoff < 110);
    CHECK(max_air >= 0.6 && max_air <= 2);
    CHECK(max_height >= 8 && max_height <= 40);
    CHECK(min_pitch < 0);
    CHECK(pitch_steps <= 3);  // eases, 20 degrees a second at 9-10 frames a second
    CHECK_EQ(tm.hard_landings, 1);
    int thuds_in_air = 0, landing_thuds = 0;
    for (const auto& e : events) {
        if (e.sfx != vette::game::Sfx::Thud) continue;
        const double t = static_cast<double>(e.t_ns) / 1e9;
        thuds_in_air += t > takeoff + 0.01 && t < landing - 0.2;
        landing_thuds += t >= landing - 0.2 && t < landing + 0.5;
    }
    CHECK_EQ(thuds_in_air, 0);
    CHECK_EQ(landing_thuds, 1);
}
