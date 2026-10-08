#pragma once
// Enhanced mode, extended draw distance: the original city as one renderable model.
//
// The model is built at startup from the player's own VETTE.EXE (DOS 1.1), running in the host
// emulator. Nothing derived from the game files is stored in this repository. References:
// re/notes/05-world-data.md (map, cell types, object formats) and 03-renderer-and-visibility.md
// (draw order, visibility, colours).
//
// How it is built (world.cpp):
//  - The map grid, the cell-type lists and the segment-245A models are plain data and are decoded
//    directly from the unpacked image.
//  - The 125+ code-drawn objects are hand-written routines. Each one runs in a scratch copy of the
//    machine with the renderer's entry points hooked (transform, fill, lines, face culling, models),
//    so it hands over its geometry instead of drawing it. The routines decide their level of detail
//    from the camera and a few game flags; probing them under a grid of camera positions and flag
//    values collects every variant they can produce (world_probe.cpp).
//
// Coordinates. All positions are original world units (1 unit ~ 3 in):
//   +X = north, +Y = east, +Z = up (the original's left-handed frame).
//   Cell = 0x800 units, big tile = 16 x 16 cells, map = 5 x 5 big tiles = 80 x 80 cells.
//   Cell (cx, cy) spans x = cx*0x800 .. +0x800 and y = cy*0x800 .. +0x800 (cx counts north, cy east);
//   its list positions are relative to (cx*0x800, cy*0x800, elevation*224).
//   Absolute positions use 32-bit integers over the whole map (0 .. 163840). The original works in
//   16-bit coordinates relative to the camera's big tile, which wrap beyond 16 cells.
//
// Object-local coordinates are relative to the list entry's position (its "entry position"):
// a cell's origin plus the entry's (dx, dy, dz). Geometry is in world axes unless stated otherwise.
//
// Drawing the whole map (the Maximum draw distance):
//  - for every cell, its type's list1 in list order (the ground layer: roads, kerbs and markings are
//    coplanar and rely on that order), then list2 (buildings and other sortables); each entry draws
//    routine(entry.routine)->select(state) at World::entry_position();
//  - skip entries whose routine is `compound` and draw every World::compounds instance once instead
//    (its variant's `calls` are routines placed at the instance position + offset);
//  - within a variant draw the parts in order, and each part's prims in order; a prim with a Cull is
//    drawn only while face_visible();
//  - DrawState::finish_flag follows Variant::sets_finish_flag in list order (start/finish posts);
//  - vehicles and pedestrians are not part of the static world: draw World::models with the live
//    entities' positions and angles (model_to_world()).
// The original's 3D view is reproduced pixel for pixel from this model by world_reference.h.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace vette::host {
class Machine;
}

