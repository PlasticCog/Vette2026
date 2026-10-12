#include "enhanced/menu_car.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

#include "enhanced/scene_geometry.h"
#include "enhanced/world.h"
#include "game/x86.h"
#include "host/machine.h"

namespace vette::enhanced {
namespace {

using host::Cpu;
using game::kDataSeg;
using game::rd16;

constexpr uint16_t kCode = game::emu_seg(0x3009);
constexpr uint16_t kScreen = 0xE074, kScreenLeft = 0xE0E2;  // the opponent screen: its entry, its way out
constexpr uint16_t kDrawCar = 0xE470;    // inside 3009:E44E, AX = the model
constexpr uint16_t kCarDrawn = 0xE476;   // back from draw_model
constexpr uint16_t kTransformed = 0xB7DD;  // draw_model, its vertices in camera space (DS:266E, 6 bytes each)
constexpr uint16_t kCamVerts = 0x266E, kVertexCount = 0xE020;
constexpr uint16_t kCentreX = 0x3169, kCentreY = 0x316B, kOutlines = 0xE0D8;
constexpr uint64_t kStaleNs = 500'000'000;

struct V {
    float x, y, z;
};

// A drawing's placement of the model: camera point = origin + R * model point.
struct Placement {
    V origin{};
    double r[3][3] = {};  // columns: the model's x, y, z axes in camera space
    V place(const std::array<int16_t, 3>& m) const {
        return {static_cast<float>(origin.x + r[0][0] * m[0] + r[0][1] * m[1] + r[0][2] * m[2]),
                static_cast<float>(origin.y + r[1][0] * m[0] + r[1][1] * m[1] + r[1][2] * m[2]),
                static_cast<float>(origin.z + r[2][0] * m[0] + r[2][1] * m[1] + r[2][2] * m[2])};
    }
};

// The placement a drawing's camera-space vertices came from, off its reference frame (the model's vertices
// 0-3: the origin, and points 50 units along -x, +z and +y); false unless every vertex agrees with it to
// within the original's rounding (the reference frame is then not what it should be).
bool placement_of(const std::vector<V>& cam, const game::ModelData& md, Placement& out) {
    if (cam.size() < 4 || md.verts.size() != cam.size()) return false;
    const V o = cam[0];
    out.origin = o;
    const V ax[3] = {{(o.x - cam[1].x) / 50, (o.y - cam[1].y) / 50, (o.z - cam[1].z) / 50},
                     {(cam[3].x - o.x) / 50, (cam[3].y - o.y) / 50, (cam[3].z - o.z) / 50},
                     {(cam[2].x - o.x) / 50, (cam[2].y - o.y) / 50, (cam[2].z - o.z) / 50}};
    for (int c = 0; c < 3; ++c) {
        out.r[0][c] = ax[c].x;
        out.r[1][c] = ax[c].y;
        out.r[2][c] = ax[c].z;
    }
    // Only the vertices the view's faces use: the original leaves the others untransformed (stale).
    int octant = 0;
    for (int i = 1; i <= 3; ++i) {
        const V d{cam[static_cast<size_t>(i)].x - o.x, cam[static_cast<size_t>(i)].y - o.y, cam[static_cast<size_t>(i)].z - o.z};
        if (d.x * o.x + d.y * o.y + d.z * o.z < 0) octant |= 1 << (i - 1);
    }
    std::vector<bool> used(cam.size(), false);
    for (const uint16_t fi : md.order[static_cast<size_t>(octant)]) {
        if (fi >= md.faces.size()) continue;
        for (const auto& prim : md.faces[fi].prims) {
            for (const uint16_t v : prim) {
                if (v < used.size()) used[v] = true;
            }
        }
    }
    for (size_t i = 0; i < cam.size(); ++i) {
        if (!used[i]) continue;
        const V p = out.place(md.verts[i]);
        if (std::fabs(p.x - cam[i].x) > 4 || std::fabs(p.y - cam[i].y) > 4 || std::fabs(p.z - cam[i].z) > 4) return false;
    }
    return true;
}

// The placement a fraction t of the way from a to b: the origin between theirs, the turn from a's axes
// to b's taken t of the way round the same axis.
Placement between(const Placement& a, const Placement& b, double t) {
    Placement out;
    out.origin = {static_cast<float>(a.origin.x + (b.origin.x - a.origin.x) * t),
                  static_cast<float>(a.origin.y + (b.origin.y - a.origin.y) * t),
                  static_cast<float>(a.origin.z + (b.origin.z - a.origin.z) * t)};
    // M = b * a^T: the turn from a to b.
    double m[3][3] = {};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            for (int k = 0; k < 3; ++k) m[i][j] += b.r[i][k] * a.r[j][k];
        }
    }
    const double cos_angle = std::clamp((m[0][0] + m[1][1] + m[2][2] - 1) / 2, -1.0, 1.0);
    const double angle = std::acos(cos_angle);
    double kx = m[2][1] - m[1][2], ky = m[0][2] - m[2][0], kz = m[1][0] - m[0][1];
    const double kl = std::sqrt(kx * kx + ky * ky + kz * kz);
    if (angle < 1e-4 || kl < 1e-9) {
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) out.r[i][j] = a.r[i][j];
        }
        return out;
    }
    kx /= kl;
    ky /= kl;
    kz /= kl;
    // Rodrigues: R(t) = I + sin(ta) K + (1 - cos(ta)) K^2, then onto a.
    const double sn = std::sin(angle * t), cs = 1 - std::cos(angle * t);
    const double k[3][3] = {{0, -kz, ky}, {kz, 0, -kx}, {-ky, kx, 0}};
    double rt[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double k2 = 0;
            for (int q = 0; q < 3; ++q) k2 += k[i][q] * k[q][j];
            rt[i][j] = (i == j ? 1 : 0) + sn * k[i][j] + cs * k2;
        }
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            out.r[i][j] = 0;
            for (int q = 0; q < 3; ++q) out.r[i][j] += rt[i][q] * a.r[q][j];
        }
    }
    return out;
}

}  // namespace

