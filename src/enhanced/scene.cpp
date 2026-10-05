#include "enhanced/scene.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>

#include "enhanced/scene_geometry.h"
#include "enhanced/world_probe.h"

namespace vette::enhanced {

using geometry::P3;

namespace {

constexpr uint32_t kPalette[16] = {0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
                                   0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};
constexpr float kFocal = 256;  // project_vertices: x * 256 / z (3009:A6A9)
constexpr float kNear = 1;     // the original's near plane, z >= 1
// Race-frame pixel x covers [x, x + 1). The original's pixel is centre + trunc(X * 256 / Z), so for points
// right of and below the centre the continuous projection already falls inside it.
constexpr float kPixelOffset = 0.0f;
constexpr double kDeg = 3.14159265358979323846 / 180.0;
constexpr int kCells = kMapCells * kMapCells;

// Data in DS (notes 03, 04, 05).
constexpr uint16_t kViewLeft = 0x315E, kViewTop = 0x315A, kViewRight = 0x3160, kViewBottom = 0x315C;
constexpr uint16_t kCentreX = 0x3169, kCentreY = 0x316B;
constexpr uint16_t kCamRow = 0x2C93, kCamCol = 0x2C95;
constexpr uint16_t kListA = 0xEF5A, kListB = 0xEF8C;
constexpr uint16_t kPlayerRow = 0x2D57, kPlayerCol = 0x2D59;
constexpr uint16_t kOpponentTile = 0x2B70, kChaseTile = 0x2B6E, kOpponentHighway = 0x842B, kChase = 0xF7C2;
constexpr uint16_t kCellTypeVar = 0x3142, kCellRecord = 0x324A, kCellZ = 0x2CBB;
constexpr uint16_t kBigRows = 0x856F, kBigCols = 0x8571;

struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

struct M3 {
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    V3 operator()(V3 v) const {
        return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
                m[6] * v.x + m[7] * v.y + m[8] * v.z};
    }
};
M3 operator*(const M3& a, const M3& b) {
    M3 r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r.m[i * 3 + j] = a.m[i * 3] * b.m[j] + a.m[i * 3 + 1] * b.m[3 + j] + a.m[i * 3 + 2] * b.m[6 + j];
        }
    }
    return r;
}

// The rotation rotate_local() applies (world-axis local vectors), as a matrix.
M3 local_rotation(double yaw, double pitch, double roll) {
    M3 r;
    const Vec3d cols[3] = {rotate_local({1, 0, 0}, yaw, pitch, roll), rotate_local({0, 1, 0}, yaw, pitch, roll),
                           rotate_local({0, 0, 1}, yaw, pitch, roll)};
    for (int c = 0; c < 3; ++c) {
        r.m[c] = static_cast<float>(cols[c].x);
        r.m[3 + c] = static_cast<float>(cols[c].y);
        r.m[6 + c] = static_cast<float>(cols[c].z);
    }
    return r;
}

// model_to_world() as a matrix (model axes x east, y down, z north).
M3 model_rotation(double yaw, double pitch, double roll) {
    M3 r;
    const Vec3d cols[3] = {model_to_world({1, 0, 0}, yaw, pitch, roll), model_to_world({0, 1, 0}, yaw, pitch, roll),
                           model_to_world({0, 0, 1}, yaw, pitch, roll)};
    for (int c = 0; c < 3; ++c) {
        r.m[c] = static_cast<float>(cols[c].x);
        r.m[3 + c] = static_cast<float>(cols[c].y);
        r.m[6 + c] = static_cast<float>(cols[c].z);
    }
    return r;
}

struct Ram {
    const uint8_t* p = nullptr;
    uint8_t u8(uint16_t seg, uint16_t off) const { return p[((uint32_t{seg} << 4) + off) & 0xFFFFF]; }
    uint16_t u16(uint16_t seg, uint16_t off) const {
        return static_cast<uint16_t>(u8(seg, off) | u8(seg, static_cast<uint16_t>(off + 1)) << 8);
    }
    uint8_t d8(uint16_t off) const { return u8(addr::kDataSeg, off); }
    uint16_t d16(uint16_t off) const { return u16(addr::kDataSeg, off); }
    int16_t s16(uint16_t off) const { return static_cast<int16_t>(d16(off)); }
};

// The original's cell window (draw_world_cells 3009:30C6): world offset added to the camera cell's
// origin (16-bit) and cell offset, per helper 35F0..371B.
struct WinCell {
    uint16_t cx, dx;
    int16_t bx, ax;
};
constexpr WinCell k35F0{0xF800, 0xF800, -1, -1}, k35FF{0xF000, 0xF800, -2, -1}, k360E{0xF800, 0xF000, -1, -2},
    k361D{0xF800, 0, -1, 0}, k362C{0xF000, 0, -2, 0}, k363B{0xF800, 0x800, -1, 1}, k364A{0xF000, 0x800, -2, 1},
    k3659{0xF800, 0x1000, -1, 2}, k3668{0, 0xF800, 0, -1}, k3677{0, 0xF000, 0, -2}, k369C{0, 0x800, 0, 1},
    k36A9{0, 0x1000, 0, 2}, k36B6{0x800, 0xF800, 1, -1}, k36C5{0x800, 0xF000, 1, -2}, k36D4{0x0FD0, 0xF800, 2, -1},
    k36E3{0x800, 0, 1, 0}, k36F0{0x0FD0, 0, 2, 0}, k36FD{0x800, 0x800, 1, 1}, k370C{0x800, 0x1000, 1, 2},
    k371B{0x1000, 0x800, 2, 1}, kOwnCell{0, 0, 0, 0};

} // namespace

SceneColour ega_colour(int index) {
    const uint32_t c = kPalette[index & 15];
    return {static_cast<float>((c >> 16) & 255) / 255.0f, static_cast<float>((c >> 8) & 255) / 255.0f,
            static_cast<float>(c & 255) / 255.0f};
}

SceneColour scene_colour(Colour c) {
    const SceneColour a = ega_colour(c.base());
    if (!c.dithered()) {
        return a;
    }
    const SceneColour b = ega_colour(c.second());
    return {(a.r + b.r) * 0.5f, (a.g + b.g) * 0.5f, (a.b + b.b) * 0.5f};
}

struct SceneBuilder::Impl {
    const World& world;