namespace vette::enhanced {

constexpr int kCellSize = 0x800;
constexpr int kBigTileCells = 16;
constexpr int kElevationStep = 224;
constexpr int kMapCells = 80;  // per side, as the shipped map is 5 x 5 big tiles (read from the data)
constexpr int kModelCount = 59;
// Model slots: the original's, then code-drawn objects drawn as models (enhanced/object_models.h).
constexpr int kModelSlots = 100;

struct Vec3i {
    int32_t x = 0, y = 0, z = 0;
    friend constexpr bool operator==(const Vec3i&, const Vec3i&) = default;
};

// A colour byte exactly as the original's fill and line routines take it (notes 03, "Faces, clipping,
// fill"). The low nibble is the base EGA colour (the default EGA palette, indices 0..15). If the high
// nibble is non-zero and differs from the low nibble, the polygon is a two-colour checkerboard: the
// original fills it with the base colour, then overlays the high-nibble colour on every other pixel.
// For the polygon's rows counted from its top screen row r = 0, 1, ..., pixel x gets the second colour
// when (x + r) is even (mask 0AAh on row 0, 55h on row 1, ...).
struct Colour {
    uint8_t raw = 0;
    constexpr uint8_t base() const { return raw & 0x0F; }
    constexpr uint8_t second() const { return static_cast<uint8_t>(raw >> 4); }
    constexpr bool dithered() const { return second() != 0 && second() != base(); }
};

// Back-face culling, as the original evaluates it (in camera space, which keeps the rotation-invariant
// results; see face_visible()).
struct Cull {
    enum class Kind : uint8_t {
        None,    // always drawn
        Behind,  // 5-byte face: visible iff (B - A) . (A - camera) > 0, i.e. B is a point behind the face
        Plane,   // 6-byte face: normal from three vertices (the original's 16-bit cross product)
    };
    Kind kind = Kind::None;
    uint16_t part = 0;    // index into Variant::parts of the vertices below
    uint16_t v[3] = {};   // vertex indices into that part (Behind: v[0] = A, v[1] = B)
};

struct Prim {
    enum class Kind : uint8_t {
        Polygon,         // convex fill (3009:B5B3)
        PolygonAlt,      // the alternative filler (3009:B63F): handles concave outlines
        Line,            // one segment, indices[first], indices[first + 1]
    };
    Kind kind = Kind::Polygon;
    Colour colour;
    uint32_t first = 0;   // into Part::indices
    uint16_t count = 0;   // vertices (the original's closing repeat is removed)
    Cull cull;
};

// One vertex set of an object plus what was drawn with it, in the original's draw order.
struct Part {
    enum class Source : uint8_t {
        Packed,  // packed vertex block (3009:3879): verts are object-local, ready to use
        Plain,   // plain {x, y, z} list rotated about `origin` (3009:393F): verts are local, see rotation
        Model,   // a segment-245A model placed at `origin` (3009:B9BA / B9E6): see `model`
    };
    // How a Plain or Model part is rotated about `origin`. The rotation is the original's object matrix
    // (rotate_local()); `yaw`, `pitch` and `roll` are degrees.
    enum class Rotation : uint8_t {
        None,       // no rotation (Packed, and models drawn with 3009:B9E6)
        Fixed,      // constant angles
        CameraYaw,  // a billboard: yaw = the camera's yaw + `yaw`, so it faces the camera (the original's
                    // Q15 product overflows at some yaws and mirrors it; the trees are symmetric)
        Animated,   // yaw advances by `anim_step` degrees every time the original draws the object
        Inherited,  // the original reuses the previous object's matrix (a quirk); taken as yaw = pitch = roll = 0
    };
    Source source = Source::Packed;
    Rotation rotation = Rotation::None;
    int16_t yaw = 0, pitch = 0, roll = 0;
    int16_t anim_step = 0;
    uint16_t anim_angle_addr = 0;  // DS offset of the animated angle (diagnostics)
    Vec3i origin;                  // object-local
    std::vector<Vec3i> verts;      // Packed: object-local. Plain: local world axes (x north, y east, z up).
    std::vector<uint16_t> indices;
    std::vector<Prim> prims;

    // Model parts.
    uint8_t model = 0;  // World::models[model]

    // The original's own data, for bit-exact reproduction (validation): the packed vertex words that
    // follow the origin (3 per vertex after vertex 0), and the DS offset the block or list came from.
    std::vector<int16_t> packed;
    uint16_t data_addr = 0;
};

// A sub-entry of a multi-cell compound structure (Golden Gate, Bay Bridge): drawn like a list entry,
// at the compound's entry position plus (dx, dy, dz).
struct SubCall {
    uint16_t routine = 0;
    Vec3i offset;
};

// The game state a variant was produced under. Each mask has one bit per value of that input, set if
// the variant appears for that value (all bits set = does not depend on it).
struct Conditions {
    uint8_t facing = 0x3;       // bit f: DS:35C3 == f, the axis the camera faces (0 = +-x, 1 = +-y)
    uint8_t lod_key = 0x3;      // bit 0: sort key (cs:259E) < 800h (near), bit 1: >= 800h (far)
    uint8_t windows = 0x3;      // bit 0: window detail on (DS:2ABE == 0), bit 1: off
    uint8_t course = 0x1E;      // bit c: DS:2CD5 == c (course 1..4)
    uint8_t finish_flag = 0x3;  // bit 0: DS:38D7 == 0, bit 1: != 0 (a finish banner was drawn before)
    bool positional = false;    // depends on the camera position relative to the object
    int32_t min_distance = 0;   // positional: nearest / farthest probe producing it (camera to entry
    int32_t max_distance = 0;   // position, max of |dx|, |dy|; only a guide, the tests are per axis)
};

struct Variant {
    std::vector<Part> parts;    // drawn in this order
    std::vector<SubCall> calls; // compound routines only
    Conditions when;
    int8_t sets_finish_flag = -1;  // -1: leaves DS:38D7 alone, else the value it stores (0 or 1)
    uint32_t primitives = 0;       // total prims (model parts count as one)
    uint8_t detail = 0;            // 0 = the routine's most detailed variant (most primitives)
    uint64_t signature = 0;        // identity of the original's draw trace (validation matches on it)

