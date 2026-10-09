#include "game/model_tools.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace vette::game {
namespace {

constexpr double kPi = 3.14159265358979323846;

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 mul(const Vec3& a, double k) { return {a[0] * k, a[1] * k, a[2] * k}; }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
Vec3 unit(const Vec3& a) {
    const double l = std::sqrt(dot(a, a));
    return l > 1e-12 ? mul(a, 1 / l) : Vec3{0, 0, 0};
}
Vec3 as_vec(const ModelData::Vertex& v) { return {double(v[0]), double(v[1]), double(v[2])}; }
ModelData::Vertex as_vertex(const Vec3& p) {
    ModelData::Vertex v{};
    for (size_t c = 0; c < 3; ++c) v[c] = static_cast<int16_t>(std::clamp(std::lround(p[c]), -32000L, 32000L));
    return v;
}

// Newell's normal of points (not unit length).
Vec3 newell(const std::vector<Vec3>& pts) {
    Vec3 n{0, 0, 0};
    for (size_t i = 0; i < pts.size(); ++i) {
        const Vec3& a = pts[i];
        const Vec3& b = pts[(i + 1) % pts.size()];
        n[0] += (a[1] - b[1]) * (a[2] + b[2]);
        n[1] += (a[2] - b[2]) * (a[0] + b[0]);
        n[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
    return n;
}

// A face's points, sorted per polygon and the polygons sorted: the same for any winding or start.
std::vector<std::vector<uint16_t>> shape_key(const ModelData::Face& f) {
    std::vector<std::vector<uint16_t>> key;
    for (auto prim : f.prims) {
        std::sort(prim.begin(), prim.end());
        key.push_back(std::move(prim));
    }
    std::sort(key.begin(), key.end());
    return key;
}

std::string no_room(const ModelData& m, size_t more) {
    return "no room: that needs " + std::to_string(more) + " more vertices, and the game takes " +
           std::to_string(kMaxVertices - kReferenceVertices) + " (this has " +
           std::to_string(m.verts.size() - kReferenceVertices) + ")";
}

}  // namespace

void faces_changed(ModelData& m) {
    for (auto& o : m.order) o.clear();
}

void delete_vertices(ModelData& m, const std::set<uint16_t>& gone) {
    std::vector<int> remap(m.verts.size(), -1);
    std::vector<ModelData::Vertex> kept;
    for (size_t i = 0; i < m.verts.size(); ++i) {
        if (i >= kReferenceVertices && gone.count(static_cast<uint16_t>(i))) continue;
        remap[i] = static_cast<int>(kept.size());
        kept.push_back(m.verts[i]);
    }
    m.verts = std::move(kept);
    std::vector<ModelData::Face> faces;
    for (ModelData::Face f : m.faces) {
        std::vector<std::vector<uint16_t>> prims;
        for (const auto& prim : f.prims) {
            std::vector<uint16_t> p;
            for (const uint16_t v : prim) {
                if (v < remap.size() && remap[v] >= 0) p.push_back(static_cast<uint16_t>(remap[v]));
            }
            if (p.size() >= (f.lines() ? 2u : 3u)) prims.push_back(std::move(p));
        }
        if (prims.empty()) continue;
        f.prims = std::move(prims);
        faces.push_back(std::move(f));
    }
    m.faces = std::move(faces);
    faces_changed(m);
}

void delete_faces(ModelData& m, const std::set<uint16_t>& gone) {
    std::vector<ModelData::Face> faces;
    for (size_t i = 0; i < m.faces.size(); ++i) {
        if (!gone.count(static_cast<uint16_t>(i))) faces.push_back(m.faces[i]);
    }
    m.faces = std::move(faces);
    faces_changed(m);
}

Vec3 polygon_normal(const ModelData& m, const std::vector<uint16_t>& prim) {
    std::vector<Vec3> pts;
    for (const uint16_t v : prim) pts.push_back(as_vec(m.verts[v]));
    return unit(newell(pts));
}

Vec3 front_of(const ModelData& m, const std::vector<uint16_t>& prim) { return mul(polygon_normal(m, prim), -1); }

bool convex(const ModelData& m, const std::vector<uint16_t>& prim) {
    if (prim.size() <= 3) return true;
    const Vec3 n = polygon_normal(m, prim);
    int sign = 0;
    for (size_t i = 0; i < prim.size(); ++i) {
        const Vec3 a = as_vec(m.verts[prim[i]]), b = as_vec(m.verts[prim[(i + 1) % prim.size()]]);
        const Vec3 c = as_vec(m.verts[prim[(i + 2) % prim.size()]]);
        const double s = dot(cross(sub(b, a), sub(c, b)), n);
        if (std::abs(s) < 1e-6) continue;
        const int sg = s > 0 ? 1 : -1;
        if (sign && sg != sign) return false;
        sign = sg;
    }
    return true;
}

void set_fill_kind(const ModelData& m, ModelData::Face& f) {
    if (f.lines()) return;
    bool all_convex = true;
    for (const auto& prim : f.prims) all_convex = all_convex && convex(m, prim);
    f.flags = static_cast<uint16_t>((f.flags & ~0x0E) | (all_convex ? 0 : 2));
}

void usual_style(const ModelData& m, uint16_t& flags, uint8_t& outline) {
    std::map<std::pair<uint16_t, uint8_t>, int> count;
    for (const auto& f : m.faces) {
        if (!f.lines()) ++count[{static_cast<uint16_t>(f.flags & 0xA001), f.outline}];
    }
    flags = 0x2001;
    outline = 0;
    int best = 0;
    for (const auto& [k, n] : count) {
        if (n > best) {
            best = n;
            flags = k.first;
            outline = k.second;
        }
    }
}

uint16_t vertex_at(ModelData& m, const ModelData::Vertex& p) {
    for (size_t i = kReferenceVertices; i < m.verts.size(); ++i) {
        if (m.verts[i] == p) return static_cast<uint16_t>(i);
    }
    m.verts.push_back(p);
    return static_cast<uint16_t>(m.verts.size() - 1);
}

ModelData::Vertex mirrored(const ModelData::Vertex& v) { return {static_cast<int16_t>(-v[0]), v[1], v[2]}; }

int mirror_twin(const ModelData& m, uint16_t v) {
    if (v < kReferenceVertices || v >= m.verts.size()) return -1;
    if (m.verts[v][0] == 0) return v;
    const ModelData::Vertex want = mirrored(m.verts[v]);
    for (size_t i = kReferenceVertices; i < m.verts.size(); ++i) {
        if (i != v && m.verts[i] == want) return static_cast<int>(i);
    }
    return -1;
}

int mirror_face(const ModelData& m, uint16_t f) {
    if (f >= m.faces.size()) return -1;
    ModelData::Face image = m.faces[f];
    for (auto& prim : image.prims) {
        for (uint16_t& v : prim) {
            const int t = mirror_twin(m, v);
            if (t < 0) return -1;
            v = static_cast<uint16_t>(t);
        }
    }
    const auto key = shape_key(image);
    if (key == shape_key(m.faces[f])) return f;
    for (size_t g = 0; g < m.faces.size(); ++g) {
        if (m.faces[g].lines() == image.lines() && shape_key(m.faces[g]) == key) return static_cast<int>(g);
    }
    return -1;
}

std::vector<std::pair<uint16_t, uint16_t>> mirror_partners(const ModelData& m, const std::set<uint16_t>& moving) {
    std::vector<std::pair<uint16_t, uint16_t>> out;
    for (const uint16_t v : moving) {
        const int t = mirror_twin(m, v);
        if (t == v) out.emplace_back(v, v);
        else if (t >= 0 && !moving.count(static_cast<uint16_t>(t))) out.emplace_back(v, static_cast<uint16_t>(t));
    }
    return out;
}

void keep_mirrored(ModelData& m, const std::vector<std::pair<uint16_t, uint16_t>>& partners) {
    for (const auto& [v, t] : partners) {
        if (v == t) m.verts[v][0] = 0;
        else m.verts[t] = mirrored(m.verts[v]);
    }
}

int add_mirror_face(ModelData& m, uint16_t f) {
    if (f >= m.faces.size() || mirror_face(m, f) >= 0) return -1;
    // Room for the twins it needs.
    size_t missing = 0;
    for (const auto& prim : m.faces[f].prims) {
        for (const uint16_t v : prim) missing += mirror_twin(m, v) < 0 ? 1 : 0;
    }
    if (m.verts.size() + missing > static_cast<size_t>(kMaxVertices)) return -1;
    ModelData::Face image = m.faces[f];
    for (auto& prim : image.prims) {
        for (uint16_t& v : prim) v = vertex_at(m, mirrored(m.verts[v]));
        if (!image.lines()) std::reverse(prim.begin(), prim.end());
    }
    m.faces.push_back(std::move(image));
    faces_changed(m);
    return static_cast<int>(m.faces.size() - 1);
}

std::vector<uint16_t> mirror_copy_vertices(ModelData& m, const std::vector<uint16_t>& verts) {
    std::vector<uint16_t> made;
    for (const uint16_t v : verts) {
        if (v >= m.verts.size()) continue;
        if (m.verts.size() >= static_cast<size_t>(kMaxVertices) && mirror_twin(m, v) < 0) break;
        made.push_back(vertex_at(m, mirrored(m.verts[v])));
    }
    return made;
}

std::vector<uint16_t> mirror_copy_faces(ModelData& m, const std::vector<uint16_t>& faces) {
    std::vector<uint16_t> made;
    for (const uint16_t f : faces) {
        if (f >= m.faces.size()) continue;
        ModelData::Face copy = m.faces[f];
        bool room = true;
        for (auto& prim : copy.prims) {
            for (uint16_t& v : prim) {
                if (m.verts.size() >= static_cast<size_t>(kMaxVertices) && mirror_twin(m, v) < 0) room = false;
                if (room) v = vertex_at(m, mirrored(m.verts[v]));
            }
            if (!copy.lines()) std::reverse(prim.begin(), prim.end());
        }
        if (!room) break;
        m.faces.push_back(std::move(copy));
        made.push_back(static_cast<uint16_t>(m.faces.size() - 1));
    }
    faces_changed(m);
    return made;
}

const char* shape_name(Shape s) {
    switch (s) {
    case Shape::Cube: return "Cube";
    case Shape::Pyramid: return "Pyramid";
    case Shape::Wedge: return "Wedge";
    case Shape::Roof: return "Roof";
    case Shape::Cylinder: return "Cylinder";
    case Shape::Cone: return "Cone";
    case Shape::Sphere: return "Sphere";
    case Shape::Plane: return "Plane";
    }
    return "";
}

namespace {

constexpr int kRound = 8;  // sides of the round shapes

struct ShapeMesh {
    std::vector<Vec3> verts;  // within -1..1
    std::vector<std::vector<int>> faces;
};

ShapeMesh shape_mesh(Shape s) {
    ShapeMesh m;
    // The base square (y = 1, the bottom: y is down) and its corners, south-west first.
    const auto base = [&](double y) {
        m.verts.push_back({-1, y, -1});
        m.verts.push_back({1, y, -1});
        m.verts.push_back({1, y, 1});
        m.verts.push_back({-1, y, 1});
    };
    const auto ring = [&](double y, double r) {
        const int first = static_cast<int>(m.verts.size());
        for (int i = 0; i < kRound; ++i) {
            const double a = 2 * kPi * (i + 0.5) / kRound;
            m.verts.push_back({r * std::cos(a), y, r * std::sin(a)});
        }
        return first;
    };
    switch (s) {
    case Shape::Cube:
        for (int i = 0; i < 8; ++i) m.verts.push_back({i & 4 ? 1.0 : -1.0, i & 2 ? 1.0 : -1.0, i & 1 ? 1.0 : -1.0});
        m.faces = {{0, 2, 3, 1}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 5, 7, 3}};
        break;
    case Shape::Pyramid:
        base(1);
        m.verts.push_back({0, -1, 0});
        m.faces = {{0, 1, 2, 3}, {0, 1, 4}, {1, 2, 4}, {2, 3, 4}, {3, 0, 4}};
        break;
    case Shape::Wedge:  // a ramp rising to the north
        base(1);
        m.verts.push_back({-1, -1, 1});
        m.verts.push_back({1, -1, 1});
        m.faces = {{0, 1, 2, 3}, {3, 2, 5, 4}, {0, 1, 5, 4}, {0, 3, 4}, {1, 2, 5}};
        break;
    case Shape::Roof:  // a ridge running north-south
        base(1);
        m.verts.push_back({0, -1, -1});
        m.verts.push_back({0, -1, 1});
        m.faces = {{0, 1, 2, 3}, {0, 3, 5, 4}, {1, 2, 5, 4}, {0, 1, 4}, {3, 2, 5}};
        break;
    case Shape::Cylinder: {
        const int bottom = ring(1, 1), top = ring(-1, 1);
        std::vector<int> b, t;
        for (int i = 0; i < kRound; ++i) {
            const int j = (i + 1) % kRound;
            m.faces.push_back({bottom + i, bottom + j, top + j, top + i});
            b.push_back(bottom + i);
            t.push_back(top + i);
        }
        m.faces.push_back(b);
        m.faces.push_back(t);
        break;
    }
    case Shape::Cone: {
        const int bottom = ring(1, 1);
        m.verts.push_back({0, -1, 0});
        const int apex = static_cast<int>(m.verts.size() - 1);
        std::vector<int> b;
        for (int i = 0; i < kRound; ++i) {
            m.faces.push_back({bottom + i, bottom + (i + 1) % kRound, apex});
            b.push_back(bottom + i);
        }
        m.faces.push_back(b);
        break;
    }
    case Shape::Sphere: {
        m.verts.push_back({0, -1, 0});  // the top
        int rings[3];
        for (int k = 0; k < 3; ++k) {
            const double lat = kPi / 4 * (1 - k);  // 45, 0, -45 degrees
            rings[k] = ring(-std::sin(lat), std::cos(lat));
        }
        m.verts.push_back({0, 1, 0});  // the bottom
        const int bottom = static_cast<int>(m.verts.size() - 1);
        for (int i = 0; i < kRound; ++i) {
            const int j = (i + 1) % kRound;
            m.faces.push_back({0, rings[0] + i, rings[0] + j});
            for (int k = 0; k < 2; ++k) m.faces.push_back({rings[k] + i, rings[k] + j, rings[k + 1] + j, rings[k + 1] + i});
            m.faces.push_back({bottom, rings[2] + j, rings[2] + i});
        }
        break;
    }
    case Shape::Plane:
        base(0);
        m.faces = {{0, 1, 2, 3}};
        break;
    }
    return m;
}

}  // namespace

int shape_vertices(Shape s) { return static_cast<int>(shape_mesh(s).verts.size()); }

std::vector<uint16_t> add_shape(ModelData& m, Shape s, const Vec3& at, int size, uint8_t colour, std::string& why) {
    ShapeMesh mesh = shape_mesh(s);
    if (m.verts.size() + mesh.verts.size() > static_cast<size_t>(kMaxVertices)) {
        why = no_room(m, mesh.verts.size());
        return {};
    }
    uint16_t usual = 0;
    uint8_t outline = 0;
    usual_style(m, usual, outline);
    const bool open = s == Shape::Plane;
    const double half = std::max(1, size) / 2.0;
    const uint16_t first = static_cast<uint16_t>(m.verts.size());
    Vec3 centre{0, 0, 0};
    for (const Vec3& v : mesh.verts) {
        m.verts.push_back(as_vertex(add(at, mul(v, half))));
        centre = add(centre, mul(v, 1.0 / static_cast<double>(mesh.verts.size())));
    }
    std::vector<uint16_t> made;
    for (const auto& f : mesh.faces) {
        // Wound to face out from the shape's middle (the plane: up), from its own points, before rounding.
        std::vector<Vec3> pts;
        Vec3 middle{0, 0, 0};
        for (const int i : f) {
            pts.push_back(mesh.verts[static_cast<size_t>(i)]);
            middle = add(middle, mul(mesh.verts[static_cast<size_t>(i)], 1.0 / static_cast<double>(f.size())));
        }
        const Vec3 out = open ? Vec3{0, -1, 0} : sub(middle, centre);
        const bool reverse = dot(mul(newell(pts), -1), out) < 0;
        ModelData::Face face;
        face.colour = colour;
        face.outline = outline;
        face.flags = static_cast<uint16_t>((usual & 0x2000) | (open ? 0 : 0x0001));
        std::vector<uint16_t> prim;
        for (const int i : f) prim.push_back(static_cast<uint16_t>(first + i));
        if (reverse) std::reverse(prim.begin(), prim.end());
        face.prims.push_back(std::move(prim));
        set_fill_kind(m, face);
        m.faces.push_back(std::move(face));
        made.push_back(static_cast<uint16_t>(m.faces.size() - 1));
    }
    faces_changed(m);
    return made;
}

std::vector<uint16_t> extrude(ModelData& m, const std::vector<uint16_t>& faces, double distance, std::string& why) {
    std::vector<uint16_t> fills;
    for (const uint16_t f : faces) {
        if (f < m.faces.size() && !m.faces[f].lines() && std::find(fills.begin(), fills.end(), f) == fills.end()) fills.push_back(f);
    }
    if (fills.empty()) {
        why = "pick faces to extrude (filled ones, not lines)";
        return {};
    }
    using Edge = std::pair<uint16_t, uint16_t>;
    const auto edges_of = [&](uint16_t f) {
        std::vector<Edge> out;
        for (const auto& prim : m.faces[f].prims) {
            for (size_t i = 0; i < prim.size(); ++i) out.emplace_back(prim[i], prim[(i + 1) % prim.size()]);
        }
        return out;
    };
    const auto undirected = [](Edge e) { return e.first < e.second ? e : Edge{e.second, e.first}; };

    // Groups: faces joined by an edge.
    std::vector<int> group(fills.size(), -1);
    int groups = 0;
    for (size_t i = 0; i < fills.size(); ++i) {
        if (group[i] >= 0) continue;
        group[i] = groups;
        std::vector<size_t> todo{i};
        while (!todo.empty()) {
            const size_t a = todo.back();
            todo.pop_back();
            std::set<Edge> mine;
            for (const Edge e : edges_of(fills[a])) mine.insert(undirected(e));
            for (size_t b = 0; b < fills.size(); ++b) {
                if (group[b] >= 0) continue;
                for (const Edge e : edges_of(fills[b])) {
                    if (mine.count(undirected(e))) {
                        group[b] = groups;
                        todo.push_back(b);
                        break;
                    }
                }
            }
        }
        ++groups;
    }
    // Room for the new vertices: one for each vertex of each group.
    size_t needed = 0;
    for (int g = 0; g < groups; ++g) {
        std::set<uint16_t> vs;
        for (size_t i = 0; i < fills.size(); ++i) {
            if (group[i] != g) continue;
            for (const auto& prim : m.faces[fills[i]].prims) vs.insert(prim.begin(), prim.end());
        }
        needed += vs.size();
    }
    if (m.verts.size() + needed > static_cast<size_t>(kMaxVertices)) {
        why = no_room(m, needed);
        return {};
    }
    const size_t first_new_face = m.faces.size();
    for (int g = 0; g < groups; ++g) {
        std::vector<uint16_t> mine;
        for (size_t i = 0; i < fills.size(); ++i) {
            if (group[i] == g) mine.push_back(fills[i]);
        }
        // The way out: the group's fronts together (a group facing every way goes face by face).
        Vec3 sum{0, 0, 0};
        for (const uint16_t f : mine) {
            for (const auto& prim : m.faces[f].prims) sum = add(sum, front_of(m, prim));
        }
        if (std::sqrt(dot(sum, sum)) < 0.3 && mine.size() > 1) {
            for (const uint16_t f : mine) {
                std::string w;
                extrude(m, {f}, distance, w);
            }
            continue;
        }
        const Vec3 step = mul(unit(sum), distance);
        std::map<uint16_t, uint16_t> moved;
        for (const uint16_t f : mine) {
            for (const auto& prim : m.faces[f].prims) {
                for (const uint16_t v : prim) {
                    if (moved.count(v)) continue;
                    m.verts.push_back(as_vertex(add(as_vec(m.verts[v]), step)));
                    moved[v] = static_cast<uint16_t>(m.verts.size() - 1);
                }
            }
        }
        // The group's edge: its faces' edges that no other face of the group runs back along.
        std::set<Edge> directed;
        for (const uint16_t f : mine) {
            for (const Edge e : edges_of(f)) directed.insert(e);
        }
        bool attached = false;
        std::vector<ModelData::Face> sides;
        for (const uint16_t f : mine) {
            for (const Edge e : edges_of(f)) {
                if (directed.count({e.second, e.first})) continue;
                for (size_t o = 0; o < first_new_face && !attached; ++o) {
                    if (std::find(mine.begin(), mine.end(), static_cast<uint16_t>(o)) != mine.end() || m.faces[o].lines()) continue;
                    for (const Edge oe : edges_of(static_cast<uint16_t>(o))) {
                        if (undirected(oe) == undirected(e)) attached = true;
                    }
                }
                ModelData::Face side = m.faces[f];
                side.prims = {{e.first, e.second, moved[e.second], moved[e.first]}};
                set_fill_kind(m, side);
                sides.push_back(std::move(side));
            }
        }
        // Standing free: a base stays where the faces were, facing the other way.
        if (!attached) {
            for (const uint16_t f : mine) {
                ModelData::Face base = m.faces[f];
                for (auto& prim : base.prims) std::reverse(prim.begin(), prim.end());
                sides.push_back(std::move(base));
            }
        }
        for (const uint16_t f : mine) {
            for (auto& prim : m.faces[f].prims) {
                for (uint16_t& v : prim) v = moved[v];
            }
        }
        for (auto& s : sides) m.faces.push_back(std::move(s));
    }
    faces_changed(m);
    return fills;
}

}  // namespace vette::game
