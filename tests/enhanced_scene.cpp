// The Enhanced SceneBuilder (src/enhanced/scene.h): its geometry helpers (scene_geometry.h), the colours,
// and whole builds on a small synthetic world (projection, viewport, painter's order, radius and view
// culls, warm rebuilds). The builder is checked against the original game by vette_world
// (--scene-compare, --scene-shots).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
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

namespace {

// The vertices drawn in a colour: their x and y ranges.
struct Extent {
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    int n = 0;
};
Extent extent(const Scene& s, const SceneColour& c) {
    Extent e;
    for (const SceneVertex& v : s.vertices) {
        if (!close(v.r, c.r) || !close(v.g, c.g) || !close(v.b, c.b)) continue;
        e.x0 = std::min(e.x0, v.x);
        e.x1 = std::max(e.x1, v.x);
        e.y0 = std::min(e.y0, v.y);
        e.y1 = std::max(e.y1, v.y);
        ++e.n;
    }
    return e;
}

void mirror_viewports(SyntheticWorld& sw) {
    // DS:35A3 / 3587 / 3595: x_left, y_top, x_right, y_bottom, centre_y, centre_x, height.
    const uint16_t ahead[7] = {192, 0, 319, 35, 18, 256, 36}, right[7] = {80, 96, 127, 119, 108, 104, 120};
    for (int k = 0; k < 7; ++k) {
        sw.w16(static_cast<uint16_t>(0x35A3 + 2 * k), ahead[k]);
        sw.w16(static_cast<uint16_t>(0x3587 + 2 * k), right[k]);
    }
}

} // namespace

TEST(enhanced_scene_mirror) {
    // The camera faces north; the cyan half-wall (east half) stands 4096 units behind it. Looking back
    // (yaw 180) east is on the left; the mirror flips it to the right of the mirror's centre (256, 18).
    SyntheticWorld sw;
    mirror_viewports(sw);
    sw.place(38, 40, 1, {{0x1003, 1024, 1024, 0}}, {});
    SceneBuilder builder(sw.world);
    Scene scene;
    SceneOptions o;
    o.ground = false;
    o.mirror = true;
    builder.build(sw.ram.data(), o, scene);
    CHECK_EQ(scene.view_x0, 192);
    CHECK_EQ(scene.view_y0, 0);
    CHECK_EQ(scene.view_x1, 320);
    CHECK_EQ(scene.view_y1, 36);
    CHECK_EQ(first_triangle(scene, ega_colour(11)), 0);  // its own sky first
    const Extent e = extent(scene, ega_colour(3));
    CHECK_EQ(e.n, 4);
    CHECK(close(e.x0, 256));
    CHECK(close(e.x1, 288));
    CHECK(close(e.y0, 18 - 246 * 256.0 / 4096));
    CHECK(close(e.y1, 18 + 10 * 256.0 / 4096));

    // Looking right (view offset +85, camera yaw 85): the mirror turns +95 more and uses 3587.
    sw.w16(addr::kCamera + 6, 85);
    sw.w16(addr::kViewOffset, 0x55);
    builder.build(sw.ram.data(), o, scene);
    CHECK_EQ(scene.view_x0, 80);
    CHECK_EQ(scene.view_y0, 96);
    const Extent r = extent(scene, ega_colour(3));
    CHECK_EQ(r.n, 4);
    CHECK(close(r.x0, 104));
    CHECK(close(r.x1, 136));
    CHECK(close(r.y1, 108 + 10 * 256.0 / 4096));
}