    // --- Built once -------------------------------------------------------------------------------------
    struct PrimData {
        uint32_t first = 0, count = 0;  // triangles (polygon-local indices, 3 each) in tris
        bool line = false;
    };
    struct PartData {
        std::vector<V3> verts;
        std::vector<PrimData> prims;
        M3 rotation;          // Fixed rotation (Plain / Model parts)
        bool rotated = false;
    };
    struct VariantData {
        std::vector<PartData> parts;
    };
    struct RoutineData {
        std::vector<VariantData> variants;
        V3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};  // local bounds over all variants
        V3 centre;
        float radius = -1;  // bounding sphere of the bounds (< 0: unknown, never culled)
    };
    std::vector<RoutineData> routines;  // parallel to world.routines
    std::vector<uint16_t> tris;
    struct MeshData {
        std::vector<V3> verts;
        std::vector<std::vector<PrimData>> faces;  // per face, per primitive
        float radius = 0, ylo = 0, yhi = 0;
    };
    std::array<MeshData, kModelCount> meshes, far_meshes;
    struct Entry {
        int routine = -1;  // index into world.routines
        int16_t dx = 0, dy = 0, dz = 0;
    };
    struct TypeData {
        std::vector<Entry> list1, list2;
        V3 lo{0, 0, 0}, hi{kCellSize, kCellSize, 0};
    };
    std::array<TypeData, 256> types;
    struct Piece {
        int cell = 0, routine = -1;
        int32_t x = 0, y = 0, z = 0;
    };
    // Per course 1..4: compound pieces sorted by cell (CSR by cell), and each cell's bounds.
    struct CourseData {
        std::vector<Piece> pieces;
        std::vector<uint32_t> start;  // kCells + 1
        std::vector<float> box;       // 6 per cell: lo x y z, hi x y z (absolute)
    };
    std::array<CourseData, 5> courses;

    // --- Per frame ------------------------------------------------------------------------------------------
    Ram ram;
    Scene* out = nullptr;
    const SceneOptions* opt = nullptr;
    double cam_x = 0, cam_y = 0, cam_z = 0;  // absolute
    uint16_t cam16_x = 0, cam16_y = 0, cam16_z = 0;
    int yaw = 0, pitch = 0, roll = 0;
    M3 view;  // world delta (x north, y east, z up) -> camera (X right, Y down, Z forward)
    float cx = 160, cy = 60, left = 0, right = 319, top = 0, bottom = 119;
    float vx0 = 0, vx1 = 320, vy0 = 0, vy1 = 120;  // the view rect with a margin for line widths
    DrawState state;
    bool finish = false;
    uint8_t facing = 0;
    std::vector<V3> cam;          // camera-space vertices of the current object
    struct P2 {
        float x, y;
    };
    std::vector<P2> scr;          // their projections (valid where cam z >= the near plane)
    std::vector<uint32_t> part_at;  // where each part's vertices start in `cam`
    void project_from(size_t from) {
        scr.resize(cam.size());
        for (size_t i = from; i < cam.size(); ++i) {
            const V3& c = cam[i];
            if (c.z >= kNear) scr[i] = {px({c.x, c.y, c.z}), py({c.x, c.y, c.z})};
        }
    }
    P3 poly[300], clipped[300];

    struct Vehicle {
        int cell = -1;
        int32_t x = 0, y = 0;
        int16_t z = 0;
        uint16_t routine = 0, entity = 0;
        std::array<int16_t, 3> pos16{};
        int next = -1;
        uint16_t key = 0;  // the sort key the original gives it
        bool window = false;
    };
    std::vector<Vehicle> vehicles;
    std::vector<int> cell_head;  // first vehicle per cell
    struct ModelDraw {
        int model = 0;
        M3 rotation;
        V3 offset;  // relative to the vehicle's position
        bool outline = true;
    };
    std::vector<ModelDraw> vehicle_models;
    std::vector<std::pair<uint32_t, uint32_t>> vehicle_model_range;  // per vehicle
    std::vector<uint16_t> seen_entities;
    std::unique_ptr<Tracer> tracer;
    Trace trace;

    struct Sortable {
        float key;
        int kind;  // 0 static entry, 1 vehicle
        int index;
        Entry entry;
        int32_t x, y, z;
    };
    std::vector<Sortable> sortables;
    std::array<int, 2 * kMapCells + 2> bucket_count{};
    std::vector<int> order, sorted;
    std::vector<int32_t> entity_first;  // per DS offset: the first vehicle of that entity this frame
    bool compound_done = false;         // DS:2AC0 (original window mode without a hook)
    std::vector<std::array<int16_t, 3>> hook_angles;

    explicit Impl(const World& w);
    void prepare_routines();
    void prepare_models();
    void prepare_types();
    void prepare_courses();

    // Frame.
    void setup(const SceneOptions& options);
    V3 to_camera(double x, double y, double z) const {
        return view(V3{static_cast<float>(x - cam_x), static_cast<float>(y - cam_y), static_cast<float>(z - cam_z)});
    }
    bool box_visible(const float* box) const;
    // Frustum planes through the eye (unit normals pointing inside): left, right, top, bottom.
    V3 planes[4];
    bool sphere_visible(V3 c, float r) const {
        if (c.z + r < kNear) return false;
        for (const V3& n : planes) {
            if (dot(n, c) < -r) return false;
        }
        return true;
    }
    void collect_vehicles(bool fallback);
    void add_vehicle(uint16_t entity, uint16_t routine, int cell, int32_t x, int32_t y, int16_t z, bool window);
    void trace_vehicles();
    void draw_vehicle(int index);
    void draw_ground();
    void draw_cell_enhanced(int cell);
    void draw_original_window();
    void draw_entry(const Entry& e, int32_t x, int32_t y, int32_t z, std::array<int16_t, 3> pos16, uint16_t key,
                    bool own, bool sortable);
    void draw_variant(int routine, const Variant& v, int32_t x, int32_t y, int32_t z,
                      const std::vector<std::array<int16_t, 3>>* angles);
    void draw_model(int model, const M3& rotation, V3 origin_cam, bool outline);
    uint16_t lod_key = 0;  // original window mode: cs:259E as the original has it (stale for list1)

    // Emission. With `quantize` (original window mode, for comparisons), the original's integer
    // arithmetic: camera-space values truncated as its Q15 matrix leaves them, and the projection
    // truncated to a pixel, the vertex at that pixel's centre.
    bool quantize = false;
    V3 fin(V3 c) const {
        if (!quantize) return c;
        constexpr float q = 32767.0f / 32768.0f;
        return {std::floor(c.x * q), std::floor(c.y * q), std::floor(c.z * q)};
    }
    float px(const P3& v) const {
        return quantize ? cx + std::trunc(v.x * kFocal / v.z) + 0.5f : v.x * kFocal / v.z + cx + kPixelOffset;
    }
    float py(const P3& v) const {
        return quantize ? cy + std::trunc(v.y * kFocal / v.z) + 0.5f : v.y * kFocal / v.z + cy + kPixelOffset;
    }
    // The near-plane crossing from `behind` towards `front`: float, or (quantize) the original's Q15
    // fraction (3009:A83D): t = (1 - za) * 8000h / (zb - za), x = xa + floor(t * (xb - xa) / 8000h).
    P3 crossing(const P3& behind, const P3& front) const {
        if (!quantize) {
            const float t = (kNear - behind.z) / (front.z - behind.z);
            return {behind.x + t * (front.x - behind.x), behind.y + t * (front.y - behind.y), kNear};
        }
        const auto w16 = [](double v) { return static_cast<int32_t>(static_cast<int16_t>(static_cast<int64_t>(v))); };
        const int32_t den = w16(front.z - behind.z);
        int32_t t = den ? (w16(1 - behind.z) * 32768) / den : 0x7FFF;
        if (t > 0x7FFF || t < -0x8000) t = 0x7FFF;
        const auto step = [&](float a, float b) {
            return static_cast<float>(w16(a) + (static_cast<int64_t>(t) * w16(b - a) >> 15));
        };
        return {step(behind.x, front.x), step(behind.y, front.y), kNear};
    }
    int clip(const P3* in, int n, P3* outp) const {
        int m = 0;
        for (int i = 0; i < n; ++i) {
            const P3& a = in[i];
            const P3& b = in[(i + 1) % n];
            const bool ia = a.z >= kNear, ib = b.z >= kNear;
            if (ia) outp[m++] = a;
            if (ia != ib) outp[m++] = ia ? crossing(b, a) : crossing(a, b);
        }
        return m;
    }
    void vertex(float x, float y, const SceneColour& c, float a) {
        out->vertices.push_back({x, y, c.r, c.g, c.b, a});
    }
    SceneVertex* grow_vertices(size_t n) {
        const size_t at = out->vertices.size();
        out->vertices.resize(at + n);
        return &out->vertices[at];
    }
    int32_t* grow_indices(size_t n) {
        const size_t at = out->indices.size();
        out->indices.resize(at + n);
        return &out->indices[at];
    }
    void emit_polygon(const P3* pts, int n, const uint16_t* tri, uint32_t ntri, const SceneColour& c, float a);
    // A polygon whose points are cv[idx[k]] (projections sv[idx[k]]).
    void emit_indexed(const V3* cv, const P2* sv, const uint16_t* idx, int n, const uint16_t* tri, uint32_t ntri,
                      const SceneColour& c, float a);
    void emit_line(P3 a, P3 b, const SceneColour& c);
    void emit_segment(const V3* cv, const P2* sv, uint16_t ia, uint16_t ib, const SceneColour& c) {
        if (cv[ia].z >= kNear && cv[ib].z >= kNear) {
            emit_quad_2d(sv[ia].x, sv[ia].y, cv[ia].z, sv[ib].x, sv[ib].y, cv[ib].z, c);
        } else {
            emit_line({cv[ia].x, cv[ia].y, cv[ia].z}, {cv[ib].x, cv[ib].y, cv[ib].z}, c);
        }
    }
    void draw_prim(const Part& p, const Prim& prim, const PrimData& pr, const V3* cv, const P2* sv);
    bool visible(const Cull& cull) const;
    // A line's half width in output pixels at depth z: line_world_width in perspective, at least
    // line_width / 2 and at most half a race-frame pixel (the width the original draws every line at).
    float line_half_width(float z) const {
        const float race_px = std::min(opt->pixel_w, opt->pixel_h);
        const float w = opt->line_world_width * kFocal / std::max(z, kNear) * race_px;
        return 0.5f * std::clamp(w, opt->line_width, std::max(opt->line_width, race_px));
    }
    void emit_quad_2d(float x0, float y0, float z0, float x1, float y1, float z1, const SceneColour& c);
};

// --- Preparation ------------------------------------------------------------------------------------------

SceneBuilder::Impl::Impl(const World& w) : world(w) {
    prepare_routines();
    prepare_models();
    prepare_types();
    prepare_courses();
    cam.reserve(4096);
    scr.reserve(4096);
    part_at.reserve(64);
    vehicles.reserve(512);
    vehicle_models.reserve(512);
    vehicle_model_range.reserve(512);
    sortables.reserve(64);
    order.reserve(kCells);
    sorted.reserve(kCells);
    entity_first.assign(0x10000, -1);
    cell_head.assign(kCells, -1);
    hook_angles.reserve(16);
}

void SceneBuilder::Impl::prepare_routines() {
    routines.resize(world.routines.size());
    std::vector<uint16_t> local;
    for (size_t r = 0; r < world.routines.size(); ++r) {
        const Routine& routine = world.routines[r];
        RoutineData& rd = routines[r];
        const auto grow = [&](V3 p) {
            rd.lo = {std::min(rd.lo.x, p.x), std::min(rd.lo.y, p.y), std::min(rd.lo.z, p.z)};
            rd.hi = {std::max(rd.hi.x, p.x), std::max(rd.hi.y, p.y), std::max(rd.hi.z, p.z)};
        };
        for (const Variant& v : routine.variants) {
            VariantData& vd = rd.variants.emplace_back();
            for (const Part& p : v.parts) {
                PartData& pd = vd.parts.emplace_back();
                for (const Vec3i& q : p.verts) {
                    pd.verts.push_back({static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z)});
                }
                const V3 o{static_cast<float>(p.origin.x), static_cast<float>(p.origin.y), static_cast<float>(p.origin.z)};
                if (p.source == Part::Source::Packed) {
                    for (const V3& q : pd.verts) grow(q);
                } else if (p.source == Part::Source::Plain) {
                    float rad = 0;
                    for (const V3& q : pd.verts) rad = std::max(rad, std::sqrt(dot(q, q)));
                    grow({o.x - rad, o.y - rad, o.z - rad});
                    grow({o.x + rad, o.y + rad, o.z + rad});
                    if (p.rotation == Part::Rotation::Fixed) {
                        pd.rotation = local_rotation(p.yaw, p.pitch, p.roll);
                        pd.rotated = true;
                    }
                } else {
                    const Model& m = world.models[p.model];
                    float rad = 0, ylo = 0, yhi = 0;
                    for (const Vec3i& q : m.near_mesh.verts) {
                        rad = std::max(rad, std::sqrt(float(q.x) * q.x + float(q.z) * q.z));
                        ylo = std::min(ylo, float(-q.y));
                        yhi = std::max(yhi, float(-q.y));
                    }
                    grow({o.x - rad, o.y - rad, o.z + ylo});
                    grow({o.x + rad, o.y + rad, o.z + yhi});
                    // Unrotated models (3009:B9E6) still go from model axes to world axes.
                    pd.rotation = p.rotation == Part::Rotation::None ? model_rotation(0, 0, 0)
                                                                     : model_rotation(p.yaw, p.pitch, p.roll);
                    pd.rotated = true;
                }
                for (const Prim& prim : p.prims) {
                    PrimData pr;
                    pr.first = static_cast<uint32_t>(tris.size() / 3);
                    if (prim.kind == Prim::Kind::Line) {
                        pr.line = true;
                    } else {
                        P3 pts[256];
                        const int n = std::min<int>(prim.count, 256);
                        for (int k = 0; k < n; ++k) {
                            const V3 q = pd.verts[p.indices[prim.first + static_cast<uint32_t>(k)]];
                            pts[k] = {q.x, q.y, q.z};
                        }
                        local.clear();
                        if (prim.kind == Prim::Kind::PolygonAlt) {
                            geometry::triangulate(pts, n, local);
                        } else {
                            for (int k = 1; k + 1 < n; ++k) {
                                local.insert(local.end(), {0, static_cast<uint16_t>(k), static_cast<uint16_t>(k + 1)});
                            }
                        }
                        tris.insert(tris.end(), local.begin(), local.end());
                    }
                    pr.count = static_cast<uint32_t>(tris.size() / 3) - pr.first;
                    pd.prims.push_back(pr);
                }
            }
        }
    }
}

