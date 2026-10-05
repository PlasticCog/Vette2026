// The Enhanced SceneBuilder (src/enhanced/scene.h): its geometry helpers (scene_geometry.h), the colours,
// and whole builds on a small synthetic world (projection, viewport, painter's order, radius and view
// culls, warm rebuilds). The builder is checked against the original game by vette_world
// (--scene-compare, --scene-shots).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "enhanced/scene.h"
#include "enhanced/scene_geometry.h"
#include "enhanced/world.h"
#include "enhanced/world_probe.h"
#include "test.h"

using namespace vette::enhanced;
namespace geo = vette::enhanced::geometry;

namespace {

bool close(double a, double b, double eps = 1e-3) { return std::fabs(a - b) <= eps; }

// Twice the signed area of a 2D polygon.
double area2(const float* x, const float* y, int n) {
    double a = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) a += double(x[j]) * y[i] - double(x[i]) * y[j];
    return a;
}

// Sum of the triangles' |areas| and whether they all wind like the polygon.
struct TriSum {
    double area = 0;
    bool same_winding = true;
};
TriSum triangles_2d(const float* x, const float* y, const std::vector<uint16_t>& tri, double polygon_sign) {
    TriSum s;
    for (size_t t = 0; t + 2 < tri.size(); t += 3) {
        const float tx[3] = {x[tri[t]], x[tri[t + 1]], x[tri[t + 2]]};
        const float ty[3] = {y[tri[t]], y[tri[t + 1]], y[tri[t + 2]]};
        const double a = area2(tx, ty, 3);
        s.area += std::fabs(a);
        if (a * polygon_sign < -1e-6) s.same_winding = false;
    }
    return s;
}

double area3(const geo::P3* p, const std::vector<uint16_t>& tri) {
    double a = 0;
    for (size_t t = 0; t + 2 < tri.size(); t += 3) {
        const geo::P3 &A = p[tri[t]], &B = p[tri[t + 1]], &C = p[tri[t + 2]];
        const double ux = B.x - A.x, uy = B.y - A.y, uz = B.z - A.z;
        const double vx = C.x - A.x, vy = C.y - A.y, vz = C.z - A.z;
        const double cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
        a += std::sqrt(cx * cx + cy * cy + cz * cz);
    }
    return a;
}

} // namespace

TEST(enhanced_scene_ear_clip_convex_and_concave) {
    // A square, both windings: 2 triangles covering it.
    for (const int dir : {1, -1}) {
        float x[] = {0, 10, 10, 0};
        float y[] = {0, 0, 10, 10};
        if (dir < 0) {
            std::reverse(x, x + 4);
            std::reverse(y, y + 4);
        }
        std::vector<uint16_t> tri;
        CHECK(geo::ear_clip(x, y, 4, tri));
        CHECK_EQ(tri.size(), size_t{6});
        const double a = area2(x, y, 4);
        const TriSum s = triangles_2d(x, y, tri, a);
        CHECK(close(s.area, std::fabs(a)));
        CHECK(s.same_winding);
    }
    // An L (concave at vertex 3): n - 2 triangles, none outside the outline.
    const float lx[] = {0, 20, 20, 8, 8, 0};
    const float ly[] = {0, 0, 6, 6, 20, 20};
    std::vector<uint16_t> tri;
    CHECK(geo::ear_clip(lx, ly, 6, tri));
    CHECK_EQ(tri.size(), size_t{12});
    const double a = area2(lx, ly, 6);
    const TriSum s = triangles_2d(lx, ly, tri, a);
    CHECK(close(s.area, std::fabs(a)));
    CHECK(s.same_winding);
    // An arrow head (concave at the notch), clockwise.
    const float ax[] = {0, 5, 10, 5};
    const float ay[] = {0, 10, 0, 4};
    tri.clear();
    CHECK(geo::ear_clip(ax, ay, 4, tri));
    const double aa = area2(ax, ay, 4);
    const TriSum sa = triangles_2d(ax, ay, tri, aa);
    CHECK(close(sa.area, std::fabs(aa)));
    CHECK(sa.same_winding);
}