namespace {

// Highway mode on the synthetic world: one slice type (a straight 128-unit slice of road, 256 wide
// across, grey) for every type, a route of 40 straight slices, and the ring along +y from (4000h, 4000h).
void freeway(SyntheticWorld& sw) {
    sw.ram[(uint32_t{addr::kDataSeg} << 4) + addr::kHighway] = 0xFF;
    const uint16_t record = 0xA100, polys = 0xA120, poly = 0xA130, block = 0xA160, route = 0xA200;
    for (int t = 0; t < 11; ++t) {
        sw.w16(static_cast<uint16_t>(0x7A72 + 2 * t), record);
        sw.w16(static_cast<uint16_t>(0x7A88 + 2 * t), record);
    }
    sw.w16(record, 8);
    sw.w16(record + 2, polys);
    sw.w16(record + 4, block);
    sw.w16(record + 6, 0xFFFF);
    sw.w16(record + 8, 0);
    sw.w16(polys, poly);
    sw.ram[(uint32_t{addr::kDataSeg} << 4) + polys + 2] = 8;
    sw.w16(polys + 3, 0xFFFF);
    sw.w16(poly, 8);
    for (int k = 0; k <= 8; ++k) sw.w16(static_cast<uint16_t>(poly + 2 + 2 * k), static_cast<uint16_t>(4 * (k % 8)));
    sw.w16(static_cast<uint16_t>(poly + 20), 0xFFFF);
    const int16_t verts[8][3] = {{0, 0, 0}, {64, 0, 0}, {128, 0, 0}, {128, 0, 256}, {64, 0, 256}, {0, 0, 256}, {-16, 0, 256}, {-16, 0, 0}};
    for (int k = 0; k < 8; ++k) {
        for (int j = 0; j < 3; ++j) sw.w16(static_cast<uint16_t>(block + 6 + 6 * k + 2 * j), static_cast<uint16_t>(verts[k][j]));
    }
    sw.w16(route, 40 << 8);  // type 0, 40 slices
    sw.w16(route + 2, 0);
    sw.w16(route + 4, 0xFFFF);
    sw.w16(0x8154, route);
    for (int k = 0; k < 32; ++k) {
        sw.w16(static_cast<uint16_t>(0x8234 + 6 * k), 0x4000);
        sw.w16(static_cast<uint16_t>(0x8236 + 6 * k), static_cast<uint16_t>(0x4000 + 128 * k));
        sw.w16(static_cast<uint16_t>(0x8238 + 6 * k), 0);
    }
    sw.w16(0x804A, 0x4000);
    sw.w16(0x804C, 0x4000);
    // On the road's centre line, 300 units along, facing along it (east).
    sw.w16(addr::kCamera, 0x4080);
    sw.w16(addr::kCamera + 2, 0x4000 + 300);
    sw.w16(addr::kCamera + 6, 90);
}

} // namespace

TEST(enhanced_scene_freeway) {
    SyntheticWorld sw;
    freeway(sw);
    SceneBuilder builder(sw.world);
    Scene scene;
    SceneOptions o;
    o.ground = false;
    builder.build(sw.ram.data(), o, scene);
    // The slices ahead of the camera, past the ring's 32 to the route's end; the road symmetric about
    // the centre, reaching down to the view's bottom and up to the horizon.
    CHECK(scene.stats.slices >= 36 && scene.stats.slices <= 39);
    CHECK_EQ(scene.stats.cells, 0);
    const Extent road = extent(scene, ega_colour(8));
    CHECK(road.n > 0);
    CHECK(road.y0 > 60 && road.y0 < 61);
    CHECK(road.y1 > 119);
    CHECK(close(road.x0 + road.x1, 320, 1e-2));

    // The same view after highway_frame re-centred its frame (the ring still in the old one): the
    // current slice (DS:804A) and the camera moved alike, the ring not.
    const std::vector<SceneVertex> before = scene.vertices;
    sw.w16(0x804A, 0x4000 - 0x800);
    sw.w16(0x804C, 0x4000 + 0x300);
    sw.w16(addr::kCamera, 0x4080 - 0x800);
    sw.w16(addr::kCamera + 2, 0x4000 + 300 + 0x300);
    builder.build(sw.ram.data(), o, scene);
    CHECK_EQ(scene.vertices.size(), before.size());
    bool same = scene.vertices.size() == before.size();
    for (size_t i = 0; same && i < before.size(); ++i) {
        same = close(scene.vertices[i].x, before[i].x, 1e-2) && close(scene.vertices[i].y, before[i].y, 1e-2);
    }
    CHECK(same);

    // In the mirror, the slices behind (up to the route's start) are the ones drawn.
    mirror_viewports(sw);
    o.mirror = true;
    builder.build(sw.ram.data(), o, scene);
    CHECK(scene.stats.slices >= 2 && scene.stats.slices <= 4);
}