void SceneBuilder::Impl::prepare_models() {
    std::vector<uint16_t> local;
    for (size_t id = 0; id < kModelCount * 2; ++id) {
        const Model& m = world.models[id % kModelCount];
        if (!m.present) continue;
        const bool far = id >= kModelCount;
        const ModelMesh& mesh = far ? m.far_mesh : m.near_mesh;
        MeshData& md = (far ? far_meshes : meshes)[id % kModelCount];
        for (const Vec3i& q : mesh.verts) {
            md.verts.push_back({static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z)});
            md.radius = std::max(md.radius, std::sqrt(float(q.x) * q.x + float(q.y) * q.y + float(q.z) * q.z));
        }
        for (const ModelFace& f : mesh.faces) {
            auto& faces = md.faces.emplace_back();
            for (const auto& prim : f.prims) {
                PrimData pr;
                pr.first = static_cast<uint32_t>(tris.size() / 3);
                if (f.lines()) {
                    pr.line = true;
                } else {
                    P3 pts[256];
                    const int n = std::min<int>(static_cast<int>(prim.size()), 256);
                    for (int k = 0; k < n; ++k) {
                        const V3& q = md.verts[prim[static_cast<size_t>(k)]];
                        pts[k] = {q.x, q.y, q.z};
                    }
                    local.clear();
                    if (f.kind() == 2) {
                        geometry::triangulate(pts, n, local);
                    } else {
                        for (int k = 1; k + 1 < n; ++k) {
                            local.insert(local.end(), {0, static_cast<uint16_t>(k), static_cast<uint16_t>(k + 1)});
                        }
                    }
                    tris.insert(tris.end(), local.begin(), local.end());
                }
                pr.count = static_cast<uint32_t>(tris.size() / 3) - pr.first;
                faces.push_back(pr);
            }
        }
    }
}

void SceneBuilder::Impl::prepare_types() {
    for (RoutineData& rd : routines) {
        if (rd.lo.x > rd.hi.x) continue;
        rd.centre = {(rd.lo.x + rd.hi.x) * 0.5f, (rd.lo.y + rd.hi.y) * 0.5f, (rd.lo.z + rd.hi.z) * 0.5f};
        const V3 h = rd.hi - rd.centre;
        rd.radius = std::sqrt(dot(h, h)) + 8;
    }
    const auto index_of = [this](uint16_t address) {
        const auto it = std::lower_bound(world.routines.begin(), world.routines.end(), address,
                                         [](const Routine& r, uint16_t a) { return r.address < a; });
        return it != world.routines.end() && it->address == address ? static_cast<int>(it - world.routines.begin()) : -1;
    };
    for (size_t t = 0; t < 256; ++t) {
        const CellType& ct = world.types[t];
        TypeData& td = types[t];
        if (!ct.used) continue;
        td.lo = {0, 0, -64};
        td.hi = {kCellSize, kCellSize, 64};
        for (const auto* list : {&ct.list1, &ct.list2}) {
            for (const ListEntry& e : *list) {
                Entry en{index_of(e.routine), e.dx, e.dy, e.dz};
                (list == &ct.list1 ? td.list1 : td.list2).push_back(en);
                if (en.routine < 0 || world.routines[static_cast<size_t>(en.routine)].compound) continue;
                const RoutineData& rd = routines[static_cast<size_t>(en.routine)];
                if (rd.lo.x > rd.hi.x) continue;
                td.lo = {std::min(td.lo.x, rd.lo.x + e.dx), std::min(td.lo.y, rd.lo.y + e.dy), std::min(td.lo.z, rd.lo.z + e.dz)};
                td.hi = {std::max(td.hi.x, rd.hi.x + e.dx), std::max(td.hi.y, rd.hi.y + e.dy), std::max(td.hi.z, rd.hi.z + e.dz)};
            }
        }
    }
}

void SceneBuilder::Impl::prepare_courses() {
    for (int course = 1; course <= 4; ++course) {
        CourseData& cd = courses[static_cast<size_t>(course)];
        DrawState st;
        st.course = course;
        for (const CompoundInstance& ci : world.compounds) {
            const Routine* r = world.routine(ci.routine);
            const Variant* v = r ? r->select(st) : nullptr;
            if (!v) continue;
            for (const SubCall& c : v->calls) {
                const Routine* pr = world.routine(c.routine);
                if (!pr) continue;
                Piece p;
                p.routine = static_cast<int>(pr - world.routines.data());
                p.x = ci.position.x + c.offset.x;
                p.y = ci.position.y + c.offset.y;
                p.z = ci.position.z + c.offset.z;
                const int pcx = std::clamp(p.x / kCellSize, 0, world.cells_x() - 1);
                const int pcy = std::clamp(p.y / kCellSize, 0, world.cells_y() - 1);
                p.cell = pcx * kMapCells + pcy;
                cd.pieces.push_back(p);
            }
        }
        std::stable_sort(cd.pieces.begin(), cd.pieces.end(), [](const Piece& a, const Piece& b) { return a.cell < b.cell; });
        cd.start.assign(kCells + 1, 0);
        for (const Piece& p : cd.pieces) ++cd.start[static_cast<size_t>(p.cell) + 1];
        for (int i = 0; i < kCells; ++i) cd.start[static_cast<size_t>(i) + 1] += cd.start[static_cast<size_t>(i)];
        // Cell bounds: the type's contents plus compound pieces, with a margin for vehicles.
        cd.box.assign(static_cast<size_t>(kCells) * 6, 0);
        for (int cxi = 0; cxi < world.cells_x() && cxi < kMapCells; ++cxi) {
            for (int cyi = 0; cyi < world.cells_y() && cyi < kMapCells; ++cyi) {
                const Cell& cell = world.cell(cxi, cyi);
                const TypeData& td = types[cell.type];
                const float ox = float(cxi * kCellSize), oy = float(cyi * kCellSize), oz = float(cell.elevation * kElevationStep);
                float* b = &cd.box[static_cast<size_t>(cxi * kMapCells + cyi) * 6];
                b[0] = std::min(ox + td.lo.x, ox) - 256;
                b[1] = std::min(oy + td.lo.y, oy) - 256;
                b[2] = std::min(oz + td.lo.z, 0.0f) - 64;
                b[3] = std::max(ox + td.hi.x, ox + kCellSize) + 256;
                b[4] = std::max(oy + td.hi.y, oy + kCellSize) + 256;
                b[5] = std::max(oz + td.hi.z, oz) + 256;
            }
        }
        for (const Piece& p : cd.pieces) {
            const RoutineData& rd = routines[static_cast<size_t>(p.routine)];
            if (rd.lo.x > rd.hi.x) continue;
            float* b = &cd.box[static_cast<size_t>(p.cell) * 6];
            b[0] = std::min(b[0], p.x + rd.lo.x);
            b[1] = std::min(b[1], p.y + rd.lo.y);
            b[2] = std::min(b[2], p.z + rd.lo.z);
            b[3] = std::max(b[3], p.x + rd.hi.x);
            b[4] = std::max(b[4], p.y + rd.hi.y);
            b[5] = std::max(b[5], p.z + rd.hi.z);
        }
    }
}

// --- Frame setup --------------------------------------------------------------------------------------------

