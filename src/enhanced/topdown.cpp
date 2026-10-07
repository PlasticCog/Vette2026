#include "enhanced/topdown.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace vette::enhanced {
namespace {

// The default EGA palette (the race view's attribute registers are the defaults).
constexpr uint32_t kEga[16] = {0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
                               0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};

}  // namespace

TopDown::TopDown(const World& world, int px_per_cell, int cells_x, int cells_y)
    : world_(world),
      px_(px_per_cell),
      scale_(px_per_cell / double(kCellSize)),
      cells_x_(cells_x),
      cells_y_(cells_y),
      width_(cells_y * px_per_cell),
      height_(cells_x * px_per_cell),
      cells_(static_cast<size_t>(cells_x) * static_cast<size_t>(cells_y)),
      ground_(1, 0) {
    img_.assign(static_cast<size_t>(width_) * static_cast<size_t>(height_), 0);
}

TopDown::TopDown(const World& world, int px_per_cell)
    : TopDown(world, px_per_cell, world.cells_x(), world.cells_y()) {
    cells_ = world.cells;
    ground_.assign(world.big_tile_ground.begin(), world.big_tile_ground.end());
}

void TopDown::set_cell(int cx, int cy, Cell cell) {
    if (cx >= 0 && cy >= 0 && cx < cells_x_ && cy < cells_y_) cells_[static_cast<size_t>(cx * cells_y_ + cy)] = cell;
}

void TopDown::set_ground(int big_tile, uint8_t colour) {
    if (big_tile >= 0 && big_tile < static_cast<int>(ground_.size())) ground_[static_cast<size_t>(big_tile)] = colour;
}

void TopDown::render() {
    int x, y, w, h;
    render_cells(0, 0, cells_x_, cells_y_, x, y, w, h);
}

void TopDown::render_cells(int cx0, int cy0, int cx1, int cy1, int& x, int& y, int& w, int& h) {
    cx0 = std::clamp(cx0, 0, cells_x_);
    cx1 = std::clamp(cx1, cx0, cells_x_);
    cy0 = std::clamp(cy0, 0, cells_y_);
    cy1 = std::clamp(cy1, cy0, cells_y_);
    // Pixels: x from the cells' y (east), y from the cells' x (north at the top).
    clip_x0_ = cy0 * px_;
    clip_x1_ = cy1 * px_;
    clip_y0_ = height_ - cx1 * px_;
    clip_y1_ = height_ - cx0 * px_;
    x = clip_x0_;
    y = clip_y0_;
    w = clip_x1_ - clip_x0_;
    h = clip_y1_ - clip_y0_;
    const int big_cols = std::max(1, world_.big_cols);
    for (int cx = cx0; cx < cx1; ++cx) {
        for (int cy = cy0; cy < cy1; ++cy) {
            const auto bt = static_cast<size_t>((cx / kBigTileCells) * big_cols + cy / kBigTileCells);
            const uint32_t c = kEga[(bt < ground_.size() ? ground_[bt] : 0) & 15];
            for (int v = height_ - (cx + 1) * px_; v < height_ - cx * px_; ++v) {
                for (int u = cy * px_; u < (cy + 1) * px_; ++u) img_[static_cast<size_t>(v * width_ + u)] = c;
            }
        }
    }
    // Objects reach a little past their cells: the neighbours' are drawn too, clipped.
    draw_window(cx0 - 1, cy0 - 1, cx1 + 1, cy1 + 1);
    if (grid) {
        for (int i = 0; i <= cells_x_; i += kBigTileCells) {
            const int v = std::max(0, height_ - 1 - i * px_);
            for (int u = 0; u < width_; ++u) put(u, v, 0xFFFFFF);
        }
        for (int i = 0; i <= cells_y_; i += kBigTileCells) {
            const int u = std::min(width_ - 1, i * px_);
            for (int v = 0; v < height_; ++v) put(u, v, 0xFFFFFF);
        }
    }
}

