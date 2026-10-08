// The city's code-drawn objects as models (enhanced/object_models.h, game/model_pack.h objects): a building
// made a model, reshaped, and drawn as that model by the game and in the Enhanced view's world.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "enhanced/object_models.h"
#include "enhanced/world.h"
#include "game/model_pack.h"
#include "game/options.h"
#include "game/x86.h"
#include "host/machine.h"
#include "test.h"

namespace fs = std::filesystem;
namespace en = vette::enhanced;
namespace game = vette::game;

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

vette::host::MachineConfig config_for(const fs::path& dir) {
    vette::host::MachineConfig config;
    config.game_dir = dir;
    config.save_dir = fs::temp_directory_path() / "vette2026_object_models_test";
    config.cpu_hz = 140'000'000;
    return config;
}

constexpr uint16_t kBuilding = 0x2EF4;  // the city's commonest building (353 cells)

}  // namespace

TEST(object_models_from_the_city) {
    const fs::path dir = game_dir();
    if (!fs::exists(dir / "VETTE.EXE")) {
        std::printf("  SKIPPED: no VETTE.EXE in %s\n", dir.string().c_str());
        return;
    }
    vette::host::Machine m(config_for(dir));
    std::string error;
    CHECK(m.boot(error));
    CHECK(game::run_until_started(m));
    en::World world;
    CHECK(en::extract_world(m, world, error));
    const auto objects = en::city_objects(world);
    std::printf("  %zu city objects; the commonest %04X in %d cells (%s)\n", objects.size(),
                objects.empty() ? 0 : objects.front().routine, objects.empty() ? 0 : objects.front().cells,
                objects.empty() ? "" : objects.front().name.c_str());
    CHECK(objects.size() >= 10);
    bool found = false;
    for (const auto& o : objects) found = found || o.routine == kBuilding;
    CHECK(found);
    const auto model = en::routine_model(world, kBuilding, error);
    CHECK(model.has_value());
    if (!model) return;
    CHECK(game::check_model(*model).empty());
    // Its window detail, culled with its walls; something on top of its walls.
    int culled = 0;
    for (const auto& f : model->faces) culled += f.flags & 1;
    CHECK(culled >= 5);
    CHECK(model->verts.size() > 20);
    // A model and a landmark drawn as one aren't objects to convert.
    CHECK(!en::routine_model(world, 0xB92A, error));
}

TEST(object_models_drawn_by_the_game) {
    const fs::path dir = game_dir();
    if (!fs::exists(dir / "VETTE.EXE")) {
        std::printf("  SKIPPED: no VETTE.EXE in %s\n", dir.string().c_str());
        return;
    }
    // The building made a model, its top raised 400.
    game::ModelData building;
    {
        vette::host::Machine m(config_for(dir));
        std::string error;
        CHECK(m.boot(error));
        CHECK(game::run_until_started(m));
        en::World world;
        CHECK(en::extract_world(m, world, error));
        const auto model = en::routine_model(world, kBuilding, error);
        CHECK(model.has_value());
        if (!model) return;
        building = *model;
    }
    int16_t top = 0;
    for (size_t i = game::kReferenceVertices; i < building.verts.size(); ++i) top = std::min(top, building.verts[i][1]);
    for (size_t i = game::kReferenceVertices; i < building.verts.size(); ++i) {
        if (building.verts[i][1] == top) building.verts[i][1] = static_cast<int16_t>(top - 400);
    }
    game::ModelPack pack;
    pack.objects[kBuilding] = building;
    CHECK(game::object_models(pack).size() == 1 && game::object_models(pack)[0].second == game::kModelCount);
    // The text round trip.
    std::string error;
    const auto again = game::ModelPack::parse(pack.serialize(), error);
    CHECK(again.has_value() && again->objects.count(kBuilding) && again->objects.at(kBuilding) == building);

    vette::host::MachineConfig race_config = config_for(dir);
    race_config.cpu_hz = vette::host::MachineConfig{}.cpu_hz;  // (the menus' keys below are timed for it)
    vette::host::Machine m(race_config);
    game::install_model_pack(m, pack);
    CHECK(m.boot(error));
    game::install_skip_manual_check(m.cpu());
    game::install_idle_skip(m);
    CHECK(game::run_until_started(m));
    m.run_for(100'000'000);
    // The slot after the original's 59, and the Chinatown gate moved to make room.
    const uint16_t ms = game::emu_seg(0x245A);
    const uint16_t slot = game::rd16(m.memory(), ms, static_cast<uint16_t>(0x6FF8 + 8 * game::kModelCount));
    CHECK(slot != 0 && game::rd16(m.memory(), ms, slot) == 0x8000);
    CHECK_EQ(game::rd16(m.memory(), ms, game::rd16(m.memory(), ms, 0x6FF8 + 8 * 14)), 0x8000);
    const auto read_back = game::read_model(m.memory(), 14);
    CHECK(read_back.has_value());

    // The Enhanced view's world: the routine is the model now, as reshaped.
    en::World world;
    CHECK(en::extract_world(m, world, error));
    CHECK(en::use_object_models(world, m, game::object_models(pack), error));
    const en::Routine* r = world.routine(kBuilding);
    CHECK(r && r->variants.size() == 1 && r->variants[0].parts.size() == 1);
    if (r && !r->variants.empty() && !r->variants[0].parts.empty()) {
        CHECK(r->variants[0].parts[0].source == en::Part::Source::Model);
        CHECK_EQ(int(r->variants[0].parts[0].model), game::kModelCount);
    }
    const en::Model& mm = world.models[game::kModelCount];
    CHECK(mm.present);
    int highest = 0;
    for (const auto& v : mm.near_mesh.verts) highest = std::min(highest, v.y);
    CHECK_EQ(highest, top - 400);

    // The game draws it (its model's slot looked up, 3009:B9F6): into a race (the README's keys), the player
    // put in the city among its buildings.
    int drawn = 0;
    m.cpu().add_watch(vette::host::Cpu::linear(game::emu_seg(0x3009), 0xB9F6), [&drawn](vette::host::Cpu& c) {
        drawn += c.regs.r[vette::host::AX] == game::kModelCount;
    });
    const auto press = [&](uint8_t sc) {
        m.key(sc);
        m.run_for(100'000'000);
        m.key(static_cast<uint8_t>(sc | 0x80));
    };
    const auto until = [&](double s) {
        while (m.emulated_ns() < static_cast<uint64_t>(s * 1e9)) m.run_for(10'000'000);
    };
    until(13), press(0x39), until(17), press(0x1C), until(21.2), press(0x1C), until(25), press(0x1C), until(30.5), press(0x1C);
    until(40);
    // In the street just south of a cell with the building, looking north at it.
    int bx = -1, by = -1;
    for (int cx = 1; cx < world.cells_x() && bx < 0; ++cx) {
        for (int cy = 0; cy < world.cells_y() && bx < 0; ++cy) {
            for (const auto& e : world.types[world.cell(cx, cy).type].list2) {
                if (e.routine == kBuilding) bx = cx, by = cy;
            }
        }
    }
    CHECK(bx > 0);
    const int sx = bx - 1;
    const auto put = [&](uint16_t off, int v) { game::wr16(m.memory(), game::kDataSeg, off, static_cast<uint16_t>(v)); };
    put(0x2D57, sx / 16), put(0x2D59, by / 16), put(0x2D35, sx % 16 * 2048 + 1024), put(0x2D37, by % 16 * 2048 + 128);
    put(0x2D3B, 0);
    until(44);
    std::printf("  the building drawn as model %d %d times in 4 s\n", game::kModelCount, drawn);
    CHECK(drawn > 0);
}