TEST(enhanced_scene_ear_clip_collinear) {
    // A square with an extra point in the middle of each of two edges.
    const float x[] = {0, 5, 10, 10, 10, 0};
    const float y[] = {0, 0, 0, 5, 10, 10};
    std::vector<uint16_t> tri;
    CHECK(geo::ear_clip(x, y, 6, tri));
    const double a = area2(x, y, 6);
    const TriSum s = triangles_2d(x, y, tri, a);
    CHECK(close(s.area, std::fabs(a)));
    CHECK(s.same_winding);
    for (const uint16_t i : tri) CHECK(i < 6);
}

TEST(enhanced_scene_triangulate_3d) {
    // A concave polygon in a vertical plane (x = 7): the dominant-plane projection keeps it simple.
    const geo::P3 p[] = {{7, 0, 0}, {7, 20, 0}, {7, 20, 6}, {7, 8, 6}, {7, 8, 20}, {7, 0, 20}};
    std::vector<uint16_t> tri;
    CHECK(geo::triangulate(p, 6, tri));
    CHECK_EQ(tri.size(), size_t{12});
    CHECK(close(area3(p, tri), 2 * (20.0 * 6 + 8.0 * 14)));
    // Tilted: the same shape rotated about the z axis by 30 degrees.
    geo::P3 q[6];
    const double c = std::cos(0.5235987756), sn = std::sin(0.5235987756);
    for (int i = 0; i < 6; ++i) {
        q[i] = {static_cast<float>(p[i].x * c - p[i].y * sn), static_cast<float>(p[i].x * sn + p[i].y * c), p[i].z};
    }
    tri.clear();
    CHECK(geo::triangulate(q, 6, tri));
    CHECK(close(area3(q, tri), 2 * (20.0 * 6 + 8.0 * 14), 1e-2));
}

TEST(enhanced_scene_clip_near) {
    geo::P3 out[8];
    // All in front: unchanged.
    const geo::P3 front[] = {{0, 0, 5}, {1, 0, 5}, {0, 1, 6}};
    CHECK_EQ(geo::clip_near(front, 3, 1, out), 3);
    // All behind: nothing.
    const geo::P3 behind[] = {{0, 0, -5}, {1, 0, 0.5f}, {0, 1, -1}};
    CHECK_EQ(geo::clip_near(behind, 3, 1, out), 0);
    // One vertex behind: a quad, every point on or past the plane, the crossings on it.
    const geo::P3 one[] = {{0, 0, -1}, {4, 0, 3}, {0, 4, 3}};
    const int n = geo::clip_near(one, 3, 1, out);
    CHECK_EQ(n, 4);
    int on_plane = 0;
    for (int i = 0; i < n; ++i) {
        CHECK(out[i].z >= 1 - 1e-5f);
        if (close(out[i].z, 1, 1e-5)) ++on_plane;
    }
    CHECK_EQ(on_plane, 2);
    // The crossing of (0,0,-1)-(4,0,3) with z = 1 is at its midpoint.
    bool found = false;
    for (int i = 0; i < n; ++i) found |= close(out[i].x, 2) && close(out[i].y, 0) && close(out[i].z, 1);
    CHECK(found);
}

TEST(enhanced_scene_degenerate) {
    const geo::P3 line[] = {{0, 0, 1}, {1, 1, 2}, {2, 2, 3}};
    CHECK(geo::degenerate(line, 3));
    const geo::P3 tri[] = {{0, 0, 1}, {10, 0, 1}, {0, 10, 1}};
    CHECK(!geo::degenerate(tri, 3));
}

TEST(enhanced_scene_colours) {
    const SceneColour black = ega_colour(0), white = ega_colour(15), brown = ega_colour(6);
    CHECK(close(black.r, 0) && close(black.g, 0) && close(black.b, 0));
    CHECK(close(white.r, 1) && close(white.g, 1) && close(white.b, 1));
    CHECK(close(brown.r, 0xAA / 255.0) && close(brown.g, 0x55 / 255.0) && close(brown.b, 0));
    // A checkerboard pair averages its two colours; a repeated or zero high nibble is the base colour.
    Colour c;
    c.raw = 0xF0;  // white over black
    const SceneColour grey = scene_colour(c);
    CHECK(close(grey.r, 0.5) && close(grey.g, 0.5) && close(grey.b, 0.5));
    c.raw = 0x44;
    const SceneColour red = scene_colour(c);
    CHECK(close(red.r, ega_colour(4).r) && close(red.g, 0) && close(red.b, 0));
    c.raw = 0x09;
    CHECK(close(scene_colour(c).b, ega_colour(9).b));
}