void SceneBuilder::Impl::setup(const SceneOptions& options) {
    opt = &options;
    cam16_x = ram.d16(addr::kCamera);
    cam16_y = ram.d16(addr::kCamera + 2);
    cam16_z = ram.d16(addr::kCamera + 4);
    yaw = ram.s16(addr::kCamera + 6);
    pitch = ram.s16(addr::kCamera + 8);
    roll = ram.s16(addr::kCamera + 10);
    cam_x = double(ram.s16(kCamRow)) * 0x8000 + cam16_x;
    cam_y = double(ram.s16(kCamCol)) * 0x8000 + cam16_y;
    cam_z = static_cast<int16_t>(cam16_z);
    // The camera matrix (3009:3F2D) from the negated angles, applied to (dy, -dz, dx).
    const double sa = std::sin(-yaw * kDeg), ca = std::cos(-yaw * kDeg);
    const double sb = std::sin(-pitch * kDeg), cb = std::cos(-pitch * kDeg);
    const double sc = std::sin(-roll * kDeg), cc = std::cos(-roll * kDeg);
    const double m[9] = {sa * sb * sc + ca * cc, sa * sb * cc + ca * sc, -sa * cb,  //
                         -cb * sc,               cb * cc,               sb,        //
                         ca * sb * sc + sa * cc, -ca * sb * cc + sa * sc, ca * cb};
    for (int j = 0; j < 3; ++j) {
        view.m[j * 3 + 0] = static_cast<float>(m[6 + j]);   // dx
        view.m[j * 3 + 1] = static_cast<float>(m[0 + j]);   // dy
        view.m[j * 3 + 2] = static_cast<float>(-m[3 + j]);  // dz
    }
    left = ram.s16(kViewLeft);
    top = ram.s16(kViewTop);
    right = ram.s16(kViewRight);
    bottom = ram.s16(kViewBottom);
    cx = ram.s16(kCentreX);
    cy = ram.s16(kCentreY);
    const float mx = options.line_width / std::max(options.pixel_w, 1e-3f) + 1;
    const float my = options.line_width / std::max(options.pixel_h, 1e-3f) + 1;
    vx0 = left - mx;
    vx1 = right + 1 + mx;
    vy0 = top - my;
    vy1 = bottom + 1 + my;
    {
        const auto plane = [](float a, float b, float c) {
            const float l = std::sqrt(a * a + b * b + c * c);
            return V3{a / l, b / l, c / l};
        };
        const float l = left - cx - 2, r = right + 1 - cx + 2, t = top - cy - 2, b = bottom + 1 - cy + 2;
        planes[0] = plane(kFocal, 0, -l);   // X f - l Z >= 0
        planes[1] = plane(-kFocal, 0, r);   // r Z - X f >= 0
        planes[2] = plane(0, kFocal, -t);
        planes[3] = plane(0, -kFocal, b);
    }
    out->view_x0 = static_cast<int>(left);
    out->view_y0 = static_cast<int>(top);
    out->view_x1 = static_cast<int>(right) + 1;
    out->view_y1 = static_cast<int>(bottom) + 1;
    state.course = std::clamp<int>(ram.d16(addr::kCourse), 1, 4);
    state.windows = ram.d8(addr::kWindowsOff) == 0;
    finish = ram.d8(addr::kFinishFlag) != 0;
    // The facing axis of the heading's sector (30C6).
    facing = (yaw >= 0x2D && yaw < 0x87) || (yaw >= 0xE1 && yaw < 0x13B) ? 1 : 0;
}

// Whether a box (absolute lo xyz, hi xyz) can be in view: not entirely beyond one frustum plane.
bool SceneBuilder::Impl::box_visible(const float* b) const {
    constexpr float kMargin = 8;
    const float l = left - cx - kMargin, r = right - cx + kMargin, t = top - cy - kMargin, d = bottom - cy + kMargin;
    int all = 0x1F;
    for (int i = 0; i < 8; ++i) {
        const V3 p = to_camera(b[(i & 1) ? 3 : 0], b[(i & 2) ? 4 : 1], b[(i & 4) ? 5 : 2]);
        int code = 0;
        if (p.z < kNear) code |= 1;
        if (p.x * kFocal < l * p.z) code |= 2;
        if (p.x * kFocal > r * p.z) code |= 4;
        if (p.y * kFocal < t * p.z) code |= 8;
        if (p.y * kFocal > d * p.z) code |= 16;
        all &= code;
        if (!all) return true;
    }
    return all == 0;
}

// --- Emission -----------------------------------------------------------------------------------------------

void SceneBuilder::Impl::emit_polygon(const P3* pts, int n, const uint16_t* tri, uint32_t ntri, const SceneColour& c,
                                      float a) {
    bool all_front = true;
    for (int k = 0; k < n; ++k) all_front &= pts[k].z >= kNear;
    if (all_front) {
        float sx[256], sy[256];
        int outside = 0xF;
        for (int k = 0; k < n; ++k) {
            sx[k] = px(pts[k]);
            sy[k] = py(pts[k]);
            outside &= (sx[k] < vx0 ? 1 : 0) | (sx[k] > vx1 ? 2 : 0) | (sy[k] < vy0 ? 4 : 0) | (sy[k] > vy1 ? 8 : 0);
        }
        if (outside) return;  // entirely beyond one side of the view
        SceneVertex* v = grow_vertices(static_cast<size_t>(n));
        const auto base = static_cast<int32_t>(out->vertices.size()) - n;
        for (int k = 0; k < n; ++k) v[k] = {sx[k], sy[k], c.r, c.g, c.b, a};
        int32_t* ix = grow_indices(ntri * 3);
        for (uint32_t t = 0; t < ntri * 3; ++t) ix[t] = base + tri[t];
        out->stats.triangles += static_cast<int>(ntri);
        return;
    }
    for (uint32_t t = 0; t < ntri; ++t) {
        const P3 corner[3] = {pts[tri[3 * t]], pts[tri[3 * t + 1]], pts[tri[3 * t + 2]]};
        P3 cl[4];
        const int m = clip(corner, 3, cl);
        if (m < 3) continue;
        const auto base = static_cast<int32_t>(out->vertices.size());
        for (int k = 0; k < m; ++k) vertex(px(cl[k]), py(cl[k]), c, a);
        for (int k = 1; k + 1 < m; ++k) {
            out->indices.push_back(base);
            out->indices.push_back(base + k);
            out->indices.push_back(base + k + 1);
            ++out->stats.triangles;
        }
    }
}

void SceneBuilder::Impl::emit_indexed(const V3* cv, const P2* sv, const uint16_t* idx, int n, const uint16_t* tri,
                                      uint32_t ntri, const SceneColour& c, float a) {
    bool all_front = true;
    int outside = 0xF;
    for (int k = 0; k < n; ++k) {
        const uint16_t i = idx[k];
        all_front &= cv[i].z >= kNear;
        if (!all_front) break;
        const P2 q = sv[i];
        outside &= (q.x < vx0 ? 1 : 0) | (q.x > vx1 ? 2 : 0) | (q.y < vy0 ? 4 : 0) | (q.y > vy1 ? 8 : 0);
    }
    if (all_front) {
        if (outside) return;
        SceneVertex* v = grow_vertices(static_cast<size_t>(n));
        const auto base = static_cast<int32_t>(out->vertices.size()) - n;
        for (int k = 0; k < n; ++k) {
            const P2 q = sv[idx[k]];
            v[k] = {q.x, q.y, c.r, c.g, c.b, a};
        }
        int32_t* ix = grow_indices(ntri * 3);
        for (uint32_t t = 0; t < ntri * 3; ++t) ix[t] = base + tri[t];
        out->stats.triangles += static_cast<int>(ntri);
        return;
    }
    for (int k = 0; k < n; ++k) {
        const V3 q = cv[idx[k]];
        poly[k] = {q.x, q.y, q.z};
    }
    emit_polygon(poly, n, tri, ntri, c, a);
}

void SceneBuilder::Impl::emit_quad_2d(float x0, float y0, float z0, float x1, float y1, float z1,
                                     const SceneColour& c) {
    // A segment as a quad line_half_width() wide at each end (z0, z1: the ends' depths), with square
    // caps half that long.
    if ((x0 < vx0 && x1 < vx0) || (x0 > vx1 && x1 > vx1) || (y0 < vy0 && y1 < vy0) || (y0 > vy1 && y1 > vy1)) return;
    const float pw = opt->pixel_w, ph = opt->pixel_h;
    float dx = (x1 - x0) * pw, dy = (y1 - y0) * ph;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < opt->min_line_length) {
        ++out->stats.lines_dropped;
        return;
    }
    if (len < 1e-4f) {
        dx = 1;
        dy = 0;
        len = 1;
    }
    const float ex = dx / len, ey = dy / len;  // unit vector along, output pixels
    const auto corners = [&](float h, float& ax, float& ay, float& bx, float& by) {
        const float ux = ex * h, uy = ey * h, nx = -uy, ny = ux;  // along, across
        ax = (-ux + nx) / pw;
        ay = (-uy + ny) / ph;
        bx = (-ux - nx) / pw;
        by = (-uy - ny) / ph;
    };
    float ax0, ay0, bx0, by0, ax1, ay1, bx1, by1;
    corners(line_half_width(z0), ax0, ay0, bx0, by0);
    corners(line_half_width(z1), ax1, ay1, bx1, by1);
    SceneVertex* v = grow_vertices(4);
    const auto base = static_cast<int32_t>(out->vertices.size()) - 4;
    v[0] = {x0 + ax0, y0 + ay0, c.r, c.g, c.b, 1};
    v[1] = {x0 + bx0, y0 + by0, c.r, c.g, c.b, 1};
    v[2] = {x1 - ax1, y1 - ay1, c.r, c.g, c.b, 1};
    v[3] = {x1 - bx1, y1 - by1, c.r, c.g, c.b, 1};
    int32_t* ix = grow_indices(6);
    ix[0] = base;
    ix[1] = base + 1;
    ix[2] = base + 2;
    ix[3] = base;
    ix[4] = base + 2;
    ix[5] = base + 3;
    out->stats.triangles += 2;
    ++out->stats.lines;
}

void SceneBuilder::Impl::emit_line(P3 a, P3 b, const SceneColour& c) {
    if (a.z < kNear && b.z < kNear) return;
    if (a.z < kNear) {
        a = crossing(a, b);
    } else if (b.z < kNear) {
        b = crossing(b, a);
    }
    emit_quad_2d(px(a), py(a), a.z, px(b), py(b), b.z, c);
}

// --- Objects ------------------------------------------------------------------------------------------------

