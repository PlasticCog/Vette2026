// An edited model (game/model_pack.h) in the Enhanced view's world: the game's memory holds it at
// segment 8000h, and the extraction reads it from there.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "enhanced/world.h"
#include "game/city_map.h"
#include "game/model_pack.h"
#include "host/machine.h"
#include "test.h"

namespace fs = std::filesystem;

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

}  // namespace

TEST(enhanced_world_has_the_edited_model) {
    const fs::path dir = game_dir();
    if (!fs::exists(dir / "VETTE.EXE")) {
        std::printf("  SKIPPED: no VETTE.EXE in %s\n", dir.string().c_str());
        return;
    }
    // The original's generic car (model 3), read from a first start.
    vette::game::ModelData car;
    {
        vette::host::MachineConfig config;
        config.game_dir = dir;
        config.save_dir = fs::temp_directory_path() / "vette2026_model_pack_test";
        vette::host::Machine m(config);
        std::string error;
        CHECK(m.boot(error));
        m.run_for(2'000'000'000);
        const auto read = vette::game::read_model(m.memory(), 3);
        CHECK(read.has_value());
        if (!read) return;
        car = *read;
    }
    // Twice as tall, every face magenta, and a new face on top.
    for (size_t i = vette::game::kReferenceVertices; i < car.verts.size(); ++i) car.verts[i][1] = static_cast<int16_t>(car.verts[i][1] * 2);
    for (auto& f : car.faces) f.colour = 13;
    vette::game::ModelData::Face top;
    top.flags = 0x2000;
    top.colour = 14;
    top.prims = {{4, 5, 6}};
    car.faces.push_back(top);
    for (auto& o : car.order) o.clear();
    vette::game::ModelPack pack;
    pack.models[3] = car;

    vette::host::MachineConfig config;
    config.game_dir = dir;
    config.save_dir = fs::temp_directory_path() / "vette2026_model_pack_test";
    vette::host::Machine m(config);
    vette::game::install_model_pack(m, pack);
    std::string error;
    CHECK(m.boot(error));
    m.run_for(2'000'000'000);
    vette::enhanced::World world;
    CHECK(vette::enhanced::extract_world(m, world, error));
    const vette::enhanced::Model& model = world.models[3];
    CHECK(model.present);
    CHECK_EQ(model.near_mesh.verts.size(), car.verts.size());
    for (size_t i = 0; i < car.verts.size() && i < model.near_mesh.verts.size(); ++i) {
        CHECK_EQ(model.near_mesh.verts[i].y, car.verts[i][1]);
    }
    CHECK_EQ(model.near_mesh.faces.size(), car.faces.size());
    for (const auto& f : model.near_mesh.faces) CHECK(f.colour.base() == 13 || f.colour.base() == 14);
}
