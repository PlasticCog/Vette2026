#pragma once
// Validation only: the original's own 3D view redrawn from the extracted World, the way the original
// draws it (re/notes/03-renderer-and-visibility.md), so `vette_world --validate` can compare it with
// the hosted original's frame pixel by pixel.
//
//  - The cell window, cell positions and big-tile wrap: a port of draw_world_cells (3009:30C6) and
//    draw_cell (31EE), reading cells and lists from the World, not from the game's memory.
//  - Each object routine still runs once on a scratch copy of the frame's memory, in the original's
//    order, only to learn which of the World's variants the original picks (its LOD rules), the
//    dynamic angles (billboards, animation) and the game state it updates (compound guard, finish
//    flag). The geometry drawn is the World's.
//  - Sortables: the World's list2, plus the cell's vehicles collected by the original routines
//    3009:32F8 / 34D6 on the scratch copy; culled and depth-sorted by a port of 4562 / 44D0 / 4686.
//  - Exact fixed-point transform (camera matrix and axis table from the frame), projection
//    (game/projection.h), viewport clipping (40F7..43C2), the Bresenham edge walker (A377), span fill
//    with the polygon-relative checkerboard (9F3B), back-face tests (3B78 / 3BF5 / 9CAF / 9CD8).
//    the near-plane and guard-band clips (A7C0, A8D4..), the alternative filler for concave faces
//    (B63F, A4F1), and both line drawers (5170 with its single-byte quirk, A1FF for model lines and
//    outlines).

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "enhanced/world.h"

namespace vette::enhanced {

class Tracer;

struct ReferenceStats {
    int cells = 0;
    int objects = 0;      // static objects drawn (list entries and compound pieces)
    int unmatched = 0;    // objects whose original draw trace matched no World variant
    int vehicles = 0;
    int polygons = 0, lines = 0, models = 0;
    int clipped_near = 0;  // polygons through the near-plane / guard-band path
    // from_world: how far the World API's float vertices land from the original's exact camera-space
    // vertices, in camera units (largest component), for packed, plain and model parts.
    struct Deviation {
        size_t vertices = 0, over4 = 0, over16 = 0;
        int max = 0;
        uint16_t worst_routine = 0;
    };
    std::array<Deviation, 3> deviation{};
    std::vector<std::string> notes;
    // Draw calls in order (routine, x, y, z in the camera big tile's 16-bit frame).
    struct Call {
        uint16_t routine;
        int16_t x, y, z;
        friend bool operator==(const Call&, const Call&) = default;
    };
    std::vector<Call> calls;
};

class ReferenceRenderer {
public:
    explicit ReferenceRenderer(const World& world);
    ~ReferenceRenderer();
    ReferenceRenderer(const ReferenceRenderer&) = delete;
    ReferenceRenderer& operator=(const ReferenceRenderer&) = delete;

    // `ram`: the machine's 1 MB memory at draw_world_cells' entry (3009:30C6). `pixels`: the frame
    // (EGA colour indices, `width` x 200) as it was then, with sky, ground and horizon; drawn into.
    ReferenceStats render(const uint8_t* ram, std::vector<uint8_t>& pixels, int width);

    bool debug = false;  // print what is drawn

    // Build every vertex from the World API in floating point (Part::verts, rotate_local,
    // model_to_world, a float camera from the angles) instead of the original's fixed-point words and
    // matrices: what a modern renderer would do. The rest (clipping, rasterization) stays exact.
    bool from_world = false;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vette::enhanced
