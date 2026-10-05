// Pure parts of the Enhanced world model (src/enhanced/world.h, world_decode.h, world_probe.h): the
// packed-vertex decoder, the model and list decoders on a synthetic memory image, the geometry
// conventions, variant selection and trace signatures. The extraction itself is checked against the
// original game by vette_world (--validate, --teleport).

#include <cstddef>
#include <vector>

#include "enhanced/world.h"
#include "enhanced/world_decode.h"
#include "enhanced/world_probe.h"
#include "test.h"

using namespace vette::enhanced;

namespace {

// A 1 MB memory image with little-endian word writes.
struct FakeRam {
    std::vector<uint8_t> ram = std::vector<uint8_t>(0x100000, 0);
    void w16(uint16_t seg, uint16_t off, uint16_t v) {
        const uint32_t a = ((uint32_t{seg} << 4) + off) & 0xFFFFF;
        ram[a] = static_cast<uint8_t>(v);
        ram[(a + 1) & 0xFFFFF] = static_cast<uint8_t>(v >> 8);
    }
    ImageView view() const { return {ram.data()}; }
};

bool near(double a, double b) { return a - b < 1e-9 && b - a < 1e-9; }

} // namespace

// SAR then ADC 0: the last bit shifted out rounds up.
static_assert(sar_round(3072, 4) == 192);
static_assert(sar_round(7, 1) == 4);
static_assert(sar_round(-7, 1) == -3);
static_assert(sar_round(5, 0) == 5);
static_assert(sar_round(-1000, 20) == 0);  // counts past 15 leave the sign; CF = the sign bit
static_assert(sar_round(1024, 33) == 512);  // the 286 masks the count to 5 bits

TEST(enhanced_unpack_vertices) {
    const AxisTable t = ideal_axis_table();
    // Word 0 selects axis 0 (east) with k = 1, shift 2: +256 east. Word 2 selects axis 2 (north),
    // k = 1, negative, shift 3: -128 north. Word 1 selects axis 1 (up) with k = 3 (selector 18), no shift.
    const int16_t words[] = {
        0x0206, 0, 0,                       // vertex 1
        0, 0x0012, static_cast<int16_t>(0x03FA),  // vertex 2: +3072 up, -128 north
        0x0030, 0, 0,                       // vertex 3: selector 48 reaches axis 0's k = 11
        0x0036, 0, 0,                       // vertex 4: selector 54 crosses into axis 1 (up), k = 1
    };
    std::vector<V3s> out;
    CHECK(unpack_vertices({100, 200, 300}, words, 4, t, out));
    CHECK_EQ(out.size(), size_t{5});
    CHECK(out[0] == (V3s{100, 200, 300}));
    CHECK(out[1] == (V3s{100, 456, 300}));
    CHECK(out[2] == (V3s{-28, 456, 3372}));
    CHECK(out[3] == (V3s{-28, 456 + 11 * 1024, 3372}));
    CHECK(out[4] == (V3s{-28, 456 + 11 * 1024, 3372 + 1024}));

    // A selector that isn't a multiple of 6, or a zero selector with a shift, reads outside the table.
    const int16_t bad1[] = {0x0003, 0, 0};
    const int16_t bad2[] = {0x0100, 0, 0};
    CHECK(!unpack_vertices({0, 0, 0}, bad1, 1, t, out));
    CHECK(!unpack_vertices({0, 0, 0}, bad2, 1, t, out));
}

TEST(enhanced_unpack_wraps_16bit) {
    const AxisTable t = ideal_axis_table();
    const int16_t words[] = {0x0036 /* +1024 up via word 0 */, 0, 0};
    std::vector<V3s> out;
    CHECK(unpack_vertices({0, 0, 32000}, words, 1, t, out));
    CHECK_EQ(out[1][2], static_cast<int16_t>(-32512));  // 32000 + 1024 wraps, as the original's ADD
}