struct MenuCar::Capture {
    struct Shot {
        int model = -1;
        std::vector<V> cam;
        float cx = 160, cy = 100;
        bool outlines = true;
        uint64_t at_ns = 0;
    };
    bool on_screen = false, drawing = false;
    Shot current, last, before;  // being drawn, the last drawn, the one before it
    std::map<int, game::ModelData> models;  // as the game has them (with a set of one's own in place)
};

MenuCar::MenuCar(host::Machine& machine) : machine_(machine), capture_(std::make_shared<Capture>()) {
    Cpu& cpu = machine.cpu();
    auto c = capture_;
    cpu.add_watch(Cpu::linear(kCode, kScreen), [c](Cpu&) { c->on_screen = true; });
    cpu.add_watch(Cpu::linear(kCode, kScreenLeft), [c](Cpu&) {
        c->on_screen = false;
        c->last.at_ns = c->before.at_ns = 0;
    });
    cpu.add_watch(Cpu::linear(kCode, kDrawCar), [c](Cpu& cpu_) {
        c->drawing = true;
        c->current = {};
        c->current.model = cpu_.regs.r[host::AX];
    });
    cpu.add_watch(Cpu::linear(kCode, kTransformed), [c](Cpu& cpu_) {
        if (!c->drawing) return;
        host::Memory& m = cpu_.memory();
        const uint16_t n = rd16(m, kDataSeg, kVertexCount);
        if (n > 128) return;
        c->current.cam.clear();
        for (uint16_t i = 0; i < n; ++i) {
            const auto at = static_cast<uint16_t>(kCamVerts + 6 * i);
            c->current.cam.push_back({static_cast<float>(static_cast<int16_t>(rd16(m, kDataSeg, at))),
                                      static_cast<float>(static_cast<int16_t>(rd16(m, kDataSeg, static_cast<uint16_t>(at + 2)))),
                                      static_cast<float>(static_cast<int16_t>(rd16(m, kDataSeg, static_cast<uint16_t>(at + 4))))});
        }
        c->current.cx = static_cast<float>(static_cast<int16_t>(rd16(m, kDataSeg, kCentreX)));
        c->current.cy = static_cast<float>(static_cast<int16_t>(rd16(m, kDataSeg, kCentreY)));
        c->current.outlines = game::rd8(m, kDataSeg, kOutlines) != 0;
    });
    host::Machine* machine_ptr = &machine;
    cpu.add_watch(Cpu::linear(kCode, kCarDrawn), [c, machine_ptr](Cpu&) {
        if (!c->drawing) return;
        c->drawing = false;
        if (c->current.cam.empty()) return;
        c->before = std::move(c->last);
        c->last = c->current;
        c->last.at_ns = machine_ptr->emulated_ns();
    });
}

