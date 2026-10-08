#include "enhanced/scene.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#endif

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
constexpr int kMaxLayer = kMaxDepthLayer;
// Vehicles and pedestrians narrower than this on screen (output pixels) are drawn as the original's far box.
constexpr float kSmallModel = 8;
// Coplanar enough to be painted on (world units, about 3 inches each).
constexpr float kLayerTolerance = 4;

// Data in DS (notes 03, 04, 05).
constexpr uint16_t kViewLeft = 0x315E, kViewTop = 0x315A, kViewRight = 0x3160, kViewBottom = 0x315C;
constexpr uint16_t kCentreX = 0x3169, kCentreY = 0x316B;
constexpr uint16_t kCamRow = 0x2C93, kCamCol = 0x2C95;
constexpr uint16_t kListA = 0xEF5A, kListB = 0xEF8C;
constexpr uint16_t kPlayerRow = 0x2D57, kPlayerCol = 0x2D59;
constexpr uint16_t kOpponentTile = 0x2B70, kChaseTile = 0x2B6E, kOpponentHighway = 0x842B, kChase = 0xF7C2;
constexpr uint16_t kCellTypeVar = 0x3142, kCellRecord = 0x324A, kCellZ = 0x2CBB;
constexpr uint16_t kBigRows = 0x856F, kBigCols = 0x8571;

// The rear-view mirror's viewport descriptors (draw_mirror_view 3009:0686-06D2), by the view direction.
constexpr uint16_t kMirrorViewAhead = 0x35A3, kMirrorViewRight = 0x3587, kMirrorViewLeft = 0x3595;
constexpr int kSkyColour = 0x0B;  // cs:57DF, fill_sky_ground

// Highway mode (notes 03 "Highway mode", notes 04 section 8; highway_frame 3009:775E).
constexpr uint16_t kEndOfRoad = 0x8411;      // byte: the route's end is within the ring; the city is drawn too
constexpr uint16_t kRouteSegments = 0x8154;  // the route's segments {b slice type, b slices, w heading}..FFFF
constexpr uint16_t kRingBase = 0x804A;       // x, y, z, heading of the current slice (the ring's start)
constexpr uint16_t kSegment = 0x8158, kSlice = 0x815C;          // the current slice: segment, slice in it
constexpr uint16_t kRingSegment = 0x8232, kRingSlice = 0x8230;  // the ring's first slice when it was built
constexpr uint16_t kRing = 0x8234;           // 32 x {x, y, heading}, built by 7A0E
constexpr uint16_t kSliceRecords = 0x7A72;   // per slice type, its detailed record (7A0E: ring slices 6-18)
constexpr uint16_t kSliceRecordsFar = 0x7A88;  // ... and its plain one (the other ring slices)
constexpr uint16_t kCarKeys = 0x340A;        // per highway car, its sort key (FFFF: not drawn)
constexpr uint16_t kHighwayCars = 0x82F4;    // 11 x 16h bytes (notes 04 section 8)
constexpr uint16_t kCarActive = 0x8420, kCarHeights = 0x8222;
constexpr uint16_t kExternalView = 0x2ACF;   // byte: the camera is outside the car (helicopter view)
constexpr uint16_t kPlayerCar = 0x2D35;
constexpr uint16_t kDrawPlayerCar = 0x28D3;  // draw_player_car_chase
constexpr int kRingSlices = 32, kHighwayCarSlots = 11, kSliceVerts = 21;
constexpr int kSliceLength = 128;            // a straight slice's advance (type 0, vertex 2)
constexpr double kTrafficFadeSeconds = 0.75;  // SceneOptions::smooth_traffic
constexpr int kGroundHighway = 6;            // cs:57E0 while DS:2AD4 != 0 (5A41)

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
        uint32_t seq = 0;  // its place in the compounds' own drawing order
    };
    // Per course 1..4: compound pieces sorted by cell (CSR by cell), and each cell's bounds.
    // A compound piece's primitive cut along the cell grid (prepare_chunks): a convex polygon, or a line.
    struct Chunk {
        uint32_t piece = 0;    // CourseData::pieces
        uint16_t variant = 0;  // of the piece's routine
        uint16_t prim = 0;     // the variant's primitive, counted over its parts in order
        uint32_t first = 0;    // CourseData::chunk_pts
        uint8_t count = 0;     // points (2: a line)
        Colour colour;
    };
    struct CourseData {
        std::vector<Piece> pieces;
        std::vector<uint32_t> start;  // kCells + 1
        std::vector<float> box;       // 6 per cell: lo x y z, hi x y z (absolute)
        // Pieces whose geometry is fixed (no models, billboards, animation or painter-order
        // alternatives) are drawn by their chunks instead, each in its cell's turn of the walk.
        std::vector<uint8_t> chunked;      // per piece
        std::vector<Chunk> chunks;         // by cell, then in piece and primitive order
        std::vector<uint32_t> chunk_start; // kCells + 1
        std::vector<V3> chunk_pts;         // absolute
        std::vector<uint32_t> prim_base;   // per piece: its first slot in prim_visible
        uint32_t prim_slots = 0;
    };
    std::array<CourseData, 5> courses;
    void prepare_chunks(CourseData& cd);
    // Per frame: each chunked piece's variant (-2 not chosen yet, -1 none) and its primitives' culling.
    std::vector<int16_t> piece_variant;
    std::vector<uint8_t> prim_visible;
    int choose_piece(const CourseData& cd, uint32_t piece);

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
    void observe_window(uint16_t e, int cell, int32_t x, int32_t y, int16_t z) {
        const V3 v = to_camera(x, y, z);
        opt->observer->copy({e, cell, x, y, v.x, v.y, v.z, true, true});
    }

    // SceneOptions::placement for an entity in map cell gx, gy.
    game::Placement placement_of(uint16_t e, int gx, int gy) const {
        if (!opt->placement || gx < 0 || gy < 0 || gx >= world.cells_x() || gy >= world.cells_y()) return game::Placement::Default;
        return opt->placement(e, gx, gy, world.cell(gx, gy).type);
    }

    // Replicas (SceneOptions::replicas). The traffic AI drives its lanes as if every cell had the same
    // roads (two-way, along the cell's edges, notes 04 section 7), and a pedestrian walks its square as if on
    // the same pavement; the original's window shows them wherever it looks. A copy is kept only in a cell
    // where the road along the entity's way (relative to the cell) is the one in the cells its moves were
    // made for: of all the cells of its pattern, those where most of the way is on road (pavement for a
    // pedestrian), and of these the commonest road (choose_replicas). It doesn't depend on the camera, so a
    // copy stays as long as its way does: a car's whole loop is checked (rotating round it changes nothing).
    // The road: every object's flat faces of road (colour 8) or pavement (7 or 8) at ground level (within
    // kGroundLevel of its cell's ground: not a freeway deck), by the object's bounds; listed per cell.
    static constexpr int kGroundLevel = 16;
    struct RoadBox {
        float x0, y0, x1, y1;
        uint64_t kind;  // the object's routine and its cell's height
        bool road;      // else pavement only
    };
    int roads_course = -1;
    std::vector<RoadBox> road_boxes;
    std::vector<uint32_t> road_start, road_index;  // per cell (CSR): the boxes over it
    void prepare_roads();
    // What lies under a point: 0 nothing (for traffic: no road), else the sum of its objects' kinds.
    uint64_t road_at(int32_t wx, int32_t wy, bool traffic) const {
        if (wx < 0 || wy < 0 || wx >= world.cells_x() * kCellSize || wy >= world.cells_y() * kCellSize) return 0;
        const size_t c = static_cast<size_t>(wx / kCellSize * world.cells_y() + wy / kCellSize);
        uint64_t k = 0;
        const float x = static_cast<float>(wx), y = static_cast<float>(wy);
        for (uint32_t i = road_start[c]; i < road_start[c + 1]; ++i) {
            const RoadBox& b = road_boxes[road_index[i]];
            if ((b.road || !traffic) && x >= b.x0 && x < b.x1 && y >= b.y0 && y < b.y1) k += b.kind;
        }
        return k;
    }
    // An entity's copies: which cells of its pattern keep one (per big tile, the 16 cells of its class), for
    // the state it was decided in.
    struct ReplicaChoice {
        uint64_t state = 0;
        std::array<uint16_t, 25> keep{};  // per big tile, a bit per class cell (i * 4 + j: cell (c + 4i, c + 4j))
    };
    std::unordered_map<uint32_t, ReplicaChoice> replica_choices;  // by entity and list (an entity can be in two)
    std::vector<std::array<int32_t, 2>> samples;
    struct PatternCell {
        uint64_t signature = 0;  // what lies under the way, in order
        int score = 0;           // points of the way on road (pavement)
        int tile = 0, bit = 0;
    };
    std::vector<PatternCell> pattern;
    const ReplicaChoice& choose_replicas(uint16_t e, bool traffic, uint16_t ec, uint16_t list, const uint16_t* lists, int tiles,
                                         int cols);
    std::unique_ptr<Tracer> tracer;
    bool tracer_loaded = false;  // holds this build's memory
    void load_tracer() {
        if (tracer_loaded) return;
        if (!tracer) {
            tracer = std::make_unique<Tracer>(ram.p);
        } else {
            tracer->load(ram.p);
        }
        tracer_loaded = true;
    }
    Trace trace;
    // Camera-dependent routines: the variant the original's own routine picks for where the camera is,
    // run on the tracer with the camera pulled in to the object (see positional()); remembered while
    // the camera is far from the object, where the pulled-in camera doesn't move.
    std::unordered_map<uint64_t, const Variant*> positional_cache;
    std::vector<int8_t> side_dependent;  // per routine: -1 not known yet, 0 no, 1 yes
    const Variant* positional(int index, const Routine& r, int32_t x, int32_t y, int32_t z, uint16_t key);
    const Variant* trace_choice(const Routine& r, int32_t x, int32_t y, int32_t z, int dx, int dy, int dz, bool own,
                                uint16_t near_key);

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
    int window_steps[6] = {-1, -1, -1, -1, -1, -1};  // the original's window this frame: its steps' map cells
    std::vector<size_t> window_slots;
    std::vector<int32_t> entity_first;  // per DS offset: the first vehicle of that entity this frame
    bool compound_done = false;         // DS:2AC0 (original window mode without a hook)
    std::vector<std::array<int16_t, 3>> hook_angles;

    // The mirror (SceneOptions::mirror): the projection's x negated (camera space stays a true camera,
    // so culling and winding tests are unchanged), the view's camera-space x range swapped.
    bool mirror = false;
    float xs = 1;                  // +1, or -1 in the mirror
    float xl = -160, xr = 160;     // the view's camera-space X * f / Z range
    std::array<M3, 360> heading_rot;  // model_rotation(yaw, 0, 0) for each whole degree

    // Freeway (highway mode).
    bool freeway = false;
    struct SliceType {
        bool valid = false;
        int count = 0;  // vertices
        V3 verts[kSliceVerts];  // model axes: x along the road, y down, z across
        int dh = 0;             // heading change to the next slice, degrees
        struct Poly {
            uint8_t n = 0;
            uint8_t idx[16] = {};
            uint8_t colour = 0;
        };
        struct Line {
            uint8_t a = 0, b = 0, colour = 0;
        };
        int npolys = 0, nlines = 0;
        Poly polys[4];
        Line lines[32];
    };
    std::array<SliceType, 16> slice_types, slice_types_far;
    struct Segment {
        uint8_t type, slices;
    };
    std::vector<Segment> segments;
    struct Slice {
        double x = 0, y = 0;  // absolute, the camera's frame
        int heading = 0;
        int type = 0;
        float dist = 0;
    };
    std::vector<Slice> slices;
    std::vector<int> slice_order;
    struct HighwayObject {
        double x = 0, y = 0, z = 0;
        int model = 0;
        int heading = 0;
        float dist = 0;
        int player = -1;  // >= 0: the player's car, the vehicle_models range
        int16_t key = 0;  // the original's sort key (DS:340A)
        float alpha = 1;  // SceneOptions::smooth_traffic: fading in or out
    };
    std::vector<HighwayObject> highway_objects;
    void read_slice_types(uint16_t table, std::array<SliceType, 16>& types_out);
    void draw_freeway();
    // SceneOptions::smooth_traffic: each highway car slot as last seen, and the cars the original took
    // off, drawn on along the road while they fade out. A car's place is its slice (the route's slices
    // counted from its start) and where on it, along the road from the slice's start and across, so it
    // doesn't depend on the frame the original re-centres.
    struct TrafficCar {
        bool on = false;
        double since = 0;  // when it came (seconds)
        int slice = -1;
        double along = 0, across = 0;
        int heading = 0, model = -1, lane = -1;
        double speed = 0;  // units per second
        float alpha = 1;
    };
    std::array<TrafficCar, kHighwayCarSlots> traffic_slots{};
    struct Departed {
        TrafficCar car;
        double gone = 0;
    };
    std::vector<Departed> departed;
    bool traffic_seen = false;  // false: the cars on the road now were there all along (they don't fade in)
    double traffic_time = 0;
    void reset_traffic() {
        traffic_slots = {};
        departed.clear();
        traffic_seen = false;
    }
    float fade = 1;  // the alpha of what's drawn now (a fading car)
    void draw_sky();
    void draw_plane(int colour);

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
    void transform_variant(int routine, const Variant& v, int32_t x, int32_t y, int32_t z,
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
        return quantize ? cx + std::trunc(xs * v.x * kFocal / v.z) + 0.5f : xs * v.x * kFocal / v.z + cx + kPixelOffset;
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
    void vertex(float x, float y, float z, const SceneColour& c, float a) {
        out->vertices.push_back({x, y, c.r, c.g, c.b, a, depth(z)});
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
    // Depth (SceneVertex::depth): kNear / z, raised by a layer bias, 1 + s * kDepthStep. A primitive's
    // layer s is one above the highest of the earlier primitives of its group that it lies on (coplanar
    // within kLayerTolerance and overlapping it on screen), else 0, and at least 1 for lines and markings:
    // what is painted on a surface (markings on a road, windows on a wall, outlines on faces, a car's
    // details) wins over it as in painter's order, while everything else is decided by its true depth.
    // Groups: a cell's ground layer (list 1, the bridges' pieces) with, in turn, each of its sortables
    // (list 2, vehicles: one's flat primitives may lie on the ground layer, not on another sortable); a
    // freeway slice; a highway car; a window step (original window mode). The backdrop (the big tiles'
    // ground, the water beyond the map, the freeway's plane, the mirror's sky) has depth 0: it never hides
    // anything.
    struct Layer {
        float x0, y0, x1, y1;  // screen box (race pixels)
        float mx, my;          // the outline's centre
        uint32_t pts, pts_n;   // its outline on screen: layer_pts[pts .. pts + pts_n)
        bool convex;           // ... made convex (make_convex)
        V3 n;                  // polygon: unit normal; line: unit direction (camera space)
        V3 p, q;               // polygon: centroid; line: its ends
        float d;               // polygon: n . centroid
        float len, tol2;       // line: length, twice the tolerance at its farther end
        uint8_t kind;          // 0 neither (degenerate), 1 polygon, 2 line
        uint16_t s;
    };
    std::vector<Layer> layers;  // the current group's primitives so far
    // ... for the scan, as arrays: their boxes and orientations (faces: normal; lines: direction, length and
    // twice the tolerance; line: 1 for a line).
    struct LayerScan {
        std::vector<float> x0, y0, x1, y1, nx, ny, nz, len, tol2, line;
        size_t n = 0;
        void push(const Layer& l) {
            if (n == x0.size()) {
                const size_t cap = std::max<size_t>(256, 2 * n);
                for (std::vector<float>* v : {&x0, &y0, &x1, &y1, &nx, &ny, &nz, &len, &tol2, &line}) v->resize(cap);
            }
            x0[n] = l.x0;
            y0[n] = l.y0;
            x1[n] = l.x1;
            y1[n] = l.y1;
            nx[n] = l.n.x;
            ny[n] = l.n.y;
            nz[n] = l.n.z;
            len[n] = l.len;
            tol2[n] = l.tol2;
            line[n] = l.kind == 2 ? 1.0f : 0.0f;
            ++n;
        }
    } scan;
    std::vector<uint32_t> candidates;
    void find_candidates(const Layer& l);
    std::vector<P2> layer_pts;
    float bias = 1;             // 1 + s * kDepthStep of the primitive being emitted
    bool backdrop = false;      // emitting the backdrop: depth 0
    bool stripe = false;        // emitting a marking's stripe
    bool small = false;         // a small vehicle: its far box, true depth only (lines one layer up), no layers
    int group_top = 0;  // the group's highest layer so far
    // A point to come back to in the group.
    struct GroupMark {
        size_t layers, pts;
        int top;
        float flat_lo, flat_hi;
    };
    GroupMark group_mark() const { return {layers.size(), layer_pts.size(), group_top, flat_lo, flat_hi}; }
    void group_reset(const GroupMark& m) {
        layers.resize(m.layers);
        layer_pts.resize(m.pts);
        scan.n = m.layers;
        group_top = m.top;
        own_from = m.layers;
        flat_lo = m.flat_lo;
        flat_hi = m.flat_hi;
    }
    // The heights (camera space, along up_cam) the group's flat primitives lie at.
    float flat_lo = 1e30f, flat_hi = -1e30f;
    // The group's primitives from here on are an object's own: before them (the ground layer), only a flat
    // primitive of it is looked for what it lies on.
    size_t own_from = 0;
    void begin_group() {
        group_top = 0;
        own_from = 0;
        flat_lo = 1e30f;
        flat_hi = -1e30f;
        layers.clear();
        layer_pts.clear();
        scan.n = 0;
    }
    bool no_depth = false;  // SceneOptions::depth off: every depth 0, no layers
    float depth(float z) const { return backdrop || no_depth ? 0.0f : bias * (kNear / std::max(z, kNear)); }
    static float tolerance(float z) { return kLayerTolerance + 1e-4f * std::fabs(z); }
    bool lies_on(const Layer& l, const Layer& o) const;
    void make_convex(Layer& l);
    bool overlap(Layer& l, Layer& o);
    bool inside(Layer& l, float x, float y);
    void assign_layer(Layer& l, const float* sx, const float* sy, int ns);
    // A polygon (camera-space points point(0..n-1)) whose outline on screen is (sx, sy)[0..ns).
    template <typename Point>
    void layer_polygon(int n, Point point, const float* sx, const float* sy, int ns) {
        if (backdrop || no_depth) return;
        if (small) {
            bias = 1;
            return;
        }
        Layer l{};
        V3 nrm, c;
        for (int k = 0, j = n - 1; k < n; j = k++) {
            const V3 a = point(j), b = point(k);
            nrm.x += (a.y - b.y) * (a.z + b.z);
            nrm.y += (a.z - b.z) * (a.x + b.x);
            nrm.z += (a.x - b.x) * (a.y + b.y);
            c = c + a;
        }
        const float len = std::sqrt(dot(nrm, nrm));
        c = {c.x / float(n), c.y / float(n), c.z / float(n)};
        if (len > 1e-6f) {
            l.kind = 1;
            l.n = {nrm.x / len, nrm.y / len, nrm.z / len};
            l.p = c;
            l.d = dot(l.n, c);
        }
        assign_layer(l, sx, sy, ns);
    }
    // A line from a to b (camera space) drawn as the quad (sx, sy)[0..4).
    void layer_line(V3 a, V3 b, const float* sx, const float* sy) {
        if (backdrop || no_depth) return;
        if (small) {
            bias = 1 + kDepthStep;  // a line: on something
            return;
        }
        Layer l{};
        const V3 d = b - a;
        const float len = std::sqrt(dot(d, d));
        if (len > 1e-6f) {
            l.kind = 2;
            l.n = {d.x / len, d.y / len, d.z / len};
            l.p = a;
            l.q = b;
            l.len = len;
            l.tol2 = 2 * tolerance(std::max(std::fabs(a.z), std::fabs(b.z)));
        }
        assign_layer(l, sx, sy, 4);
    }
    std::vector<float> clip_points;  // emit_polygon: per clipped triangle, its count, then x, y, camera z each
    std::vector<P2> hull_in, hull_out;

    void emit_polygon(const P3* pts, int n, const uint16_t* tri, uint32_t ntri, const SceneColour& c, float a);
    // A polygon whose points are cv[idx[k]] (projections sv[idx[k]]).
    void emit_indexed(const V3* cv, const P2* sv, const uint16_t* idx, int n, const uint16_t* tri, uint32_t ntri,
                      const SceneColour& c, float a);
    void emit_line(P3 a, P3 b, const SceneColour& c);
    void emit_segment(const V3* cv, const P2* sv, uint16_t ia, uint16_t ib, const SceneColour& c) {
        if (cv[ia].z >= kNear && cv[ib].z >= kNear) {
            const P2 a = sv[ia], b = sv[ib];
            if ((a.x < vx0 && b.x < vx0) || (a.x > vx1 && b.x > vx1) || (a.y < vy0 && b.y < vy0) || (a.y > vy1 && b.y > vy1)) {
                return;  // entirely beyond one side of the view (a stripe there too: it's thinner than the margin)
            }
        }
        const int marking = ribbons ? emit_ribbon(cv[ia], cv[ib], c) : 0;
        if (marking == 1) return;
        thin_line = marking == 2;
        if (cv[ia].z >= kNear && cv[ib].z >= kNear) {
            emit_quad_2d(cv[ia], cv[ib], sv[ia].x, sv[ia].y, sv[ib].x, sv[ib].y, c);
        } else {
            emit_line({cv[ia].x, cv[ia].y, cv[ia].z}, {cv[ib].x, cv[ib].y, cv[ib].z}, c);
        }
        thin_line = false;
    }
    // Ground markings: while drawing the ground layer, the bridges' pieces and the freeway, horizontal
    // lines at the object's ground level are stripes on the road (SceneOptions::marking_width). Returns
    // 0 for any other line, 1 when the stripe covers the line, 2 when (some of) it projects thinner than
    // a pixel: the caller then draws the line too, as thin as a line gets (thin_line).
    bool ribbons = false;
    bool thin_line = false;
    V3 up_cam;  // world up (+z) in camera space
    int emit_ribbon(V3 a, V3 b, const SceneColour& c);
    void draw_prim(const Part& p, const Prim& prim, const PrimData& pr, const V3* cv, const P2* sv);
    bool visible(const Cull& cull) const;
    // A line from a to b (camera space), at (x0, y0) and (x1, y1) on screen.
    void emit_quad_2d(V3 a, V3 b, float x0, float y0, float x1, float y1, const SceneColour& c);
    // A line's half width in output pixels at depth z (SceneOptions::line_world_width).
    float line_half_width(float z) const {
        const float race_px = std::max(opt->pixel_w, opt->pixel_h);
        const float lo = std::max(opt->line_width, 0.25f * race_px), hi = std::max(lo, opt->line_max * race_px);
        if (thin_line) return 0.5f * opt->line_width;  // a marking's far part: no thicker than its stripe
        return 0.5f * std::clamp(opt->line_world_width * kFocal / std::max(z, kNear) * race_px, lo, hi);
    }
    float ribbon_base_z = 0;  // the world height markings lie at (the object's base)
    bool line_piece = false;  // a cell's part of a longer line: kept however short it projects
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
    for (int d = 0; d < 360; ++d) heading_rot[static_cast<size_t>(d)] = model_rotation(d, 0, 0);
    side_dependent.assign(world.routines.size(), -1);
    segments.reserve(512);
    slices.reserve(4096);
    slice_order.reserve(4096);
    highway_objects.reserve(16);
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
                p.seq = static_cast<uint32_t>(cd.pieces.size());
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
        prepare_chunks(cd);
    }
}