bool TopDown::compound_listed(const CompoundInstance& ci) const {
    if (cells_x_ != world_.cells_x() || cells_y_ != world_.cells_y()) return false;
    for (const uint16_t idx : ci.cells) {
        const int cx = idx / cells_y_, cy = idx % cells_y_;
        if (cx >= cells_x_) continue;
        const CellType& ct = world_.types[cell(cx, cy).type];
        for (const auto* list : {&ct.list1, &ct.list2}) {
            for (const ListEntry& e : *list) {
                if (e.routine == ci.routine) return true;
            }
        }
    }
    return false;
}

void TopDown::draw_window(int cx0, int cy0, int cx1, int cy1) {
    struct Sortable {
        double z;
        const Variant* v;
        Vec3i pos;
    };
    std::vector<Sortable> late;
    for (int cx = std::max(0, cx0); cx < std::min(cells_x_, cx1); ++cx) {
        for (int cy = std::max(0, cy0); cy < std::min(cells_y_, cy1); ++cy) {
            const Cell& c = cell(cx, cy);
            const CellType& ct = world_.types[c.type];
            DrawState state;
            const Vec3i origin{cx * kCellSize, cy * kCellSize, c.elevation * kElevationStep};
            for (const auto* list : {&ct.list1, &ct.list2}) {
                for (const ListEntry& e : *list) {
                    const Routine* r = world_.routine(e.routine);
                    if (!r || r->compound) continue;
                    const Variant* v = r->select(state);
                    if (!v) continue;
                    if (v->sets_finish_flag >= 0) state.finish_flag = v->sets_finish_flag != 0;
                    const Vec3i pos{origin.x + e.dx, origin.y + e.dy, origin.z + e.dz};
                    if (list == &ct.list1) {
                        draw(*v, pos);
                    } else {
                        late.push_back({double(pos.z), v, pos});
                    }
                }
            }
        }
    }
    for (const CompoundInstance& ci : world_.compounds) {
        if (!compound_listed(ci)) continue;
        const Routine* r = world_.routine(ci.routine);
        const Variant* v = r ? r->select(DrawState{}) : nullptr;
        if (!v) continue;
        for (const SubCall& s : v->calls) {
            const Routine* sr = world_.routine(s.routine);
            const Variant* sv = sr ? sr->select(DrawState{}) : nullptr;
            if (sv) {
                late.push_back({double(ci.position.z + s.offset.z), sv,
                                {ci.position.x + s.offset.x, ci.position.y + s.offset.y, ci.position.z + s.offset.z}});
            }
        }
    }
    std::stable_sort(late.begin(), late.end(), [](const Sortable& a, const Sortable& b) { return a.z < b.z; });
    for (const Sortable& s : late) draw(*s.v, s.pos);
}

