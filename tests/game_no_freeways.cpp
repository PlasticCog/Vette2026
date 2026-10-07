// No freeways (game/no_freeways.h), with the game files: the roads join the city's parts into one, and the
// computer opponent drives course 1 to its finish on them, never on a freeway.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "game/city_map.h"
#include "game/drivable.h"
#include "game/no_freeways.h"
#include "game/options.h"
#include "game/x86.h"
#include "host/machine.h"
#include "test.h"

namespace fs = std::filesystem;
using vette::game::CityMap;

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
    if (!env.empty()) return env;
    return fs::path(__FILE__).parent_path().parent_path() / "Game";
}

std::unique_ptr<vette::host::Machine> boot(const fs::path& dir, bool no_freeways) {
    vette::host::MachineConfig config;
    config.game_dir = dir;
    config.save_dir = fs::temp_directory_path() / "vette2026_no_freeways_test";
    auto m = std::make_unique<vette::host::Machine>(config);
    if (no_freeways) vette::game::install_no_freeways(*m);
    std::string error;
    if (!m->boot(error)) {
        std::printf("  boot failed: %s\n", error.c_str());
        return nullptr;
    }
    vette::game::install_skip_manual_check(m->cpu());
    vette::game::install_idle_skip(*m);
    return m;
}

constexpr uint64_t kMs = 1'000'000;

// The courses' starts and finishes (notes 05 section 7), absolute.
struct Place {
    int32_t x, y;
};
constexpr Place kPlaces[] = {{4064, 4480},                                   // course 1 start, the Zoo
                             {65 * 2048 + 1024, 2 * 2048 + 1024},            // course 1 finish, Vista Point
                             {18 * 2048 + 1024, 75 * 2048 + 1024},           // course 2 finish, the Bay Bridge's end
                             {32768 + 6048, 4 * 32768 + 22912}};             // course 3 start, the same

int region_near(const vette::game::DrivableMap& d, Place p) {
    for (int r = 0; r <= 2048; r += 32) {
        for (int k = -r; k <= r; k += 32) {
            for (const auto& [dx, dy] : {std::pair{k, -r}, std::pair{k, r}, std::pair{-r, k}, std::pair{r, k}}) {
                if (const int g = d.region_at(p.x + dx, p.y + dy); g >= 0) return g;
            }
        }
    }
    return -1;
}

}  // namespace

TEST(no_freeways_join_the_city) {
    const fs::path dir = game_dir();
    if (!fs::exists(dir / "VETTE.EXE")) {
        std::printf("  SKIPPED: no VETTE.EXE in %s\n", dir.string().c_str());
        return;
    }
    auto m = boot(dir, false);
    CHECK(m != nullptr);
    if (!m) return;
    m->run_for(300 * kMs);
    std::string error;
    auto map = CityMap::read(m->memory(), error);
    CHECK(map.has_value());
    if (!map) return;
    // The original: only freeways join the Zoo and Vista Point.
    const auto before = vette::game::find_drivable(m->memory(), *map);
    CHECK(region_near(before, kPlaces[0]) != region_near(before, kPlaces[1]));
    // With the roads: every start and finish in one region.
    vette::game::add_no_freeway_roads(*map);
    const auto after = vette::game::find_drivable(m->memory(), *map);
    const int g = region_near(after, kPlaces[0]);
    CHECK(g >= 0);
    for (const Place& p : kPlaces) CHECK_EQ(region_near(after, p), g);
    // And the opponent's new roads run on it, clear of every wall.
    for (const auto& road : vette::game::no_freeway_opponent_roads()) {
        for (size_t k = 0; k + 1 < road.size(); ++k) {
            const int n = std::max(1, std::max(std::abs(road[k + 1].first - road[k].first), std::abs(road[k + 1].second - road[k].second)) / 32);
            bool clear = true;
            for (int i = 0; i <= n; ++i) {
                const int32_t x = road[k].first + (road[k + 1].first - road[k].first) * i / n;
                const int32_t y = road[k].second + (road[k + 1].second - road[k].second) * i / n;
                clear = clear && after.region_at(x, y) == g;
            }
            CHECK(clear);
        }
    }
}

TEST(no_freeways_opponent_drives_course_1) {
    const fs::path dir = game_dir();
    if (!fs::exists(dir / "VETTE.EXE")) {
        std::printf("  SKIPPED: no VETTE.EXE in %s\n", dir.string().c_str());
        return;
    }
    auto m = boot(dir, true);
    CHECK(m != nullptr);
    if (!m) return;
    // Title, car, level, opponent, course 1 (as the README's race script); then the opponent drives.
    const auto press = [&](uint8_t sc) {
        m->key(sc);
        m->run_for(100 * kMs);
        m->key(static_cast<uint8_t>(sc | 0x80));
    };
    const auto until = [&](double seconds) {
        while (m->emulated_ns() < static_cast<uint64_t>(seconds * 1e9)) m->run_for(10 * kMs);
    };
    until(13);
    press(0x39);
    until(17);
    press(0x1C);
    until(21.2);
    press(0x1C);
    until(25);
    press(0x1C);
    until(30.5);
    press(0x1C);
    auto& mem = m->memory();
    const auto w = [&](uint16_t off) { return static_cast<int16_t>(vette::game::rd16(mem, vette::game::kDataSeg, off)); };
    bool freeway = false;
    double finish_at = 0;
    for (int s = 31; s < 330 && finish_at == 0; ++s) {
        until(s);
        freeway = freeway || vette::game::rd8(mem, vette::game::kDataSeg, 0x842B) != 0;
        // The opponent (DS:2F09): at Vista Point, the course's finish (cell 65,2)?
        const long x = static_cast<long>(w(0x2F2B)) * 0x8000 + static_cast<uint16_t>(w(0x2F09));
        const long y = static_cast<long>(w(0x2F2D)) * 0x8000 + static_cast<uint16_t>(w(0x2F0B));
        if (x / 2048 >= 65 && y / 2048 <= 3) finish_at = s;
    }
    std::printf("  the opponent reached Vista Point at %.0f s, %s\n", finish_at,
                freeway ? "on a freeway at times" : "never on a freeway");
    CHECK(finish_at > 0);
    CHECK(!freeway);
}