    // Painter-order alternatives: when the face `unless_visible` points away from the camera, the
    // original draws the primitives in `order` (part, prim) instead of part by part (the Transamerica
    // pyramid picks its order from one face this way). A depth-buffered renderer can ignore it.
    struct Reorder {
        Cull unless_visible;
        std::vector<std::pair<uint16_t, uint16_t>> order;
    };
    std::vector<Reorder> reorders;
};

// State a renderer chooses variants by. For the Maximum draw distance, every object uses its most
// detailed variant among those valid for the course, the window toggle and the finish flag.
struct DrawState {
    int course = 1;            // 1..4
    bool windows = true;       // building window detail (the original's W toggle; Enhanced default on)
    bool finish_flag = false;  // DS:38D7, updated in list order by Variant::sets_finish_flag
};

struct Routine {
    uint16_t address = 0;  // near offset in code segment 3009
    bool compound = false; // drawn once per frame through `calls` (World::compounds places it once)
    bool camera_dependent = false;  // some variant depends on the camera position or facing
    std::vector<Variant> variants;
    std::string error;     // non-empty if it could not be captured

    // The most detailed variant valid for `state` (nullptr if none, e.g. a banner of another course).
    const Variant* select(const DrawState& state) const;
};

struct ListEntry {
    uint16_t routine = 0;
    int16_t dx = 0, dy = 0, dz = 0;
};

struct CellType {
    bool used = false;           // appears on the map
    uint16_t address = 0;        // DS offset of the record
    uint8_t ground_class = 0;    // header low byte: ground shape 0..9 (notes 05 section 3)
    uint8_t collision_class = 0; // header high byte: index into DS:C0A6
    std::vector<ListEntry> list1;  // ground layer: draw in this order (coplanar roads and markings)
    std::vector<ListEntry> list2;  // sortables: depth-sorted with the cell's vehicles
};

struct Cell {
    uint8_t type = 0;
    uint8_t elevation = 0;  // z = elevation * 224
};

// A segment-245A model (notes 05 section 6). Vertices are model-local as stored: x = east, y = DOWN,
// z = north at yaw 0, in world units. Vertices 0..3 are the octant reference frame
// (0,0,0), (-50,0,0), (0,0,50), (0,50,0).
struct ModelFace {
    uint16_t address = 0;  // 245A offset
    uint16_t flags = 0;    // bit 0: screen-space winding cull (after the near-plane clip); bits 1-3:
                           // kind; bit 13: outline the clipped polygon in colour_hi while DS:E0D8 is
                           // set (always, except the opponent's car while far away); bit 14: never
                           // drawn; bit 15: checkerboard only (screen-door: the base colour on the
                           // dither pixels, the rest untouched)
    Colour colour;
    uint8_t colour_hi = 0;
    // Primitive kind from bits 1-3: 0 convex fill, 2 the alternative filler, 4 and 6 polylines.
    uint8_t kind() const { return static_cast<uint8_t>(flags & 0x0E); }
    bool lines() const { return kind() >= 4; }
    std::vector<std::vector<uint16_t>> prims;  // vertex indices (fills: closing repeat removed;
                                               // polylines: every point, segment k = [k], [k + 1])
};

struct ModelMesh {
    uint16_t header = 0;  // 245A offset
    std::vector<Vec3i> verts;
    std::vector<ModelFace> faces;
    // Per view octant (model_octant()): faces to draw, back to front, and the vertices the original
    // skips transforming in that view (unused by the listed faces).
    std::array<std::vector<uint16_t>, 8> order;
    std::array<std::vector<uint16_t>, 8> skipped;
};

struct Model {
    bool present = false;
    ModelMesh near_mesh;    // drawn while the sort key (cs:259E) < 800h
    ModelMesh far_mesh;     // a generic box in far_colour
    uint16_t far_colour = 0;
};

// A compound structure, once, at its true position (the original draws it from whichever window
// cell lists it first, once per frame, guarded by DS:2AC0).
struct CompoundInstance {
    uint16_t routine = 0;
    Vec3i position;              // absolute entry position its `calls` are relative to
    std::vector<uint16_t> cells; // cells whose lists reference it (cx * 80 + cy)
};

struct World {
    int big_rows = 0, big_cols = 0;  // big tiles (5 x 5); cells = 16x as many per side
    int cells_x() const { return big_rows * kBigTileCells; }
    int cells_y() const { return big_cols * kBigTileCells; }
    std::vector<Cell> cells;  // [cx * cells_y() + cy]
    const Cell& cell(int cx, int cy) const { return cells[static_cast<size_t>(cx * cells_y() + cy)]; }
    // Absolute position of a list entry of cell (cx, cy).
    Vec3i entry_position(int cx, int cy, const ListEntry& e) const {
        return {cx * kCellSize + e.dx, cy * kCellSize + e.dy, cell(cx, cy).elevation * kElevationStep + e.dz};
    }
    // Ground height at absolute (x, y): the cell's elevation * 224 plus its ground-shape class's slope
    // (4160:0515, notes 05 section 3); positions off the map clamp to the edge cells.
    int32_t ground_z(int32_t x, int32_t y) const;
    std::array<uint8_t, 25> big_tile_ground{};  // ground colour per big tile (DS:8556): 7 land, 9 water