void SceneBuilder::Impl::draw_model(int model, const M3& rotation, V3 origin_cam, bool outline) {
    if (model < 0 || model >= kModelCount || !world.models[static_cast<size_t>(model)].present) return;
    const Model& m = world.models[static_cast<size_t>(model)];
    // B9F6: beyond sort key 800h the original draws a generic box (original window mode only).
    const bool far = opt->original_window && lod_key >= 0x800;
    const ModelMesh& mesh = far ? m.far_mesh : m.near_mesh;
    const MeshData& md = (far ? far_meshes : meshes)[static_cast<size_t>(model)];
    const M3 a = view * rotation;
    const size_t base = cam.size();
    for (const V3& v : md.verts) cam.push_back(fin(a(v) + origin_cam));
    project_from(base);
    const V3* cv = &cam[base];
    const P2* sv = &scr[base];
    ++out->stats.models;
    // 9CAF: one bit per reference axis, set when (V_i - V0) . V0 < 0.
    int octant = 0;
    for (int i = 1; i <= 3; ++i) {
        if (dot(cv[i] - cv[0], cv[0]) < 0) octant |= 1 << (i - 1);
    }
    for (const uint16_t fi : mesh.order[static_cast<size_t>(octant)]) {
        const ModelFace& f = mesh.faces[fi];
        if (f.flags & 0x4000) continue;
        const SceneColour col = scene_colour(f.colour);
        const float alpha = (f.flags & 0x8000) ? 0.5f : 1.0f;
        for (size_t pi = 0; pi < f.prims.size(); ++pi) {
            const auto& prim = f.prims[pi];
            const PrimData& pd = md.faces[fi][pi];
            if (pd.line) {
                const SceneColour lc = ega_colour(f.colour.base());
                for (size_t k = 0; k + 1 < prim.size(); ++k) emit_segment(cv, sv, prim[k], prim[k + 1], lc);
                continue;
            }
            const int n = static_cast<int>(std::min<size_t>(prim.size(), 256));
            if ((f.flags & 1) && n >= 3) {
                // 9CD8, the screen winding of the first three points: the orientation of the camera
                // relative to their plane.
                if (dot(cv[prim[0]], cross(cv[prim[1]], cv[prim[2]])) <= 0) continue;
            }
            emit_indexed(cv, sv, prim.data(), n, &tris[pd.first * 3], pd.count, col, alpha);
            if (outline && (f.flags & 0x2000)) {
                const SceneColour oc = ega_colour(f.colour_hi);
                for (int k = 0; k < n; ++k) emit_segment(cv, sv, prim[static_cast<size_t>(k)], prim[static_cast<size_t>((k + 1) % n)], oc);
            }
        }
    }
    cam.resize(base);
    scr.resize(base);
}

void SceneBuilder::Impl::draw_variant(int routine, const Variant& v, int32_t x, int32_t y, int32_t z,
                                      const std::vector<std::array<int16_t, 3>>* angles) {
    const RoutineData& rd = routines[static_cast<size_t>(routine)];
    const VariantData& vd = rd.variants[static_cast<size_t>(&v - world.routines[static_cast<size_t>(routine)].variants.data())];
    ++out->stats.objects;
    const V3 entry = to_camera(x, y, z);
    cam.clear();
    part_at.clear();
    size_t plain = 0;
    // Transform every part first: a primitive may cull on another part's vertices (windows).
    for (size_t i = 0; i < v.parts.size(); ++i) {
        const Part& p = v.parts[i];
        const PartData& pd = vd.parts[i];
        part_at.push_back(static_cast<uint32_t>(cam.size()));
        const V3 o{static_cast<float>(p.origin.x), static_cast<float>(p.origin.y), static_cast<float>(p.origin.z)};
        if (p.source == Part::Source::Packed) {
            for (const V3& q : pd.verts) cam.push_back(fin(view(q) + entry));
        } else if (p.source == Part::Source::Plain) {
            M3 rot = pd.rotation;
            if (angles && plain < angles->size()) {
                const auto& an = (*angles)[plain];
                rot = local_rotation(an[0], an[1], an[2]);
            } else if (p.rotation == Part::Rotation::CameraYaw) {
                rot = local_rotation(yaw + p.yaw, p.pitch, p.roll);
            } else if (p.rotation == Part::Rotation::Animated) {
                rot = local_rotation(static_cast<int16_t>(ram.d16(p.anim_angle_addr) + p.anim_step), p.pitch, p.roll);
            } else if (!pd.rotated) {
                rot = M3{};
            }
            ++plain;
            const M3 a = view * rot;
            const V3 t = view(o) + entry;
            for (const V3& q : pd.verts) cam.push_back(fin(a(q) + t));
        }
    }
    project_from(0);
    // A painter-order alternative applies when its deciding face points away (the pyramid).
    const Variant::Reorder* reorder = nullptr;
    for (const Variant::Reorder& r : v.reorders) {
        if (!reorder && !visible(r.unless_visible)) reorder = &r;
    }
    for (size_t i = 0; i < v.parts.size(); ++i) {
        const Part& p = v.parts[i];
        const PartData& pd = vd.parts[i];
        if (p.source == Part::Source::Model) {
            const V3 o{static_cast<float>(p.origin.x), static_cast<float>(p.origin.y), static_cast<float>(p.origin.z)};
            const size_t keep = cam.size();
            draw_model(p.model, pd.rotation, view(o) + entry, true);
            cam.resize(keep);
            scr.resize(keep);
            continue;
        }
        if (reorder) continue;
        for (size_t k = 0; k < p.prims.size(); ++k) {
            draw_prim(p, p.prims[k], pd.prims[k], &cam[part_at[i]], &scr[part_at[i]]);
        }
    }
    if (reorder) {
        for (const auto& [part, k] : reorder->order) {
            const Part& p = v.parts[part];
            draw_prim(p, p.prims[k], vd.parts[part].prims[k], &cam[part_at[part]], &scr[part_at[part]]);
        }
    }
}

bool SceneBuilder::Impl::visible(const Cull& cull) const {
    if (cull.kind == Cull::Kind::None) return true;
    const V3* c = &cam[part_at[cull.part]];
    if (cull.kind == Cull::Kind::Behind) {
        const V3 a = c[cull.v[0]], b = c[cull.v[1]];
        return dot(b - a, a) > 0;
    }
    const V3 p0 = c[cull.v[0]];
    return dot(cross(c[cull.v[1]] - p0, c[cull.v[2]] - p0), p0) > 0;
}

void SceneBuilder::Impl::draw_prim(const Part& p, const Prim& prim, const PrimData& pr, const V3* cv, const P2* sv) {
    if (!visible(prim.cull)) return;
    const uint16_t* idx = &p.indices[prim.first];
    if (pr.line) {
        emit_segment(cv, sv, idx[0], idx[1], ega_colour(prim.colour.base()));
        return;
    }
    if (pr.count == 0 && prim.count == 2) {
        emit_segment(cv, sv, idx[0], idx[1], ega_colour(prim.colour.base()));  // a two-point "polygon"
        return;
    }
    emit_indexed(cv, sv, idx, std::min<int>(prim.count, 256), &tris[pr.first * 3], pr.count, scene_colour(prim.colour), 1);
}

void SceneBuilder::Impl::draw_entry(const Entry& e, int32_t x, int32_t y, int32_t z, std::array<int16_t, 3> pos16,
                                    uint16_t key, bool own, bool sortable) {
    if (e.routine < 0) return;
    if (sortable) lod_key = key;
    const Routine& r = world.routines[static_cast<size_t>(e.routine)];
    const Variant* v = nullptr;
    const std::vector<std::array<int16_t, 3>>* angles = nullptr;
    if (opt->hook) {
        SceneHook::Object o;
        o.routine = r.address;
        o.position = pos16;
        o.sort_key = key;
        o.sortable = sortable;
        o.facing = facing;
        o.own_cell = own;
        hook_angles.clear();
        v = opt->hook->choose(o, hook_angles);
        angles = &hook_angles;
    } else {
        DrawState st = state;
        st.finish_flag = finish;
        v = r.select(st);
    }
    if (!v) return;
    if (v->sets_finish_flag >= 0) finish = v->sets_finish_flag != 0;
    if (!opt->hook && v->calls.empty()) {
        const RoutineData& rd = routines[static_cast<size_t>(e.routine)];
        if (rd.radius >= 0 && !sphere_visible(to_camera(x + rd.centre.x, y + rd.centre.y, z + rd.centre.z), rd.radius)) {
            return;
        }
    }
    if (!v->calls.empty()) {
        // A compound drawn whole (original window mode), once per frame (DS:2AC0, one flag for all).
        if (!opt->hook) {
            if (compound_done) return;
            compound_done = true;
        }
        for (const SubCall& c : v->calls) {
            const Routine* pr = world.routine(c.routine);
            if (!pr) continue;
            Entry pe{static_cast<int>(pr - world.routines.data()), 0, 0, 0};
            draw_entry(pe, x + c.offset.x, y + c.offset.y, z + c.offset.z,
                       {static_cast<int16_t>(pos16[0] + c.offset.x), static_cast<int16_t>(pos16[1] + c.offset.y),
                        static_cast<int16_t>(pos16[2] + c.offset.z)},
                       key, own, false);
        }
        return;
    }
    draw_variant(e.routine, *v, x, y, z, angles);
}

// --- Vehicles -----------------------------------------------------------------------------------------------

void SceneBuilder::Impl::add_vehicle(uint16_t entity, uint16_t routine, int cell, int32_t x, int32_t y, int16_t z,
                                     bool window) {
    if (cell < 0 || cell >= kCells) return;
    Vehicle v;
    v.entity = entity;
    v.routine = routine;
    v.cell = cell;
    v.x = x;
    v.y = y;
    v.z = z;
    v.window = window;
    // The original's 16-bit frame (camera big tile), for the draw routine.
    v.pos16 = {static_cast<int16_t>(cam16_x + (x - static_cast<int32_t>(std::lround(cam_x)))),
               static_cast<int16_t>(cam16_y + (y - static_cast<int32_t>(std::lround(cam_y)))), z};
    vehicles.push_back(v);
}