TEST(enhanced_decode_model_mesh) {
    FakeRam m;
    const uint16_t seg = 0x2000, hdr = 0x0100, verts = 0x0200, octs = 0x0300, list = 0x0400, face = 0x0500;
    m.w16(seg, hdr, seg);
    m.w16(seg, hdr + 2, 5);
    m.w16(seg, hdr + 4, verts);
    m.w16(seg, hdr + 6, octs);
    const int16_t v[5][3] = {{0, 0, 0}, {-50, 0, 0}, {0, 0, 50}, {0, 50, 0}, {10, -20, 30}};
    for (int i = 0; i < 5; ++i) {
        for (int c = 0; c < 3; ++c) m.w16(seg, static_cast<uint16_t>(verts + 6 * i + 2 * c), static_cast<uint16_t>(v[i][c]));
    }
    for (int o = 0; o < 8; ++o) m.w16(seg, static_cast<uint16_t>(octs + 2 * o), list);
    m.w16(seg, list, 1);  // countdown 1 = vertex 4 is skipped in this view
    m.w16(seg, list + 2, 0xFFFF);
    m.w16(seg, list + 4, face);
    m.w16(seg, list + 6, 0xFFFF);
    m.w16(seg, face, 0x0001);      // winding-culled convex fill
    m.w16(seg, face + 2, 0x0C07);  // colour 07, high byte 0C
    m.w16(seg, face + 4, 3);       // 3 vertices, closing repeat
    m.w16(seg, face + 6, 4 * 1);
    m.w16(seg, face + 8, 4 * 2);
    m.w16(seg, face + 10, 4 * 3);
    m.w16(seg, face + 12, 4 * 1);
    m.w16(seg, face + 14, 0xFFFF);

    ModelMesh mesh;
    std::string error;
    CHECK(decode_model_mesh(m.view(), seg, hdr, mesh, error));
    CHECK_EQ(mesh.verts.size(), size_t{5});
    CHECK(mesh.verts[4] == (Vec3i{10, -20, 30}));
    CHECK_EQ(mesh.faces.size(), size_t{1});
    CHECK_EQ(mesh.faces[0].colour.raw, uint8_t{0x07});
    CHECK_EQ(mesh.faces[0].colour_hi, uint8_t{0x0C});
    CHECK(mesh.faces[0].prims == (std::vector<std::vector<uint16_t>>{{1, 2, 3}}));
    CHECK(!mesh.faces[0].lines());
    for (int o = 0; o < 8; ++o) {
        CHECK(mesh.order[static_cast<size_t>(o)] == std::vector<uint16_t>{0});
        CHECK(mesh.skipped[static_cast<size_t>(o)] == std::vector<uint16_t>{4});
    }
    // The far box's colour patch.
    CHECK(decode_model_mesh(m.view(), seg, hdr, mesh, error, 0x0009, {static_cast<uint16_t>(face + 2)}));
    CHECK_EQ(mesh.faces[0].colour.raw, uint8_t{0x09});

    // A header in another segment is refused.
    m.w16(seg, hdr, 0x1234);
    CHECK(!decode_model_mesh(m.view(), seg, hdr, mesh, error));
}

TEST(enhanced_read_entry_list) {
    FakeRam m;
    const uint16_t ds = 0x3000;
    m.w16(ds, 0x10, 0x7270);
    m.w16(ds, 0x12, 0);
    m.w16(ds, 0x14, static_cast<uint16_t>(-64));
    m.w16(ds, 0x16, 224);
    m.w16(ds, 0x18, 0xFFFF);
    std::vector<ListEntry> l;
    CHECK_EQ(read_entry_list(m.view(), ds, 0x10, l), uint16_t{0x1A});
    CHECK_EQ(l.size(), size_t{1});
    CHECK_EQ(l[0].routine, uint16_t{0x7270});
    CHECK_EQ(l[0].dy, int16_t{-64});
    CHECK_EQ(l[0].dz, int16_t{224});
    m.w16(ds, 0x18, 0x1234);  // no terminator within the limit
    CHECK_EQ(read_entry_list(m.view(), ds, 0x10, l, 1), uint16_t{0});
}

