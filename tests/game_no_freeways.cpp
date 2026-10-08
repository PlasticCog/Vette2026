// No freeways (game/no_freeways.h), with the game files: the roads join the city's parts into one, and the
// computer opponent drives course 1 to its finish on them, never on a freeway.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

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
    vette::game::add_no_freeway_cell_types(m->memory());
    vette::game::add_no_freeway_roads(*map);
    const auto after = vette::game::find_drivable(m->memory(), *map);
    const int g = region_near(after, kPlaces[0]);
    CHECK(g >= 0);
    for (const Place& p : kPlaces) CHECK_EQ(region_near(after, p), g);
    // And the opponent's new roads run on it, clear of every wall.
    const auto boxes = vette::game::collision_boxes(m->memory(), *map);
    for (const auto& road : vette::game::no_freeway_opponent_roads()) {
        for (size_t k = 0; k + 1 < road.size(); ++k) {
            const int n = std::max(1, std::max(std::abs(road[k + 1].first - road[k].first), std::abs(road[k + 1].second - road[k].second)) / 32);
            bool clear = true;
            for (int i = 0; i <= n; ++i) {
                const int32_t x = road[k].first + (road[k + 1].first - road[k].first) * i / n;
                const int32_t y = road[k].second + (road[k + 1].second - road[k].second) * i / n;
                const int at = after.region_at(x, y);
                clear = clear && (at == g || at < 0) && vette::game::point_clear(boxes, x, y, 12);
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

TEST(no_freeways_placement) {
    using vette::game::Placement;
    using vette::game::no_freeway_placement;
    constexpr uint16_t kBridgeCar = 0xEB84, kCityCar = 0xE194, kPedestrian = 0xE8E8;
    // The Golden Gate's cars: the whole coast road, from the Zoo to the approach, where the original's rules
    // take over; nowhere else in the city or the Marina.
    CHECK(no_freeway_placement(kBridgeCar, 1, 2, 0xD0) == Placement::Allow);
    CHECK(no_freeway_placement(kBridgeCar, 29, 2, 0x50) == Placement::Allow);
    CHECK(no_freeway_placement(kBridgeCar, 50, 2, 0x29) == Placement::Default);
    CHECK(no_freeway_placement(kBridgeCar, 0, 2, 0xF8) == Placement::Deny);
    CHECK(no_freeway_placement(kBridgeCar, 10, 6, 0xCF) == Placement::Deny);
    CHECK(no_freeway_placement(kBridgeCar, 35, 6, 0x34) == Placement::Deny);
    // The city's cars keep the original's rules; pedestrians keep off the water and the coast road.
    CHECK(no_freeway_placement(kCityCar, 29, 2, 0x50) == Placement::Default);
    CHECK(no_freeway_placement(kPedestrian, 10, 1, 0x01) == Placement::Deny);
    CHECK(no_freeway_placement(kPedestrian, 29, 2, 0x50) == Placement::Deny);
    CHECK(no_freeway_placement(kPedestrian, 35, 4, 0x34) == Placement::Default);
}

TEST(no_freeways_coast_traffic) {
    const fs::path dir = game_dir();
    if (!fs::exists(dir / "VETTE.EXE")) {
        std::printf("  SKIPPED: no VETTE.EXE in %s\n", dir.string().c_str());
        return;
    }
    auto m = boot(dir, true);
    CHECK(m != nullptr);
    if (!m) return;
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
    until(40);
    auto& mem = m->memory();
    const auto w = [&](uint16_t off) { return vette::game::rd16(mem, vette::game::kDataSeg, off); };
    // The city's big tiles share a list with the Golden Gate's 9 cars in it, and the Marina's has pedestrians.
    const uint16_t city = w(0xEF5A);  // big tile 0, the Zoo's
    CHECK(city != 0xF01E);
    for (const int bt : {5, 6, 7, 11, 12}) CHECK_EQ(w(static_cast<uint16_t>(0xEF5A + 2 * bt)), city);
    CHECK_EQ(w(0xEF8C + 2 * 10), 0xEFC0);
    // The new road types are in the cell type table, with no collision boxes.
    CHECK_EQ(w(w(0x9D73 + 2 * 0x50)), 0x2C00);
    std::vector<uint16_t> bridge;
    for (uint16_t p = city; w(p) != 0xFFFF; p = static_cast<uint16_t>(p + 4)) {
        if (w(p) >= 0xEB56 && w(p) < 0xECF4) bridge.push_back(p);
    }
    CHECK_EQ(bridge.size(), size_t{9});
    // With the camera at the Zoo (not the bridge's tile), they move all the same, and both lists agree on
    // their cells.
    std::vector<uint16_t> before;
    for (const uint16_t p : bridge) before.push_back(w(static_cast<uint16_t>(w(p) + 2)));
    until(45);
    CHECK_EQ(w(0x843A), 0);
    int moved = 0;
    for (size_t i = 0; i < bridge.size(); ++i) {
        moved += w(static_cast<uint16_t>(w(bridge[i]) + 2)) != before[i];
        for (uint16_t q = 0xF0B0; w(q) != 0xFFFF; q = static_cast<uint16_t>(q + 4)) {
            if (w(q) == w(bridge[i])) CHECK_EQ(w(static_cast<uint16_t>(q + 2)), w(static_cast<uint16_t>(bridge[i] + 2)));
        }
    }
    CHECK_EQ(moved, 9);
}