namespace {

// The last triangle drawn over a race-frame point, or -1.
int top_triangle(const Scene& s, float px, float py) {
    int top = -1;
    for (size_t t = 0; t + 2 < s.indices.size(); t += 3) {
        const SceneVertex& a = s.vertices[static_cast<size_t>(s.indices[t])];
        const SceneVertex& b = s.vertices[static_cast<size_t>(s.indices[t + 1])];
        const SceneVertex& c = s.vertices[static_cast<size_t>(s.indices[t + 2])];
        const float d1 = (px - b.x) * (a.y - b.y) - (a.x - b.x) * (py - b.y);
        const float d2 = (px - c.x) * (b.y - c.y) - (b.x - c.x) * (py - c.y);
        const float d3 = (px - a.x) * (c.y - a.y) - (c.x - a.x) * (py - a.y);
        const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
        if (!(neg && pos)) top = static_cast<int>(t / 3);
    }
    return top;
}

} // namespace

TEST(enhanced_scene_compound_painter_order) {
    // A bridge's side wall (a compound piece, 64 high) runs north for 8 cells, 300 units left of the camera,
    // from 4 cells behind it; a tall green quad stands in a cell ahead and further left. The wall must
    // cover the green where they overlap, although the wall is anchored in a cell farther away than the
    // green's: the compound is drawn cell by cell, each part with the cell it lies in.
    SyntheticWorld sw;
    const auto packed = [](uint16_t address, std::vector<Vec3i> verts, uint8_t colour) {
        Routine r;
        r.address = address;
        Variant& v = r.variants.emplace_back();
        Part& p = v.parts.emplace_back();
        p.source = Part::Source::Packed;
        p.verts = std::move(verts);
        p.indices = {0, 1, 2, 3};
        Prim prim;
        prim.kind = Prim::Kind::Polygon;
        prim.colour.raw = colour;
        prim.count = 4;
        p.prims.push_back(prim);
        v.primitives = 1;
        return r;
    };
    Routine bridge;
    bridge.address = 0x3000;
    bridge.compound = true;
    bridge.variants.emplace_back().calls.push_back({0x3001, {0, 0, 0}});
    sw.world.routines.push_back(std::move(bridge));
    sw.world.routines.push_back(packed(0x3001, {{0, 0, 0}, {16384, 0, 0}, {16384, 0, 64}, {0, 0, 64}}, 5));
    sw.world.routines.push_back(packed(0x3002, {{-1024, 0, 0}, {1024, 0, 0}, {1024, 0, 1024}, {-1024, 0, 1024}}, 2));
    CompoundInstance ci;
    ci.routine = 0x3000;
    ci.position = {36 * 2048, 40 * 2048 + 1024 - 300, 0};
    sw.world.compounds.push_back(ci);
    sw.place(42, 39, 1, {{0x3002, 1024, 1024 - 1500 + 2048, 0}}, {});
    SceneBuilder builder(sw.world);
    Scene scene;
    SceneOptions o;
    o.ground = false;
    builder.build(sw.ram.data(), o, scene);
    const int t = top_triangle(scene, 66, 50);
    CHECK(t >= 0);
    if (t >= 0) {
        const SceneVertex& v = scene.vertices[static_cast<size_t>(scene.indices[static_cast<size_t>(t) * 3])];
        CHECK(close(v.r, ega_colour(5).r) && close(v.b, ega_colour(5).b));
    }
    // ... and the green shows above the wall.
    const int above = top_triangle(scene, 66, 20);
    CHECK(above >= 0);
    if (above >= 0) {
        const SceneVertex& v = scene.vertices[static_cast<size_t>(scene.indices[static_cast<size_t>(above) * 3])];
        CHECK(close(v.g, ega_colour(2).g) && close(v.r, 0));
    }
}

// The white vertices of a scene with one lane line on the road (a ground-layer line), `side` units right
// of the camera (which is 10 units up), from 200 to 1500 units ahead.
static std::vector<SceneVertex> lane_line_vertices(int16_t side, float pixel) {
    SyntheticWorld sw;
    Routine line;
    line.address = 0x3003;
    Variant& v = line.variants.emplace_back();
    Part& p = v.parts.emplace_back();
    p.source = Part::Source::Packed;
    p.verts = {{200, side, 0}, {1500, side, 0}};
    p.indices = {0, 1};
    Prim prim;
    prim.kind = Prim::Kind::Line;
    prim.colour.raw = 15;
    prim.count = 2;
    p.prims.push_back(prim);
    v.primitives = 1;
    sw.world.routines.push_back(std::move(line));
    sw.place(40, 40, 1, {{0x3003, 1024, 1024, 0}}, {});
    SceneBuilder builder(sw.world);
    Scene scene;
    SceneOptions o;
    o.ground = false;
    o.pixel_w = o.pixel_h = pixel;
    builder.build(sw.ram.data(), o, scene);
    std::vector<SceneVertex> white;
    for (const SceneVertex& sv : scene.vertices) {
        if (close(sv.r, 1) && close(sv.g, 1) && close(sv.b, 1)) white.push_back(sv);
    }
    return white;
}