TEST(enhanced_geometry_conventions) {
    // Yaw turns local north towards east.
    const Vec3d r = rotate_local({1, 0, 0}, 90, 0, 0);
    CHECK(near(r.x, 0) && near(r.y, 1) && near(r.z, 0));
    const Vec3d r2 = rotate_local({0, 1, 0}, 90, 0, 0);  // local east goes south
    CHECK(near(r2.x, -1) && near(r2.y, 0));
    // Model axes: x east, y down, z north.
    const Vec3d m = model_to_world({3, -5, 10}, 0);
    CHECK(near(m.x, 10) && near(m.y, 3) && near(m.z, 5));
    const Vec3d m90 = model_to_world({0, 0, 10}, 90);
    CHECK(near(m90.x, 0) && near(m90.y, 10));
    // Octant bits: -x side, +z side, +y (below) side.
    CHECK_EQ(model_octant({-100, -100, 100}), 3);
    CHECK_EQ(model_octant({100, 100, -100}), 4);
}

TEST(enhanced_face_visible) {
    const Vec3d v[3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    Cull behind;
    behind.kind = Cull::Kind::Behind;
    behind.v[0] = 0;  // A, on the face
    behind.v[1] = 1;  // B, behind it (+x)
    CHECK(face_visible(behind, v, {-10, 0, 0}));
    CHECK(!face_visible(behind, v, {10, 0, 0}));
    // A three-vertex normal is visible from exactly one side.
    Cull plane;
    plane.kind = Cull::Kind::Plane;
    plane.v[0] = 0;
    plane.v[1] = 1;
    plane.v[2] = 2;
    CHECK(face_visible(plane, v, {0, 0, 10}) != face_visible(plane, v, {0, 0, -10}));
    CHECK(face_visible(Cull{}, v, {0, 0, 0}));
}

TEST(enhanced_colour) {
    Colour c;
    c.raw = 0x07;
    CHECK(!c.dithered());
    c.raw = 0x77;
    CHECK(!c.dithered());
    c.raw = 0x9B;
    CHECK(c.dithered());
    CHECK_EQ(c.base(), uint8_t{0x0B});
    CHECK_EQ(c.second(), uint8_t{0x09});
}

TEST(enhanced_select_variant) {
    Routine r;
    Variant banner;  // course 1's finish banner
    banner.when.course = 1u << 1;
    banner.primitives = 7;
    banner.sets_finish_flag = 1;
    Variant none;  // other courses: nothing
    none.when.course = (1u << 2) | (1u << 3) | (1u << 4);
    r.variants = {banner, none};
    DrawState s;
    s.course = 1;
    CHECK(r.select(s) == &r.variants[0]);
    s.course = 3;
    CHECK(r.select(s) == &r.variants[1]);

    Routine w;  // windows on / off
    Variant with;
    with.when.windows = 1;
    Variant without;
    without.when.windows = 2;
    w.variants = {with, without};
    s.windows = true;
    CHECK(w.select(s) == &w.variants[0]);
    s.windows = false;
    CHECK(w.select(s) == &w.variants[1]);
}

TEST(enhanced_ground_z) {
    World w;
    w.big_rows = w.big_cols = 1;
    w.cells.assign(256, Cell{});
    w.cells[0] = {5, 1};  // cell (0, 0): type 5, elevation 1
    w.types[5].used = true;
    w.types[5].ground_class = 2;  // rises with u (x) across 0x700
    CHECK_EQ(w.ground_z(0, 0), 224);
    CHECK_EQ(w.ground_z(0x380, 0x500), 224 + (0x380 >> 3));
    CHECK_EQ(w.ground_z(0x7FF, 0), 224 + (0x700 >> 3));
    w.types[5].ground_class = 0;
    CHECK_EQ(w.ground_z(0x380, 0x500), 224);
}

TEST(enhanced_trace_signature) {
    Trace a;
    TraceEvent e;
    e.op = TraceEvent::Op::Plain;
    e.addr = 0x4263;
    e.count = 7;
    e.angles = {10, 0, 0};
    a.events.push_back(e);
    Trace b = a;
    b.events[0].angles = {200, 0, 0};  // angles are dynamic state, not identity
    CHECK_EQ(a.signature(), b.signature());
    b.events[0].pos = {1, 0, 0};
    CHECK(a.signature() != b.signature());
}