// The bridges' pieces are long: a deck slab, a side wall or a cable runs across many cells, so drawing it
// whole in one cell's turn of the far-to-near walk breaks the painter's order (a slab that starts
// behind the camera covers the nearer road's markings, the cables and the hills ahead). Cut along
// the cell grid, every part of it lies in one cell and is drawn with that cell, in the compound's own
// order there, the way the walk is valid for everything else.
void SceneBuilder::Impl::prepare_chunks(CourseData& cd) {
    cd.chunked.assign(cd.pieces.size(), 0);
    cd.prim_base.assign(cd.pieces.size(), 0);
    std::vector<Chunk> chunks;
    std::vector<int> chunk_cell;
    const auto clip_axis = [](const std::vector<V3>& in, int axis, float bound, bool keep_above) {
        std::vector<V3> out;
        const auto coord = [axis](const V3& v) { return axis == 0 ? v.x : v.y; };
        for (size_t i = 0; i < in.size(); ++i) {
            const V3& a = in[i];
            const V3& b = in[(i + 1) % in.size()];
            const float ca = coord(a) - bound, cb = coord(b) - bound;
            const bool ia = keep_above ? ca >= 0 : ca <= 0, ib = keep_above ? cb >= 0 : cb <= 0;
            if (ia) out.push_back(a);
            if (ia != ib) {
                const float t = ca / (ca - cb);
                out.push_back(V3{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z)});
            }
        }
        return out;
    };
    std::vector<uint32_t> by_seq(cd.pieces.size());
    for (uint32_t i = 0; i < by_seq.size(); ++i) by_seq[i] = i;
    std::sort(by_seq.begin(), by_seq.end(), [&](uint32_t a, uint32_t b) { return cd.pieces[a].seq < cd.pieces[b].seq; });
    for (const uint32_t pi : by_seq) {
        const Piece& p = cd.pieces[pi];
        const Routine& r = world.routines[static_cast<size_t>(p.routine)];
        const RoutineData& rd = routines[static_cast<size_t>(p.routine)];
        bool fixed = !r.variants.empty();
        uint32_t most = 0;
        for (const Variant& v : r.variants) {
            fixed &= v.calls.empty() && v.reorders.empty();
            uint32_t n = 0;
            for (const Part& part : v.parts) {
                fixed &= part.source == Part::Source::Packed ||
                         (part.source == Part::Source::Plain &&
                          (part.rotation == Part::Rotation::None || part.rotation == Part::Rotation::Fixed));
                n += static_cast<uint32_t>(part.prims.size());
            }
            most = std::max(most, n);
        }
        if (!fixed) continue;
        cd.chunked[pi] = 1;
        cd.prim_base[pi] = cd.prim_slots;
        cd.prim_slots += most;
        const V3 at{static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)};
        for (size_t vi = 0; vi < r.variants.size(); ++vi) {
            const Variant& v = r.variants[vi];
            const VariantData& vd = rd.variants[vi];
            uint16_t g = 0;
            for (size_t k = 0; k < v.parts.size(); ++k) {
                const Part& part = v.parts[k];
                const PartData& pd = vd.parts[k];
                // The part's vertices, absolute.
                std::vector<V3> wv;
                const V3 o{static_cast<float>(part.origin.x), static_cast<float>(part.origin.y), static_cast<float>(part.origin.z)};
                for (const V3& q : pd.verts) {
                    wv.push_back(part.source == Part::Source::Packed ? at + q : at + o + (pd.rotated ? pd.rotation(q) : q));
                }
                for (size_t pr = 0; pr < part.prims.size(); ++pr, ++g) {
                    const Prim& prim = part.prims[pr];
                    const PrimData& prd = pd.prims[pr];
                    const uint16_t* idx = &part.indices[prim.first];
                    const auto add = [&](const std::vector<V3>& pts) {
                        if (pts.size() < 2) return;
                        V3 c{0, 0, 0};
                        for (const V3& q : pts) c = c + q;
                        const float inv = 1.0f / static_cast<float>(pts.size());
                        const int cx = std::clamp(static_cast<int>(std::floor(c.x * inv / kCellSize)), 0, world.cells_x() - 1);
                        const int cy = std::clamp(static_cast<int>(std::floor(c.y * inv / kCellSize)), 0, world.cells_y() - 1);
                        Chunk ch;
                        ch.piece = pi;
                        ch.variant = static_cast<uint16_t>(vi);
                        ch.prim = g;
                        ch.first = static_cast<uint32_t>(cd.chunk_pts.size());
                        ch.count = static_cast<uint8_t>(std::min<size_t>(pts.size(), 255));
                        ch.colour = prim.colour;
                        for (size_t q = 0; q < ch.count; ++q) cd.chunk_pts.push_back(pts[q]);
                        chunks.push_back(ch);
                        chunk_cell.push_back(cx * kMapCells + cy);
                    };
                    // Every cell the shape's bounds touch, each with its part of the shape.
                    const auto cut = [&](const std::vector<V3>& shape) {
                        float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
                        for (const V3& q : shape) {
                            x0 = std::min(x0, q.x);
                            x1 = std::max(x1, q.x);
                            y0 = std::min(y0, q.y);
                            y1 = std::max(y1, q.y);
                        }
                        const int gx0 = static_cast<int>(std::floor(x0 / kCellSize)), gx1 = static_cast<int>(std::floor(x1 / kCellSize));
                        const int gy0 = static_cast<int>(std::floor(y0 / kCellSize)), gy1 = static_cast<int>(std::floor(y1 / kCellSize));
                        if (gx0 == gx1 && gy0 == gy1) {
                            add(shape);
                            return;
                        }
                        for (int gx = gx0; gx <= gx1; ++gx) {
                            for (int gy = gy0; gy <= gy1; ++gy) {
                                std::vector<V3> c = shape;
                                if (shape.size() == 2) {
                                    // A line: the part of it within the cell.
                                    const V3 a = shape[0], b = shape[1];
                                    float t0 = 0, t1 = 1;
                                    const auto slab = [&](float pa, float pb, float lo, float hi) {
                                        const float d = pb - pa;
                                        if (std::fabs(d) < 1e-6f) return pa >= lo && pa <= hi;
                                        float ta = (lo - pa) / d, tb = (hi - pa) / d;
                                        if (ta > tb) std::swap(ta, tb);
                                        t0 = std::max(t0, ta);
                                        t1 = std::min(t1, tb);
                                        return t0 < t1;
                                    };
                                    if (!slab(a.x, b.x, float(gx * kCellSize), float((gx + 1) * kCellSize)) ||
                                        !slab(a.y, b.y, float(gy * kCellSize), float((gy + 1) * kCellSize))) {
                                        continue;
                                    }
                                    c = {V3{a.x + t0 * (b.x - a.x), a.y + t0 * (b.y - a.y), a.z + t0 * (b.z - a.z)},
                                         V3{a.x + t1 * (b.x - a.x), a.y + t1 * (b.y - a.y), a.z + t1 * (b.z - a.z)}};
                                } else {
                                    c = clip_axis(c, 0, float(gx * kCellSize), true);
                                    if (c.size() >= 3) c = clip_axis(c, 0, float((gx + 1) * kCellSize), false);
                                    if (c.size() >= 3) c = clip_axis(c, 1, float(gy * kCellSize), true);
                                    if (c.size() >= 3) c = clip_axis(c, 1, float((gy + 1) * kCellSize), false);
                                    if (c.size() < 3) continue;
                                }
                                add(c);
                            }
                        }
                    };
                    if (prd.line || (prd.count == 0 && prim.count == 2)) {
                        cut({wv[idx[0]], wv[idx[1]]});
                    } else {
                        for (uint32_t t = 0; t < prd.count; ++t) {
                            const uint16_t* tri = &tris[(prd.first + t) * 3];
                            cut({wv[idx[tri[0]]], wv[idx[tri[1]]], wv[idx[tri[2]]]});
                        }
                    }
                }
            }
        }
    }
    // By cell, keeping the compound's order within each.
    std::vector<uint32_t> by_cell(chunks.size());
    for (uint32_t i = 0; i < by_cell.size(); ++i) by_cell[i] = i;
    std::stable_sort(by_cell.begin(), by_cell.end(), [&](uint32_t a, uint32_t b) { return chunk_cell[a] < chunk_cell[b]; });
    cd.chunks.clear();
    cd.chunk_start.assign(kCells + 1, 0);
    for (const uint32_t i : by_cell) {
        cd.chunks.push_back(chunks[i]);
        ++cd.chunk_start[static_cast<size_t>(chunk_cell[i]) + 1];
    }
    for (int i = 0; i < kCells; ++i) cd.chunk_start[static_cast<size_t>(i) + 1] += cd.chunk_start[static_cast<size_t>(i)];
    // The cells' bounds take in their chunks.
    for (size_t k = 0; k < by_cell.size(); ++k) {
        const Chunk& ch = cd.chunks[k];
        float* b = &cd.box[static_cast<size_t>(chunk_cell[by_cell[k]]) * 6];
        for (uint32_t q = 0; q < ch.count; ++q) {
            const V3& v = cd.chunk_pts[ch.first + q];
            b[0] = std::min(b[0], v.x);
            b[1] = std::min(b[1], v.y);
            b[2] = std::min(b[2], v.z);
            b[3] = std::max(b[3], v.x);
            b[4] = std::max(b[4], v.y);
            b[5] = std::max(b[5], v.z);
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
    // The mirror's camera and viewport (3009:0686-06D8): the view offset picks both.
    mirror = options.mirror;
    xs = mirror ? -1.0f : 1.0f;
    uint16_t viewport = 0;
    if (mirror) {
        const int16_t offset = ram.s16(addr::kViewOffset);
        if (offset == 0x55) {
            yaw += 95;
            if (yaw >= 360) yaw -= 360;
            viewport = kMirrorViewRight;
        } else if (offset == -0x55) {
            yaw -= 95;
            if (yaw < 0) yaw += 360;
            viewport = kMirrorViewLeft;
        } else {
            yaw -= 180;
            if (yaw < 0) yaw += 360;
            viewport = kMirrorViewAhead;
        }
        pitch = -pitch;
    }
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
    up_cam = view(V3{0, 0, 1});
    if (viewport) {
        // A descriptor: x_left, y_top, x_right, y_bottom, centre_y, centre_x, height (set_viewport 3778).
        left = ram.s16(viewport);
        top = ram.s16(static_cast<uint16_t>(viewport + 2));
        right = ram.s16(static_cast<uint16_t>(viewport + 4));
        bottom = ram.s16(static_cast<uint16_t>(viewport + 6));
        cy = ram.s16(static_cast<uint16_t>(viewport + 8));
        cx = ram.s16(static_cast<uint16_t>(viewport + 10));
    } else {
        left = ram.s16(kViewLeft);
        top = ram.s16(kViewTop);
        right = ram.s16(kViewRight);
        bottom = ram.s16(kViewBottom);
        cx = ram.s16(kCentreX);
        cy = ram.s16(kCentreY);
    }
    xl = mirror ? cx - (right + 1) : left - cx;
    xr = mirror ? cx - left : right + 1 - cx;
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
        const float l = xl - 2, r = xr + 2, t = top - cy - 2, b = bottom + 1 - cy + 2;
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
    const float l = xl - kMargin, r = xr - 1 + kMargin, t = top - cy - kMargin, d = bottom - cy + kMargin;
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

// Whether l lies on o: coplanar (a line: on the plane, or along the same line), within the tolerance.
bool SceneBuilder::Impl::lies_on(const Layer& l, const Layer& o) const {
    const auto on_plane = [](const Layer& pl, V3 v) { return std::fabs(dot(pl.n, v) - pl.d) <= tolerance(v.z); };
    if (o.kind == 1) {
        if (l.kind == 1) return std::fabs(dot(l.n, o.n)) > 0.999f && on_plane(o, l.p);
        return l.kind == 2 && on_plane(o, l.p) && on_plane(o, l.q);
    }
    if (o.kind == 2) {
        if (l.kind == 1) return on_plane(l, o.p) && on_plane(l, o.q);
        if (l.kind == 2) {
            // Along the same line (not merely meeting it at a corner).
            const auto off = [&](V3 v) {
                const V3 c = cross(v - o.p, o.n);
                return dot(c, c) <= tolerance(v.z) * tolerance(v.z);
            };
            return off(l.p) && off(l.q);
        }
    }
    return false;
}

// A layer's outline on screen as a convex polygon (in place): as drawn if it is convex already, else its
// convex hull (monotone chain).
void SceneBuilder::Impl::make_convex(Layer& l) {
    if (l.convex) return;
    l.convex = true;
    P2* p = &layer_pts[l.pts];
    const uint32_t n = l.pts_n;
    if (n < 3) return;
    const auto turn = [](const P2& o, const P2& a, const P2& b) {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };
    bool pos = false, neg = false;
    for (uint32_t i = 0, j = n - 1, k = n - 2; i < n; k = j, j = i++) {
        const float t = turn(p[k], p[j], p[i]);
        pos |= t > 0;
        neg |= t < 0;
    }
    if (!(pos && neg)) return;  // convex (or degenerate)
    hull_in.assign(p, p + n);
    std::sort(hull_in.begin(), hull_in.end(),
              [](const P2& a, const P2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    hull_out.clear();
    for (int pass = 0; pass < 2; ++pass) {
        const size_t start = hull_out.size();
        for (size_t i = 0; i < hull_in.size(); ++i) {
            const P2 pt = hull_in[pass == 0 ? i : hull_in.size() - 1 - i];
            while (hull_out.size() >= start + 2 && turn(hull_out[hull_out.size() - 2], hull_out.back(), pt) <= 0) {
                hull_out.pop_back();
            }
            hull_out.push_back(pt);
        }
        hull_out.pop_back();  // the chains' last points start the other chain
    }
    std::copy(hull_out.begin(), hull_out.end(), p);
    l.pts_n = static_cast<uint32_t>(hull_out.size());
}

// Whether their outlines on screen overlap (more than touch): no separating axis among their edges' normals.
bool SceneBuilder::Impl::overlap(Layer& l, Layer& o) {
    make_convex(l);
    make_convex(o);
    if (l.pts_n < 3 || o.pts_n < 3) return true;  // degenerate: the boxes overlap
    const P2* a = &layer_pts[l.pts];
    const P2* b = &layer_pts[o.pts];
    const auto separated = [](const P2* p, uint32_t np, const P2* q, uint32_t nq) {
        for (uint32_t i = 0, j = np - 1; i < np; j = i++) {
            const P2 e0 = p[j], e1 = p[i];
            const float ax = e0.y - e1.y, ay = e1.x - e0.x;  // the edge's normal
            const float eps = 0.02f * (std::fabs(ax) + std::fabs(ay));
            float pmin = 1e30f, pmax = -1e30f, qmin = 1e30f, qmax = -1e30f;
            for (uint32_t k = 0; k < np; ++k) {
                const float d = ax * p[k].x + ay * p[k].y;
                pmin = std::min(pmin, d);
                pmax = std::max(pmax, d);
            }
            for (uint32_t k = 0; k < nq; ++k) {
                const float d = ax * q[k].x + ay * q[k].y;
                qmin = std::min(qmin, d);
                qmax = std::max(qmax, d);
            }
            if (pmax <= qmin + eps || qmax <= pmin + eps) return true;
        }
        return false;
    };
    return !separated(a, l.pts_n, b, o.pts_n) && !separated(b, o.pts_n, a, l.pts_n);
}

// Whether (x, y) is inside l's outline on screen, clear of its edges.
bool SceneBuilder::Impl::inside(Layer& l, float x, float y) {
    make_convex(l);
    if (l.pts_n < 3) return false;
    const P2* h = &layer_pts[l.pts];
    bool pos = false, neg = false;
    for (uint32_t i = 0, j = l.pts_n - 1; i < l.pts_n; j = i++) {
        const P2 a = h[j], b = h[i];
        const float ex = b.x - a.x, ey = b.y - a.y;
        const float t = ex * (y - a.y) - ey * (x - a.x), eps = 0.05f * (std::fabs(ex) + std::fabs(ey));
        if (std::fabs(t) <= eps) return false;
        (t > 0 ? pos : neg) = true;
    }
    return !(pos && neg);
}

// The group's primitives whose boxes overlap l's and which could be coplanar with it by their orientation (a
// face: a parallel face, or a line lying in its plane; a line: a face whose plane it lies in, or a parallel
// line), in drawing order, into `candidates`.
void SceneBuilder::Impl::find_candidates(const Layer& l) {
    candidates.clear();
    const size_t n = scan.n;
    const float up = dot(l.n, up_cam);
    const bool flat = l.kind == 1 ? std::fabs(up) > 0.999f : std::fabs(up) < 0.02f;
    // A flat primitive of an object looks in the ground layer too, if it is at a height something lies at there.
    const float height = dot(l.p, up_cam), tol = tolerance(l.p.z);
    const bool ground = flat && height >= flat_lo - tol && height <= flat_hi + tol;
    const float* bx0 = scan.x0.data();
    const float* by0 = scan.y0.data();
    const float* bx1 = scan.x1.data();
    const float* by1 = scan.y1.data();
    const float* nx = scan.nx.data();
    const float* ny = scan.ny.data();
    const float* nz = scan.nz.data();
    const float* ln = scan.len.data();
    const float* lt = scan.tol2.data();
    const float* line = scan.line.data();
    const bool face = l.kind == 1;
    size_t i = ground ? 0 : std::min(own_from, n);
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
    const __m128 X0 = _mm_set1_ps(l.x0), Y0 = _mm_set1_ps(l.y0), X1 = _mm_set1_ps(l.x1), Y1 = _mm_set1_ps(l.y1);
    const __m128 NX = _mm_set1_ps(l.n.x), NY = _mm_set1_ps(l.n.y), NZ = _mm_set1_ps(l.n.z);
    const __m128 LEN = _mm_set1_ps(l.len), TOL2 = _mm_set1_ps(l.tol2), PARALLEL = _mm_set1_ps(0.999f);
    const __m128 HALF = _mm_set1_ps(0.5f), SIGN = _mm_set1_ps(-0.0f);
    for (; i + 4 <= n; i += 4) {
        __m128 m = _mm_and_ps(_mm_cmplt_ps(_mm_loadu_ps(bx0 + i), X1), _mm_cmplt_ps(X0, _mm_loadu_ps(bx1 + i)));
        m = _mm_and_ps(m, _mm_and_ps(_mm_cmplt_ps(_mm_loadu_ps(by0 + i), Y1), _mm_cmplt_ps(Y0, _mm_loadu_ps(by1 + i))));
        if (_mm_movemask_ps(m) == 0) continue;
        const __m128 dot = _mm_add_ps(
            _mm_add_ps(_mm_mul_ps(NX, _mm_loadu_ps(nx + i)), _mm_mul_ps(NY, _mm_loadu_ps(ny + i))),
            _mm_mul_ps(NZ, _mm_loadu_ps(nz + i)));
        const __m128 d = _mm_andnot_ps(SIGN, dot);
        const __m128 is_line = _mm_cmpgt_ps(_mm_loadu_ps(line + i), HALF);
        const __m128 parallel = _mm_cmpgt_ps(d, PARALLEL);
        // Faces: a parallel face, or a line whose direction lies in the plane (over its length, within the
        // tolerance). Lines: a face whose plane the line lies in, or a parallel line.
        const __m128 in_plane = face ? _mm_cmple_ps(_mm_mul_ps(d, _mm_loadu_ps(ln + i)), _mm_loadu_ps(lt + i))
                                     : _mm_cmple_ps(_mm_mul_ps(d, LEN), TOL2);
        const __m128 aligned = face ? _mm_or_ps(_mm_and_ps(is_line, in_plane), _mm_andnot_ps(is_line, parallel))
                                   : _mm_or_ps(_mm_and_ps(is_line, parallel), _mm_andnot_ps(is_line, in_plane));
        const int bits = _mm_movemask_ps(_mm_and_ps(m, aligned));
        for (int k = 0; k < 4; ++k) {
            if (bits >> k & 1) candidates.push_back(static_cast<uint32_t>(i + static_cast<size_t>(k)));
        }
    }
#endif
    for (; i < n; ++i) {
        if (!(bx0[i] < l.x1 && l.x0 < bx1[i] && by0[i] < l.y1 && l.y0 < by1[i])) continue;
        const float d = std::fabs(l.n.x * nx[i] + l.n.y * ny[i] + l.n.z * nz[i]);
        const bool is_line = line[i] > 0.5f;
        const bool aligned =
            face ? (is_line ? d * ln[i] <= lt[i] : d > 0.999f) : (is_line ? d > 0.999f : d * l.len <= l.tol2);
        if (aligned) candidates.push_back(static_cast<uint32_t>(i));
    }
}

// The primitive's layer (see Layer): one above the highest earlier primitive of the group it lies on.
void SceneBuilder::Impl::assign_layer(Layer& l, const float* sx, const float* sy, int ns) {
    l.x0 = l.y0 = 1e30f;
    l.x1 = l.y1 = -1e30f;
    for (int k = 0; k < ns; ++k) {
        l.x0 = std::min(l.x0, sx[k]);
        l.y0 = std::min(l.y0, sy[k]);
        l.x1 = std::max(l.x1, sx[k]);
        l.y1 = std::max(l.y1, sy[k]);
    }
    if (l.kind == 2) {
        l.mx = 0.25f * (sx[0] + sx[1] + sx[2] + sx[3]);  // a line's centre (its quad's)
        l.my = 0.25f * (sy[0] + sy[1] + sy[2] + sy[3]);
    }
    // The outline is kept for faces only (a line's box and centre do).
    l.pts = static_cast<uint32_t>(layer_pts.size());
    l.pts_n = 0;
    if (l.kind == 1) {
        l.pts_n = static_cast<uint32_t>(ns);
        for (int k = 0; k < ns; ++k) layer_pts.push_back({sx[k], sy[k]});
    }

    if (l.kind == 0) {
        candidates.clear();  // degenerate: on nothing
    } else {
        find_candidates(l);
    }
    int s = 0;
    for (size_t k = candidates.size(); k-- > 0 && s <= group_top;) {
        Layer& o = layers[candidates[k]];
        if (o.s < s || !lies_on(l, o)) continue;  // (can't raise it)
        // A line on a face or along a line: their boxes overlap, that will do. A face over a line it contains:
        // only if the line runs inside it (not along its edge: the outline of a neighbouring face). Faces: if
        // their outlines overlap.
        if (l.kind == 2 || (o.kind == 2 ? inside(l, o.mx, o.my) : overlap(l, o))) s = o.s + 1;
    }
    // Lines and markings are always drawn on something (a face's outline, the road): at least one layer up,
    // also over a coplanar surface of another group (a neighbouring cell's road under a kerb line).
    if (l.kind == 2 || stripe) s = std::max(s, 1);
    s = std::min(s, kMaxLayer);
    l.s = static_cast<uint16_t>(s);
    layers.push_back(l);
    scan.push(l);
    group_top = std::max(group_top, s);
    const float up = std::fabs(dot(l.n, up_cam));
    if ((l.kind == 1 && up > 0.999f) || (l.kind == 2 && up < 0.02f)) {
        const float height = dot(l.p, up_cam);
        flat_lo = std::min(flat_lo, height);
        flat_hi = std::max(flat_hi, height);
    }
    bias = 1 + static_cast<float>(s) * kDepthStep;
    out->stats.max_layer = std::max(out->stats.max_layer, s);
    ++out->stats.layers[static_cast<size_t>(s)];  // (s <= kMaxLayer)
}

void SceneBuilder::Impl::emit_polygon(const P3* pts, int n, const uint16_t* tri, uint32_t ntri, const SceneColour& c,
                                      float a) {
    bool all_front = true;
    for (int k = 0; k < n; ++k) all_front &= pts[k].z >= kNear;
    if (all_front) {
        float sx[256], sy[256];
        int outside = 0xF;
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (int k = 0; k < n; ++k) {
            sx[k] = px(pts[k]);
            sy[k] = py(pts[k]);
            outside &= (sx[k] < vx0 ? 1 : 0) | (sx[k] > vx1 ? 2 : 0) | (sy[k] < vy0 ? 4 : 0) | (sy[k] > vy1 ? 8 : 0);
            x0 = std::min(x0, sx[k]);
            y0 = std::min(y0, sy[k]);
            x1 = std::max(x1, sx[k]);
            y1 = std::max(y1, sy[k]);
        }
        if (outside) return;  // entirely beyond one side of the view
        layer_polygon(n, [&](int k) { return V3{pts[k].x, pts[k].y, pts[k].z}; }, sx, sy, n);
        SceneVertex* v = grow_vertices(static_cast<size_t>(n));
        const auto base = static_cast<int32_t>(out->vertices.size()) - n;
        for (int k = 0; k < n; ++k) v[k] = {sx[k], sy[k], c.r, c.g, c.b, a, depth(pts[k].z)};
        int32_t* ix = grow_indices(ntri * 3);
        for (uint32_t t = 0; t < ntri * 3; ++t) ix[t] = base + tri[t];
        out->stats.triangles += static_cast<int>(ntri);
        return;
    }
    // Crossing the near plane: each triangle clipped (to a triangle or a quad).
    clip_points.clear();
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (uint32_t t = 0; t < ntri; ++t) {
        const P3 corner[3] = {pts[tri[3 * t]], pts[tri[3 * t + 1]], pts[tri[3 * t + 2]]};
        P3 cl[4];
        const int m = clip(corner, 3, cl);
        if (m < 3) continue;
        clip_points.push_back(static_cast<float>(m));
        for (int k = 0; k < m; ++k) {
            const float x = px(cl[k]), y = py(cl[k]);
            clip_points.insert(clip_points.end(), {x, y, cl[k].z});
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
    }
    if (clip_points.empty()) return;
    const float bx[4] = {x0, x1, x1, x0}, by[4] = {y0, y0, y1, y1};  // (its box will do)
    layer_polygon(n, [&](int k) { return V3{pts[k].x, pts[k].y, pts[k].z}; }, bx, by, 4);
    for (size_t i = 0; i < clip_points.size();) {
        const int m = static_cast<int>(clip_points[i++]);
        const auto base = static_cast<int32_t>(out->vertices.size());
        for (int k = 0; k < m; ++k, i += 3) vertex(clip_points[i], clip_points[i + 1], clip_points[i + 2], c, a);
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
        float sx[256], sy[256];
        for (int k = 0; k < n; ++k) {
            sx[k] = sv[idx[k]].x;
            sy[k] = sv[idx[k]].y;
        }
        layer_polygon(n, [&](int k) { return cv[idx[k]]; }, sx, sy, n);
        SceneVertex* v = grow_vertices(static_cast<size_t>(n));
        const auto base = static_cast<int32_t>(out->vertices.size()) - n;
        for (int k = 0; k < n; ++k) {
            const P2 q = sv[idx[k]];
            v[k] = {q.x, q.y, c.r, c.g, c.b, a, depth(cv[idx[k]].z)};
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

void SceneBuilder::Impl::emit_quad_2d(V3 ea, V3 eb, float x0, float y0, float x1, float y1, const SceneColour& c) {
    // A segment as a quad line_half_width() wide at each end (at the ends' depths), ending at its ends
    // (butt caps).
    const float z0 = ea.z, z1 = eb.z;
    if ((x0 < vx0 && x1 < vx0) || (x0 > vx1 && x1 > vx1) || (y0 < vy0 && y1 < vy0) || (y0 > vy1 && y1 > vy1)) return;
    const float pw = opt->pixel_w, ph = opt->pixel_h;
    float dx = (x1 - x0) * pw, dy = (y1 - y0) * ph;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < opt->min_line_length && !line_piece) {
        ++out->stats.lines_dropped;
        return;
    }
    const float h0 = line_half_width(z0), h1 = line_half_width(z1);
    // A very short piece still shows as a dot as long as the line is wide.
    const float h = std::max(h0, h1);
    if (len < 2 * h) {
        const float ex = len > 1e-4f ? dx / len : 1, ey = len > 1e-4f ? dy / len : 0;
        const float mx = 0.5f * (x0 + x1), my = 0.5f * (y0 + y1);
        x0 = mx - ex * h / pw;
        y0 = my - ey * h / ph;
        x1 = mx + ex * h / pw;
        y1 = my + ey * h / ph;
        dx = 2 * h * ex;
        dy = 2 * h * ey;
        len = 2 * h;
    }
    const float ux = -dy / len, uy = dx / len;  // across, unit, output pixels
    const float ax = ux * h0 / pw, ay = uy * h0 / ph, bx = ux * h1 / pw, by = uy * h1 / ph;
    const float qx[4] = {x0 + ax, x0 - ax, x1 - bx, x1 + bx}, qy[4] = {y0 + ay, y0 - ay, y1 - by, y1 + by};
    layer_line(ea, eb, qx, qy);
    const float d0 = depth(z0), d1 = depth(z1);
    SceneVertex* v = grow_vertices(4);
    const auto base = static_cast<int32_t>(out->vertices.size()) - 4;
    v[0] = {x0 + ax, y0 + ay, c.r, c.g, c.b, fade, d0};
    v[1] = {x0 - ax, y0 - ay, c.r, c.g, c.b, fade, d0};
    v[2] = {x1 - bx, y1 - by, c.r, c.g, c.b, fade, d1};
    v[3] = {x1 + bx, y1 + by, c.r, c.g, c.b, fade, d1};
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

// A marking painted on the road: the segment widened to a stripe in the horizontal plane through it,
// clipped at the near plane and projected like any polygon, so it lies flat and foreshortens.
int SceneBuilder::Impl::emit_ribbon(V3 a, V3 b, const SceneColour& c) {
    const V3 d = b - a;
    const float len = std::sqrt(dot(d, d));
    if (len < 1e-3f || std::fabs(dot(d, up_cam)) > 0.05f * len + 0.5f) return 0;  // not horizontal
    // At the object's ground level (rails, fence tops and other edges higher up are lines).
    if (std::fabs(static_cast<float>(cam_z) + dot(a, up_cam) - ribbon_base_z) > 4) return 0;
    // Beyond this depth even a stripe facing the camera is thinner than an output pixel.
    const float far_z = opt->marking_width * kFocal * std::max(opt->pixel_w, opt->pixel_h);
    if (a.z > far_z && b.z > far_z) return 2;
    V3 side = cross(d, up_cam);
    const float sl = std::sqrt(dot(side, side));
    if (sl < 1e-6f) return 0;
    const float k = 0.5f * opt->marking_width / sl;
    side = {side.x * k, side.y * k, side.z * k};
    // Its thickness on screen at each end, in output pixels: the projection's derivative across it,
    // square to the stripe's own direction on screen, since a stripe running into the distance is far
    // thinner than it is wide (near the camera plane: wide enough).
    const auto width = [&](const V3& p) {
        if (p.z < 2 * kNear) return 1e9f;
        const float iz = kFocal / (p.z * p.z);
        const auto screen = [&](const V3& v, float& x, float& y) {
            x = (v.x * p.z - p.x * v.z) * iz * opt->pixel_w;
            y = (v.y * p.z - p.y * v.z) * iz * opt->pixel_h;
        };
        float wx = 0, wy = 0, dx = 0, dy = 0;
        screen(side, wx, wy);
        screen(d, dx, dy);
        const float dl = std::sqrt(dx * dx + dy * dy);
        if (dl < 1e-6f) return 2 * std::sqrt(wx * wx + wy * wy);  // seen end on
        return 2 * std::fabs(wx * dy - wy * dx) / dl;
    };
    const float wa = width(a), wb = width(b);
    if (std::max(wa, wb) < 1.0f) return 2;
    const V3 q[4] = {a - side, a + side, b + side, b - side};
    const P3 pts[4] = {{q[0].x, q[0].y, q[0].z}, {q[1].x, q[1].y, q[1].z}, {q[2].x, q[2].y, q[2].z}, {q[3].x, q[3].y, q[3].z}};
    static constexpr uint16_t kQuad[6] = {0, 1, 2, 0, 2, 3};
    stripe = true;
    emit_polygon(pts, 4, kQuad, 2, c, 1);
    stripe = false;
    return std::min(wa, wb) >= 1.0f ? 1 : 2;
}

void SceneBuilder::Impl::emit_line(P3 a, P3 b, const SceneColour& c) {
    if (a.z < kNear && b.z < kNear) return;
    if (a.z < kNear) {
        a = crossing(a, b);
    } else if (b.z < kNear) {
        b = crossing(b, a);
    }
    emit_quad_2d({a.x, a.y, a.z}, {b.x, b.y, b.z}, px(a), py(a), px(b), py(b), c);
}

// --- Objects ------------------------------------------------------------------------------------------------

void SceneBuilder::Impl::draw_model(int model, const M3& rotation, V3 origin_cam, bool outline) {
    if (model < 0 || model >= kModelCount || !world.models[static_cast<size_t>(model)].present) return;
    const Model& m = world.models[static_cast<size_t>(model)];
    const bool saved_ribbons = ribbons;
    ribbons = false;
    // B9F6: beyond sort key 800h the original draws a generic box (original window mode only).
    const bool far = (opt->original_window && lod_key >= 0x800) || small;
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
        const float alpha = ((f.flags & 0x8000) ? 0.5f : 1.0f) * fade;
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
    ribbons = saved_ribbons;
}

void SceneBuilder::Impl::draw_variant(int routine, const Variant& v, int32_t x, int32_t y, int32_t z,
                                      const std::vector<std::array<int16_t, 3>>* angles) {
    const RoutineData& rd = routines[static_cast<size_t>(routine)];
    const VariantData& vd = rd.variants[static_cast<size_t>(&v - world.routines[static_cast<size_t>(routine)].variants.data())];
    ++out->stats.objects;
    transform_variant(routine, v, x, y, z, angles);
    const V3 entry = to_camera(x, y, z);
    ribbon_base_z = static_cast<float>(z);
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

// Every part of a variant into camera space (cam, part_at) and projected (scr).
void SceneBuilder::Impl::transform_variant(int routine, const Variant& v, int32_t x, int32_t y, int32_t z,
                                           const std::vector<std::array<int16_t, 3>>* angles) {
    const RoutineData& rd = routines[static_cast<size_t>(routine)];
    const VariantData& vd = rd.variants[static_cast<size_t>(&v - world.routines[static_cast<size_t>(routine)].variants.data())];
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
        if (r.camera_dependent && !opt->original_window) {
            v = positional(e.routine, r, x, y, z, sortable ? key : 0);
        }
        if (!v) {
            DrawState st = state;
            st.finish_flag = finish;
            v = r.select(st);
        }
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

// The variant of a camera-dependent routine. The bridges' pieces and the Yerba Buena tunnel pick their
// primitives, and the order they're drawn in, by which side of the piece the camera is on (above or
// below a deck, on it or beside it); all of these routines (the street markings too) also drop detail
// with distance and with the axis the camera faces (DS:35C3). The routine itself decides, run on the
// tracer with the camera moved in to within 480 units of the object on each axis (in 8-unit steps), on the same side,
// once facing each axis: the side tests come out as the original would have them, the distance tests as
// up close, and the more detailed of the two facings is taken. Matched to the extracted variants by the
// trace's signature; nullptr if none matches (the caller then takes the most detailed). A matched
// variant may be empty: the original draws nothing from that side.
const Variant* SceneBuilder::Impl::positional(int index, const Routine& r, int32_t x, int32_t y, int32_t z,
                                              uint16_t key) {
    // In steps of 8 units: the camera's moves within a step don't change the choice, which is remembered.
    constexpr int kNearBox = 480, kStep = 8;
    // Routines whose choice doesn't depend on the camera's side (the street markings: only distance and
    // facing) are found once, from the eight sides at the near box's corners, and get the most detailed.
    int8_t& sided = side_dependent[static_cast<size_t>(index)];
    if (sided == 0) return nullptr;
    const auto pull = [](double d) {
        return kStep * static_cast<int>(std::round(std::clamp<double>(d, -kNearBox, kNearBox) / kStep));
    };
    const int dx = pull(cam_x - x), dy = pull(cam_y - y), dz = pull(cam_z - z);
    const bool own = std::floor(cam_x / kCellSize) == std::floor(double(x) / kCellSize) &&
                     std::floor(cam_y / kCellSize) == std::floor(double(y) / kCellSize);
    const uint16_t near_key = key ? static_cast<uint16_t>(std::min<int>(key, kNearBox)) : 0;
    Hash64 h;
    h.word(static_cast<uint16_t>(index));
    for (const int32_t v : {x, y, z}) {
        h.word(static_cast<uint16_t>(v));
        h.word(static_cast<uint16_t>(v >> 16));
    }
    for (const int v : {dx, dy, dz}) h.word(static_cast<uint16_t>(v));
    h.byte(own ? 1 : 0);
    h.byte(near_key ? 1 : 0);
    h.byte(static_cast<uint8_t>(state.course | (state.windows ? 8 : 0) | (finish ? 16 : 0)));
    const auto it = positional_cache.find(h.h);
    if (it != positional_cache.end()) return it->second;
    if (sided < 0) {
        DrawState st = state;
        st.finish_flag = finish;
        const Variant* most = r.select(st);
        bool same = true;
        for (int corner = 0; corner < 8 && same; ++corner) {
            same = trace_choice(r, x, y, z, corner & 1 ? kNearBox : -kNearBox, corner & 2 ? kNearBox : -kNearBox,
                                corner & 4 ? kNearBox : -kNearBox, false, near_key) == most;
        }
        sided = same ? 0 : 1;
        if (same) return nullptr;
    }
    const Variant* found = trace_choice(r, x, y, z, dx, dy, dz, own, near_key);
    positional_cache.emplace(h.h, found);
    return found;
}

// The routine run on the tracer for a camera at (dx, dy, dz) from the object, once facing each axis: the
// more detailed of the two variants it drew (nullptr if neither matches an extracted one).
const Variant* SceneBuilder::Impl::trace_choice(const Routine& r, int32_t x, int32_t y, int32_t z, int dx, int dy,
                                                int dz, bool own, uint16_t near_key) {
    load_tracer();
    const auto ox = static_cast<uint16_t>(x & 0x7FFF), oy = static_cast<uint16_t>(y & 0x7FFF);
    const std::array<int16_t, 3> pos16{static_cast<int16_t>(ox), static_cast<int16_t>(oy), static_cast<int16_t>(z)};
    const Variant* found = nullptr;
    for (const uint8_t facing_axis : {uint8_t{0}, uint8_t{1}}) {
        tracer->begin();
        tracer->wr16(addr::kDataSeg, addr::kCamera, static_cast<uint16_t>(ox + dx));
        tracer->wr16(addr::kDataSeg, addr::kCamera + 2, static_cast<uint16_t>(oy + dy));
        tracer->wr16(addr::kDataSeg, addr::kCamera + 4, static_cast<uint16_t>(z + dz));
        tracer->wr16(addr::kDataSeg, addr::kCamera + 6, static_cast<uint16_t>(yaw));
        tracer->wr16(addr::kDataSeg, addr::kCamera + 8, static_cast<uint16_t>(pitch));
        for (int k = 0; k < 3; ++k) {
            tracer->wr16(addr::kDataSeg, static_cast<uint16_t>(addr::kObjPos + 2 * k), static_cast<uint16_t>(pos16[static_cast<size_t>(k)]));
        }
        if (near_key) tracer->wr16(addr::kCodeSeg, addr::kSortKey, near_key);
        tracer->wr8(addr::kDataSeg, addr::kFacing, facing_axis);
        tracer->wr8(addr::kDataSeg, addr::kOwnCell, own ? 0xFF : 0);
        tracer->run(r.address, pos16, trace);
        tracer->end();
        if (!trace.error.empty()) continue;
        const uint64_t sig = trace.signature();
        for (const Variant& v : r.variants) {
            if (v.signature == sig) {
                if (!found || v.primitives > found->primitives) found = &v;
                break;
            }
        }
    }
    return found;
}

// A chunked piece's variant this frame (chosen as draw_entry would: the original's choice for camera-
// dependent routines, else the most detailed), and which of its primitives face the camera.
int SceneBuilder::Impl::choose_piece(const CourseData& cd, uint32_t piece) {
    int16_t& slot = piece_variant[piece];
    if (slot != -2) return slot;
    slot = -1;
    const Piece& p = cd.pieces[piece];
    const Routine& r = world.routines[static_cast<size_t>(p.routine)];
    const Variant* v = r.camera_dependent ? positional(p.routine, r, p.x, p.y, p.z, 0) : nullptr;
    if (!v) {
        DrawState st = state;
        st.finish_flag = finish;
        v = r.select(st);
    }
    if (!v) return slot;
    if (v->sets_finish_flag >= 0) finish = v->sets_finish_flag != 0;
    slot = static_cast<int16_t>(v - r.variants.data());
    ++out->stats.objects;
    transform_variant(p.routine, *v, p.x, p.y, p.z, nullptr);
    uint32_t g = cd.prim_base[piece];
    for (const Part& part : v->parts) {
        for (const Prim& prim : part.prims) prim_visible[g++] = visible(prim.cull) ? 1 : 0;
    }
    return slot;
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
    int (&window_cells)[6] = window_steps;  // the map cells the window's steps drew
    std::fill(std::begin(window_cells), std::end(window_cells), -1);
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
        // The map cell the step draws, and where: the original draws it at its own position (two cells ahead
        // 48 units short: 36D4 / 36F0), the Enhanced view at the cell's (where its scenery is).
        const int gx = std::clamp(row * 16 + bx, 0, kMapCells - 1), gy = std::clamp(col * 16 + ax, 0, kMapCells - 1);
        const int cell = gx * kMapCells + gy;
        int32_t base_x = gx * kCellSize, base_y = gy * kCellSize;
        if (opt->original_window) {
            const auto draw16_x = static_cast<uint16_t>(w.cx + ox), draw16_y = static_cast<uint16_t>(w.dx + oy);
            base_x = static_cast<int32_t>(std::lround(cam_x)) + static_cast<int16_t>(draw16_x - cam16_x);
            base_y = static_cast<int32_t>(std::lround(cam_y)) + static_cast<int16_t>(draw16_y - cam16_y);
        }
        window_cells[si] = cell;
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
            if (!match || (slot >= 3 && placement_of(e, gx, gy) == game::Placement::Deny)) continue;
            seen_entities.push_back(e);
            int32_t x, y;
            int16_t z;
            entity_xyz(e, x, y, z);
            add_vehicle(e, ram.d16(e), cell, base_x + (x & 0x7FF), base_y + (y & 0x7FF), z, true);
            vehicles.back().window = true;
            if (opt->observer && slot >= 3) observe_window(e, cell, base_x + (x & 0x7FF), base_y + (y & 0x7FF), z);
        }
        // List B (34D6): pedestrians, positions already cell-relative.
        const uint16_t lb = ram.d16(static_cast<uint16_t>(kListB + 2 * bt));
        for (int slot = 0; slot < 64; ++slot) {
            const auto at = static_cast<uint16_t>(lb + 4 * slot);
            const uint16_t e = ram.d16(at);
            if (e == 0xFFFF) break;
            const uint16_t ec = ram.d16(static_cast<uint16_t>(at + 2));
            if ((ec & 0x33) != (ci & 0x33) || placement_of(e, gx, gy) == game::Placement::Deny) continue;
            seen_entities.push_back(e);
            int32_t x, y;
            int16_t z;
            entity_xyz(e, x, y, z);
            add_vehicle(e, ram.d16(e), cell, base_x + static_cast<int16_t>(x), base_y + static_cast<int16_t>(y), z, true);
            if (opt->observer) observe_window(e, cell, base_x + static_cast<int16_t>(x), base_y + static_cast<int16_t>(y), z);
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
    // A replica isn't placed in the window's cells: there, the original's own binding (above) has drawn
    // the cell's traffic and pedestrians already.
    std::vector<uint32_t> replicated;  // entity | list << 16: an entity can be in two lists
    if (opt->replicas) prepare_roads();
    const auto place = [&](uint16_t e, int row, int col, uint16_t ec, bool cell_relative, bool replica,
                           const ReplicaChoice* choice) {
        if (row < 0 || col < 0 || row >= rows || col >= cols) return;
        const int gx = row * 16 + (ec >> 4 & 15), gy = col * 16 + (ec & 15);
        if (!in_radius(gx, gy)) return;
        if (replica && std::find(std::begin(window_cells), std::end(window_cells), gx * kMapCells + gy) != std::end(window_cells)) {
            return;
        }
        const game::Placement placement = placement_of(e, gx, gy);
        if (placement == game::Placement::Deny) return;
        int32_t x, y;
        int16_t z;
        entity_xyz(e, x, y, z);
        const int32_t bx = gx * kCellSize, by = gy * kCellSize;
        const int32_t wx = cell_relative ? bx + static_cast<int16_t>(x) : bx + (x & 0x7FF);
        const int32_t wy = cell_relative ? by + static_cast<int16_t>(y) : by + (y & 0x7FF);
        if (choice) {
            ++out->stats.replicas;
            const int k = (ec >> 6 & 3) * 4 + (ec >> 2 & 3);
            const bool keep = placement == game::Placement::Allow || (choice->keep[static_cast<size_t>(row * cols + col)] >> k & 1) != 0;
            if (opt->observer) {
                const V3 v = to_camera(wx, wy, z);
                opt->observer->copy({e, gx * kMapCells + gy, wx, wy, v.x, v.y, v.z, keep, false});
            }
            if (!keep) {
                ++out->stats.replicas_dropped;
                return;
            }
        }
        add_vehicle(e, ram.d16(e), gx * kMapCells + gy, wx, wy, z, false);
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
            if (slot <= 2) {
                if (seen(e)) continue;
                seen_entities.push_back(e);
                if (slot == 0) {
                    place(e, ram.s16(kPlayerRow), ram.s16(kPlayerCol), ec, false, false, nullptr);
                } else if (slot == 1 && opponent_ok) {
                    place(e, opp_tile / 5, opp_tile % 5, ec, false, false, nullptr);
                } else if (slot == 2 && chase) {
                    place(e, chase_tile / 5, chase_tile % 5, ec, false, false, nullptr);
                }
                continue;
            }
            if (slot == 3 && chase) continue;
            if (ram.d16(static_cast<uint16_t>(e + 0x1C)) == 0) continue;
            if (opt->replicas) {
                // Every cell of its pattern (but the window's: place()) laid out along its route as the cells
                // its route was made for (choose_replicas).
                const uint32_t key = e | static_cast<uint32_t>(la) << 16;
                if (std::find(replicated.begin(), replicated.end(), key) != replicated.end()) continue;
                replicated.push_back(key);
                const ReplicaChoice& choice = choose_replicas(e, true, ec, la, lists_a, rows * cols, cols);
                for (int t = 0; t < rows * cols; ++t) {
                    if (lists_a[t] != la) continue;
                    for (int c = 0; c < 256; ++c) {
                        if ((c & 0x33) == (ec & 0x33)) place(e, t / cols, t % cols, static_cast<uint16_t>(c), false, true, &choice);
                    }
                }
            } else {
                if (seen(e)) continue;
                seen_entities.push_back(e);
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
                if (best >= 0) place(e, best / cols, best % cols, ec, false, false, nullptr);
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
                const uint32_t key = e | static_cast<uint32_t>(lb) << 16;
                if (std::find(replicated.begin(), replicated.end(), key) != replicated.end()) continue;
                replicated.push_back(key);
                const ReplicaChoice& choice = choose_replicas(e, false, ec, lb, lists_b, rows * cols, cols);
                for (int t = 0; t < rows * cols; ++t) {
                    if (lists_b[t] != lb) continue;
                    for (int c = 0; c < 256; ++c) {
                        if ((c & 0x33) == (ec & 0x33)) place(e, t / cols, t % cols, static_cast<uint16_t>(c), true, true, &choice);
                    }
                }
            }
        }
    }
}

// The road boxes (see RoadBox), for the course being raced.
void SceneBuilder::Impl::prepare_roads() {
    if (roads_course == state.course && !road_start.empty()) return;
    roads_course = state.course;
    replica_choices.clear();
    road_boxes.clear();
    const auto mix = [](uint64_t h) {
        h ^= h >> 33;
        h *= 0xFF51AFD7ED558CCDull;
        h ^= h >> 33;
        h *= 0xC4CEB9FE1A85EC53ull;
        h ^= h >> 33;
        return h;
    };
    // Per routine: the height of its most detailed variant's flat road face (colour 8) and pavement face
    // (7 or 8), if it has one.
    constexpr int32_t kNone = INT32_MIN;
    std::vector<std::array<int32_t, 2>> surface(world.routines.size(), {kNone, kNone});
    for (size_t r = 0; r < world.routines.size(); ++r) {
        const Routine& rt = world.routines[r];
        if (rt.compound || rt.variants.empty()) continue;
        for (const Part& part : rt.variants.front().parts) {
            if (part.source == Part::Source::Model || part.rotation != Part::Rotation::None) continue;
            for (const Prim& pr : part.prims) {
                if (pr.kind == Prim::Kind::Line || pr.count < 3 || pr.colour.dithered()) continue;
                const int c = pr.colour.base();
                if (c != 7 && c != 8) continue;
                const int32_t z0 = part.verts[part.indices[pr.first]].z + part.origin.z;
                bool flat = true;
                for (uint32_t k = 1; k < pr.count && flat; ++k) flat = part.verts[part.indices[pr.first + k]].z + part.origin.z == z0;
                if (!flat) continue;
                if (c == 8 && surface[r][0] == kNone) surface[r][0] = z0;
                if (surface[r][1] == kNone) surface[r][1] = z0;
            }
        }
    }
    // Each object once: a structure several cells list at one place is one object.
    std::unordered_set<uint64_t> placed;
    const auto add = [&](int routine, int32_t x, int32_t y, int32_t dz, int elevation) {
        if (routine < 0) return;
        const RoutineData& rd = routines[static_cast<size_t>(routine)];
        const auto& sf = surface[static_cast<size_t>(routine)];
        const bool road = sf[0] != kNone && std::abs(sf[0] + dz) <= kGroundLevel;
        const bool paved = sf[1] != kNone && std::abs(sf[1] + dz) <= kGroundLevel;
        if ((!road && !paved) || rd.lo.x > rd.hi.x) return;
        const uint64_t id = mix(static_cast<uint64_t>(routine) << 40 ^ static_cast<uint64_t>(static_cast<uint32_t>(x)) << 20 ^
                                static_cast<uint32_t>(y)) ^ mix(static_cast<uint32_t>(dz) + 0x9E3779B97F4A7C15ull);
        if (!placed.insert(id).second) return;
        RoadBox b;
        b.x0 = static_cast<float>(x) + rd.lo.x;
        b.y0 = static_cast<float>(y) + rd.lo.y;
        b.x1 = static_cast<float>(x) + rd.hi.x;
        b.y1 = static_cast<float>(y) + rd.hi.y;
        b.kind = mix(static_cast<uint64_t>(routine) << 16 ^ static_cast<uint64_t>(elevation & 0xFF) ^ 0x5EED000000ull) | 1;
        b.road = road;
        if (b.x1 > b.x0 && b.y1 > b.y0) road_boxes.push_back(b);
    };
    for (int gx = 0; gx < world.cells_x(); ++gx) {
        for (int gy = 0; gy < world.cells_y(); ++gy) {
            const Cell& c = world.cell(gx, gy);
            for (const Entry& e : types[c.type].list1) add(e.routine, gx * kCellSize + e.dx, gy * kCellSize + e.dy, e.dz, c.elevation);
        }
    }
    if (state.course >= 1 && state.course <= 4) {
        for (const Piece& pc : courses[static_cast<size_t>(state.course)].pieces) {
            const int gx = std::clamp(pc.x / kCellSize, 0, world.cells_x() - 1), gy = std::clamp(pc.y / kCellSize, 0, world.cells_y() - 1);
            const int elevation = world.cell(gx, gy).elevation;
            add(pc.routine, pc.x, pc.y, pc.z - elevation * kElevationStep, elevation);
        }
    }
    // By cell (CSR).
    const size_t cells = static_cast<size_t>(world.cells_x()) * static_cast<size_t>(world.cells_y());
    road_start.assign(cells + 1, 0);
    const auto cover = [&](const RoadBox& b, auto&& f) {
        const int x0 = std::max(0, static_cast<int>(std::floor(b.x0 / kCellSize)));
        const int x1 = std::min(world.cells_x() - 1, static_cast<int>(std::ceil(b.x1 / kCellSize)) - 1);
        const int y0 = std::max(0, static_cast<int>(std::floor(b.y0 / kCellSize)));
        const int y1 = std::min(world.cells_y() - 1, static_cast<int>(std::ceil(b.y1 / kCellSize)) - 1);
        for (int gx = x0; gx <= x1; ++gx) {
            for (int gy = y0; gy <= y1; ++gy) f(static_cast<size_t>(gx * world.cells_y() + gy));
        }
    };
    for (const RoadBox& b : road_boxes) cover(b, [&](size_t c) { ++road_start[c + 1]; });
    for (size_t c = 0; c < cells; ++c) road_start[c + 1] += road_start[c];
    road_index.assign(road_start[cells], 0);
    std::vector<uint32_t> fill(road_start.begin(), road_start.end() - 1);
    for (size_t i = 0; i < road_boxes.size(); ++i) cover(road_boxes[i], [&](size_t c) { road_index[fill[c]++] = static_cast<uint32_t>(i); });
}

// Which cells of an entity's pattern keep a copy. Its way, relative to its cell: for a traffic entity whose
// route is a loop (its cell moves, BE86 / D150, get back to where they started), the roads of every cell of
// the loop; else its position, its target and its next target; for a pedestrian its square (0x11A a side,
// either way round, BB72). Of all the cells of its pattern (its class in
// every big tile with its list), those where most of the way is on road (pavement for a pedestrian) are the
// cells it was made for; the copies are where the road along the way is the commonest of theirs (the roads
// under the way, in order; with a tie, each of the commonest).
const SceneBuilder::Impl::ReplicaChoice& SceneBuilder::Impl::choose_replicas(uint16_t e, bool traffic, uint16_t ec,
                                                                              uint16_t list, const uint16_t* lists, int tiles,
                                                                              int cols) {
    samples.clear();
    const int32_t x = traffic ? (ram.d16(static_cast<uint16_t>(e + 2)) & 0x7FF) : ram.s16(static_cast<uint16_t>(e + 2));
    const int32_t y = traffic ? (ram.d16(static_cast<uint16_t>(e + 4)) & 0x7FF) : ram.s16(static_cast<uint16_t>(e + 4));
    uint64_t state_hash = (static_cast<uint64_t>(ec & 0x33) << 16 ^ list ^ static_cast<uint64_t>(roads_course) << 24 ^
                           (traffic ? 0 : 1ull << 30)) * 0x9E3779B97F4A7C15ull;
    for (int t = 0; t < tiles; ++t) state_hash ^= lists[t] == list ? 1ull << (32 + t) : 0;
    bool positional = true;  // the decision depends on where in its cell it is
    if (traffic) {
        const uint16_t path = ram.d16(static_cast<uint16_t>(e + 0x1A));
        const int32_t tx = ram.s16(path), ty = ram.s16(static_cast<uint16_t>(path + 2));
        const int32_t nx = ram.s16(static_cast<uint16_t>(path + 8)), ny = ram.s16(static_cast<uint16_t>(path + 10));
        // The route's moves from where it is in it, once round (if they get back to where they started).
        const uint16_t from = ram.d16(static_cast<uint16_t>(e + 0x18));
        std::array<std::array<int, 2>, 16> moves{};
        int count = 0, sum_x = 0, sum_y = 0;
        for (uint16_t route = from; count < 16; ++count) {
            if (ram.d16(route) == 0xFFFF) route = ram.d16(static_cast<uint16_t>(route + 2));
            if (count > 0 && route == from) break;
            moves[static_cast<size_t>(count)] = {ram.s16(route) / kCellSize, ram.s16(static_cast<uint16_t>(route + 2)) / kCellSize};
            sum_x += moves[static_cast<size_t>(count)][0];
            sum_y += moves[static_cast<size_t>(count)][1];
            route = static_cast<uint16_t>(route + 4);
        }
        // A loop: its cells, relative to the entity's. The moves start from its cell or from its next target's
        // (by where it is in its turn): the one whose loop passes through both.
        const int next_cx = nx >= 0 ? nx / kCellSize : -1 - (-nx - 1) / kCellSize;
        const int next_cy = ny >= 0 ? ny / kCellSize : -1 - (-ny - 1) / kCellSize;
        std::array<std::array<int, 2>, 16> cells{};
        bool loop = false;
        if (count < 16 && sum_x == 0 && sum_y == 0) {
            for (const auto& anchor : {std::array<int, 2>{0, 0}, std::array<int, 2>{next_cx, next_cy}}) {
                bool own = false, next = false;
                int at_x = anchor[0], at_y = anchor[1];
                for (int k = 0; k < count; ++k) {
                    cells[static_cast<size_t>(k)] = {at_x, at_y};
                    own |= at_x == 0 && at_y == 0;
                    next |= at_x == next_cx && at_y == next_cy;
                    at_x += moves[static_cast<size_t>(k)][0];
                    at_y += moves[static_cast<size_t>(k)][1];
                }
                if (own && next) {
                    loop = true;
                    break;
                }
            }
        }
        if (loop) {
            // The roads of the loop's cells: along both edges of each (x 1792..2048, y 0..256: notes 04 section 7),
            // at the middle of each direction's lanes, every 256 units. The same wherever on the loop it is.
            positional = false;
            for (int k = 0; k < count; ++k) {
                const int32_t ox = cells[static_cast<size_t>(k)][0] * kCellSize, oy = cells[static_cast<size_t>(k)][1] * kCellSize;
                state_hash = (state_hash ^ static_cast<uint64_t>(static_cast<uint32_t>(ox) ^ static_cast<uint32_t>(oy) * 31u)) * 0x100000001B3ull;
                for (int32_t lane : {64, 192}) {
                    for (int32_t a = 128; a < kCellSize; a += 256) {
                        samples.push_back({ox + a, oy + lane});
                        samples.push_back({ox + kCellSize - 256 + lane, oy + a});
                    }
                }
            }
        } else {
            // Else (the bridges' routes, straight on): from where it is past its next target.
            const auto step_to = [&](int32_t ax, int32_t ay, int32_t bx, int32_t by) {
                const int32_t dx = bx - ax, dy = by - ay;
                const int n = std::max(1, static_cast<int>(std::max(std::abs(dx), std::abs(dy)) / 128));  // a point every 128 units
                for (int k = 1; k <= n; ++k) samples.push_back({ax + dx * k / n, ay + dy * k / n});
            };
            samples.push_back({x, y});
            step_to(x, y, tx, ty);
            step_to(tx, ty, nx, ny);
            state_hash ^= (static_cast<uint64_t>(static_cast<uint16_t>(nx)) << 16 ^ static_cast<uint16_t>(ny)) * 0xC2B2AE3D27D4EB4Full;
        }
    } else {
        constexpr int32_t kSide = 0x11A;
        for (int i = -2; i <= 2; ++i) {
            for (int j = -2; j <= 2; ++j) samples.push_back({x + i * kSide / 2, y + j * kSide / 2});
        }
    }
    // The decision stands while its way is the same (and, unless that is a loop, while it stays in the same
    // square of 256 units).
    if (positional) {
        state_hash ^= (static_cast<uint64_t>(static_cast<uint32_t>(x >> 8)) << 8 ^ static_cast<uint32_t>(y >> 8)) * 0xD6E8FEB86659FD93ull;
    }
    if (replica_choices.size() > 4096) replica_choices.clear();
    ReplicaChoice& choice = replica_choices[e | static_cast<uint32_t>(list) << 16];
    if (choice.state == state_hash) return choice;
    choice.state = state_hash;
    choice.keep.fill(0);
    // Each cell of the pattern: what lies under the way, and how much of it is road.
    pattern.clear();
    int best = 1;
    for (int t = 0; t < tiles; ++t) {
        if (lists[t] != list) continue;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                const int gx = (t / cols) * 16 + (ec >> 4 & 3) + 4 * i, gy = (t % cols) * 16 + (ec & 3) + 4 * j;
                const int32_t cx0 = gx * kCellSize, cy0 = gy * kCellSize;
                PatternCell pc;
                pc.tile = t;
                pc.bit = i * 4 + j;
                for (const auto& q : samples) {
                    const uint64_t k = road_at(cx0 + q[0], cy0 + q[1], traffic);
                    pc.signature = (pc.signature ^ k) * 0x100000001B3ull + 0x9E3779B97F4A7C15ull;
                    pc.score += k != 0 ? 1 : 0;
                }
                best = std::max(best, pc.score);
                pattern.push_back(pc);
            }
        }
    }
    // The commonest roads of those with the most road (every one as common; no copy where none of the way is
    // on road).
    std::sort(pattern.begin(), pattern.end(), [](const PatternCell& a, const PatternCell& b) {
        return a.score != b.score ? a.score > b.score : a.signature < b.signature;
    });
    size_t end = 0, most = 0;
    while (end < pattern.size() && pattern[end].score == best) ++end;
    for (size_t k = 0; k < end;) {
        size_t n = k;
        while (n < end && pattern[n].signature == pattern[k].signature) ++n;
        most = std::max(most, n - k);
        k = n;
    }
    for (size_t k = 0; k < end;) {
        size_t n = k;
        while (n < end && pattern[n].signature == pattern[k].signature) ++n;
        if (n - k == most) {
            for (size_t m = k; m < n; ++m) {
                auto& keep = choice.keep[static_cast<size_t>(pattern[m].tile)];
                keep = static_cast<uint16_t>(keep | 1u << pattern[m].bit);
            }
        }
        k = n;
    }
    return choice;
}

// Runs each vehicle's own draw routine on the scratch copy (its model, angles and height over the
// ground, the player only in external views, ...). A replica reuses its entity's first result.
void SceneBuilder::Impl::trace_vehicles() {
    vehicle_models.clear();
    vehicle_model_range.clear();
    if (vehicles.empty()) return;
    load_tracer();
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
    bool drawn = false;
    const float scale = std::max(opt->pixel_w, opt->pixel_h);  // output pixels per race pixel
    for (uint32_t k = first; k < last; ++k) {
        const ModelDraw& md = vehicle_models[k];
        const float radius = meshes[static_cast<size_t>(md.model) % kModelCount].radius;
        const V3 c = to_camera(v.x + md.offset.x, v.y + md.offset.y, v.z + md.offset.z);
        if (!opt->original_window) {
            if (!sphere_visible(c, radius + 8)) continue;
            // A speck (narrower on screen than an output pixel): left out; it grows in from there.
            if (c.z > 0 && 2 * radius * kFocal * scale < c.z) continue;
        }
        cam.clear();
        // Small (a few output pixels across): the original's far box (B9F6), by its true depth only.
        small = !opt->original_window && c.z > 0 && 2 * radius * kFocal * scale < kSmallModel * c.z;
        draw_model(md.model, md.rotation, c, md.outline);
        small = false;
        drawn = true;
    }
    if (drawn) ++out->stats.vehicles;
}

// --- Cells --------------------------------------------------------------------------------------------------

void SceneBuilder::Impl::draw_ground() {
    backdrop = true;
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
    backdrop = false;
}

void SceneBuilder::Impl::draw_cell_enhanced(int cell) {
    const int gx = cell / kMapCells, gy = cell % kMapCells;
    const Cell& c = world.cell(gx, gy);
    const TypeData& td = types[c.type];
    const int32_t ox = gx * kCellSize, oy = gy * kCellSize, oz = c.elevation * kElevationStep;
    ++out->stats.cells;
    begin_group();
    const std::array<int16_t, 3> no16{};
    ribbons = !opt->original_window;  // the ground layer and the bridges: markings lie on the road
    for (const Entry& e : td.list1) {
        if (e.routine < 0 || world.routines[static_cast<size_t>(e.routine)].compound) continue;
        draw_entry(e, ox + e.dx, oy + e.dy, oz + e.dz, no16, 0, false, false);
    }
    // Compound pieces lying in this cell (whole), and the parts of chunked pieces that do, in the
    // compound's order.
    const CourseData& cd = courses[static_cast<size_t>(state.course)];
    uint32_t wi = cd.start[static_cast<size_t>(cell)];
    const uint32_t wend = cd.start[static_cast<size_t>(cell) + 1];
    uint32_t ci = cd.chunk_start[static_cast<size_t>(cell)];
    const uint32_t cend = cd.chunk_start[static_cast<size_t>(cell) + 1];
    static constexpr uint16_t kFan[21] = {0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5, 0, 5, 6, 0, 6, 7, 0, 7, 8};
    while (wi < wend || ci < cend) {
        if (wi < wend && cd.chunked[wi]) {
            ++wi;
            continue;
        }
        if (wi < wend && (ci >= cend || cd.pieces[wi].seq <= cd.pieces[cd.chunks[ci].piece].seq)) {
            const Piece& p = cd.pieces[wi++];
            draw_entry(Entry{p.routine, 0, 0, 0}, p.x, p.y, p.z, no16, 0, false, false);
            continue;
        }
        const Chunk& ch = cd.chunks[ci++];
        if (choose_piece(cd, ch.piece) != ch.variant || !prim_visible[cd.prim_base[ch.piece] + ch.prim]) continue;
        ribbon_base_z = static_cast<float>(cd.pieces[ch.piece].z);
        P3 pts[16];
        const int n = std::min<int>(ch.count, 9);
        for (int k = 0; k < n; ++k) {
            const V3& w = cd.chunk_pts[ch.first + static_cast<uint32_t>(k)];
            const V3 q = to_camera(w.x, w.y, w.z);
            pts[k] = {q.x, q.y, q.z};
        }
        if (n == 2) {
            const V3 a{pts[0].x, pts[0].y, pts[0].z}, b{pts[1].x, pts[1].y, pts[1].z};
            const int marking = ribbons ? emit_ribbon(a, b, ega_colour(ch.colour.base())) : 0;
            if (marking == 1) continue;
            line_piece = true;
            thin_line = marking == 2;
            emit_line(pts[0], pts[1], ega_colour(ch.colour.base()));
            line_piece = false;
            thin_line = false;
        } else {
            emit_polygon(pts, n, kFan, static_cast<uint32_t>(n - 2), scene_colour(ch.colour), 1);
        }
    }
    ribbons = false;
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
        const V3 p = to_camera(vh.x, vh.y, vh.z);
        // Only the window's (far_vehicles off): drawn where the original draws them (4686's culls), so that,
        // without a depth buffer, nothing shows that the original wouldn't.
        if (!opt->far_vehicles && (p.z < -0x80 || p.z >= 0x1400 || std::fabs(p.x) >= p.z + 0x80)) continue;
        Sortable s{};
        s.kind = 1;
        s.index = v;
        s.key = p.z < 0 ? float(0x400) : p.z;
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
    // Each one's layers: on the ground layer's primitives, and on its own; not on another sortable's.
    const GroupMark ground_layer = group_mark();
    for (const Sortable& s : sortables) {
        group_reset(ground_layer);
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
        // The mirror's pass (DS:18 = FF) leaves out the two far cells (30F9..3123).
        if (mirror && (si == 0 || si == 3)) continue;
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
        begin_group();
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
        const int cell =
            std::clamp(row * 16 + bx, 0, kMapCells - 1) * kMapCells + std::clamp(col * 16 + ax, 0, kMapCells - 1);
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

// --- Mirror and freeway -------------------------------------------------------------------------------------

// The mirror's own sky (fill_sky_ground 5A5B fills its viewport; the ground quads then cover what lies
// below the horizon).
void SceneBuilder::Impl::draw_sky() {
    const SceneColour c = ega_colour(kSkyColour);
    SceneVertex* v = grow_vertices(4);
    const auto base = static_cast<int32_t>(out->vertices.size()) - 4;
    v[0] = {left, top, c.r, c.g, c.b, 1};
    v[1] = {right + 1, top, c.r, c.g, c.b, 1};
    v[2] = {right + 1, bottom + 1, c.r, c.g, c.b, 1};
    v[3] = {left, bottom + 1, c.r, c.g, c.b, 1};
    int32_t* ix = grow_indices(6);
    const int32_t quad[6] = {0, 1, 2, 0, 2, 3};
    for (int k = 0; k < 6; ++k) ix[k] = base + quad[k];
    out->stats.triangles += 2;
}

// A ground plane under the camera out to the horizon (highway mode: the freeway has no city around it).
void SceneBuilder::Impl::draw_plane(int colour) {
    constexpr double kFar = 4.0e6;
    const V3 c[4] = {to_camera(cam_x - kFar, cam_y - kFar, 0), to_camera(cam_x + kFar, cam_y - kFar, 0),
                     to_camera(cam_x + kFar, cam_y + kFar, 0), to_camera(cam_x - kFar, cam_y + kFar, 0)};
    const P3 pts[4] = {{c[0].x, c[0].y, c[0].z}, {c[1].x, c[1].y, c[1].z}, {c[2].x, c[2].y, c[2].z}, {c[3].x, c[3].y, c[3].z}};
    static constexpr uint16_t kQuad[6] = {0, 1, 2, 0, 2, 3};
    backdrop = true;
    emit_polygon(pts, 4, kQuad, 2, ega_colour(colour), 1);
    backdrop = false;
}

// The slice types' geometry from the game's records (7A72[type]: {w vertices, w polygons, w block,
// w lines, w heading change}; block + 6: the vertices; polygons: {w list, b colour}..FFFF with lists
// {w n, (n + 1) w index*4}..FFFF (fill_poly_list B5B3); lines: {w list, b colour}..FFFF with lists
// {b, b n, n x {b index*4, b index*4}} (draw_line_list 405E)).
void SceneBuilder::Impl::read_slice_types(uint16_t table, std::array<SliceType, 16>& types_out) {
    for (int t = 0; t < static_cast<int>(types_out.size()); ++t) {
        SliceType& st = types_out[static_cast<size_t>(t)];
        st = SliceType{};
        if (t > 10) continue;
        const uint16_t rec = ram.d16(static_cast<uint16_t>(table + 2 * t));
        const int count = ram.d16(rec);
        const uint16_t block = ram.d16(static_cast<uint16_t>(rec + 4));
        if (count < 3 || count > kSliceVerts) continue;
        st.count = count;
        for (int k = 0; k < count; ++k) {
            const auto at = static_cast<uint16_t>(block + 6 + 6 * k);
            st.verts[k] = {static_cast<float>(ram.s16(at)), static_cast<float>(ram.s16(static_cast<uint16_t>(at + 2))),
                           static_cast<float>(ram.s16(static_cast<uint16_t>(at + 4)))};
        }
        st.dh = ram.s16(static_cast<uint16_t>(rec + 8));
        for (uint16_t e = ram.d16(static_cast<uint16_t>(rec + 2)); ram.d16(e) != 0xFFFF && st.npolys < 4;
             e = static_cast<uint16_t>(e + 3)) {
            const uint8_t colour = ram.d8(static_cast<uint16_t>(e + 2));
            for (uint16_t q = ram.d16(e); ram.d16(q) != 0xFFFF && st.npolys < 4;) {
                const int n = ram.d16(q);
                if (n < 3 || n > 16) break;
                SliceType::Poly& pl = st.polys[st.npolys++];
                pl.n = static_cast<uint8_t>(n);
                pl.colour = colour;
                for (int k = 0; k < n; ++k) {
                    const int i = ram.d16(static_cast<uint16_t>(q + 2 + 2 * k)) / 4;
                    pl.idx[k] = static_cast<uint8_t>(std::min(i, count - 1));
                }
                q = static_cast<uint16_t>(q + 2 + 2 * (n + 1));
            }
        }
        const uint16_t lines = ram.d16(static_cast<uint16_t>(rec + 6));
        if (lines != 0xFFFF) {
            for (uint16_t e = lines; ram.d16(e) != 0xFFFF; e = static_cast<uint16_t>(e + 3)) {
                const uint16_t q = ram.d16(e);
                const uint8_t colour = ram.d8(static_cast<uint16_t>(e + 2));
                const int n = ram.d8(static_cast<uint16_t>(q + 1));
                for (int k = 0; k < n && st.nlines < 32; ++k) {
                    const int a = ram.d8(static_cast<uint16_t>(q + 2 + 2 * k)) / 4;
                    const int b = ram.d8(static_cast<uint16_t>(q + 3 + 2 * k)) / 4;
                    if (a < count && b < count) {
                        st.lines[st.nlines++] = {static_cast<uint8_t>(a), static_cast<uint8_t>(b), colour};
                    }
                }
            }
        }
        st.valid = st.npolys > 0;
    }
}

// The freeway (highway mode): the route's road slices around the current one, then the highway cars
// and the player's car, far first. 7A0E builds the original's ring of 32 slices from the current slice
// each frame (each slice is the previous one advanced by its type's vertex 2 at its heading, the
// heading then changed by the type's step); this continues it both ways to the draw distance. The
// positions are 16-bit, in a frame highway_frame re-centres at each new segment (78B8): the ring and the
// cars in memory may still be in the previous frame, so they are moved by the difference between the
// current slice's position (DS:804A) and the ring's slice for it.
void SceneBuilder::Impl::draw_freeway() {
    read_slice_types(kSliceRecords, slice_types);
    if (opt->original_window) read_slice_types(kSliceRecordsFar, slice_types_far);
    segments.clear();
    const uint16_t list = ram.d16(kRouteSegments);
    for (int k = 0; k < 1024; ++k) {
        const uint16_t w = ram.d16(static_cast<uint16_t>(list + 4 * k));
        if (w == 0xFFFF) break;
        segments.push_back({static_cast<uint8_t>(w & 0xFF), static_cast<uint8_t>(w >> 8)});
    }
    if (segments.empty()) return;
    // Global slice numbers: a segment's slices follow the previous segment's (7A0E: slice n of a
    // segment with n >= its length is the next segment's first).
    const auto global = [&](int seg, int pos) {
        int g = 0;
        for (int k = 0; k < seg && k < static_cast<int>(segments.size()); ++k) g += segments[static_cast<size_t>(k)].slices;
        return g + pos;
    };
    int total = 0;
    for (const Segment& sg : segments) total += sg.slices;
    const int ring0 = global(ram.s16(kRingSegment), ram.s16(kRingSlice));
    const int current = global(ram.s16(kSegment), ram.s16(kSlice));
    if (ring0 < 0 || ring0 >= total || current < 0) return;
    const auto ring_x = [&](int k) { return ram.d16(static_cast<uint16_t>(kRing + 6 * k)); };
    const auto ring_y = [&](int k) { return ram.d16(static_cast<uint16_t>(kRing + 6 * k + 2)); };
    const auto ring_h = [&](int k) { return static_cast<int>(ram.s16(static_cast<uint16_t>(kRing + 6 * k + 4))); };
    const int d = current - ring0;
    uint16_t off_x = 0, off_y = 0;
    if (d >= 0 && d < kRingSlices) {
        off_x = static_cast<uint16_t>(ram.d16(kRingBase) - ring_x(d));
        off_y = static_cast<uint16_t>(ram.d16(static_cast<uint16_t>(kRingBase + 2)) - ring_y(d));
    }
    const double z = ram.s16(static_cast<uint16_t>(kRingBase + 4));
    // 16-bit positions relative to the camera (the camera big tile's row/column don't apply here).
    const auto abs_x = [&](uint16_t v) { return cam_x + static_cast<int16_t>(static_cast<uint16_t>(v + off_x - cam16_x)); };
    const auto abs_y = [&](uint16_t v) { return cam_y + static_cast<int16_t>(static_cast<uint16_t>(v + off_y - cam16_y)); };
    const auto type_of = [&](int g) {
        for (const Segment& sg : segments) {
            if (g < sg.slices) return static_cast<int>(sg.type);
            g -= sg.slices;
        }
        return -1;
    };
    const auto wrap = [](int h) { return ((h % 360) + 360) % 360; };
    const auto advance = [&](int type, int heading) {
        const SliceType& st = slice_types[static_cast<size_t>(type) & 15];
        return heading_rot[static_cast<size_t>(wrap(heading))](st.verts[2]);
    };

    // Slices from `span` behind the current one to `span` ahead; the ring's positions where it has them.
    const int span = opt->radius >= kMapCells ? total : opt->radius * kCellSize / kSliceLength + kRingSlices;
    int lo = std::max(0, current - std::min(span, 1024)), hi = std::min(total, current + span);
    if (opt->original_window) {
        // Validation: the ring highway_frame builds from the current slice and draws, 31 down to 0
        // (the mirror's 78E2: its first 8, 7 down to 0).
        lo = std::min(current, total);
        hi = std::min(current + (mirror ? 8 : kRingSlices), total);
    }
    const int ring_end = std::min(ring0 + kRingSlices, total);
    if (hi <= lo) return;
    slices.assign(static_cast<size_t>(hi - lo), Slice{});
    const auto at = [&](int g) -> Slice& { return slices[static_cast<size_t>(g - lo)]; };
    const int first = std::max(lo, ring0), last = std::min(hi, ring_end);
    for (int g = first; g < last; ++g) {
        Slice& sl = at(g);
        sl.x = abs_x(ring_x(g - ring0));
        sl.y = abs_y(ring_y(g - ring0));
        sl.heading = wrap(ring_h(g - ring0));
        sl.type = type_of(g);
    }
    if (first < last) {
        // Ahead of the ring.
        Slice prev = at(last - 1);
        for (int g = last; g < hi; ++g) {
            Slice sl;
            const V3 a = advance(prev.type, prev.heading);
            sl.x = prev.x + a.x;
            sl.y = prev.y + a.y;
            sl.heading = wrap(prev.heading + slice_types[static_cast<size_t>(prev.type) & 15].dh);
            sl.type = type_of(g);
            at(g) = sl;
            prev = sl;
        }
        // Behind it.
        Slice next = at(first);
        for (int g = first - 1; g >= lo; --g) {
            Slice sl;
            sl.type = type_of(g);
            sl.heading = wrap(next.heading - slice_types[static_cast<size_t>(sl.type) & 15].dh);
            const V3 a = advance(sl.type, sl.heading);
            sl.x = next.x - a.x;
            sl.y = next.y - a.y;
            at(g) = sl;
            next = sl;
        }
    }

    // Far to near, those in view.
    slice_order.clear();
    for (size_t i = 0; i < slices.size(); ++i) {
        Slice& sl = slices[i];
        if (sl.type < 0 || !slice_types[static_cast<size_t>(sl.type) & 15].valid) continue;
        const V3 mid = heading_rot[static_cast<size_t>(sl.heading)](V3{64, 0, 128});
        const V3 c = to_camera(sl.x + mid.x, sl.y + mid.y, z + mid.z);
        if (!opt->original_window && !sphere_visible(c, 200)) continue;
        sl.dist = opt->original_window ? static_cast<float>(i) : dot(c, c);
        slice_order.push_back(static_cast<int>(i));
    }
    std::sort(slice_order.begin(), slice_order.end(),
              [&](int a, int b) { return slices[static_cast<size_t>(a)].dist > slices[static_cast<size_t>(b)].dist; });
    static constexpr uint16_t kFan[42] = {0, 1, 2,  0, 2, 3,  0, 3, 4,  0, 4, 5,   0, 5, 6,   0, 6, 7,   0, 7, 8,
                                          0, 8, 9,  0, 9, 10, 0, 10, 11, 0, 11, 12, 0, 12, 13, 0, 13, 14, 0, 14, 15};
    ribbons = !opt->original_window;  // the lane dashes lie on the road
    ribbon_base_z = static_cast<float>(z);
    for (const int i : slice_order) {
        const Slice& sl = slices[static_cast<size_t>(i)];
        // 7A0E: the ring's slices 6 to 18 take the detailed record (the original window only; else always).
        const bool plain = opt->original_window && (i < 6 || i > 18);
        const SliceType& st = (plain ? slice_types_far : slice_types)[static_cast<size_t>(sl.type) & 15];
        if (!st.valid) continue;
        const M3 a = view * heading_rot[static_cast<size_t>(sl.heading)];
        const V3 t = to_camera(sl.x, sl.y, z);
        begin_group();
        cam.clear();
        for (int k = 0; k < st.count; ++k) cam.push_back(fin(a(st.verts[k]) + t));
        project_from(0);
        for (int k = 0; k < st.npolys; ++k) {
            const SliceType::Poly& pl = st.polys[k];
            uint16_t idx[16];
            for (int j = 0; j < pl.n; ++j) idx[j] = pl.idx[j];
            emit_indexed(cam.data(), scr.data(), idx, pl.n, kFan, static_cast<uint32_t>(pl.n - 2),
                         ega_colour(pl.colour & 15), 1);
        }
        for (int k = 0; k < st.nlines; ++k) {
            const SliceType::Line& ln = st.lines[k];
            emit_segment(cam.data(), scr.data(), ln.a, ln.b, ega_colour(ln.colour & 15));
        }
        ++out->stats.slices;
    }
    ribbons = false;

    // Highway cars (highway_draw_cars 7F09: at {x, y} of the record, z by type from DS:8222, yaw =
    // heading + 270, model = type, B9D6) and, outside the car, the player's (775E: 28D3).
    const bool fades = opt->smooth_traffic && !opt->original_window;
    const double now = opt->time_s;
    if (!fades || now < traffic_time - 1) reset_traffic();  // (time went back: another race)
    const auto on_road = [&](int g) { return g >= lo && g + 1 < hi; };
    // The road's direction at slice g (to the next one), unit length.
    const auto direction = [&](int g, double& ux, double& uy) {
        const double dx = at(g + 1).x - at(g).x, dy = at(g + 1).y - at(g).y;
        const double len = std::max(1e-6, std::sqrt(dx * dx + dy * dy));
        ux = dx / len;
        uy = dy / len;
        return len;
    };
    highway_objects.clear();
    for (int i = 0; i < kHighwayCarSlots; ++i) {
        const auto rec = static_cast<uint16_t>(kHighwayCars + 0x16 * i);
        const bool on = ram.d8(static_cast<uint16_t>(kCarActive + i)) != 0;
        HighwayObject o;
        if (on) {
            o.key = ram.s16(static_cast<uint16_t>(kCarKeys + 2 * i));
            o.model = ram.d8(static_cast<uint16_t>(rec + 0x13));
            o.x = abs_x(ram.d16(static_cast<uint16_t>(rec + 4)));
            o.y = abs_y(ram.d16(static_cast<uint16_t>(rec + 6)));
            o.z = ram.d8(static_cast<uint16_t>(kCarHeights + o.model));
            o.heading = wrap(ram.s16(static_cast<uint16_t>(rec + 0x0C)) + 270);
        }
        // Slot 10 is the opponent's (or the other player's), which doesn't come and go.
        if (fades && i < kHighwayCarSlots - 1) {
            TrafficCar& was = traffic_slots[static_cast<size_t>(i)];
            TrafficCar car;
            if (on) {
                car.on = true;
                car.slice = global(ram.s16(rec), ram.s16(static_cast<uint16_t>(rec + 2)));
                car.model = o.model;
                car.lane = ram.d8(static_cast<uint16_t>(rec + 0x12));
                car.heading = o.heading;
                car.speed = ram.s16(static_cast<uint16_t>(rec + 0x0E));
                if (on_road(car.slice)) {
                    double ux = 0, uy = 0;
                    direction(car.slice, ux, uy);
                    const double dx = o.x - at(car.slice).x, dy = o.y - at(car.slice).y;
                    car.along = dx * ux + dy * uy;
                    car.across = dy * ux - dx * uy;
                } else {
                    car.slice = -1;
                }
            }
            // The same car as before: same slot, model and lane, and about where it was (a slot can be
            // freed and filled again within a frame).
            const bool same = was.on && on && was.model == car.model && was.lane == car.lane &&
                              std::abs(car.slice - was.slice) <= 3;
            if (was.on && !same && was.slice >= 0) departed.push_back({was, now});
            if (on) {
                car.since = same ? was.since : traffic_seen ? now : -1e9;
                car.alpha = static_cast<float>(std::clamp((now - car.since) / kTrafficFadeSeconds, 0.0, 1.0));
                o.alpha = car.alpha;
            }
            was = car;
        }
        if (!on) continue;
        if (opt->original_window && !mirror && o.key < 0) continue;  // the mirror's keys come later (4021:1358)
        highway_objects.push_back(o);
    }
    if (fades) {
        traffic_seen = true;
        traffic_time = now;
        // The cars the original took off: on along the road at their speed, fading out.
        for (size_t k = 0; k < departed.size();) {
            const Departed& dep = departed[k];
            const double dt = now - dep.gone;
            const float alpha = dep.car.alpha * static_cast<float>(1 - dt / kTrafficFadeSeconds);
            int g = dep.car.slice;
            if (dt < 0 || alpha <= 0 || !on_road(g)) {
                departed.erase(departed.begin() + static_cast<std::ptrdiff_t>(k));
                continue;
            }
            double ux = 0, uy = 0;
            double rest = dep.car.along + dep.car.speed * dt;  // from slice g's start
            double len = direction(g, ux, uy);
            while (rest > len && on_road(g + 1)) {
                rest -= len;
                ++g;
                len = direction(g, ux, uy);
            }
            HighwayObject o;
            o.model = dep.car.model;
            o.x = at(g).x + ux * rest - uy * dep.car.across;
            o.y = at(g).y + uy * rest + ux * dep.car.across;
            o.z = ram.d8(static_cast<uint16_t>(kCarHeights + o.model));
            o.heading = wrap(dep.car.heading + at(g).heading - at(dep.car.slice).heading);
            o.alpha = alpha;
            highway_objects.push_back(o);
            ++k;
        }
    }
    vehicle_models.clear();
    if (ram.d8(kExternalView) != 0) {
        load_tracer();
        const std::array<int16_t, 3> pos16{ram.s16(kPlayerCar), ram.s16(static_cast<uint16_t>(kPlayerCar + 2)),
                                           ram.s16(static_cast<uint16_t>(kPlayerCar + 4))};
        for (int k = 0; k < 3; ++k) {
            tracer->wr16(addr::kDataSeg, static_cast<uint16_t>(addr::kObjPos + 2 * k),
                         static_cast<uint16_t>(pos16[static_cast<size_t>(k)]));
        }
        tracer->wr16(addr::kCodeSeg, addr::kSortKey, 0);
        tracer->run(kDrawPlayerCar, pos16, trace);
        for (const TraceEvent& e : trace.events) {
            if (e.op != TraceEvent::Op::Model) continue;
            ModelDraw md;
            md.model = e.addr;
            md.rotation = e.aux == 2 ? model_rotation(0, 0, 0)
                                     : model_rotation(e.angles[0], e.aux == 1 ? e.angles[1] : 0, e.aux == 1 ? e.angles[2] : 0);
            md.offset = {static_cast<float>(e.pos[0]), static_cast<float>(e.pos[1]), static_cast<float>(e.pos[2])};
            md.outline = e.outline_enable != 0;
            vehicle_models.push_back(md);
        }
        HighwayObject o;
        o.x = cam_x + static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint16_t>(pos16[0]) - cam16_x));
        o.y = cam_y + static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint16_t>(pos16[1]) - cam16_y));
        o.z = pos16[2];
        o.player = 0;
        highway_objects.push_back(o);
    }
    for (HighwayObject& o : highway_objects) {
        const V3 c = to_camera(o.x, o.y, o.z);
        // The original: the player's car first (775E), then the cars by key, far first (7F09, 4686).
        o.dist = !opt->original_window || mirror ? dot(c, c) : o.player >= 0 ? 1e30f : static_cast<float>(o.key);
    }
    std::sort(highway_objects.begin(), highway_objects.end(),
              [](const HighwayObject& a, const HighwayObject& b) { return a.dist > b.dist; });
    for (const HighwayObject& o : highway_objects) {
        begin_group();
        if (o.player >= 0) {
            lod_key = 0;
            for (const ModelDraw& md : vehicle_models) {
                const V3 c = to_camera(o.x + md.offset.x, o.y + md.offset.y, o.z + md.offset.z);
                cam.clear();
                draw_model(md.model, md.rotation, c, md.outline);
            }
            continue;
        }
        if (o.model < 0 || o.model >= kModelCount) continue;
        const V3 c = to_camera(o.x, o.y, o.z);
        if (!opt->original_window && !sphere_visible(c, meshes[static_cast<size_t>(o.model)].radius + 8)) continue;
        ++out->stats.vehicles;
        lod_key = static_cast<uint16_t>(o.key);
        cam.clear();
        fade = o.alpha;
        draw_model(o.model, heading_rot[static_cast<size_t>(o.heading)], c, true);
        fade = 1;
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
    m.tracer_loaded = false;
    m.backdrop = false;
    m.no_depth = !options.depth;
    m.begin_group();
    {
        const Impl::CourseData& cd = m.courses[static_cast<size_t>(m.state.course)];
        m.piece_variant.assign(cd.pieces.size(), -2);
        m.prim_visible.assign(cd.prim_slots, 0);
    }
    if (m.positional_cache.size() > 65536) m.positional_cache.clear();
    m.lod_key = m.ram.u16(addr::kCodeSeg, addr::kSortKey);
    // Highway mode: the freeway, and the city only once the end of the road is in sight (0342-034E).
    m.freeway = m.ram.d8(addr::kHighway) == 0xFF;
    if (!m.freeway) m.reset_traffic();
    const bool city = !m.freeway || m.ram.d8(kEndOfRoad) != 0;
    out.stats.city = city;

    // Vehicles and pedestrians, bound to cells.
    std::fill(m.cell_head.begin(), m.cell_head.end(), -1);
    std::fill(std::begin(m.window_steps), std::end(m.window_steps), -1);
    if (city) {
        m.collect_vehicles(!options.original_window && options.far_vehicles);
        m.trace_vehicles();
        for (size_t i = m.vehicles.size(); i-- > 0;) {
            Impl::Vehicle& v = m.vehicles[i];
            v.next = m.cell_head[static_cast<size_t>(v.cell)];
            m.cell_head[static_cast<size_t>(v.cell)] = static_cast<int>(i);
        }
    } else {
        m.vehicles.clear();
    }
    if (m.mirror && !options.original_window) m.draw_sky();  // validation: the original's own fill is underneath

    if (options.original_window) {
        if (city) m.draw_original_window();
        if (m.freeway) m.draw_freeway();
    } else if (!city) {
        if (options.ground) m.draw_plane(kGroundHighway);
        m.draw_freeway();
    } else {
        if (options.ground) m.draw_ground();
        if (m.freeway) m.draw_freeway();  // the end of the road, about to join the city
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
        if (!options.depth) {
            // Painter's order: the cells of the original's window among themselves in its order (so its
            // traffic and pedestrians are hidden as the original hides them), in the places the walk has
            // for them.
            m.window_slots.clear();
            const int* steps = m.window_steps;
            for (size_t i = 0; i < m.sorted.size(); ++i) {
                if (std::find(steps, steps + 6, m.sorted[i]) != steps + 6) m.window_slots.push_back(i);
            }
            size_t k = 0;
            for (int si = 0; si < 6 && k < m.window_slots.size(); ++si) {
                const int cell = steps[si];
                if (cell < 0 || std::find(steps, steps + si, cell) != steps + si) continue;  // (once)
                bool listed = false;
                for (const size_t slot : m.window_slots) listed |= m.sorted[slot] == cell;
                if (listed) m.sorted[m.window_slots[k++]] = cell;
            }
        }
        for (const int cell : m.sorted) m.draw_cell_enhanced(cell);
    }
    out.stats.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace vette::enhanced
