// The city map (game/city_map.h): the file format, and with the game files, the original's map read from
// memory and an edited one put into the game as it starts.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "game/city_map.h"
#include "host/machine.h"
#include "test.h"

namespace fs = std::filesystem;
using vette::game::CityMap;

namespace {

CityMap sample() {
    CityMap m;
    m.name = "Test map";
    for (int bt = 0; bt < CityMap::kBigTiles; ++bt) {
        m.layout[static_cast<size_t>(bt)] = static_cast<uint8_t>(bt % CityMap::kDesigns);
        m.ground[static_cast<size_t>(bt)] = bt % 2 ? 9 : 7;
    }
    for (int d = 0; d < CityMap::kDesigns; ++d) {
        for (int i = 0; i < 256; ++i) {
            m.designs[static_cast<size_t>(d)].cells[static_cast<size_t>(i)] = {static_cast<uint8_t>((d * 31 + i) & 0xFF),
                                                                               static_cast<uint8_t>(i % 4)};
        }
    }
    return m;
}

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

std::unique_ptr<vette::host::Machine> boot(const fs::path& dir, const CityMap* install) {
    vette::host::MachineConfig config;
    config.game_dir = dir;
    config.save_dir = fs::temp_directory_path() / "vette2026_city_map_test";
    auto m = std::make_unique<vette::host::Machine>(config);
    if (install) vette::game::install_city_map(*m, *install);
    std::string error;
    if (!m->boot(error)) {
        std::printf("  boot failed: %s\n", error.c_str());
        return nullptr;
    }
    m->run_for(300'000'000);  // past the unpacking and the start
    return m;
}

}  // namespace

TEST(city_map_file_round_trip) {
    const CityMap m = sample();
    std::string error;
    const auto back = CityMap::parse(m.serialize(), error);
    CHECK(back.has_value());
    if (!back) {
        std::printf("  %s\n", error.c_str());
        return;
    }
    CHECK(*back == m);
    CHECK_EQ(back->name, std::string("Test map"));
    // Cell (cx, cy) through the layout: big tile 6 (row 1, col 1) uses design 6.
    CHECK(back->cell(16 + 3, 16 + 5) == m.designs[6].cells[3 * 16 + 5]);
}

TEST(city_map_file_errors) {
    std::string error;
    CHECK(!CityMap::parse("hello\n", error));
    CHECK(!CityMap::parse("VETTE2026 MAP 1\nname=x\n", error));  // incomplete
    std::string text = sample().serialize();
    text.replace(text.find("design 3"), 8, "design 99");
    CHECK(!CityMap::parse(text, error));
    CHECK(error.find("design") != std::string::npos);
}

TEST(city_map_original_and_installed) {
    const fs::path dir = game_dir();
    if (!fs::exists(dir / "VETTE.EXE")) {
        std::printf("  SKIPPED: no VETTE.EXE in %s\n", dir.string().c_str());
        return;
    }
    auto original = boot(dir, nullptr);
    CHECK(original != nullptr);
    if (!original) return;
    std::string error;
    const auto map = CityMap::read(original->memory(), error);
    CHECK(map.has_value());
    if (!map) {
        std::printf("  %s\n", error.c_str());
        return;
    }
    // Notes 05: big tile (0,0) is T0, the water filler T10 is used 10 times, the course starts/finishes.
    CHECK_EQ(map->layout[0], 0);
    CHECK_EQ(map->sharing(10), 10);
    CHECK_EQ(map->ground[0], 7);
    CHECK_EQ(map->cell(65, 2).type, 75);   // course 1's finish (Vista Point)
    CHECK_EQ(map->cell(18, 75).type, 74);  // course 2's (Bay Bridge)
    CHECK_EQ(map->cell(1, 2).type, 208);   // course 3's (the Zoo)

    // An edited map goes in as the game starts.
    CityMap edited = *map;
    edited.cell(5, 5) = {edited.cell(5, 6).type, 2};
    edited.ground[24] = 7;
    auto played = boot(dir, &edited);
    CHECK(played != nullptr);
    if (!played) return;
    const auto seen = CityMap::read(played->memory(), error);
    CHECK(seen.has_value() && *seen == edited);
}