namespace {

// A flat 5 x 5 big-tile world, empty except for the walls a test places: routines 1001h (blue), 1002h
// (green) and 1004h (red) are a 1024 x 256 upright quad facing north, centred on the entry; 1003h (cyan)
// is its east half only; 1005h (magenta) is a 512 x 256 quad facing east, north of the entry. Each is one
// Packed part with one Polygon prim.
struct SyntheticWorld {
    World world;
    std::vector<uint8_t> ram = std::vector<uint8_t>(0x100000, 0);

    SyntheticWorld() {
        world.big_rows = world.big_cols = 5;
        world.cells.assign(static_cast<size_t>(kMapCells * kMapCells), Cell{});
        world.big_tile_ground.fill(7);
        for (const int colour : {1, 2, 3, 4, 5}) {
            Routine r;
            r.address = static_cast<uint16_t>(0x1000 + colour);
            Variant& v = r.variants.emplace_back();
            Part& p = v.parts.emplace_back();
            p.source = Part::Source::Packed;
            if (colour == 3) {
                p.verts = {{0, 0, 0}, {0, 512, 0}, {0, 512, 256}, {0, 0, 256}};
            } else if (colour == 5) {
                p.verts = {{0, 0, 0}, {512, 0, 0}, {512, 0, 256}, {0, 0, 256}};
            } else {
                p.verts = {{0, -512, 0}, {0, 512, 0}, {0, 512, 256}, {0, -512, 256}};
            }
            p.indices = {0, 1, 2, 3};
            Prim prim;
            prim.kind = Prim::Kind::Polygon;
            prim.colour.raw = static_cast<uint8_t>(colour);
            prim.count = 4;
            p.prims.push_back(prim);
            v.primitives = 1;
            world.routines.push_back(std::move(r));
        }
        // Camera: big tile (2, 2), at the centre of cell (40, 40), 10 units up, facing north (+x).
        w16(addr::kCamera, 0x4400);
        w16(addr::kCamera + 2, 0x4400);
        w16(addr::kCamera + 4, 10);
        w16(0x2C93, 2);
        w16(0x2C95, 2);
        // The normal race viewport: 320 x 120, centre (160, 60).
        w16(0x315E, 0);
        w16(0x315A, 0);
        w16(0x3160, 319);
        w16(0x315C, 119);
        w16(0x3169, 160);
        w16(0x316B, 60);
        w16(addr::kCourse, 1);
        w16(0x856F, 5);
        w16(0x8571, 5);
        // No vehicles or pedestrians: every big tile's lists point at an end marker.
        w16(0x9000, 0xFFFF);
        for (int bt = 0; bt < 25; ++bt) {
            w16(static_cast<uint16_t>(0xEF5A + 2 * bt), 0x9000);
            w16(static_cast<uint16_t>(0xEF8C + 2 * bt), 0x9000);
        }
    }
    void w16(uint16_t off, uint16_t v) {
        const uint32_t a = (uint32_t{addr::kDataSeg} << 4) + off;
        ram[a] = static_cast<uint8_t>(v);
        ram[a + 1] = static_cast<uint8_t>(v >> 8);
    }
    // A cell type holding the given walls (routine address, dx, dy) in list 1 or list 2.
    void place(int cx, int cy, uint8_t type, std::vector<ListEntry> list1, std::vector<ListEntry> list2) {
        CellType& t = world.types[type];
        t.used = true;
        t.list1 = std::move(list1);
        t.list2 = std::move(list2);
        world.cells[static_cast<size_t>(cx * kMapCells + cy)].type = type;
    }
};

// First triangle (index into the triangle list) drawn in a colour, or -1.
int first_triangle(const Scene& s, const SceneColour& c) {
    for (size_t t = 0; t < s.indices.size() / 3; ++t) {
        const SceneVertex& v = s.vertices[static_cast<size_t>(s.indices[t * 3])];
        if (close(v.r, c.r) && close(v.g, c.g) && close(v.b, c.b)) return static_cast<int>(t);
    }
    return -1;
}

} // namespace