MenuCar::~MenuCar() = default;

bool MenuCar::build(uint64_t now_ns, float line_width, bool smooth, bool dither, Scene& out) const {
    const Capture::Shot& last = capture_->last;
    if (!capture_->on_screen || last.at_ns == 0 || now_ns - last.at_ns > kStaleNs) return false;
    auto it = capture_->models.find(last.model);
    if (it == capture_->models.end()) {
        auto read = game::read_model(machine_.memory(), last.model);
        if (!read) return false;
        it = capture_->models.emplace(last.model, std::move(*read)).first;
    }
    const game::ModelData& md = it->second;
    if (md.verts.size() != last.cam.size()) return false;
    // Smooth: from the drawing before the last towards the last over the time between them (the last's
    // own vertices once that has gone by). The car is turned rigidly between the two: each drawing's
    // placement is read off the model's reference frame (vertices 0-3: the origin and its axes, 50 units
    // long), and the model's own vertices are placed at the angle in between. Blending the two drawings'
    // vertices instead would squash the car between them, and a tail light and the panel it sits on could
    // then disagree about which way they face, the light showing through the body.
    Capture::Shot blended;
    const Capture::Shot* shot = &last;
    const Capture::Shot& prev = capture_->before;
    if (smooth && prev.at_ns != 0 && prev.model == last.model && prev.cam.size() == last.cam.size() && last.at_ns > prev.at_ns) {
        const double t = std::min(1.0, static_cast<double>(now_ns - last.at_ns) / static_cast<double>(last.at_ns - prev.at_ns));
        Placement a, b;
        if (placement_of(prev.cam, md, a) && placement_of(last.cam, md, b)) {
            blended = last;
            const Placement at = between(a, b, t);
            for (size_t i = 0; i < blended.cam.size(); ++i) blended.cam[i] = at.place(md.verts[i]);
        } else {
            blended = last;
            for (size_t i = 0; i < blended.cam.size(); ++i) {
                blended.cam[i].x = static_cast<float>(prev.cam[i].x + (last.cam[i].x - prev.cam[i].x) * t);
                blended.cam[i].y = static_cast<float>(prev.cam[i].y + (last.cam[i].y - prev.cam[i].y) * t);
                blended.cam[i].z = static_cast<float>(prev.cam[i].z + (last.cam[i].z - prev.cam[i].z) * t);
            }
        }
        shot = &blended;
    }
    const Capture::Shot& s = *shot;

    out.vertices.clear();
    out.indices.clear();
    out.view_x0 = kBoxX;
    out.view_y0 = kBoxY;
    out.view_x1 = kBoxX + kBoxW;
    out.view_y1 = kBoxY + kBoxH;
    const auto& cv = s.cam;
    const auto screen = [&](const V& v, float& x, float& y) {
        x = s.cx + v.x * 256 / v.z;
        y = s.cy + v.y * 256 / v.z;
    };
    const auto vertex = [&](const V& v, const SceneColour& c, float a, float dx = 0, float dy = 0) {
        SceneVertex sv;
        screen(v, sv.x, sv.y);
        sv.x += dx;
        sv.y += dy;
        sv.r = c.r;
        sv.g = c.g;
        sv.b = c.b;
        sv.a = a;
        sv.r2 = c.r2;
        sv.g2 = c.g2;
        sv.b2 = c.b2;
        sv.a2 = a;
        sv.dither = c.dither;
        sv.depth = 0;  // in the original's order (its faces back to front), as it paints them: no depth test
        out.indices.push_back(static_cast<int32_t>(out.vertices.size()));
        out.vertices.push_back(sv);
    };
    const auto segment = [&](const V& a, const V& b, const SceneColour& c) {
        float ax = 0, ay = 0, bx = 0, by = 0;
        screen(a, ax, ay);
        screen(b, bx, by);
        const float dx = bx - ax, dy = by - ay, l = std::sqrt(dx * dx + dy * dy);
        if (l < 1e-4f) return;
        const float nx = -dy / l * line_width / 2, ny = dx / l * line_width / 2;
        vertex(a, c, 1, nx, ny);
        vertex(a, c, 1, -nx, -ny);
        vertex(b, c, 1, nx, ny);
        vertex(a, c, 1, -nx, -ny);
        vertex(b, c, 1, -nx, -ny);
        vertex(b, c, 1, nx, ny);
    };
    const auto dot = [](const V& a, const V& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    const auto cross = [](const V& a, const V& b) { return V{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; };
    const auto minus = [](const V& a, const V& b) { return V{a.x - b.x, a.y - b.y, a.z - b.z}; };
    // 9CAF: the view's octant, a bit per reference axis set when (V_i - V0) . V0 < 0.
    int octant = 0;
    for (int i = 1; i <= 3; ++i) {
        if (dot(minus(cv[static_cast<size_t>(i)], cv[0]), cv[0]) < 0) octant |= 1 << (i - 1);
    }
    for (const uint16_t fi : md.order[static_cast<size_t>(octant)]) {
        if (fi >= md.faces.size()) continue;
        const game::ModelData::Face& f = md.faces[fi];
        if (f.flags & 0x4000) continue;
        Colour colour;
        colour.raw = f.colour;
        const SceneColour col = scene_colour(colour, dither);
        const float alpha = (f.flags & 0x8000) ? 0.5f : 1.0f;
        for (const auto& prim : f.prims) {
            bool near = true;
            for (const uint16_t v : prim) near = near && v < cv.size() && cv[v].z >= 1;
            if (!near) continue;
            if (f.lines()) {
                for (size_t k = 0; k + 1 < prim.size(); ++k) segment(cv[prim[k]], cv[prim[k + 1]], ega_colour(f.colour & 15));
                continue;
            }
            const size_t n = prim.size();
            if ((f.flags & 1) && n >= 3 && dot(cv[prim[0]], cross(cv[prim[1]], cv[prim[2]])) <= 0) continue;  // 9CD8
            std::vector<geometry::P3> pts;
            for (const uint16_t v : prim) pts.push_back({cv[v].x, cv[v].y, cv[v].z});
            std::vector<uint16_t> tri;
            if (n >= 3) geometry::triangulate(pts.data(), static_cast<int>(n), tri);
            for (const uint16_t t : tri) vertex(cv[prim[t]], col, alpha);
            if (s.outlines && (f.flags & 0x2000)) {
                for (size_t k = 0; k < n; ++k) segment(cv[prim[k]], cv[prim[(k + 1) % n]], ega_colour(f.outline & 15));
            }
        }
    }
    out.stats.triangles = static_cast<int>(out.indices.size() / 3);
    return true;
}

}  // namespace vette::enhanced
