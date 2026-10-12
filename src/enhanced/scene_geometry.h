#pragma once
// Geometry helpers of the SceneBuilder (scene.h): polygon triangulation and near-plane clipping.
// Pure functions, unit-tested in tests/enhanced_scene.cpp.

#include <cstdint>
#include <vector>

namespace vette::enhanced::geometry {

struct P3 {
    float x = 0, y = 0, z = 0;
};

// Triangulates a simple polygon (convex or concave, either winding, collinear points allowed) given
// in 2D, by ear clipping. Appends 3 indices per triangle (into the polygon's points) to `out`.
// Returns false if a degenerate remainder had to be fanned.
bool ear_clip(const float* x, const float* y, int n, std::vector<uint16_t>& out);

// The same for a planar polygon in 3D: projected onto the plane its normal is most aligned with.
bool triangulate(const P3* points, int n, std::vector<uint16_t>& out);

// Whether a planar polygon is convex: it turns the same way at every corner (straight corners allowed).
bool convex(const P3* points, int n);

// A filled polygon's triangles, appended to `out`: a fan from its first point if it's convex, else cut
// along its outline (triangulate()). The original's convex filler draws a concave outline row by row,
// from its leftmost edge to its rightmost (Lombard Street's bends are such), which no fan reproduces.
void fill_triangles(const P3* points, int n, std::vector<uint16_t>& out);

// Sutherland-Hodgman against z >= near: `in` (n points, a convex polygon or a triangle) into `out`
// (room for n + 1 points). Returns the number of points left (0 if all are behind).
int clip_near(const P3* in, int n, float near, P3* out);

// Whether a polygon has (nearly) no area.
bool degenerate(const P3* points, int n);

}  // namespace vette::enhanced::geometry