TEST(enhanced_scene_build_projection) {
    SyntheticWorld sw;
    // A red wall at the centre of cell (42, 40): 4096 units ahead of the camera.
    sw.place(42, 40, 1, {{0x1004, 1024, 1024, 0}}, {});
    SceneBuilder builder(sw.world);
    Scene scene;
    SceneOptions o;
    o.ground = false;
    builder.build(sw.ram.data(), o, scene);
    CHECK_EQ(scene.view_x0, 0);
    CHECK_EQ(scene.view_y0, 0);
    CHECK_EQ(scene.view_x1, 320);
    CHECK_EQ(scene.view_y1, 120);
    CHECK_EQ(scene.stats.objects, 1);
    CHECK_EQ(scene.stats.triangles, 2);
    CHECK_EQ(scene.indices.size(), size_t{6});
    CHECK_EQ(scene.vertices.size(), size_t{4});
    // focal 256: x = 160 +- 512 * 256 / 4096, y = 60 + (10 - z) * 256 / 4096.
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (const SceneVertex& v : scene.vertices) {
        x0 = std::min(x0, v.x);
        x1 = std::max(x1, v.x);
        y0 = std::min(y0, v.y);
        y1 = std::max(y1, v.y);
        CHECK(close(v.r, ega_colour(4).r) && close(v.a, 1));
    }
    CHECK(close(x0, 128));
    CHECK(close(x1, 192));
    CHECK(close(y0, 60 - 246 * 256.0 / 4096));
    CHECK(close(y1, 60 + 10 * 256.0 / 4096));

    // East is to the right: the cyan half-wall spans x 160..192.
    sw.place(42, 40, 1, {{0x1003, 1024, 1024, 0}}, {});
    SceneBuilder builder2(sw.world);
    builder2.build(sw.ram.data(), o, scene);
    CHECK_EQ(scene.vertices.size(), size_t{4});
    x0 = 1e9f;
    x1 = -1e9f;
    for (const SceneVertex& v : scene.vertices) {
        x0 = std::min(x0, v.x);
        x1 = std::max(x1, v.x);
    }
    CHECK(close(x0, 160));
    CHECK(close(x1, 192));
}

TEST(enhanced_scene_build_order_and_culls) {
    SyntheticWorld sw;
    // Cell (42, 40): list 2 holds a near red wall then a far blue one (drawn far first). Cell (46, 40)
    // holds a green wall (a farther cell: drawn before both). Cell (36, 40) is behind the camera.
    sw.place(42, 40, 1, {}, {{0x1004, 200, 1024, 0}, {0x1001, 1800, 1024, 0}});
    sw.place(46, 40, 2, {{0x1002, 1024, 1024, 0}}, {});
    sw.place(36, 40, 3, {{0x1002, 1024, 1024, 0}}, {});
    SceneBuilder builder(sw.world);
    Scene scene;
    SceneOptions o;
    builder.build(sw.ram.data(), o, scene);
    CHECK_EQ(scene.stats.objects, 3);  // the wall behind is culled with its cell
    CHECK(scene.stats.cells_culled > 0);
    const int red = first_triangle(scene, ega_colour(4));
    const int blue = first_triangle(scene, ega_colour(1));
    const int green = first_triangle(scene, ega_colour(2));
    const int ground = first_triangle(scene, ega_colour(7));
    CHECK(red >= 0 && blue >= 0 && green >= 0 && ground >= 0);
    CHECK(ground < green);
    CHECK(green < blue);
    CHECK(blue < red);
    for (const int32_t i : scene.indices) CHECK(i >= 0 && static_cast<size_t>(i) < scene.vertices.size());
    // Nothing on the flat ground rises above the horizon (pitch 0: row 60).
    for (const SceneVertex& v : scene.vertices) {
        if (close(v.r, ega_colour(7).r) && close(v.g, ega_colour(7).g)) CHECK(v.y >= 60 - 1e-3f);
    }

    // Radius 3: cell (46, 40) is 6 cells away.
    o.radius = 3;
    builder.build(sw.ram.data(), o, scene);
    CHECK_EQ(scene.stats.objects, 2);
    CHECK_EQ(first_triangle(scene, ega_colour(2)), -1);

    // Warm rebuilds give the same scene without growing the buffers.
    o.radius = kMapCells;
    builder.build(sw.ram.data(), o, scene);
    const std::vector<SceneVertex> first = scene.vertices;
    const size_t vcap = scene.vertices.capacity(), icap = scene.indices.capacity();
    builder.build(sw.ram.data(), o, scene);
    CHECK_EQ(scene.vertices.size(), first.size());
    CHECK_EQ(scene.vertices.capacity(), vcap);
    CHECK_EQ(scene.indices.capacity(), icap);
    bool same = scene.vertices.size() == first.size();
    for (size_t i = 0; same && i < first.size(); ++i) same = first[i].x == scene.vertices[i].x && first[i].y == scene.vertices[i].y;
    CHECK(same);
}