// The vehicles and pedestrians to draw, and the cell each one is drawn in. The window's cells take what
// the original's visibility pass binds to them this frame (32F8 / 34D6 rules); the others stay where the
// list entry was bound (traffic) or, with replicas, repeat over every cell of their 4 x 4 pattern.
void SceneBuilder::Impl::collect_vehicles(bool fallback) {
    vehicles.clear();
    seen_entities.clear();
    const int rows = std::clamp<int>(ram.s16(kBigRows), 1, 5), cols = std::clamp<int>(ram.s16(kBigCols), 1, 5);
    const int cam_row = ram.s16(kCamRow), cam_col = ram.s16(kCamCol);
    const int bx0 = cam16_x >> 11, ax0 = cam16_y >> 11;
    const uint16_t ox = cam16_x & 0xF800, oy = cam16_y & 0xF800;
    const bool chase = ram.d16(kChase) != 0;
    const bool opponent_ok = ram.d8(kOpponentHighway) == 0;
    const int opp_tile = ram.s16(kOpponentTile), chase_tile = ram.s16(kChaseTile);
    const auto seen = [this](uint16_t e) { return std::find(seen_entities.begin(), seen_entities.end(), e) != seen_entities.end(); };
    const auto entity_xyz = [this](uint16_t e, int32_t& x, int32_t& y, int16_t& z) {
        x = ram.d16(static_cast<uint16_t>(e + 2));
        y = ram.d16(static_cast<uint16_t>(e + 4));
        z = ram.s16(static_cast<uint16_t>(e + 6));
    };

    // 1. The original's window.
    WinCell steps[6];
    int nsteps = 0;
    const bool hi_y = (cam16_y & 0x7FF) >= 0x400, hi_x = (cam16_x & 0x7FF) >= 0x400;
    if (yaw >= 0x13B || yaw < 0x2D) {
        const WinCell s[] = {hi_y ? k371B : k36D4, hi_y ? k36FD : k36B6, hi_y ? k369C : k3668, k36F0, k36E3, kOwnCell};
        std::copy(std::begin(s), std::end(s), steps);
    } else if (yaw < 0x87) {
        const WinCell s[] = {hi_x ? k370C : k3659, hi_x ? k36FD : k363B, hi_x ? k36E3 : k361D, k36A9, k369C, kOwnCell};
        std::copy(std::begin(s), std::end(s), steps);
    } else if (yaw < 0xE1) {
        const WinCell s[] = {hi_y ? k364A : k35FF, hi_y ? k363B : k35F0, hi_y ? k369C : k3668, k362C, k361D, kOwnCell};
        std::copy(std::begin(s), std::end(s), steps);
    } else {
        const WinCell s[] = {hi_x ? k36C5 : k360E, hi_x ? k36B6 : k35F0, hi_x ? k36E3 : k361D, k3677, k3668, kOwnCell};
        std::copy(std::begin(s), std::end(s), steps);
    }
    nsteps = 6;
    for (int si = 0; si < nsteps; ++si) {
        const WinCell& w = steps[si];
        const bool own = si == nsteps - 1;
        int row = cam_row, col = cam_col, bx = w.bx + bx0, ax = w.ax + ax0;
        if (bx < 0) {
            bx += 16;
            if (row != 0) --row;
        } else if (bx >= 16) {
            bx -= 16;
            if (++row >= rows) --row;
        }
        if (ax < 0) {
            ax += 16;
            if (col != 0) --col;
        } else if (ax >= 16) {
            ax -= 16;
            if (++col >= cols) --col;
        }
        const int bt = row * cols + col;
        const uint16_t ci = static_cast<uint16_t>(bx * 16 + ax);
        // Where the original draws this cell, as an absolute position.
        const auto draw16_x = static_cast<uint16_t>(w.cx + ox), draw16_y = static_cast<uint16_t>(w.dx + oy);
        const int32_t base_x = static_cast<int32_t>(std::lround(cam_x)) + static_cast<int16_t>(draw16_x - cam16_x);
        const int32_t base_y = static_cast<int32_t>(std::lround(cam_y)) + static_cast<int16_t>(draw16_y - cam16_y);
        const int cell = std::clamp(base_x / kCellSize, 0, kMapCells - 1) * kMapCells +
                         std::clamp(base_y / kCellSize, 0, kMapCells - 1);
        // List A (32F8 / 33C6).
        const uint16_t la = ram.d16(static_cast<uint16_t>(kListA + 2 * bt));
        for (int slot = 0; slot < 64; ++slot) {
            const auto at = static_cast<uint16_t>(la + 4 * slot);
            const uint16_t e = ram.d16(at);
            if (e == 0xFFFF) break;
            const uint16_t ec = ram.d16(static_cast<uint16_t>(at + 2));
            bool match = false;
            if (slot == 0) {
                match = ec == ci;
            } else if (slot == 1) {
                match = opponent_ok && opp_tile == bt && ec == ci;
            } else if (slot == 2) {
                match = chase && (own || chase_tile == bt) && ec == ci;
            } else if (slot == 3 && chase) {
                match = false;
            } else {
                match = (ec & 0x33) == (ci & 0x33) && ram.d16(static_cast<uint16_t>(e + 0x1C)) != 0;
            }
            if (!match) continue;
            seen_entities.push_back(e);
            int32_t x, y;
            int16_t z;
            entity_xyz(e, x, y, z);
            add_vehicle(e, ram.d16(e), cell, base_x + (x & 0x7FF), base_y + (y & 0x7FF), z, true);
            vehicles.back().window = true;
        }
        // List B (34D6): pedestrians, positions already cell-relative.
        const uint16_t lb = ram.d16(static_cast<uint16_t>(kListB + 2 * bt));
        for (int slot = 0; slot < 64; ++slot) {
            const auto at = static_cast<uint16_t>(lb + 4 * slot);
            const uint16_t e = ram.d16(at);
            if (e == 0xFFFF) break;
            const uint16_t ec = ram.d16(static_cast<uint16_t>(at + 2));
            if ((ec & 0x33) != (ci & 0x33)) continue;
            seen_entities.push_back(e);
            int32_t x, y;
            int16_t z;
            entity_xyz(e, x, y, z);
            add_vehicle(e, ram.d16(e), cell, base_x + static_cast<int16_t>(x), base_y + static_cast<int16_t>(y), z, true);
        }
    }
    if (!fallback) return;

    // 2. Everything else within the radius.
    const int cam_cx = static_cast<int>(std::floor(cam_x / kCellSize)), cam_cy = static_cast<int>(std::floor(cam_y / kCellSize));
    const int r = opt->radius;
    const auto in_radius = [&](int gx, int gy) {
        return gx >= 0 && gy >= 0 && gx < world.cells_x() && gy < world.cells_y() && std::abs(gx - cam_cx) <= r &&
               std::abs(gy - cam_cy) <= r;
    };
    uint16_t lists_a[25], lists_b[25];
    for (int bt = 0; bt < rows * cols; ++bt) {
        lists_a[bt] = ram.d16(static_cast<uint16_t>(kListA + 2 * bt));
        lists_b[bt] = ram.d16(static_cast<uint16_t>(kListB + 2 * bt));
    }
    const auto place = [&](uint16_t e, int row, int col, uint16_t ec, bool cell_relative) {
        if (row < 0 || col < 0 || row >= rows || col >= cols) return;
        const int gx = row * 16 + (ec >> 4 & 15), gy = col * 16 + (ec & 15);
        if (!in_radius(gx, gy)) return;
        int32_t x, y;
        int16_t z;
        entity_xyz(e, x, y, z);
        const int32_t bx = gx * kCellSize, by = gy * kCellSize;
        if (cell_relative) {
            add_vehicle(e, ram.d16(e), gx * kMapCells + gy, bx + static_cast<int16_t>(x), by + static_cast<int16_t>(y), z, false);
        } else {
            add_vehicle(e, ram.d16(e), gx * kMapCells + gy, bx + (x & 0x7FF), by + (y & 0x7FF), z, false);
        }
    };
    // The list entries of every big tile; an entity shared by several lists is drawn once.
    for (int bt = 0; bt < rows * cols; ++bt) {
        const uint16_t la = lists_a[bt];
        bool first_use = true;
        for (int p = 0; p < bt; ++p) first_use &= lists_a[p] != la;
        if (!first_use) continue;
        for (int slot = 0; slot < 64; ++slot) {
            const auto at = static_cast<uint16_t>(la + 4 * slot);
            const uint16_t e = ram.d16(at);
            if (e == 0xFFFF) break;
            const uint16_t ec = ram.d16(static_cast<uint16_t>(at + 2));
            if (seen(e)) continue;
            if (slot <= 2) {
                seen_entities.push_back(e);
                if (slot == 0) {
                    place(e, ram.s16(kPlayerRow), ram.s16(kPlayerCol), ec, false);
                } else if (slot == 1 && opponent_ok) {
                    place(e, opp_tile / 5, opp_tile % 5, ec, false);
                } else if (slot == 2 && chase) {
                    place(e, chase_tile / 5, chase_tile % 5, ec, false);
                }
                continue;
            }
            if (slot == 3 && chase) continue;
            if (ram.d16(static_cast<uint16_t>(e + 0x1C)) == 0) continue;
            seen_entities.push_back(e);
            if (opt->replicas) {
                for (int t = 0; t < rows * cols; ++t) {
                    if (lists_a[t] != la) continue;
                    for (int c = 0; c < 256; ++c) {
                        if ((c & 0x33) == (ec & 0x33)) place(e, t / cols, t % cols, static_cast<uint16_t>(c), false);
                    }
                }
            } else {
                // The bound cell, in the big tile with this list nearest the camera.
                int best = -1;
                double best_d = 1e30;
                for (int t = 0; t < rows * cols; ++t) {
                    if (lists_a[t] != la) continue;
                    const double gx = (t / cols) * 16 + (ec >> 4 & 15) + 0.5, gy = (t % cols) * 16 + (ec & 15) + 0.5;
                    const double d = (gx - cam_x / kCellSize) * (gx - cam_x / kCellSize) + (gy - cam_y / kCellSize) * (gy - cam_y / kCellSize);
                    if (d < best_d) {
                        best_d = d;
                        best = t;
                    }
                }
                if (best >= 0) place(e, best / cols, best % cols, ec, false);
            }
        }
    }
    if (opt->replicas) {
        for (int bt = 0; bt < rows * cols; ++bt) {
            const uint16_t lb = lists_b[bt];
            bool first_use = true;
            for (int p = 0; p < bt; ++p) first_use &= lists_b[p] != lb;
            if (!first_use) continue;
            for (int slot = 0; slot < 64; ++slot) {
                const auto at = static_cast<uint16_t>(lb + 4 * slot);
                const uint16_t e = ram.d16(at);
                if (e == 0xFFFF) break;
                const uint16_t ec = ram.d16(static_cast<uint16_t>(at + 2));
                if (seen(e)) continue;
                seen_entities.push_back(e);
                for (int t = 0; t < rows * cols; ++t) {
                    if (lists_b[t] != lb) continue;
                    for (int c = 0; c < 256; ++c) {
                        if ((c & 0x33) == (ec & 0x33)) place(e, t / cols, t % cols, static_cast<uint16_t>(c), true);
                    }
                }
            }
        }
    }
}

