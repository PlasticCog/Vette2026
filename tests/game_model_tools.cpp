// The object editor's building steps (game/model_tools.h): shapes made closed and facing out, faces
// extruded, mirror images kept, and "front" meaning what it does in the original's own models.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <memory>
#include <string>

#include "game/city_map.h"
#include "game/model_pack.h"
#include "game/model_tools.h"
#include "host/machine.h"
#include "test.h"

namespace fs = std::filesystem;
namespace game = vette::game;
using game::ModelData;
using game::Shape;

namespace {

ModelData empty_model() {
    ModelData m;
    m.verts = {{0, 0, 0}, {-50, 0, 0}, {0, 0, 50}, {0, 50, 0}};
    return m;
}

game::Vec3 middle_of(const ModelData& m, const std::vector<uint16_t>& prim) {
    game::Vec3 c{0, 0, 0};
    for (const uint16_t v : prim) {
        for (size_t k = 0; k < 3; ++k) c[k] += m.verts[v][k] / static_cast<double>(prim.size());
    }
    return c;
}

double dot(const game::Vec3& a, const game::Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The middle of the faces' vertices.
game::Vec3 vertex_middle(const ModelData& m, const std::vector<ModelData::Face>& faces) {
    std::set<uint16_t> vs;
    for (const auto& f : faces) {
        for (const auto& prim : f.prims) vs.insert(prim.begin(), prim.end());
    }
    game::Vec3 c{0, 0, 0};
    for (const uint16_t v : vs) {
        for (size_t k = 0; k < 3; ++k) c[k] += m.verts[v][k] / static_cast<double>(vs.size());
    }
    return c;
}

// Every edge of the faces is run both ways (a closed surface, wound one way round).
bool closed(const ModelData& m) {
    std::map<std::pair<uint16_t, uint16_t>, int> edges;
    for (const auto& f : m.faces) {
        for (const auto& prim : f.prims) {
            for (size_t i = 0; i < prim.size(); ++i) ++edges[{prim[i], prim[(i + 1) % prim.size()]}];
        }
    }
    for (const auto& [e, n] : edges) {
        if (n != 1 || !edges.count({e.second, e.first})) return false;
    }
    return true;
}

// Each face's front points away from `centre`.
bool facing_out(const ModelData& m, const game::Vec3& centre) {
    for (const auto& f : m.faces) {
        const game::Vec3 c = middle_of(m, f.prims[0]);
        const game::Vec3 out{c[0] - centre[0], c[1] - centre[1], c[2] - centre[2]};
        if (dot(game::front_of(m, f.prims[0]), out) <= 0) return false;
    }
    return true;
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

}  // namespace

TEST(model_tools_shapes_are_closed_and_face_out) {
    for (int s = 0; s < game::kShapeCount; ++s) {
        const auto shape = static_cast<Shape>(s);
        ModelData m = empty_model();
        std::string why;
        const auto faces = game::add_shape(m, shape, {100, -40, 20}, 64, 12, why);
        CHECK(!faces.empty());
        CHECK_EQ(static_cast<int>(m.verts.size()) - game::kReferenceVertices, game::shape_vertices(shape));
        CHECK(game::check_model(m).empty());
        for (const auto& f : m.faces) CHECK_EQ(static_cast<int>(f.colour), 12);
        if (shape == Shape::Plane) {
            // Open: facing up (-y), seen from both sides.
            CHECK(game::front_of(m, m.faces[0].prims[0])[1] < -0.99);
            CHECK((m.faces[0].flags & 1) == 0);
            continue;
        }
        CHECK(closed(m));
        CHECK(facing_out(m, vertex_middle(m, m.faces)));
        for (const auto& f : m.faces) CHECK((f.flags & 1) != 0);
        if (!closed(m) || !facing_out(m, vertex_middle(m, m.faces))) std::printf("  (shape %s)\n", game::shape_name(shape));
    }
    // No room: nothing changes.
    ModelData m = empty_model();
    std::string why;
    for (int i = 0; i < 4; ++i) game::add_shape(m, Shape::Sphere, {0, 0, 0}, 64, 15, why);  // 4 x 26 = 104
    const size_t verts = m.verts.size(), faces = m.faces.size();
    CHECK(game::add_shape(m, Shape::Sphere, {0, 0, 0}, 64, 15, why).empty());
    CHECK(!why.empty());
    CHECK_EQ(m.verts.size(), verts);
    CHECK_EQ(m.faces.size(), faces);
}

TEST(model_tools_extrude) {
    // A cube's top (the face whose front is up) pulled up 40: four sides, still closed.
    ModelData m = empty_model();
    std::string why;
    game::add_shape(m, Shape::Cube, {0, 0, 0}, 64, 7, why);
    int top = -1;
    for (size_t f = 0; f < m.faces.size(); ++f) {
        if (game::front_of(m, m.faces[f].prims[0])[1] < -0.99) top = static_cast<int>(f);
    }
    CHECK(top >= 0);
    if (top < 0) return;
    const auto moved = game::extrude(m, {static_cast<uint16_t>(top)}, 40, why);
    CHECK_EQ(moved.size(), size_t{1});
    CHECK_EQ(m.faces.size(), size_t{10});
    CHECK_EQ(m.verts.size(), size_t{game::kReferenceVertices + 12});
    CHECK(closed(m));
    for (const uint16_t v : m.faces[static_cast<size_t>(top)].prims[0]) CHECK_EQ(static_cast<int>(m.verts[v][1]), -72);
    CHECK(game::check_model(m).empty());
    // A plane standing free becomes a box: its top, four sides and a base where it was.
    ModelData p = empty_model();
    game::add_shape(p, Shape::Plane, {0, 0, 0}, 50, 7, why);
    game::extrude(p, {0}, 30, why);
    CHECK_EQ(p.faces.size(), size_t{6});
    CHECK(closed(p));
    CHECK(facing_out(p, {0, -15, 0}));
    // Lines aren't extruded.
    ModelData l = empty_model();
    l.verts.push_back({0, 0, 0});
    l.verts.push_back({10, 0, 0});
    ModelData::Face line;
    line.flags = 4;
    line.prims = {{4, 5}};
    l.faces.push_back(line);
    CHECK(game::extrude(l, {0}, 10, why).empty());
}

TEST(model_tools_mirror) {
    // A cube east of the middle, copied across it: each face has its image, each vertex its twin.
    ModelData m = empty_model();
    std::string why;
    const auto faces = game::add_shape(m, Shape::Wedge, {60, 0, 10}, 40, 9, why);
    CHECK_EQ(game::mirror_face(m, faces[0]), -1);
    const auto copies = game::mirror_copy_faces(m, faces);
    CHECK_EQ(copies.size(), faces.size());
    CHECK(closed(m));
    for (size_t i = 0; i < faces.size(); ++i) CHECK_EQ(game::mirror_face(m, faces[i]), static_cast<int>(copies[i]));
    // The copies face out from their own middle.
    std::vector<ModelData::Face> copied;
    for (const uint16_t f : copies) copied.push_back(m.faces[f]);
    const game::Vec3 copy_mid = vertex_middle(m, copied);
    CHECK(copy_mid[0] < -59 && copy_mid[0] > -61);
    for (const uint16_t f : copies) {
        const auto c = middle_of(m, m.faces[f].prims[0]);
        CHECK(dot(game::front_of(m, m.faces[f].prims[0]), {c[0] - copy_mid[0], c[1] - copy_mid[1], c[2] - copy_mid[2]}) > 0);
    }
    // A one-sided move keeps the image: the twin follows.
    const uint16_t v = m.faces[faces[0]].prims[0][0];
    const int twin = game::mirror_twin(m, v);
    CHECK(twin >= 0 && twin != v);
    const auto partners = game::mirror_partners(m, {v});
    CHECK_EQ(partners.size(), size_t{1});
    m.verts[v] = {45, -7, 3};
    game::keep_mirrored(m, partners);
    CHECK(m.verts[static_cast<size_t>(twin)] == (ModelData::Vertex{-45, -7, 3}));
    // Both picked: neither follows the other.
    CHECK(game::mirror_partners(m, {v, static_cast<uint16_t>(twin)}).empty());
    // One on the middle stays there.
    m.verts.push_back({0, -30, 0});
    const auto mid = static_cast<uint16_t>(m.verts.size() - 1);
    CHECK_EQ(game::mirror_twin(m, mid), static_cast<int>(mid));
    const auto stay = game::mirror_partners(m, {mid});
    m.verts[mid] = {12, -30, 5};
    game::keep_mirrored(m, stay);
    CHECK(m.verts[mid] == (ModelData::Vertex{0, -30, 5}));
}

TEST(model_tools_mirror_face_made_once) {
    ModelData m = empty_model();
    m.verts.push_back({10, 0, 0});
    m.verts.push_back({20, 0, 0});
    m.verts.push_back({20, -10, 0});
    ModelData::Face f;
    f.flags = 1;
    f.colour = 4;
    f.prims = {{4, 5, 6}};
    m.faces.push_back(f);
    const int made = game::add_mirror_face(m, 0);
    CHECK_EQ(made, 1);
    CHECK_EQ(m.verts.size(), size_t{10});
    CHECK_EQ(game::add_mirror_face(m, 0), -1);
    CHECK_EQ(game::mirror_face(m, 0), 1);
    // Facing the mirror way: the front's x turned round.
    const auto a = game::front_of(m, m.faces[0].prims[0]), b = game::front_of(m, m.faces[1].prims[0]);
    CHECK(std::abs(a[0] + b[0]) < 1e-9 && std::abs(a[1] - b[1]) < 1e-9 && std::abs(a[2] - b[2]) < 1e-9);
    // A symmetric face is its own image.
    m.verts.push_back({0, -20, 0});
    ModelData::Face across;
    across.prims = {{4, 7, 10}};
    m.faces.push_back(across);
    CHECK_EQ(game::mirror_face(m, 2), 2);
    CHECK_EQ(game::add_mirror_face(m, 2), -1);
}

// The front, as model_tools takes it, is the side the original's own models show: their culled faces
// face out from the model's middle.
TEST(model_tools_front_is_the_games) {
    const fs::path dir = game_dir();
    if (!fs::exists(dir / "VETTE.EXE")) {
        std::printf("  SKIPPED: no VETTE.EXE in %s\n", dir.string().c_str());
        return;
    }
    vette::host::MachineConfig config;
    config.game_dir = dir;
    config.save_dir = fs::temp_directory_path() / "vette2026_model_tools_test";
    vette::host::Machine machine(config);
    std::string error;
    CHECK(machine.boot(error) && game::run_until_started(machine));
    int out = 0, in = 0;
    for (const int id : {1, 2, 5, 14}) {
        const auto m = game::read_model(machine.memory(), id);
        CHECK(m.has_value());
        if (!m) continue;
        game::Vec3 centre{0, 0, 0};
        for (size_t v = game::kReferenceVertices; v < m->verts.size(); ++v) {
            for (size_t k = 0; k < 3; ++k) centre[k] += m->verts[v][k] / static_cast<double>(m->verts.size() - game::kReferenceVertices);
        }
        for (const auto& f : m->faces) {
            if (f.lines() || !(f.flags & 1) || f.prims[0].size() < 3) continue;
            const auto c = middle_of(*m, f.prims[0]);
            const double d = dot(game::front_of(*m, f.prims[0]), {c[0] - centre[0], c[1] - centre[1], c[2] - centre[2]});
            (d > 0 ? out : in) += 1;
        }
    }
    std::printf("  culled faces facing out %d, in %d\n", out, in);
    CHECK(out > 4 * in);
}