TEST(enhanced_scene_build_yaw) {
    SyntheticWorld sw;
    // Facing east (yaw 90): the red wall north of the camera is out of view; the magenta wall 4096 units
    // east, which extends north of its entry, is drawn left of the centre (north is on the left).
    sw.place(42, 40, 1, {{0x1004, 1024, 1024, 0}}, {});
    sw.place(40, 42, 2, {{0x1005, 1024, 1024, 0}}, {});
    sw.w16(addr::kCamera + 6, 90);
    SceneBuilder builder(sw.world);
    Scene scene;
    SceneOptions o;
    o.ground = false;
    builder.build(sw.ram.data(), o, scene);
    CHECK_EQ(scene.stats.objects, 1);
    CHECK_EQ(first_triangle(scene, ega_colour(4)), -1);
    CHECK_EQ(scene.vertices.size(), size_t{4});
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (const SceneVertex& v : scene.vertices) {
        x0 = std::min(x0, v.x);
        x1 = std::max(x1, v.x);
        y0 = std::min(y0, v.y);
        y1 = std::max(y1, v.y);
    }
    CHECK(close(x0, 128, 1e-2));
    CHECK(close(x1, 160, 1e-2));
    CHECK(close(y0, 60 - 246 * 256.0 / 4096, 1e-2));
    CHECK(close(y1, 60 + 10 * 256.0 / 4096, 1e-2));
}

TEST(enhanced_scene_build_unrotated_model) {
    // A segment-245A model drawn without rotation (3009:B9E6) is still in model axes (x east, y down,
    // z north): a south-facing 1024 x 256 face 4096 units north of the camera spans x 128..192.
    SyntheticWorld sw;
    Model& m = sw.world.models[3];
    m.present = true;
    m.near_mesh.verts = {{0, 0, 0}, {-50, 0, 0}, {0, 0, 50}, {0, 50, 0},
                         {-512, 0, 0}, {512, 0, 0}, {512, -256, 0}, {-512, -256, 0}};
    ModelFace& f = m.near_mesh.faces.emplace_back();
    f.colour.raw = 12;
    f.prims = {{4, 5, 6, 7}};
    for (auto& order : m.near_mesh.order) order = {0};
    m.far_mesh = m.near_mesh;
    Routine r;
    r.address = 0x2000;
    Variant& v = r.variants.emplace_back();
    Part& p = v.parts.emplace_back();
    p.source = Part::Source::Model;
    p.rotation = Part::Rotation::None;
    p.model = 3;
    v.primitives = 1;  // a model part counts as one primitive and has no prims of its own
    sw.world.routines.push_back(std::move(r));
    sw.place(42, 40, 1, {{0x2000, 1024, 1024, 0}}, {});
    SceneBuilder builder(sw.world);
    Scene scene;
    SceneOptions o;
    o.ground = false;
    builder.build(sw.ram.data(), o, scene);
    CHECK_EQ(scene.stats.models, 1);
    CHECK_EQ(scene.stats.triangles, 2);
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (const SceneVertex& sv : scene.vertices) {
        x0 = std::min(x0, sv.x);
        x1 = std::max(x1, sv.x);
        y0 = std::min(y0, sv.y);
        y1 = std::max(y1, sv.y);
        CHECK(close(sv.r, ega_colour(12).r) && close(sv.g, ega_colour(12).g));
    }
    CHECK(close(x0, 128));
    CHECK(close(x1, 192));
    CHECK(close(y0, 60 - 246 * 256.0 / 4096));
    CHECK(close(y1, 60 + 10 * 256.0 / 4096));
}