TEST(enhanced_scene_ground_markings_lie_flat) {
    // A stripe marking_width wide lying on the road, so it narrows with distance as the road does
    // (1.25 * 256 / 200 = 1.6 race pixels near, 0.21 far), not a constant-width line.
    std::vector<SceneVertex> white = lane_line_vertices(10, 12);
    CHECK_EQ(white.size(), size_t{4});
    if (white.size() == 4) {
        std::sort(white.begin(), white.end(), [](const SceneVertex& a, const SceneVertex& b) { return a.y < b.y; });
        const float far = std::fabs(white[0].x - white[1].x), near = std::fabs(white[2].x - white[3].x);
        CHECK(close(near, 1.25 * 256 / 200, 0.05));
        CHECK(close(far, 1.25 * 256 / 1500, 0.05));
    }
    // 100 units to the side, the camera sees the stripe almost edge on: wide across, but on screen
    // thinner than an output pixel square to its own direction (at 320x200, a dash would fall between
    // the pixel rows), so it's a line.
    white = lane_line_vertices(100, 1);
    CHECK_EQ(white.size(), size_t{4});
    if (white.size() == 4) {
        std::sort(white.begin(), white.end(), [](const SceneVertex& a, const SceneVertex& b) { return a.y < b.y; });
        const float far = std::fabs(white[0].y - white[1].y), near = std::fabs(white[2].y - white[3].y);
        CHECK(close(near, far, 0.05));  // a constant width
    }
}

namespace {

// The depth of the vertices drawn in a colour: (lowest, highest).
std::pair<float, float> depth_range(const Scene& s, const SceneColour& c) {
    float lo = 1e9f, hi = -1e9f;
    for (const SceneVertex& v : s.vertices) {
        if (!close(v.r, c.r) || !close(v.g, c.g) || !close(v.b, c.b)) continue;
        lo = std::min(lo, v.depth);
        hi = std::max(hi, v.depth);
    }
    return {lo, hi};
}

} // namespace

TEST(enhanced_scene_depth) {
    // A red wall facing the camera 4096 units ahead with, painted on it, a blue window and a white line along
    // its top edge; then a green wall in front of it (not on it). Depth is 1 / z, raised one step for what
    // lies on an earlier primitive of its group (the window on the wall; the line on the wall or the window),
    // not for the green wall or for a face drawn after a line along its edge. The ground is the backdrop: 0.
    SyntheticWorld sw;
    Routine r;
    r.address = 0x3010;
    Variant& v = r.variants.emplace_back();
    Part& p = v.parts.emplace_back();
    p.source = Part::Source::Packed;
    p.verts = {{0, -512, 0},   {0, 512, 0},   {0, 512, 256},  {0, -512, 256},   // the wall
               {0, -100, 50},  {0, 100, 50},  {0, 100, 150},  {0, -100, 150},   // the window on it
               {-100, -50, 0}, {-100, 50, 0}, {-100, 50, 60}, {-100, -50, 60},  // a wall in front
               {0, 512, 256},  {0, 512, 400}, {0, -512, 400}};                  // a face above the line
    p.indices = {0, 1, 2, 3, 4, 5, 6, 7, 3, 2, 8, 9, 10, 11, 12, 13, 14, 3};
    const auto prim = [](Prim::Kind kind, uint8_t colour, uint32_t first, uint16_t count) {
        Prim q;
        q.kind = kind;
        q.colour.raw = colour;
        q.first = first;
        q.count = count;
        return q;
    };
    p.prims = {prim(Prim::Kind::Polygon, 4, 0, 4), prim(Prim::Kind::Polygon, 1, 4, 4), prim(Prim::Kind::Line, 15, 8, 2),
               prim(Prim::Kind::Polygon, 2, 10, 4), prim(Prim::Kind::Polygon, 5, 14, 4)};
    v.primitives = 5;
    sw.world.routines.push_back(std::move(r));
    sw.place(42, 40, 1, {}, {{0x3010, 1024, 1024, 0}});
    SceneBuilder builder(sw.world);
    Scene scene;
    SceneOptions o;
    builder.build(sw.ram.data(), o, scene);
    const float step = 1 + kDepthStep;
    const auto red = depth_range(scene, ega_colour(4)), blue = depth_range(scene, ega_colour(1));
    const auto white = depth_range(scene, ega_colour(15)), green = depth_range(scene, ega_colour(2));
    const auto magenta = depth_range(scene, ega_colour(5)), ground = depth_range(scene, ega_colour(7));
    CHECK(close(red.first, 1.0 / 4096, 1e-9) && close(red.second, 1.0 / 4096, 1e-9));
    CHECK(close(blue.first, step / 4096, 1e-9) && close(blue.second, step / 4096, 1e-9));
    CHECK(close(white.first, step / 4096, 1e-9) && close(white.second, step / 4096, 1e-9));  // on the wall only
    CHECK(close(green.first, 1.0 / 3996, 1e-9) && close(green.second, 1.0 / 3996, 1e-9));
    CHECK(close(magenta.first, 1.0 / 4096, 1e-9) && close(magenta.second, 1.0 / 4096, 1e-9));
    CHECK(ground.first == 0 && ground.second == 0);
    CHECK_EQ(scene.stats.max_layer, 1);
    CHECK_EQ(scene.stats.layers[0], 3);
    CHECK_EQ(scene.stats.layers[1], 2);

    // The mirror's sky is backdrop too.
    mirror_viewports(sw);
    o.mirror = true;
    builder.build(sw.ram.data(), o, scene);
    const auto sky = depth_range(scene, ega_colour(11));
    CHECK(sky.first == 0 && sky.second == 0);
}