// Runs each vehicle's own draw routine on the scratch copy (its model, angles and height over the
// ground, the player only in external views, ...). A replica reuses its entity's first result.
void SceneBuilder::Impl::trace_vehicles() {
    vehicle_models.clear();
    vehicle_model_range.clear();
    if (vehicles.empty()) return;
    if (!tracer) {
        tracer = std::make_unique<Tracer>(ram.p);
    } else {
        tracer->load(ram.p);
    }
    for (size_t i = 0; i < vehicles.size(); ++i) {
        Vehicle& v = vehicles[i];
        // A replica of an entity already traced: same models, its own position.
        int32_t& first_of = entity_first[v.entity];
        if (first_of >= 0) {
            vehicle_model_range.push_back(vehicle_model_range[static_cast<size_t>(first_of)]);
            continue;
        }
        first_of = static_cast<int32_t>(i);
        const int gx = v.cell / kMapCells, gy = v.cell % kMapCells;
        const Cell& cell = world.cell(gx, gy);
        tracer->wr16(addr::kDataSeg, kCellTypeVar, cell.type);
        tracer->wr16(addr::kDataSeg, kCellRecord, world.types[cell.type].address);
        tracer->wr16(addr::kDataSeg, kCellZ, static_cast<uint16_t>(cell.elevation * kElevationStep));
        for (int k = 0; k < 3; ++k) {
            tracer->wr16(addr::kDataSeg, static_cast<uint16_t>(addr::kObjPos + 2 * k), static_cast<uint16_t>(v.pos16[static_cast<size_t>(k)]));
        }
        const V3 c = to_camera(v.x, v.y, v.z);
        v.key = static_cast<uint16_t>(c.z < 0 ? 0x400 : std::min(std::trunc(c.z), 32767.0f));
        tracer->wr16(addr::kCodeSeg, addr::kSortKey, v.key);
        tracer->run(v.routine, v.pos16, trace);
        const auto first = static_cast<uint32_t>(vehicle_models.size());
        for (const TraceEvent& e : trace.events) {
            if (e.op != TraceEvent::Op::Model) continue;
            ModelDraw md;
            md.model = e.addr;
            md.rotation = e.aux == 2 ? model_rotation(0, 0, 0)
                                     : model_rotation(e.angles[0], e.aux == 1 ? e.angles[1] : 0, e.aux == 1 ? e.angles[2] : 0);
            // Offsets in the 16-bit frame: z as drawn; x, y relative to the entry.
            md.offset = {static_cast<float>(e.pos[0]), static_cast<float>(e.pos[1]), static_cast<float>(e.pos[2])};
            md.outline = e.outline_enable != 0;
            vehicle_models.push_back(md);
        }
        vehicle_model_range.push_back({first, static_cast<uint32_t>(vehicle_models.size())});
    }
    for (const Vehicle& v : vehicles) entity_first[v.entity] = -1;
}

void SceneBuilder::Impl::draw_vehicle(int index) {
    const Vehicle& v = vehicles[static_cast<size_t>(index)];
    const auto [first, last] = vehicle_model_range[static_cast<size_t>(index)];
    lod_key = v.key;
    ++out->stats.vehicles;
    for (uint32_t k = first; k < last; ++k) {
        const ModelDraw& md = vehicle_models[k];
        const float r = meshes[static_cast<size_t>(md.model) % kModelCount].radius + 8;
        if (!opt->original_window &&
            !sphere_visible(to_camera(v.x + md.offset.x, v.y + md.offset.y, v.z + md.offset.z), r)) {
            continue;
        }
        cam.clear();
        draw_model(md.model, md.rotation, to_camera(v.x + md.offset.x, v.y + md.offset.y, v.z + md.offset.z), md.outline);
    }
}

// --- Cells --------------------------------------------------------------------------------------------------

void SceneBuilder::Impl::draw_ground() {
    {
        // Beyond the map: water out to the horizon (every big tile on the map's edge is the bay or the
        // ocean), so the background's ground colour (the camera tile's) doesn't show as a band there.
        constexpr float kFar = 4.0e6f;
        const float mx = float(world.cells_x() * kCellSize), my = float(world.cells_y() * kCellSize);
        const float x0 = -kFar, y0 = -kFar, x1 = mx + kFar, y1 = my + kFar;
        const V3 c[4] = {to_camera(x0, y0, 0), to_camera(x1, y0, 0), to_camera(x1, y1, 0), to_camera(x0, y1, 0)};
        const P3 pts[4] = {{c[0].x, c[0].y, c[0].z}, {c[1].x, c[1].y, c[1].z}, {c[2].x, c[2].y, c[2].z}, {c[3].x, c[3].y, c[3].z}};
        static constexpr uint16_t kQuad[6] = {0, 1, 2, 0, 2, 3};
        Colour water;
        water.raw = 9;
        emit_polygon(pts, 4, kQuad, 2, scene_colour(water), 1);
    }
    for (int row = 0; row < world.big_rows; ++row) {
        for (int col = 0; col < world.big_cols; ++col) {
            const float x0 = float(row * 0x8000), y0 = float(col * 0x8000);
            const float box[6] = {x0, y0, 0, x0 + 0x8000, y0 + 0x8000, 0};
            if (!box_visible(box)) continue;
            const V3 c[4] = {to_camera(x0, y0, 0), to_camera(x0 + 0x8000, y0, 0), to_camera(x0 + 0x8000, y0 + 0x8000, 0),
                             to_camera(x0, y0 + 0x8000, 0)};
            const P3 pts[4] = {{c[0].x, c[0].y, c[0].z}, {c[1].x, c[1].y, c[1].z}, {c[2].x, c[2].y, c[2].z}, {c[3].x, c[3].y, c[3].z}};
            static constexpr uint16_t kQuad[6] = {0, 1, 2, 0, 2, 3};
            Colour colour;
            colour.raw = world.big_tile_ground[static_cast<size_t>(row * world.big_cols + col)];
            emit_polygon(pts, 4, kQuad, 2, scene_colour(colour), 1);
        }
    }
}

void SceneBuilder::Impl::draw_cell_enhanced(int cell) {
    const int gx = cell / kMapCells, gy = cell % kMapCells;
    const Cell& c = world.cell(gx, gy);
    const TypeData& td = types[c.type];
    const int32_t ox = gx * kCellSize, oy = gy * kCellSize, oz = c.elevation * kElevationStep;
    ++out->stats.cells;
    const std::array<int16_t, 3> no16{};
    for (const Entry& e : td.list1) {
        if (e.routine < 0 || world.routines[static_cast<size_t>(e.routine)].compound) continue;
        draw_entry(e, ox + e.dx, oy + e.dy, oz + e.dz, no16, 0, false, false);
    }
    // Compound pieces lying in this cell, in their list order.
    const CourseData& cd = courses[static_cast<size_t>(state.course)];
    for (uint32_t i = cd.start[static_cast<size_t>(cell)]; i < cd.start[static_cast<size_t>(cell) + 1]; ++i) {
        const Piece& p = cd.pieces[i];
        draw_entry(Entry{p.routine, 0, 0, 0}, p.x, p.y, p.z, no16, 0, false, false);
    }
    // Sortables: list2 and the cell's vehicles, far first by the original's keys (4562 / 44D0 / 4686).
    sortables.clear();
    if (ram.d8(addr::kNoBuildings) == 0) {
        for (const Entry& e : td.list2) {
            if (e.routine < 0) continue;
            Sortable s{};
            s.kind = 0;
            s.entry = e;
            s.x = ox + e.dx;
            s.y = oy + e.dy;
            s.z = oz + e.dz;
            const float z = to_camera(s.x, s.y, s.z).z;
            s.key = std::max(z, 0.0f);
            sortables.push_back(s);
        }
    }
    for (int v = cell_head[static_cast<size_t>(cell)]; v >= 0; v = vehicles[static_cast<size_t>(v)].next) {
        const Vehicle& vh = vehicles[static_cast<size_t>(v)];
        Sortable s{};
        s.kind = 1;
        s.index = v;
        const float z = to_camera(vh.x, vh.y, vh.z).z;
        s.key = z < 0 ? float(0x400) : z;
        sortables.push_back(s);
    }
    // Stable, far first (insertion sort; the lists are short).
    for (size_t i = 1; i < sortables.size(); ++i) {
        const Sortable s = sortables[i];
        size_t j = i;
        while (j > 0 && sortables[j - 1].key < s.key) {
            sortables[j] = sortables[j - 1];
            --j;
        }
        sortables[j] = s;
    }
    for (const Sortable& s : sortables) {
        if (s.kind == 0) {
            draw_entry(s.entry, s.x, s.y, s.z, no16, static_cast<uint16_t>(std::min(s.key, 32767.0f)), false, true);
        } else {
            draw_vehicle(s.index);
        }
    }
}