void TopDown::draw(const Variant& v, Vec3i pos) {
    for (const Part& p : v.parts) {
        std::vector<P> pts;
        if (p.source == Part::Source::Model) {
            const Model& m = world_.models[p.model];
            const double yaw = p.rotation == Part::Rotation::None ? 0 : p.yaw;
            std::vector<P> mp;
            for (const Vec3i& q : m.near_mesh.verts) {
                const Vec3d w = model_to_world(q, yaw, p.pitch, p.roll);
                mp.push_back(to_px(pos.x + p.origin.x + w.x, pos.y + p.origin.y + w.y, pos.z + p.origin.z + w.z));
            }
            // The octant for a camera high above: up is -y in model axes.
            const int oct = model_octant({0, -1e6, 0});
            for (const uint16_t fi : m.near_mesh.order[static_cast<size_t>(oct)]) {
                const ModelFace& f = m.near_mesh.faces[fi];
                if (f.flags & 0x4000) continue;
                for (const auto& prim : f.prims) {
                    std::vector<P> poly;
                    for (const uint16_t i : prim) poly.push_back(mp[i]);
                    if (f.lines()) {
                        for (size_t k = 0; k + 1 < poly.size(); ++k) seg(poly[k], poly[k + 1], f.colour.base());
                    } else {
                        fill(poly, f.colour);
                    }
                }
            }
            continue;
        }
        for (const Vec3i& q : p.verts) {
            if (p.source == Part::Source::Packed) {
                pts.push_back(to_px(pos.x + q.x, pos.y + q.y, pos.z + q.z));
            } else {
                const double yaw =
                    (p.rotation == Part::Rotation::Fixed || p.rotation == Part::Rotation::Animated) ? p.yaw : 0;
                const Vec3d w = rotate_local({double(q.x), double(q.y), double(q.z)}, yaw, p.pitch, p.roll);
                pts.push_back(to_px(pos.x + p.origin.x + w.x, pos.y + p.origin.y + w.y, pos.z + p.origin.z + w.z));
            }
        }
        for (const Prim& prim : p.prims) {
            std::vector<P> poly;
            for (uint32_t k = 0; k < prim.count; ++k) poly.push_back(pts[p.indices[prim.first + k]]);
            if (prim.kind == Prim::Kind::Line) {
                seg(poly[0], poly[1], prim.colour.base());
            } else {
                fill(poly, prim.colour);
            }
        }
    }
}

void TopDown::seg(const P& a, const P& b, uint8_t colour) {
    const int n = static_cast<int>(std::max(std::abs(b.u - a.u), std::abs(b.v - a.v))) + 1;
    for (int i = 0; i <= n; ++i) {
        const double t = double(i) / n;
        put(static_cast<int>(a.u + t * (b.u - a.u)), static_cast<int>(a.v + t * (b.v - a.v)), kEga[colour & 15]);
    }
}

void TopDown::fill(const std::vector<P>& poly, Colour c) {
    if (poly.size() < 3) {
        if (poly.size() == 2) seg(poly[0], poly[1], c.base());
        return;
    }
    double vmin = 1e30, vmax = -1e30;
    for (const P& p : poly) {
        vmin = std::min(vmin, p.v);
        vmax = std::max(vmax, p.v);
    }
    const int v0 = std::max(clip_y0_, static_cast<int>(std::ceil(vmin - 0.5)));
    const int v1 = std::min(clip_y1_ - 1, static_cast<int>(vmax));
    std::vector<double> xs;
    for (int v = v0; v <= v1; ++v) {
        const double yc = v + 0.5;
        xs.clear();
        for (size_t k = 0; k < poly.size(); ++k) {
            const P& a = poly[k];
            const P& b = poly[(k + 1) % poly.size()];
            if ((a.v <= yc) != (b.v <= yc)) xs.push_back(a.u + (yc - a.v) / (b.v - a.v) * (b.u - a.u));
        }
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            const int u0 = std::max(clip_x0_, static_cast<int>(std::ceil(xs[k] - 0.5)));
            const int u1 = std::min(clip_x1_ - 1, static_cast<int>(std::floor(xs[k + 1] - 0.5)));
            for (int u = u0; u <= u1; ++u) {
                const bool second = c.dithered() && ((u + v) & 1) == 0;
                img_[static_cast<size_t>(v * width_ + u)] = kEga[second ? c.second() : c.base()];
            }
        }
    }
}

std::vector<uint32_t> TopDown::thumbnail(const World& world, int type, int px, uint32_t background) {
    TopDown t(world, px, 1, 1);  // a one-cell map
    t.cells_[0] = Cell{static_cast<uint8_t>(type), 0};
    std::fill(t.img_.begin(), t.img_.end(), background);
    t.clip_x0_ = t.clip_y0_ = 0;
    t.clip_x1_ = t.clip_y1_ = px;
    t.draw_window(0, 0, 1, 1);
    return t.img_;
}

}  // namespace vette::enhanced
