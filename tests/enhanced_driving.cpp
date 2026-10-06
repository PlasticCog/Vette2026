// Lane Centering against the real game (enhanced/lanes.h, game/driving.h): the lane markings taken
// from the extracted city, and the assist holding the Great Highway's lane hands-off where the original
// car, a few degrees off, drifts across the road. Skipped when the game files are missing
// (Game/VETTE.EXE, or the folder in VETTE_GAME_DIR).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "enhanced/lanes.h"
#include "enhanced/world.h"
#include "game/driving.h"
#include "game/options.h"
#include "game/x86.h"
#include "host/machine.h"
#include "test.h"

namespace fs = std::filesystem;
using vette::game::Driving;
using vette::host::Machine;

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
    config.save_dir = fs::temp_directory_path() / "vette2026_lanes_test";
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

bool have_game(const fs::path& dir) {
    if (!fs::exists(dir / "VETTE.EXE")) {
        std::printf("  SKIPPED: no VETTE.EXE in %s\n", dir.string().c_str());
        return false;
    }
    return true;
}

std::shared_ptr<const vette::game::LaneMap> city_lanes(Machine& m) {
    m.run_for(2'000'000'000);  // the title screen: VETTE.EXE has unpacked itself
    vette::enhanced::World world;
    std::string error;
    if (!vette::enhanced::extract_world(m, world, error)) {
        std::printf("  extraction failed: %s\n", error.c_str());
        return nullptr;
    }
    return vette::enhanced::lane_map(world);
}

} // namespace

TEST(enhanced_lane_lines_real_city) {
    const fs::path dir = game_dir();
    if (!have_game(dir)) {
        return;
    }
    auto m = boot(dir);
    CHECK(m != nullptr);
    const auto lanes = m ? city_lanes(*m) : nullptr;
    CHECK(lanes != nullptr);
    if (!lanes) {
        return;
    }
    CHECK(lanes->size() > 10000);
    // The Great Highway (cells (1..21, 2)): markings at y 4160, 4224 (the centre line) and 4288 between
    // kerbs at 4096 and 4352. Northbound, the right lane's centre is y 4320.
    auto fix = lanes->find(6200, 4320, 0);
    CHECK(fix && std::fabs(fix->offset) < 1 && fix->direction < 0.5);
    fix = lanes->find(6200, 4300, 0);
    CHECK(fix && std::fabs(fix->offset - 20) < 1);
    fix = lanes->find(6200, 4250, 0);  // the left lane: centre 4256
    CHECK(fix && std::fabs(fix->offset - 6) < 1);
    fix = lanes->find(6200, 4192, 180);  // southbound, between 4160 and 4224
    CHECK(fix && std::fabs(fix->offset) < 1 && std::fabs(fix->direction - 180) < 0.5);
    CHECK(!lanes->find(6200, 4320, 90).has_value());
}

// Placed in the right lane (41 s), throttle held, no steering. 3 degrees off due north, the original
// drifts left across the road; with Lane Centering the heading comes back to north a degree at a time
// (three turns, never back) and the car to the lane's centre. 20 units off the centre, heading north:
// it glides over without turning at all.
TEST(enhanced_lane_centering_real_game) {
    const fs::path dir = game_dir();
    if (!have_game(dir)) {
        return;
    }
    struct Result {
        int y = 0, heading = 0;
        int turns = 0, reversals = 0, heading_changes = 0;
        double max_glide = 0;
        bool lane_seen = false;
    };
    const auto drive = [&](bool centering, uint16_t heading, uint16_t y) {
        Result r;
        auto m = boot(dir);
        if (!m) {
            return r;
        }
        Driving driving(*m, Driving::Options{false, centering});
        if (centering) {
            driving.set_lanes(city_lanes(*m));
        }
        int last_heading = -1, last_dir = 0;
        driving.on_frame = [&](const Driving::Telemetry& t) {
            if (static_cast<double>(t.t_ns) < 41.05e9) {
                return;
            }
            r.max_glide = std::max(r.max_glide, std::fabs(t.lane_glide));
            r.lane_seen = r.lane_seen || t.lane;
            r.turns = t.assist_turns;
            if (last_heading >= 0 && t.heading != last_heading) {
                ++r.heading_changes;
                const int dir = ((t.heading - last_heading + 540) % 360 - 180) > 0 ? 1 : -1;
                r.reversals += last_dir != 0 && dir != last_dir;
                last_dir = dir;
            }
            last_heading = t.heading;
        };
        struct Key {
            double at;
            uint8_t sc;
        };
        std::vector<Key> keys;
        const auto tap = [&](double t, uint8_t sc) {
            keys.push_back({t, sc});
            keys.push_back({t + 0.1, static_cast<uint8_t>(sc | 0x80)});
        };
        tap(13, 0x39);
        for (const double t : {17.0, 21.0, 25.0, 30.0}) {
            tap(t, 0x1C);
        }
        tap(37.3, 0x02);
        keys.push_back({37.5, 0x48});
        keys.push_back({38.5, 0x4D});
        keys.push_back({39.6, 0xCD});
        std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.at < b.at; });
        size_t next = 0;
        using vette::game::kDataSeg;
        for (int ms = static_cast<int>(m->emulated_ns() / 1'000'000); ms < 50'000 && !m->stopped(); ++ms) {
            for (; next < keys.size() && keys[next].at * 1000 <= ms; ++next) {
                m->key(keys[next].sc);
            }
            if (ms == 41'000) {
                vette::game::wr16(m->memory(), kDataSeg, 0x2D37, y);
                vette::game::wr16(m->memory(), kDataSeg, 0x2D3B, heading);
            }
            m->run_for(1'000'000);
        }
        r.y = vette::game::rd16(m->memory(), kDataSeg, 0x2D37);
        r.heading = vette::game::rd16(m->memory(), kDataSeg, 0x2D3B);
        return r;
    };
    const vette::game::DrivingTuning tuning;
    const Result off = drive(false, 357, 4320), on = drive(true, 357, 4320), glide = drive(true, 0, 4300);
    std::printf("  hands-off at 357 degrees from y 4320: original ends at y %d heading %d; lane centering at y %d "
                "heading %d (%d turns, %d reversals); from y 4300 heading 0: y %d, %d heading changes, glide up "
                "to %.1f units/s\n",
                off.y, off.heading, on.y, on.heading, on.turns, on.reversals, glide.y, glide.heading_changes,
                glide.max_glide);
    CHECK(off.y < 4224);  // across the centre line
    CHECK_EQ(off.heading, 357);
    CHECK(on.lane_seen);
    CHECK_EQ(on.heading, 0);
    CHECK(std::abs(on.y - 4320) <= 4);
    CHECK(on.turns <= 3);  // (the first may come before 41.05 s)
    CHECK_EQ(on.reversals, 0);
    CHECK_EQ(glide.heading_changes, 0);
    CHECK_EQ(glide.heading, 0);
    CHECK(std::abs(glide.y - 4320) <= 4);
    CHECK(glide.max_glide > 0 && glide.max_glide <= tuning.lane_centre_rate + 1e-9);
}