// Validation: the original's own window, order and culls (30C6 / 31EE / 4562 / 44D0 / 4686).
void SceneBuilder::Impl::draw_original_window() {
    const int rows = std::clamp<int>(ram.s16(kBigRows), 1, 5), cols = std::clamp<int>(ram.s16(kBigCols), 1, 5);
    const int cam_row = ram.s16(kCamRow), cam_col = ram.s16(kCamCol);
    const int bx0 = cam16_x >> 11, ax0 = cam16_y >> 11;
    const uint16_t ox = cam16_x & 0xF800, oy = cam16_y & 0xF800;
    const bool hi_y = (cam16_y & 0x7FF) >= 0x400, hi_x = (cam16_x & 0x7FF) >= 0x400;
    WinCell steps[6];
    if (yaw >= 0x13B || yaw < 0x2D) {
        const WinCell s[] = {hi_y ? k371B : k36D4, hi_y ? k36FD : k36B6, hi_y ? k369C : k3668, k36F0, k36E3, kOwnCell};
        std::copy(std::begin(s), std::end(s), steps);
    } else if (yaw < 0x87) {
        const WinCell s[] = {hi_x ? k370C : k3659, hi_x ? k36FD : k363B, hi_x ? k36E3 : k361D, k36A9, k369C, kOwnCell};
        std::copy(std::begin(s), std::end(s), steps);
    } else if (yaw < 0xE1) {
        const WinCell s[] = {hi_y ? k364A : k35FF, hi_y ? k363B : k35F0, hi_y ? k369C : k3668, k362C, k361D, kOwnCell};
        std::copy(std::begin(s), std::end(s), steps);
    } else {
        const WinCell s[] = {hi_x ? k36C5 : k360E, hi_x ? k36B6 : k35F0, hi_x ? k36E3 : k361D, k3677, k3668, kOwnCell};
        std::copy(std::begin(s), std::end(s), steps);
    }
    for (int si = 0; si < 6; ++si) {
        const WinCell& w = steps[si];
        const bool own = si == 5;
        int row = cam_row, col = cam_col, bx = w.bx + bx0, ax = w.ax + ax0;
        if (bx < 0) {
            bx += 16;
            if (row != 0) --row;
        } else if (bx >= 16) {
            bx -= 16;
            if (++row >= rows) --row;
        }
        if (ax < 0) {
            ax += 16;
            if (col != 0) --col;
        } else if (ax >= 16) {
            ax -= 16;
            if (++col >= cols) --col;
        }
        ++out->stats.cells;
        const Cell& c = world.cell(row * 16 + bx, col * 16 + ax);
        const TypeData& td = types[c.type];
        const auto draw16_x = static_cast<uint16_t>(w.cx + ox), draw16_y = static_cast<uint16_t>(w.dx + oy);
        const auto z16 = static_cast<uint16_t>(c.elevation * kElevationStep);
        const int32_t base_x = static_cast<int32_t>(std::lround(cam_x)) + static_cast<int16_t>(draw16_x - cam16_x);
        const int32_t base_y = static_cast<int32_t>(std::lround(cam_y)) + static_cast<int16_t>(draw16_y - cam16_y);
        const auto p16 = [&](const Entry& e) {
            return std::array<int16_t, 3>{static_cast<int16_t>(draw16_x + e.dx), static_cast<int16_t>(draw16_y + e.dy),
                                          static_cast<int16_t>(z16 + e.dz)};
        };
        for (const Entry& e : td.list1) {
            draw_entry(e, base_x + e.dx, base_y + e.dy, z16 + e.dz, p16(e), 0, own, false);
        }
        // Sortables with the original's culls.
        sortables.clear();
        if (ram.d8(addr::kNoBuildings) == 0) {
            for (const Entry& e : td.list2) {
                const V3 p = to_camera(base_x + e.dx, base_y + e.dy, z16 + e.dz);
                if (p.z < -0x400 || p.z >= 0x1400 || std::fabs(p.x) >= p.z + 0x600) continue;
                Sortable s{};
                s.kind = 0;
                s.entry = e;
                s.x = base_x + e.dx;
                s.y = base_y + e.dy;
                s.z = z16 + e.dz;
                s.key = std::max(std::trunc(p.z), 0.0f);
                sortables.push_back(s);
            }
        }
        const int cell = std::clamp(base_x / kCellSize, 0, kMapCells - 1) * kMapCells + std::clamp(base_y / kCellSize, 0, kMapCells - 1);
        for (int v = cell_head[static_cast<size_t>(cell)]; v >= 0; v = vehicles[static_cast<size_t>(v)].next) {
            const Vehicle& vh = vehicles[static_cast<size_t>(v)];
            const V3 p = to_camera(vh.x, vh.y, vh.z);
            if (p.z < -0x80 || p.z >= 0x1400 || std::fabs(p.x) >= p.z + 0x80) continue;
            Sortable s{};
            s.kind = 1;
            s.index = v;
            s.key = p.z < 0 ? float(0x400) : std::trunc(p.z);
            sortables.push_back(s);
        }
        for (size_t i = 1; i < sortables.size(); ++i) {
            const Sortable s = sortables[i];
            size_t j = i;
            while (j > 0 && sortables[j - 1].key < s.key) {
                sortables[j] = sortables[j - 1];
                --j;
            }
            sortables[j] = s;
        }
        for (const Sortable& s : sortables) {
            if (s.kind == 0) {
                const std::array<int16_t, 3> q{static_cast<int16_t>(draw16_x + s.entry.dx), static_cast<int16_t>(draw16_y + s.entry.dy),
                                               static_cast<int16_t>(z16 + s.entry.dz)};
                draw_entry(s.entry, s.x, s.y, s.z, q, static_cast<uint16_t>(s.key), own, true);
            } else {
                draw_vehicle(s.index);
            }
        }
    }
}

// --- Public -------------------------------------------------------------------------------------------------

SceneBuilder::SceneBuilder(const World& world) : impl_(std::make_unique<Impl>(world)) {}
SceneBuilder::~SceneBuilder() = default;

void SceneBuilder::build(const uint8_t* ram, const SceneOptions& options, Scene& out) {
    const auto start = std::chrono::steady_clock::now();
    Impl& m = *impl_;
    out.clear();
    m.out = &out;
    m.ram.p = ram;
    m.setup(options);
    m.quantize = options.original_window;
    m.compound_done = false;
    m.lod_key = m.ram.u16(addr::kCodeSeg, addr::kSortKey);

    // Vehicles and pedestrians, bound to cells.
    m.collect_vehicles(!options.original_window);
    m.trace_vehicles();
    std::fill(m.cell_head.begin(), m.cell_head.end(), -1);
    for (size_t i = m.vehicles.size(); i-- > 0;) {
        Impl::Vehicle& v = m.vehicles[i];
        v.next = m.cell_head[static_cast<size_t>(v.cell)];
        m.cell_head[static_cast<size_t>(v.cell)] = static_cast<int>(i);
    }

    if (options.original_window) {
        m.draw_original_window();
    } else {
        if (options.ground) m.draw_ground();
        // Cells within the radius, far to near by Manhattan distance from the camera's cell: a ray from
        // the camera crosses cells in increasing distance, so this is a valid painter's order for what
        // stays inside its cell. The camera's own cell is last.
        const int cam_cx = static_cast<int>(std::floor(m.cam_x / kCellSize));
        const int cam_cy = static_cast<int>(std::floor(m.cam_y / kCellSize));
        const int r = std::max(options.radius, 0);
        const Impl::CourseData& cd = m.courses[static_cast<size_t>(m.state.course)];
        m.bucket_count.fill(0);
        m.order.clear();
        const int x0 = std::max(0, cam_cx - r), x1 = std::min(m.world.cells_x() - 1, cam_cx + r);
        const int y0 = std::max(0, cam_cy - r), y1 = std::min(m.world.cells_y() - 1, cam_cy + r);
        for (int gx = x0; gx <= x1; ++gx) {
            for (int gy = y0; gy <= y1; ++gy) {
                const int cell = gx * kMapCells + gy;
                if (!m.box_visible(&cd.box[static_cast<size_t>(cell) * 6])) {
                    ++out.stats.cells_culled;
                    continue;
                }
                const int d = std::min(std::abs(gx - cam_cx) + std::abs(gy - cam_cy), 2 * kMapCells + 1);
                ++m.bucket_count[static_cast<size_t>(d)];
                m.order.push_back(cell);
            }
        }
        // Counting sort by distance, far first.
        std::array<int, 2 * kMapCells + 3> pos{};
        int at = 0;
        for (int d = 2 * kMapCells + 1; d >= 0; --d) {
            pos[static_cast<size_t>(d)] = at;
            at += m.bucket_count[static_cast<size_t>(d)];
        }
        m.sorted.resize(m.order.size());
        for (const int cell : m.order) {
            const int gx = cell / kMapCells, gy = cell % kMapCells;
            const int d = std::min(std::abs(gx - cam_cx) + std::abs(gy - cam_cy), 2 * kMapCells + 1);
            m.sorted[static_cast<size_t>(pos[static_cast<size_t>(d)]++)] = cell;
        }
        for (const int cell : m.sorted) m.draw_cell_enhanced(cell);
    }
    out.stats.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace vette::enhanced