    std::array<CellType, 256> types;
    std::vector<Routine> routines;  // sorted by address
    const Routine* routine(uint16_t address) const;
    std::array<Model, kModelSlots> models;  // the original's 0..58 (extract_world), then objects
    std::vector<CompoundInstance> compounds;

    struct Stats {
        int types_used = 0;
        int routines = 0, routines_failed = 0, compound_routines = 0;
        int variants = 0, camera_dependent = 0;
        int models = 0;
        size_t probe_runs = 0;
        double milliseconds = 0;
    } stats;
    std::vector<std::string> warnings;
};

// Builds the world from a machine whose VETTE.EXE image is unpacked (any time after the EXEPACK stub
// has run: the title screen is fine). The machine is only read: the routines run on a scratch copy.
// Returns false with `error` set if the image isn't the expected build or isn't unpacked yet.
bool extract_world(host::Machine& machine, World& out, std::string& error);

// --- Geometry helpers (floating point, for renderers) ---------------------------------------------

struct Vec3d {
    double x = 0, y = 0, z = 0;
};

// The original's object rotation (3009:3DF9 / 9D60) applied to a local vector given in world axes
// (x north, y east, z up). Yaw turns from north towards east (yaw 90 maps local north to east).
Vec3d rotate_local(Vec3d v, double yaw_deg, double pitch_deg, double roll_deg);

// A model vertex (x east, y down, z north) in world axes after the placement rotation.
Vec3d model_to_world(const Vec3i& v, double yaw_deg, double pitch_deg = 0, double roll_deg = 0);

// The model's octant for a camera at `camera_local`, the camera position relative to the model origin
// in model-local axes (x east, y down, z north): bit 0 set when the camera is on the -x side, bit 1 on
// the +z side, bit 2 on the +y (lower) side, as 3009:9CAF computes it.
int model_octant(Vec3d camera_local);

// Whether a culled primitive faces a camera at `camera`. `verts_of_part` are the vertices of part
// `cull.part` (it can be another part than the prim's: windows cull with their wall), in the same frame
// as `camera` (e.g. world space after the part's rotation).
bool face_visible(const Cull& cull, const Vec3d* verts_of_part, Vec3d camera);

} // namespace vette::enhanced
