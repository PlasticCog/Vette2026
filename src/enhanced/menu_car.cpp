#include "enhanced/menu_car.h"

#include <algorithm>
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

bool MenuCar::build(uint64_t now_ns, float line_width, bool smooth, Scene& out) const {
    const Capture::Shot& last = capture_->last;
    if (!capture_->on_screen || last.at_ns == 0 || now_ns - last.at_ns > kStaleNs) return false;
    // Smooth: from the drawing before the last towards the last over the time between them (the last's
    // own vertices once that has gone by).
    Capture::Shot blended;
    const Capture::Shot* shot = &last;
    const Capture::Shot& prev = capture_->before;
    if (smooth && prev.at_ns != 0 && prev.model == last.model && prev.cam.size() == last.cam.size() && last.at_ns > prev.at_ns) {
        const double t = std::min(1.0, static_cast<double>(now_ns - last.at_ns) / static_cast<double>(last.at_ns - prev.at_ns));
        blended = last;
        for (size_t i = 0; i < blended.cam.size(); ++i) {
            blended.cam[i].x = static_cast<float>(prev.cam[i].x + (last.cam[i].x - prev.cam[i].x) * t);
            blended.cam[i].y = static_cast<float>(prev.cam[i].y + (last.cam[i].y - prev.cam[i].y) * t);
            blended.cam[i].z = static_cast<float>(prev.cam[i].z + (last.cam[i].z - prev.cam[i].z) * t);
        }
        shot = &blended;
    }
    const Capture::Shot& s = *shot;
    auto it = capture_->models.find(s.model);
    if (it == capture_->models.end()) {
        auto read = game::read_model(machine_.memory(), s.model);
        if (!read) return false;
        it = capture_->models.emplace(s.model, std::move(*read)).first;
    }
    const game::ModelData& md = it->second;
    if (md.verts.size() != s.cam.size()) return false;

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
        const SceneColour col = scene_colour(colour);
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
