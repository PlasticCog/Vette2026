#pragma once
// Native port of the perspective projection, project_vertices (3009:A685)
// (re/notes/03-renderer-and-visibility.md, "Projection"). Pure core plus adapter, as in math3d.h.

#include <cstdint>

namespace vette::host {
class Cpu;
}

namespace vette::game {

enum class ScreenAxis { X, Y };

// What project_vertices produces for one coordinate of a vertex with depth z >= 1 (nearer vertices
// are flagged 2 and not projected).
struct ScreenCoord {
    int32_t wide;        // the 32-bit coordinate (record +400h/+404h): v*256/z + centre
    int16_t narrow;      // the 16-bit coordinate (record +0/+2), when narrow_stored
    bool narrow_stored;  // false on the 32-bit recompute path, which leaves the 16-bit field unwritten
    bool overflow;       // the 16-bit coordinate overflowed (record flags = 1): use `wide`
};

// sx = x*256/z + centre_x (and the same for y) with a signed, truncating IDIV. When the IDIV
// overflows (the game's INT 0 handler returns 7FFFh), or legitimately returns 7FFFh, the original
// recomputes a 32-bit quotient with three unsigned DIVs. That recompute is not an exact division:
// it adds floor(lo/z) to the quotient of the high word, and for y a CWD of the low word makes the
// first DIV itself overflow whenever bit 15 of |y*256| is set. Both are reproduced as they are.
ScreenCoord project_coord(int16_t v, int16_t z, int16_t centre, ScreenAxis axis);

// project_vertices (3009:A685). In: ES:BX -> DX camera-space vertices {X, Y, Z}. Out: records
// {sx, sy, flags, vertex ptr} at ES:1A86 + 8i with 32-bit copies of sx, sy at +400h, CS:9C7A = FFh if
// any vertex is behind the near plane or overflowed, counter ES:E026.
void project_vertices(host::Cpu& cpu);

} // namespace vette::game
