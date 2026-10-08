#include "enhanced/object_models.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

#include "enhanced/world_decode.h"
#include "enhanced/world_probe.h"
#include "host/machine.h"

namespace vette::enhanced {
namespace {

constexpr uint16_t kModelTable = 0x6FF8;
constexpr uint16_t kFarColour = 7;
constexpr int kMaxVerts = game::kMaxVertices - game::kReferenceVertices;
constexpr game::ModelData::Vertex kReference[game::kReferenceVertices] = {{0, 0, 0}, {-50, 0, 0}, {0, 0, 50}, {0, 50, 0}};

using V = std::array<double, 3>;
V sub(V a, V b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V cross(V a, V b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double dot(V a, V b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

DrawState most_detailed() {
    DrawState s;
    s.windows = true;
    return s;
}

// A part's vertices, object-local in world axes (x north, y east, z up); false if the part can't be a
// model's (turned to the camera, animated, or a model itself).
bool part_vertices(const Part& p, std::vector<Vec3d>& out) {
    out.clear();
    if (p.source == Part::Source::Model) return false;
    if (p.rotation == Part::Rotation::CameraYaw || p.rotation == Part::Rotation::Animated) return false;
    for (const Vec3i& v : p.verts) {
        Vec3d w{double(v.x), double(v.y), double(v.z)};
        if (p.source == Part::Source::Plain) {
            if (p.rotation == Part::Rotation::Fixed) w = rotate_local(w, p.yaw, p.pitch, p.roll);
            w = {w.x + p.origin.x, w.y + p.origin.y, w.z + p.origin.z};
        }
        out.push_back(w);
    }
    return true;
}

// World axes to the model's: x east, y down, z north.
V model_axes(const Vec3d& w) { return {w.y, -w.z, w.x}; }

}  // namespace

std::optional<game::ModelData> routine_model(const World& world, uint16_t routine, std::string& why) {
    const Routine* r = world.routine(routine);
    if (!r) {
        why = "no such object";
        return std::nullopt;
    }
    if (r->compound) {
        why = "it's a structure over several cells";
        return std::nullopt;
    }
    if (r->camera_dependent) {
        why = "it turns to the camera";
        return std::nullopt;
    }
    const Variant* v = r->select(most_detailed());
    if (!v || v->parts.empty()) {
        why = "it draws nothing";
        return std::nullopt;
    }
    game::ModelData model;
    model.verts.assign(std::begin(kReference), std::end(kReference));
    std::map<std::array<int16_t, 3>, uint16_t> index;
    std::vector<std::vector<uint16_t>> part_index(v->parts.size());
    std::vector<std::vector<V>> part_axes(v->parts.size());
    std::vector<Vec3d> world_verts;
    for (size_t pi = 0; pi < v->parts.size(); ++pi) {
        if (!part_vertices(v->parts[pi], world_verts)) {
            why = v->parts[pi].source == Part::Source::Model ? "it's drawn with a model (edit that)" : "it turns or moves";
            return std::nullopt;
        }
        for (const Vec3d& w : world_verts) {
            const V a = model_axes(w);
            std::array<int16_t, 3> q{};
            for (int c = 0; c < 3; ++c) {
                const double r_ = std::round(a[static_cast<size_t>(c)]);
                if (std::fabs(r_) > 32767) {
                    why = "it's too big for a model";
                    return std::nullopt;
                }
                q[static_cast<size_t>(c)] = static_cast<int16_t>(r_);
            }
            auto [it, added] = index.emplace(q, static_cast<uint16_t>(model.verts.size()));
            if (added) model.verts.push_back(q);
            part_index[pi].push_back(it->second);
            part_axes[pi].push_back(a);
        }
    }
    if (static_cast<int>(model.verts.size()) - game::kReferenceVertices > kMaxVerts) {
        why = "it has more than " + std::to_string(kMaxVerts) + " vertices";
        return std::nullopt;
    }
    for (size_t pi = 0; pi < v->parts.size(); ++pi) {
        const Part& part = v->parts[pi];
        for (const Prim& prim : part.prims) {
            game::ModelData::Face f;
            f.colour = prim.colour.raw;
            std::vector<uint16_t> pts;
            std::vector<V> at;
            const uint32_t n = prim.kind == Prim::Kind::Line ? 2u : prim.count;
            for (uint32_t k = 0; k < n; ++k) {
                const uint16_t i = part.indices[prim.first + k];
                if (i >= part_index[pi].size()) {
                    why = "a face uses a vertex that isn't there";
                    return std::nullopt;
                }
                pts.push_back(part_index[pi][i]);
                at.push_back(part_axes[pi][i]);
            }
            if (pts.size() > static_cast<size_t>(game::kMaxPoints)) {
                why = "a face has more than " + std::to_string(game::kMaxPoints) + " points";
                return std::nullopt;
            }
            if (prim.kind == Prim::Kind::Line || pts.size() == 2) {
                f.flags = 0x0004;  // a polyline
            } else {
                if (prim.kind == Prim::Kind::PolygonAlt) f.flags = 0x0002;  // the concave filler
                if (prim.cull.kind != Cull::Kind::None && pts.size() >= 3 && prim.cull.part < part_axes.size()) {
                    // Its front: where the routine's own test shows it (world.cpp face_visible): the cull's
                    // plane normal in these axes, or from the point behind to the face. The model's test
                    // shows a face whose first three points' normal points away from the camera's side
                    // ((p0 - camera) . n > 0), so wind it that way round and start at a convex corner.
                    const auto& cv = part_axes[prim.cull.part];
                    V front{0, 0, 0};
                    bool ok = true;
                    for (const uint16_t i : prim.cull.v) ok = ok && i < cv.size();
                    if (ok && prim.cull.kind == Cull::Kind::Plane) {
                        front = cross(sub(cv[prim.cull.v[1]], cv[prim.cull.v[0]]), sub(cv[prim.cull.v[2]], cv[prim.cull.v[0]]));
                    } else if (ok) {
                        front = sub(cv[prim.cull.v[1]], cv[prim.cull.v[0]]);
                    }
                    V normal{0, 0, 0};  // Newell's
                    for (size_t k = 0; k < at.size(); ++k) {
                        const V& a = at[k];
                        const V& b = at[(k + 1) % at.size()];
                        normal[0] += (a[1] - b[1]) * (a[2] + b[2]);
                        normal[1] += (a[2] - b[2]) * (a[0] + b[0]);
                        normal[2] += (a[0] - b[0]) * (a[1] + b[1]);
                    }
                    if (ok && dot(front, front) > 0 && dot(normal, normal) > 0) {
                        if (dot(normal, front) < 0) {
                            std::reverse(pts.begin(), pts.end());
                            std::reverse(at.begin(), at.end());
                            normal = {-normal[0], -normal[1], -normal[2]};
                        }
                        // (Newell's normal is the polygon's own: same orientation for its convex corners.)
                        for (size_t s = 0; s < at.size(); ++s) {
                            const V& p0 = at[s];
                            const V& p1 = at[(s + 1) % at.size()];
                            const V& p2 = at[(s + 2) % at.size()];
                            if (dot(cross(sub(p1, p0), sub(p2, p0)), normal) > 1e-6) {
                                std::rotate(pts.begin(), pts.begin() + static_cast<std::ptrdiff_t>(s), pts.end());
                                break;
                            }
                        }
                        f.flags |= 0x0001;
                    }
                }
            }
            f.prims.push_back(std::move(pts));
            model.faces.push_back(std::move(f));
        }
    }
    if (model.faces.empty()) {
        why = "it draws nothing";
        return std::nullopt;
    }
    game::make_orders(model);
    if (const std::string bad = game::check_model(model); !bad.empty()) {
        why = bad;
        return std::nullopt;
    }
    return model;
}

std::vector<CityObject> city_objects(const World& world) {
    std::map<uint16_t, int> cells;
    for (int cx = 0; cx < world.cells_x(); ++cx) {
        for (int cy = 0; cy < world.cells_y(); ++cy) {
            for (const ListEntry& e : world.types[world.cell(cx, cy).type].list2) ++cells[e.routine];
        }
    }
    std::vector<CityObject> out;
    for (const auto& [routine, n] : cells) {
        std::string why;
        const auto model = routine_model(world, routine, why);
        if (!model) continue;
        // Its size: the tallest buildings rise some 1000 units (250 ft).
        double lo[3] = {1e9, 1e9, 1e9}, hi[3] = {-1e9, -1e9, -1e9};
        for (size_t i = game::kReferenceVertices; i < model->verts.size(); ++i) {
            for (int c = 0; c < 3; ++c) {
                lo[c] = std::min(lo[c], double(model->verts[i][static_cast<size_t>(c)]));
                hi[c] = std::max(hi[c], double(model->verts[i][static_cast<size_t>(c)]));
            }
        }
        const double height = hi[1] - lo[1], width = std::max(hi[0] - lo[0], hi[2] - lo[2]);
        CityObject o;
        o.routine = routine;
        o.cells = n;
        o.name = height >= 700 ? "Tall building" : (height >= 200 || width >= 500) ? "Building" : "Street object";
        out.push_back(std::move(o));
    }
    std::stable_sort(out.begin(), out.end(), [](const CityObject& a, const CityObject& b) { return a.cells > b.cells; });
    return out;
}

bool use_object_models(World& world, host::Machine& machine, const std::vector<std::pair<uint16_t, int>>& objects,
                       std::string& error) {
    const ImageView img{machine.memory().ram()};
    for (const auto& [routine, id] : objects) {
        if (id < 0 || id >= kModelSlots) {
            error = "model " + std::to_string(id) + " is past the slots";
            return false;
        }
        const uint16_t header = img.u16(addr::kModelSeg, static_cast<uint16_t>(kModelTable + 8 * id));
        Model m;
        std::string e;
        if (header == 0 || !decode_model_mesh(img, addr::kModelSeg, header, m.near_mesh, e)) {
            error = "model " + std::to_string(id) + ": " + (e.empty() ? std::string("not in the table") : e);
            return false;
        }
        m.far_mesh = m.near_mesh;
        m.far_colour = kFarColour;
        m.present = true;
        world.models[static_cast<size_t>(id)] = std::move(m);
        const auto it = std::find_if(world.routines.begin(), world.routines.end(), [r = routine](const Routine& x) { return x.address == r; });
        if (it == world.routines.end()) {
            error = "no routine " + std::to_string(routine);
            return false;
        }
        Variant v;
        Part p;
        p.source = Part::Source::Model;
        p.rotation = Part::Rotation::None;
        p.model = static_cast<uint8_t>(id);
        v.parts.push_back(std::move(p));
        v.primitives = 1;
        it->variants.assign(1, std::move(v));
        it->camera_dependent = false;
    }
    return true;
}

}  // namespace vette::enhanced
