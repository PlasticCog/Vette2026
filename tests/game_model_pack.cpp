// Objects of one's own (game/model_pack.h), with the game files: the original's models read, written as
// text and back unchanged, and an edited one put into the game's memory where the game finds it.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "game/city_map.h"
#include "game/model_pack.h"
#include "game/x86.h"
#include "host/machine.h"
#include "test.h"

namespace fs = std::filesystem;
using vette::game::ModelData;
using vette::game::ModelPack;

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

// VETTE.EXE started far enough to have unpacked itself (its models in place).
std::unique_ptr<vette::host::Machine> unpacked(const fs::path& dir) {
    vette::host::MachineConfig config;
    config.game_dir = dir;
    config.save_dir = fs::temp_directory_path() / "vette2026_model_pack_test";
    auto m = std::make_unique<vette::host::Machine>(config);
    std::string error;
    if (!m->boot(error) || !vette::game::run_until_started(*m)) return nullptr;
    return m;
}

}  // namespace

TEST(model_pack_text_round_trip) {
    ModelPack pack;
    ModelData m;
    m.verts = {{0, 0, 0}, {-50, 0, 0}, {0, 0, 50}, {0, 50, 0}, {10, -20, 30}, {-10, -20, 30}, {0, -40, -5}};
    ModelData::Face f;
    f.flags = 0x2001;
    f.colour = 0x4C;
    f.outline = 0;
    f.prims = {{4, 5, 6}};
    m.faces.push_back(f);
    f.flags = 0x0004;  // a line
    f.colour = 15;
    f.prims = {{4, 6}, {5, 6}};
    m.faces.push_back(f);
    m.order[0] = {1, 0};
    pack.models[7] = m;
    std::string error;
    const auto back = ModelPack::parse(pack.serialize(), error);
    CHECK(back.has_value());
    if (back) CHECK(back->models == pack.models);
    // Wrong ones are refused with the reason.
    CHECK(!ModelPack::parse("not a models file\n", error));
    CHECK(!ModelPack::parse("VETTE2026 MODELS 1\nmodel 7\nv 0 0 0\nf 0 0 0 1 2 3\nend\n", error));
    CHECK(!error.empty());
}

TEST(model_pack_reads_and_writes_the_games_models) {
    const fs::path dir = game_dir();
    if (!fs::exists(dir / "VETTE.EXE")) {
        std::printf("  SKIPPED: no VETTE.EXE in %s\n", dir.string().c_str());
        return;
    }
    auto machine = unpacked(dir);
    CHECK(machine != nullptr);
    if (!machine) return;
    auto& mem = machine->memory();
    // Every model there is reads, checks out, and survives the text form.
    ModelPack all;
    int present = 0;
    for (int id = 0; id < vette::game::kModelCount; ++id) {
        const auto m = vette::game::read_model(mem, id);
        if (!m) continue;
        ++present;
        CHECK_EQ(vette::game::check_model(*m), std::string());
        all.models[id] = *m;
    }
    std::printf("  %d models\n", present);
    CHECK(present >= 50);
    if (present < 50) return;
    std::string error;
    const auto back = ModelPack::parse(all.serialize(), error);
    CHECK(back.has_value());
    if (back) CHECK(back->models == all.models);
    // Written into memory as they are, they read back the same (their own orders kept).
    CHECK(vette::game::write_models(mem, all, error));
    for (const auto& [id, m] : all.models) {
        const auto again = vette::game::read_model(mem, id);
        CHECK(again.has_value());
        if (again) CHECK(*again == m);
    }
    // The Corvette changed: a vertex moved and a face added; its orders are made anew.
    ModelData car = all.models[1];
    car.verts[10][1] = static_cast<int16_t>(car.verts[10][1] - 40);
    ModelData::Face fin;
    fin.flags = 0x2000;
    fin.colour = 14;
    fin.prims = {{4, 5, 6}};
    car.faces.push_back(fin);
    for (auto& o : car.order) o.clear();
    ModelPack edit;
    edit.models[1] = car;
    CHECK(vette::game::write_models(mem, edit, error));
    const auto read = vette::game::read_model(mem, 1);
    CHECK(read.has_value());
    if (read) {
        CHECK(read->verts == car.verts);
        CHECK_EQ(read->faces.size(), car.faces.size());
        for (const auto& o : read->order) CHECK_EQ(o.size(), car.faces.size());
    }
    // Its header now names segment 8000h.
    const uint16_t header = vette::game::rd16(mem, vette::game::emu_seg(0x245A), 0x6FF8 + 8);
    CHECK_EQ(vette::game::rd16(mem, vette::game::emu_seg(0x245A), header), 0x8000);
}

TEST(model_pack_segment_left_alone_in_a_race) {
    // The edited models live at 8000h: nothing of the game's may write there.
    const fs::path dir = game_dir();
    if (!fs::exists(dir / "VETTE.EXE")) {
        std::printf("  SKIPPED: no VETTE.EXE in %s\n", dir.string().c_str());
        return;
    }
    vette::host::MachineConfig config;
    config.game_dir = dir;
    config.save_dir = fs::temp_directory_path() / "vette2026_model_pack_test";
    vette::host::Machine m(config);
    std::string error;
    CHECK(m.boot(error));
    for (uint32_t a = 0x80000; a < 0x90000; ++a) m.memory().write8(a, static_cast<uint8_t>(a * 7 + 3));
    const auto press = [&](uint8_t sc) {
        m.key(sc);
        m.run_for(100'000'000);
        m.key(static_cast<uint8_t>(sc | 0x80));
    };
    const auto until = [&](double s) {
        while (m.emulated_ns() < static_cast<uint64_t>(s * 1e9)) m.run_for(10'000'000);
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
    until(60);
    int changed = 0;
    for (uint32_t a = 0x80000; a < 0x90000; ++a) changed += m.memory().read8(a) != static_cast<uint8_t>(a * 7 + 3);
    CHECK_EQ(changed, 0);
}
