#pragma once
// Building with a model (game/model_pack.h) in the object editor: shapes to start from, faces extruded,
// mirror images across the model's middle (x = 0, east-west, as the original's cars are made), and the
// editing steps the editor's tools share. Model axes: x east, y down (up is -y), z north. A face's front
// is the side the game draws when the face hides its back: seen from c, a polygon p0, p1, p2... shows its
// front when (p0 - c) . ((p1 - p0) x (p2 - p0)) > 0.

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "game/model_pack.h"

namespace vette::game {

using Vec3 = std::array<double, 3>;

// The face set changed: the view orders are made again from the faces when the model is written.
void faces_changed(ModelData& m);
// Removes vertices (not the reference frame): faces lose them, and those left with too few points go.
void delete_vertices(ModelData& m, const std::set<uint16_t>& gone);
void delete_faces(ModelData& m, const std::set<uint16_t>& gone);

// A polygon's normal (Newell), unit length, in model axes; the way its front faces.
Vec3 polygon_normal(const ModelData& m, const std::vector<uint16_t>& prim);
Vec3 front_of(const ModelData& m, const std::vector<uint16_t>& prim);
// Whether a polygon is convex in its plane (the original's plain filler takes only convex ones), and a
// face's fill kind set by it: the convex filler where it will do, else the concave one.
bool convex(const ModelData& m, const std::vector<uint16_t>& prim);
void set_fill_kind(const ModelData& m, ModelData::Face& f);
// The most usual flags (hides its back, outlined, see-through) and outline of the model's fills.
void usual_style(const ModelData& m, uint16_t& flags, uint8_t& outline);
// The vertex at `p`, or a new one there.
uint16_t vertex_at(ModelData& m, const ModelData::Vertex& p);

// --- Symmetry across x = 0 ---
ModelData::Vertex mirrored(const ModelData::Vertex& v);
// A vertex's mirror twin: itself on the middle, -1 if there's none.
int mirror_twin(const ModelData& m, uint16_t v);
// The face through a face's twins (the same kind, any order): itself if it's symmetric, -1 if none.
int mirror_face(const ModelData& m, uint16_t f);
// For keeping a one-sided change symmetric: each of `moving`'s vertices with the twin that follows it
// (not itself moving), or with itself if it's on the middle (it stays there).
std::vector<std::pair<uint16_t, uint16_t>> mirror_partners(const ModelData& m, const std::set<uint16_t>& moving);
void keep_mirrored(ModelData& m, const std::vector<std::pair<uint16_t, uint16_t>>& partners);
// Adds the mirror image of face `f` (its twins made where missing), unless it's symmetric or its image
// is there already. The new face's index, or -1.
int add_mirror_face(ModelData& m, uint16_t f);
// Mirror copies (the editor's M): of vertices, and of faces with their vertices, wound to face the
// mirror way. Returns what was made (fewer if the model ran out of room).
std::vector<uint16_t> mirror_copy_vertices(ModelData& m, const std::vector<uint16_t>& verts);
std::vector<uint16_t> mirror_copy_faces(ModelData& m, const std::vector<uint16_t>& faces);

// --- Shapes to build with ---
enum class Shape { Cube, Pyramid, Wedge, Roof, Cylinder, Cone, Sphere, Plane };
constexpr int kShapeCount = 8;
const char* shape_name(Shape s);
int shape_vertices(Shape s);
// Adds shape `s`, `size` units across, centred on `at`: faces coloured `colour` and outlined like the
// model's other faces, each wound to face out and hiding its back (the plane, open, shows both sides).
// Returns its faces; none, with `why`, if the model hasn't room for its vertices.
std::vector<uint16_t> add_shape(ModelData& m, Shape s, const Vec3& at, int size, uint8_t colour, std::string& why);

// Extrudes faces `distance` units out of their fronts: each connected group as one (along its faces'
// fronts together), with side faces along the group's edge in its faces' style; a group standing free
// keeps a base where it was. Lines are left. Returns the moved faces; none, with `why`, if the model
// hasn't room for the new vertices.
std::vector<uint16_t> extrude(ModelData& m, const std::vector<uint16_t>& faces, double distance, std::string& why);

}  // namespace vette::game