namespace {

// The copies of traffic and pedestrians SceneOptions::replicas considers, by map cell.
struct CopyLog : SceneObserver {
    std::vector<int> kept, dropped, window;
    void copy(const Copy& c) override { (c.window ? window : c.kept ? kept : dropped).push_back(c.cell); }
    void clear() { kept.clear(), dropped.clear(), window.clear(); }
};

} // namespace

TEST(enhanced_scene_replicas_follow_the_roads) {
    // Big tile (2, 2) is a grid of two-way streets (road along the west and north edge of every cell, as the
    // traffic AI drives them), but for a park at cell (37, 33) and a different road at (45, 44). A car on a
    // loop round 2 x 2 cells is copied to every cell of its 4 x 4 pattern whose loop runs on the same roads:
    // not the one whose loop crosses the park, nor the one whose loop takes the other road.
    SyntheticWorld sw;
    const auto quad = [&](uint16_t address, uint8_t colour, int32_t x, int32_t y, int extra_prims) {
        Routine r;
        r.address = address;
        Variant& v = r.variants.emplace_back();
        Part& p = v.parts.emplace_back();
        p.source = Part::Source::Packed;
        p.verts = {{0, 0, 0}, {x, 0, 0}, {x, y, 0}, {0, y, 0}};
        p.indices = {0, 1, 2, 3, 0, 2};
        Prim q;
        q.colour.raw = colour;
        q.count = 4;
        p.prims.push_back(q);
        for (int k = 0; k < extra_prims; ++k) {  // a marking: another road's
            Prim line;
            line.kind = Prim::Kind::Line;
            line.colour.raw = 15;
            line.first = 4;
            line.count = 2;
            p.prims.push_back(line);
        }
        v.primitives = static_cast<uint32_t>(p.prims.size());
        sw.world.routines.push_back(std::move(r));
    };
    quad(0x2000, 8, 2048, 256, 0);  // the west edge's road (and the corner)
    quad(0x2001, 8, 256, 1792, 0);  // the north edge's road
    quad(0x2002, 8, 256, 1792, 1);  // ... another kind of road there
    quad(0x2003, 2, 2048, 2048, 0); // a park
    for (int cx = 32; cx < 48; ++cx) {
        for (int cy = 32; cy < 48; ++cy) sw.place(cx, cy, 1, {{0x2000, 0, 0, 0}, {0x2001, 1792, 256, 0}}, {});
    }
    sw.place(37, 33, 2, {{0x2003, 0, 0, 0}}, {});
    sw.place(45, 44, 3, {{0x2000, 0, 0, 0}, {0x2002, 1792, 256, 0}}, {});
    // Its list (big tile 12 only): the player, opponent and chase car slots (a stub), then the car, in cell
    // (0, 0) of its tile heading north on its cell's north road for the corner of the next cell east.
    const auto list = uint16_t{0x9100}, stub = uint16_t{0xA100}, car = uint16_t{0xA000}, path = uint16_t{0xA040};
    const auto route = uint16_t{0xA200}, code = uint16_t{0x7000};
    sw.ram[(uint32_t{addr::kCodeSeg} << 4) + code] = 0xC3;  // their draw routine: ret
    for (int slot = 0; slot < 3; ++slot) {
        sw.w16(static_cast<uint16_t>(list + 4 * slot), stub);
        sw.w16(static_cast<uint16_t>(list + 4 * slot + 2), 0x00);
    }
    sw.w16(static_cast<uint16_t>(list + 12), car);
    sw.w16(static_cast<uint16_t>(list + 16), 0xFFFF);
    sw.w16(0xEF5A + 2 * 12, list);
    sw.w16(stub, code);
    sw.w16(car, code);
    sw.w16(car + 0x18, route);
    sw.w16(car + 0x1A, path);
    sw.w16(car + 0x1C, 0xFFFF);
    const int16_t moves[] = {0, 2048, 2048, 0, 0, -2048, -2048, 0};
    for (int k = 0; k < 8; ++k) sw.w16(static_cast<uint16_t>(route + 2 * k), static_cast<uint16_t>(moves[k]));
    sw.w16(route + 16, 0xFFFF);
    sw.w16(route + 18, route);
    const auto at = [&](uint16_t cell, int16_t x, int16_t y, int16_t tx, int16_t ty, int16_t nx, int16_t ny, int move) {
        sw.w16(static_cast<uint16_t>(list + 14), cell);
        sw.w16(car + 2, static_cast<uint16_t>(x));
        sw.w16(car + 4, static_cast<uint16_t>(y));
        sw.w16(car + 0x18, static_cast<uint16_t>(route + 4 * move));
        sw.w16(path, static_cast<uint16_t>(tx));
        sw.w16(path + 2, static_cast<uint16_t>(ty));
        sw.w16(path + 8, static_cast<uint16_t>(nx));
        sw.w16(path + 10, static_cast<uint16_t>(ny));
    };
    at(0x00, 1824, 1000, 1824, 2048, 1792, 2080, 0);
    SceneBuilder builder(sw.world);
    Scene scene;
    CopyLog log;
    SceneOptions o;
    o.replicas = true;
    o.observer = &log;
    builder.build(sw.ram.data(), o, scene);
    const auto cell = [](int cx, int cy) { return cx * kMapCells + cy; };
    // The window draws it in its own cell (40, 40); of the other 15 cells of its pattern, 13 keep a copy.
    CHECK(log.window == std::vector<int>{cell(40, 40)});
    std::sort(log.dropped.begin(), log.dropped.end());
    CHECK(log.dropped == (std::vector<int>{cell(36, 32), cell(44, 44)}));
    CHECK_EQ(log.kept.size(), size_t{13});
    CHECK_EQ(scene.stats.replicas, 15);
    CHECK_EQ(scene.stats.replicas_dropped, 2);

    // A cell on along its loop (east, in cell (0, 1), for the corner of the cell north of it): the same
    // copies, each a cell on.
    const std::vector<int> before = log.kept;
    at(0x01, 1900, 100, 1900, 200, 2100, 160, 1);
    log.clear();
    builder.build(sw.ram.data(), o, scene);
    CHECK(log.window == std::vector<int>{cell(40, 41)});
    std::sort(log.dropped.begin(), log.dropped.end());
    CHECK(log.dropped == (std::vector<int>{cell(36, 33), cell(44, 45)}));
    std::vector<int> moved;
    for (const int c : before) moved.push_back(c + 1);
    std::sort(moved.begin(), moved.end());
    std::sort(log.kept.begin(), log.kept.end());
    CHECK(log.kept == moved);

    // Without replicas: no copies, nothing considered.
    o.replicas = false;
    log.clear();
    builder.build(sw.ram.data(), o, scene);
    CHECK(log.kept.empty() && log.dropped.empty());
    CHECK_EQ(scene.stats.replicas, 0);
}
