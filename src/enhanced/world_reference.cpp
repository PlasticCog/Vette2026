#include "enhanced/world_reference.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>

#include "enhanced/world_decode.h"
#include "enhanced/world_probe.h"
#include "game/math3d.h"
#include "game/projection.h"

namespace vette::enhanced {

namespace {

using game::Mat3;
using game::Vec3;
using Op = TraceEvent::Op;

// Frame state in DS (notes 03).
constexpr uint16_t kCamMatrix = 0x32B1;
constexpr uint16_t kAxisTable = 0x3184, kAxisBase = 0x3181;
constexpr uint16_t kViewLeft = 0x315E, kViewTop = 0x315A, kViewRight = 0x3160, kViewBottom = 0x315C;
constexpr uint16_t kCentreY = 0x316B, kCentreX = 0x3169, kViewHeight = 0x316D;
constexpr uint16_t kBigRows = 0x856F, kBigCols = 0x8571;
constexpr uint16_t kCamRow = 0x2C93, kCamCol = 0x2C95, kCamCellX = 0x2C97, kCamCellY = 0x2C99;
constexpr uint16_t kCellOriginX = 0x2CC1, kCellOriginY = 0x2CC3;
constexpr uint16_t kDrawX = 0x2CC7, kDrawY = 0x2CC5, kDrawZ = 0x2CBB, kCellIndex = 0x2CC9;
constexpr uint16_t kCellType = 0x3142, kListAPtr = 0x3148, kListBPtr = 0x314A, kCellX = 0x314C, kCellY = 0x314E;
constexpr uint16_t kBigTileIndex2 = 0x3556, kCellRecord = 0x324A;
constexpr uint16_t kListA = 0xEF5A, kListB = 0xEF8C;
constexpr uint16_t kRecords = 0x35C5, kStaticCount = 0xE02A;
constexpr uint16_t kSortIndex = 0x3805, kSortIndexInit = 0x37C5;
constexpr uint16_t kCollectVehicles = 0x32F8, kCollectPedestrians = 0x34D6;

int16_t s16(uint16_t v) { return static_cast<int16_t>(v); }
int16_t w16(int32_t v) { return static_cast<int16_t>(static_cast<uint16_t>(v)); }

// IDIV of a 32-bit dividend by a 16-bit divisor; a zero divisor or a quotient outside 16 bits raises
// INT 0, whose handler returns 7FFFh (3009:2565).
int16_t idiv16(int32_t num, int16_t den) {
    if (den == 0) return 0x7FFF;
    const int32_t q = num / den;
    return (q > 0x7FFF || q < -0x8000) ? int16_t{0x7FFF} : static_cast<int16_t>(q);
}

// Sum of three 16 x 16 IMUL products accumulated in 32 bits with wraparound (ADD/ADC).
int32_t dot32(int16_t a0, int16_t b0, int16_t a1, int16_t b1, int16_t a2, int16_t b2) {
    const uint32_t sum = static_cast<uint32_t>(int32_t{a0} * b0) + static_cast<uint32_t>(int32_t{a1} * b1) +
                         static_cast<uint32_t>(int32_t{a2} * b2);
    return static_cast<int32_t>(sum);
}
int16_t add16(int a, int b) { return static_cast<int16_t>(static_cast<uint16_t>(a + b)); }

// One window cell: position offset added to the camera cell's origin, and cell index offset.
struct WinCell {
    uint16_t cx, dx;
    int16_t bx, ax;
};

struct View {
    int left, top, right, bottom, cx, cy, height;
};

struct Pt {
    int32_t x, y;
};

struct SVert {
    Vec3 cam;
    int32_t x = 0, y = 0;  // screen (32-bit), valid when flags != 2
    uint8_t flags = 0;     // 0 ok, 1 16-bit overflow, 2 behind the near plane
};

} // namespace

struct ReferenceRenderer::Impl {
    const World& world;
    std::unique_ptr<Tracer> tracer;
    ReferenceStats* st = nullptr;
    bool debug = false;
    // Geometry from the World API in floating point (Part::verts, rotate_local, model_to_world and a
    // float camera) instead of the original's fixed-point words and matrices.
    bool from_world = false;
    uint16_t cur_routine = 0;
    std::array<double, 9> fcam{};
    double cam_yaw = 0;
    std::vector<uint8_t>* px = nullptr;
    int width = 320;

    // Frame state.
    View view{};
    Vec3 cam_pos{};
    Mat3 cam{};
    AxisTable axes;
    bool mirror = false;
    int rows = 5, cols = 5;
    std::array<int16_t, 200> left{}, right{};

    explicit Impl(const World& w) : world(w) {}

    uint16_t ds16(uint16_t off) { return tracer->rd16(addr::kDataSeg, off); }
    uint8_t ds8(uint16_t off) { return tracer->rd8(addr::kDataSeg, off); }
    void set16(uint16_t off, uint16_t v) { tracer->wr16(addr::kDataSeg, off, v); }
    void set8(uint16_t off, uint8_t v) { tracer->wr8(addr::kDataSeg, off, v); }

    void note(const std::string& s) {
        if (st->notes.size() < 64 && std::find(st->notes.begin(), st->notes.end(), s) == st->notes.end()) {
            st->notes.push_back(s);
        }
    }

    // --- Raster ------------------------------------------------------------------------------------
    void plot(int x, int y, uint8_t c) {
        if (x >= view.left && x <= view.right && y >= view.top && y <= view.bottom) {
            (*px)[static_cast<size_t>(y * width + x)] = c;
        }
    }

    // The Bresenham edge walker (3009:A377): every pixel of the edge widens its row's span.
    void edge(int x0, int y0, int x1, int y1) {
        int step_x = 1;
        int adx = x1 - x0;
        if (adx < 0) {
            step_x = -1;
            adx = -adx;
        } else if (adx == 0 && (x1 == view.left || x1 == view.right)) {
            // A356: a vertical edge on the viewport border sets that side of the spans outright.
            int a = y0, b = y1;
            if (b < a) std::swap(a, b);
            for (int y = a; y <= b; ++y) {
                (x1 == view.left ? left : right)[static_cast<size_t>(y)] = static_cast<int16_t>(x1);
            }
            return;
        }
        int dy = y1 - y0;
        int row_step = 1;
        if (dy == 0) {
            const auto row = static_cast<size_t>(y1);
            const int lo = std::min(x0, x1), hi = std::max(x0, x1);
            left[row] = static_cast<int16_t>(std::min<int>(left[row], lo));
            right[row] = static_cast<int16_t>(std::max<int>(right[row], hi));
            return;
        }
        if (dy < 0) {
            row_step = -1;
            dy = -dy;
        }
        int si = 0;
        int x = x0, row = y0;
        if (dy < adx) {
            const int thr = dy >> 1;
            int l = left[static_cast<size_t>(row)], r = right[static_cast<size_t>(row)];
            for (int count = adx; count >= 0; --count) {
                if (x < l) l = x;
                if (x > r) r = x;
                x += step_x;
                si = static_cast<int16_t>(si + dy);
                if (si > thr) {
                    si = static_cast<int16_t>(si - adx);
                    left[static_cast<size_t>(row)] = static_cast<int16_t>(l);
                    right[static_cast<size_t>(row)] = static_cast<int16_t>(r);
                    row += row_step;
                    if (row < 0 || row >= 200) return;
                    l = left[static_cast<size_t>(row)];
                    r = right[static_cast<size_t>(row)];
                }
            }
        } else {
            const int thr = adx >> 1;
            for (int count = dy; count >= 0; --count) {
                if (row < 0 || row >= 200) return;
                if (x < left[static_cast<size_t>(row)]) left[static_cast<size_t>(row)] = static_cast<int16_t>(x);
                if (x > right[static_cast<size_t>(row)]) right[static_cast<size_t>(row)] = static_cast<int16_t>(x);
                row += row_step;
                si = static_cast<int16_t>(si + adx);
                if (si > thr) {
                    si = static_cast<int16_t>(si - dy);
                    x += step_x;
                }
            }
        }
    }

    // Sutherland-Hodgman against one viewport edge, with the original's Q15 intersection
    // (3009:40F7 .. 43C2): t = (edge - out) << 15 / (in - out), other = out + floor(t * delta / 8000h).
    // Sutherland-Hodgman in the original clippers' order (40F7, A7C0, A8D4, ...): v0 first if inside,
    // then for each edge v[i-1] -> v[i] its crossing and v[i], the closing edge's crossing last. The
    // order matters to the winding test, which reads the first three points.
    template <typename V, typename Inside, typename Cross>
    static std::vector<Pt> walk(const std::vector<V>& in, Inside inside, Cross cross) {
        std::vector<Pt> out;
        const size_t n = in.size();
        if (n == 0) return out;
        if (inside(in[0])) out.push_back(cross.point(in[0]));
        for (size_t i = 1; i <= n; ++i) {
            const V& prev = in[i - 1];
            const V& cur = in[i % n];
            const bool pi = inside(prev), ci = inside(cur);
            if (pi != ci) out.push_back(ci ? cross(prev, cur) : cross(cur, prev));  // (outside, inside)
            if (ci && i < n) out.push_back(cross.point(cur));
        }
        return out;
    }

    static std::vector<Pt> clip_edge(const std::vector<Pt>& in, int axis, int limit, bool keep_greater) {
        std::vector<Pt> out;
        if (in.size() < 2) {
            return out;
        }
        const auto coord = [axis](const Pt& p) { return axis == 0 ? p.x : p.y; };
        const auto inside = [&](const Pt& p) { return keep_greater ? coord(p) >= limit : coord(p) <= limit; };
        const auto cross = [&](const Pt& o, const Pt& i) {
            // Every delta is a 16-bit word, as in the original (it wraps for far-apart points).
            const int32_t num = int32_t{w16(limit - coord(o))} * 32768;
            const int16_t t = idiv16(num, w16(coord(i) - coord(o)));
            const int32_t od = axis == 0 ? o.y : o.x, id = axis == 0 ? i.y : i.x;
            const int32_t prod = int32_t{t} * w16(id - od);
            const int16_t other = w16(od + w16(prod >> 15));
            return axis == 0 ? Pt{limit, other} : Pt{other, limit};
        };
        struct C {
            decltype(cross)& f;
            Pt operator()(const Pt& o, const Pt& i) const { return f(o, i); }
            static Pt point(const Pt& p) { return p; }
        };
        return walk(in, inside, C{cross});
    }

    // The near-plane crossing of the edge from `a` (behind, z < 1) to `b` (3009:A83D, ADDA): a Q15
    // fraction t = (1 - za) << 15 / (zb - za), x = xa + floor(t (xb - xa) / 8000h), the same for y, then
    // projected at z = 1 into 32-bit screen coordinates.
    Pt near_point(const Vec3& a, const Vec3& b) const {
        const int16_t t = idiv16(int32_t{w16(1 - a.z)} * 32768, w16(b.z - a.z));
        const auto step = [t](int16_t from, int16_t to) {
            const int32_t prod = int32_t{t} * w16(to - from);
            return w16(from + w16(prod >> 15));
        };
        const int16_t x = step(a.x, b.x), y = step(a.y, b.y);
        return {int32_t{x} * 256 + view.cx, int32_t{y} * 256 + view.cy};
    }

    // Guard-band clip (3009:A8D4 / A9F8 / AB1F / AC45): Sutherland-Hodgman against x > -4000h,
    // y > -4000h, x < 4000h, y < 4000h, the crossing found by bisecting from the outside point
    // towards the inside one until it lands in a band just inside the limit.
    static std::vector<Pt> guard(std::vector<Pt> pts) {
        for (int side = 0; side < 4; ++side) {
            const bool low = side == 0 || side == 1;
            const auto c = [side](const Pt& p) { return (side == 0 || side == 2) ? p.x : p.y; };
            const auto inside = [&](const Pt& p) { return low ? c(p) > -0x4000 : c(p) < 0x4000; };
            const auto in_band = [&](const Pt& p) { return low ? c(p) < 0 : c(p) >= 0x400; };
            if (pts.size() < 2) return {};
            const auto bisect = [&](Pt o, Pt i) {
                Pt mid{};
                for (int steps = 0; steps < 64; ++steps) {
                    mid = {(o.x + i.x) >> 1, (o.y + i.y) >> 1};
                    if (!inside(mid)) {
                        o = mid;
                    } else if (in_band(mid)) {
                        break;
                    } else {
                        i = mid;
                    }
                }
                return mid;
            };
            struct C {
                decltype(bisect)& f;
                Pt operator()(const Pt& o, const Pt& i) const { return f(o, i); }
                static Pt point(const Pt& p) { return p; }
            };
            pts = walk(pts, inside, C{bisect});
        }
        return pts;
    }

    // B4B2: the polygon's screen points. Any vertex behind the near plane or overflowing 16 bits sends
    // it through the near-plane clip (A7C0) and the guard band; the result is 16-bit.
    std::vector<Pt> to_screen(const std::vector<const SVert*>& vs) {
        std::vector<Pt> pts;
        bool flagged = false;
        for (const SVert* v : vs) flagged |= v->flags != 0;
        if (!flagged) {
            for (const SVert* v : vs) pts.push_back({v->x, v->y});
            return pts;
        }
        ++st->clipped_near;
        struct C {
            const Impl& m;
            Pt operator()(const SVert* behind, const SVert* visible) const { return m.near_point(behind->cam, visible->cam); }
            static Pt point(const SVert* v) { return {v->x, v->y}; }
        };
        pts = walk(vs, [](const SVert* v) { return v->flags != 2; }, C{*this});
        pts = guard(std::move(pts));
        for (Pt& p : pts) p = {static_cast<int16_t>(p.x), static_cast<int16_t>(p.y)};
        return pts;
    }

    std::vector<Pt> clip_view(std::vector<Pt> pts) {
        pts = clip_edge(pts, 0, view.left, true);
        pts = clip_edge(pts, 1, view.top, true);
        pts = clip_edge(pts, 0, view.right, false);
        pts = clip_edge(pts, 1, view.bottom, false);
        return pts;
    }

    // Line drawers: 5170 for line lists, A1FF for model polylines and outlines.
    void line(int x0, int y0, int x1, int y1, uint8_t colour, bool a1ff = false);
    void raster_a1ff(int x0, int y0, int x1, int y1, uint8_t colour);

    // B63F's crossing lists: per row the x where each edge crosses it, kept sorted (A4F1).
    std::array<std::vector<int16_t>, 200> crossings;
    void insert_crossing(int row, int x) {
        if (row < 0 || row >= 200) return;
        auto& list = crossings[static_cast<size_t>(row)];
        auto it = list.begin();
        while (it != list.end() && !(x < *it)) ++it;
        list.insert(it, static_cast<int16_t>(x));
    }

    // A4F1: one edge into the crossing lists (rows already exclude the edge's lower end).
    void alt_edge(int x0, int y0, int x1, int y1) {
        int xs = 1, ys = 1;
        bool odd = false;
        int dx = x1 - x0;
        if (dx < 0) {
            xs = -1;
            dx = -dx;
            odd = !odd;
        } else if (dx == 0) {  // A4AE
            for (int y = std::min(y0, y1); y <= std::max(y0, y1); ++y) insert_crossing(y, x0);
            return;
        }
        int dy = y1 - y0;
        if (dy < 0) {
            ys = -1;
            dy = -dy;
            odd = !odd;
        } else if (dy == 0) {
            insert_crossing(y1, x1);
            return;
        }
        if (dy < dx) {
            // x-major: walked backwards from (x1, y1) when x and y run the same way.
            int x = x0, row = y0;
            if (!odd) {
                x = x1;
                row = y1;
                xs = -xs;
                ys = -ys;
            }
            int e = 0, n = dx;
            bool step_x = true;
            for (int guard_steps = 0; guard_steps < 4096; ++guard_steps) {
                if (step_x) x += xs;
                step_x = true;
                e += dy;
                if (e > (dy >> 1)) {
                    insert_crossing(row, x);
                    row += ys;
                    e -= dx;
                }
                --n;
                if (n > 0) continue;
                if (n < 0) break;
                x = std::clamp(x + xs, view.left, view.right);  // A5E8
                step_x = false;
            }
        } else {
            int x = x0, row = y0, e = 0;
            for (int n = dy; n >= 0; --n) {
                insert_crossing(row, x);
                row += ys;
                e += dx;
                if (e > (dx >> 1)) {
                    e -= dy;
                    x += xs;
                }
            }
        }
    }

    // fill_poly_list for one polygon: B5BC (convex: A377 spans) or B63F (`alt`: A4F1 crossing lists,
    // filled in pairs), both through B4B2 (near plane, winding test, viewport clip) and 9F3B / A064.
    void fill(const std::vector<const SVert*>& vs, uint8_t colour, bool winding, bool screen_door, int outline,
              bool alt = false) {
        std::vector<Pt> pts = to_screen(vs);
        if (debug) {
            std::printf("  fill colour %02X%s:", colour, alt ? " alt" : "");
            for (const SVert* v : vs) std::printf(" [%d,%d,%d f%d]", v->cam.x, v->cam.y, v->cam.z, v->flags);
            std::printf(" ->");
            for (const Pt& q : pts) std::printf(" (%d,%d)", q.x, q.y);
            std::printf("%c", 10);
        }
        if (winding && pts.size() >= 3) {
            // 9CD8: (x1 - x0)(y2 - y1) - (x2 - x1)(y1 - y0) must be positive.
            const int32_t a = int32_t{w16(pts[1].x - pts[0].x)} * w16(pts[2].y - pts[1].y);
            const int32_t b = int32_t{w16(pts[2].x - pts[1].x)} * w16(pts[1].y - pts[0].y);
            if (mirror ? b <= a : a <= b) return;
        }
        pts = clip_view(std::move(pts));
        if (pts.size() < 2) return;
        ++st->polygons;
        int ymin = view.height, ymax = view.top;
        for (const Pt& p : pts) {
            if (static_cast<unsigned>(p.y) < static_cast<unsigned>(ymin)) ymin = p.y;
            if (p.y > ymax && static_cast<unsigned>(p.y) < static_cast<unsigned>(view.height)) ymax = p.y;
        }
        const int rows_n = std::max(ymax - ymin, 0) + 1;
        if (ymin < 0 || ymin + rows_n > 200) return;
        // Spans per row, from either filler.
        std::vector<std::vector<std::pair<int, int>>> spans(static_cast<size_t>(rows_n));
        if (alt) {
            for (int y = ymin; y < ymin + rows_n; ++y) crossings[static_cast<size_t>(y)].clear();
            for (size_t k = 0; k < pts.size(); ++k) {
                Pt a = pts[k];
                Pt b = pts[(k + 1) % pts.size()];
                if (a.y == b.y) continue;  // B693: horizontal edges add no crossing
                if (b.y > a.y) {
                    --b.y;
                } else {
                    --a.y;
                }
                alt_edge(a.x, a.y, b.x, b.y);
            }
            for (int y = ymin; y < ymin + rows_n; ++y) {
                const auto& list = crossings[static_cast<size_t>(y)];
                for (size_t i = 0; i + 1 < list.size(); i += 2) {
                    spans[static_cast<size_t>(y - ymin)].push_back({list[i], list[i + 1]});
                }
            }
        } else {
            for (int y = ymin; y < ymin + rows_n; ++y) {
                left[static_cast<size_t>(y)] = static_cast<int16_t>(view.right);
                right[static_cast<size_t>(y)] = static_cast<int16_t>(view.left);
            }
            for (size_t k = 0; k < pts.size(); ++k) {
                const Pt& a = pts[k];
                const Pt& b = pts[(k + 1) % pts.size()];
                edge(a.x, a.y, b.x, b.y);
            }
            for (int y = ymin; y < ymin + rows_n; ++y) {
                spans[static_cast<size_t>(y - ymin)].push_back({left[static_cast<size_t>(y)], right[static_cast<size_t>(y)]});
            }
        }
        const uint8_t base = colour & 0x0F, hi = colour >> 4;
        const bool dither = hi != 0 && hi != base;
        for (int y = ymin; y < ymin + rows_n; ++y) {
            const int phase = (y - ymin) & 1;
            for (const auto& [l, r] : spans[static_cast<size_t>(y - ymin)]) {
                for (int x = l; x <= r; ++x) {
                    const bool odd_pixel = ((x + phase) & 1) != 0;
                    if (screen_door) {
                        if (!odd_pixel) plot(x, y, base);
                    } else {
                        plot(x, y, dither && !odd_pixel ? hi : base);
                    }
                }
            }
        }
        if (outline >= 0) {
            for (size_t k = 0; k < pts.size(); ++k) {
                const Pt& a = pts[k];
                const Pt& b = pts[(k + 1) % pts.size()];
                if (a.x <= b.x) {
                    raster_a1ff(a.x, a.y, b.x, b.y, static_cast<uint8_t>(outline));
                } else {
                    raster_a1ff(b.x, b.y, a.x, a.y, static_cast<uint8_t>(outline));
                }
            }
        }
    }

    // One segment of a line list (405E / B6E1): both ends behind the camera skips it; one behind
    // goes through the near-plane clip (AD6D) and the guard band, then the viewport clip and draw.
    void draw_segment(const SVert& a, const SVert& b, uint8_t colour, bool model = false) {
        if (debug) {
            std::printf("  segment (%d,%d f%d cam %d,%d,%d) - (%d,%d f%d cam %d,%d,%d) colour %02X%s%c", a.x, a.y, a.flags,
                        a.cam.x, a.cam.y, a.cam.z, b.x, b.y, b.flags, b.cam.x, b.cam.y, b.cam.z, colour,
                        model ? " model" : "", 10);
        }
        if ((a.flags | b.flags) == 0) {
            line(a.x, a.y, b.x, b.y, colour, model);
            return;
        }
        if (a.flags == 2 && b.flags == 2) return;
        // AD6D lists the visible ends first (the second end before the first), then the near-plane
        // crossing.
        std::vector<Pt> pts;
        if (b.flags != 2) pts.push_back({b.x, b.y});
        if (a.flags != 2) pts.push_back({a.x, a.y});
        if (a.flags == 2) pts.push_back(near_point(a.cam, b.cam));
        if (b.flags == 2) pts.push_back(near_point(b.cam, a.cam));
        if (!guard_segment(pts[0], pts[1])) return;
        line(static_cast<int16_t>(pts[0].x), static_cast<int16_t>(pts[0].y), static_cast<int16_t>(pts[1].x),
             static_cast<int16_t>(pts[1].y), colour, model);
    }

    // The guard band for a two-point list (3009:AE41..): an open polyline, so each end that is outside
    // is replaced by the bisected crossing, keeping the order.
    static bool guard_segment(Pt& p0, Pt& p1) {
        for (int side = 0; side < 4; ++side) {
            const bool low = side == 0 || side == 1;
            const auto c = [side](const Pt& p) { return (side == 0 || side == 2) ? p.x : p.y; };
            const auto inside = [&](const Pt& p) { return low ? c(p) > -0x4000 : c(p) < 0x4000; };
            const auto in_band = [&](const Pt& p) { return low ? c(p) < 0 : c(p) >= 0x400; };
            const bool i0 = inside(p0), i1 = inside(p1);
            if (!i0 && !i1) return false;
            if (i0 && i1) continue;
            Pt o = i0 ? p1 : p0, i = i0 ? p0 : p1, mid{};
            for (int steps = 0; steps < 64; ++steps) {
                mid = {(o.x + i.x) >> 1, (o.y + i.y) >> 1};
                if (!inside(mid)) {
                    o = mid;
                } else if (in_band(mid)) {
                    break;
                } else {
                    i = mid;
                }
            }
            (i0 ? p1 : p0) = mid;
        }
        return true;
    }

    // --- Transform ------------------------------------------------------------------------------------
    SVert project(Vec3 c) {
        SVert v;
        v.cam = c;
        if (c.z < 1) {
            v.flags = 2;
            return v;
        }
        const auto sx = game::project_coord(c.x, c.z, static_cast<int16_t>(view.cx), game::ScreenAxis::X);
        const auto sy = game::project_coord(c.y, c.z, static_cast<int16_t>(view.cy), game::ScreenAxis::Y);
        v.x = sx.wide;
        v.y = sy.wide;
        v.flags = (sx.overflow || sy.overflow) ? 1 : 0;
        return v;
    }

    static Vec3 mat_apply(Vec3 v, const std::array<int16_t, 9>& m, const std::array<int16_t, 3>& t) {
        const Mat3 mm{m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]};
        const Vec3 r = game::vec_mul_mat3(v, mm);
        return {add16(r.x, t[0]), add16(r.y, t[1]), add16(r.z, t[2])};
    }

    // Visibility of a culled primitive from camera-space vertices, as 3B78 / 3BF5 compute it.
    static bool visible(const Cull& c, const std::vector<std::vector<SVert>>& parts) {
        if (c.kind == Cull::Kind::None) return true;
        const auto& vs = parts[c.part];
        if (c.kind == Cull::Kind::Behind) {
            const Vec3& a = vs[c.v[0]].cam;
            const Vec3& b = vs[c.v[1]].cam;
            return dot32(w16(b.x - a.x), a.x, w16(b.y - a.y), a.y, w16(b.z - a.z), a.z) > 0;
        }
        const Vec3& p0 = vs[c.v[0]].cam;
        const Vec3& p1 = vs[c.v[1]].cam;
        const Vec3& p2 = vs[c.v[2]].cam;
        const auto d16 = [](int16_t a, int16_t b) { return static_cast<int16_t>(a - b); };
        const int16_t ax = d16(p2.x, p0.x), ay = d16(p2.y, p0.y), az = d16(p2.z, p0.z);
        const int16_t bx = d16(p1.x, p0.x), by = d16(p1.y, p0.y), bz = d16(p1.z, p0.z);
        const auto m16 = [](int16_t a, int16_t b) { return static_cast<int16_t>(a * b); };  // low words only
        const auto nx = static_cast<int16_t>(m16(by, az) - m16(bz, ay));
        const auto ny = static_cast<int16_t>(m16(bz, ax) - m16(bx, az));
        const auto nz = static_cast<int16_t>(m16(bx, ay) - m16(by, ax));
        return dot32(nx, p0.x, ny, p0.y, nz, p0.z) > 0;
    }

    void draw_prim(const Part& part, const Prim& prim, const std::vector<SVert>& sv,
                   const std::vector<std::vector<SVert>>& all) {
        if (!visible(prim.cull, all)) {
            if (debug) {
                std::printf("  culled prim colour %02X by kind %d part %u v %u %u %u%c", prim.colour.raw,
                            static_cast<int>(prim.cull.kind), prim.cull.part, prim.cull.v[0], prim.cull.v[1], prim.cull.v[2], 10);
            }
            return;
        }
        if (prim.kind == Prim::Kind::Line) {
            ++st->lines;
            draw_segment(sv[part.indices[prim.first]], sv[part.indices[prim.first + 1]], prim.colour.raw);
            return;
        }
        std::vector<const SVert*> poly;
        for (uint32_t k = 0; k < prim.count; ++k) poly.push_back(&sv[part.indices[prim.first + k]]);
        fill(poly, prim.colour.raw, false, false, -1, prim.kind == Prim::Kind::PolygonAlt);
    }

    // --float: how far the API's vertices land from the original's (camera units, largest component).
    void deviation(const std::vector<Vec3>& exact, const std::vector<Vec3>& api, int kind) {
        auto& d = st->deviation[static_cast<size_t>(kind)];
        for (size_t k = 0; k < exact.size() && k < api.size(); ++k) {
            const int dx = std::abs(exact[k].x - api[k].x), dy = std::abs(exact[k].y - api[k].y),
                      dz = std::abs(exact[k].z - api[k].z);
            const int m = std::max({dx, dy, dz});
            ++d.vertices;
            d.over4 += m > 4 ? 1 : 0;
            d.over16 += m > 16 ? 1 : 0;
            if (m > d.max) {
                d.max = m;
                d.worst_routine = cur_routine;
            }
        }
    }

    // A point given as an entry position (16-bit, camera big-tile frame) plus a float offset in world
    // axes, into camera space with the float camera, rounded to the original's 16-bit words.
    Vec3 to_cam(std::array<int16_t, 3> entry, Vec3d local) const {
        const double dx = w16(entry[0] - cam_pos.x) + local.x;
        const double dy = w16(entry[1] - cam_pos.y) + local.y;
        const double dz = w16(entry[2] - cam_pos.z) + local.z;
        const double in[3] = {dy, -dz, dx};
        int16_t out[3];
        for (int j = 0; j < 3; ++j) {
            const double v = in[0] * fcam[static_cast<size_t>(j)] + in[1] * fcam[static_cast<size_t>(3 + j)] +
                             in[2] * fcam[static_cast<size_t>(6 + j)];
            out[j] = static_cast<int16_t>(std::clamp(std::lround(v), -32768L, 32767L));
        }
        return {out[0], out[1], out[2]};
    }

    void draw_model(const TraceEvent& ev, const Part* part = nullptr, std::array<int16_t, 3> entry = {}) {
        if (ev.addr >= kModelCount || !world.models[ev.addr].present) {
            note("model " + std::to_string(ev.addr) + " missing");
            return;
        }
        const Model& m = world.models[ev.addr];
        const ModelMesh* mesh = ev.model_header == m.near_mesh.header ? &m.near_mesh
                                : ev.model_header == m.far_mesh.header ? &m.far_mesh
                                                                       : nullptr;
        if (!mesh) {
            note("model " + std::to_string(ev.addr) + ": unknown header");
            return;
        }
        ++st->models;
        std::vector<SVert> sv;
        std::vector<Vec3> ex;
        for (const Vec3i& p : mesh->verts) {
            ex.push_back(mat_apply({int16_t(p.x), int16_t(p.y), int16_t(p.z)}, ev.matrix, ev.translation));
        }
        if (from_world) {
            // Static models: the Part's placement; vehicles: their live angles.
            double yaw = 0, pitch = 0, roll = 0;
            if (part) {
                if (part->rotation != Part::Rotation::None) {
                    yaw = part->yaw;
                    pitch = part->pitch;
                    roll = part->roll;
                }
            } else if (ev.aux != 2) {
                yaw = ev.angles[0];
                pitch = ev.angles[1];
                roll = ev.angles[2];
            }
            std::vector<Vec3> fl;
            for (const Vec3i& p : mesh->verts) {
                const Vec3d w = model_to_world(p, yaw, pitch, roll);
                fl.push_back(to_cam(entry, {ev.pos[0] + w.x, ev.pos[1] + w.y, ev.pos[2] + w.z}));
            }
            deviation(ex, fl, 2);
            for (const Vec3& c : fl) sv.push_back(project(c));
        } else {
            for (const Vec3& c : ex) sv.push_back(project(c));
        }
        // 9CAF: one bit per reference axis, set when (V_i - V0) . V0 < 0.
        int octant = 0;
        const Vec3& v0 = sv[0].cam;
        for (int i = 1; i <= 3; ++i) {
            const Vec3& vi = sv[static_cast<size_t>(i)].cam;
            if (dot32(w16(vi.x - v0.x), v0.x, w16(vi.y - v0.y), v0.y, w16(vi.z - v0.z), v0.z) < 0) {
                octant |= 1 << (i - 1);
            }
        }
        const bool outlines = ev.outline_enable != 0;  // as it was during the draw (the opponent clears it)
        if (debug) {
            std::printf("E0D8=%02X ", ev.outline_enable);
            std::printf("model %u header %04X (%s) octant %d at %d,%d,%d\n", ev.addr, ev.model_header,
                        mesh == &m.near_mesh ? "near" : "far", octant, sv[0].cam.x, sv[0].cam.y, sv[0].cam.z);
        }
        for (const uint16_t fi : mesh->order[static_cast<size_t>(octant)]) {
            const ModelFace& f = mesh->faces[fi];
            if (debug) {
                std::printf("  face %04X flags %04X colour %02X/%02X prims %zu\n", f.address, f.flags, f.colour.raw,
                            f.colour_hi, f.prims.size());
            }
            if (f.flags & 0x4000) continue;
            const bool winding = f.flags & 1;
            const bool door = (f.flags & 0x8000) != 0;
            const int outline = (outlines && (f.flags & 0x2000)) ? f.colour_hi : -1;
            for (const auto& prim : f.prims) {
                if (f.lines()) {
                    for (size_t k = 0; k + 1 < prim.size(); ++k) {
                        ++st->lines;
                        draw_segment(sv[prim[k]], sv[prim[k + 1]], f.colour.raw, true);
                    }
                } else {
                    std::vector<const SVert*> poly;
                    for (const uint16_t v : prim) poly.push_back(&sv[v]);
                    fill(poly, f.colour.raw, winding, door, outline, f.kind() == 2);
                }
            }
        }
    }

    // Draws the World's variant `v` of routine `r` at `pos`, with the dynamic parts of `tr` (the
    // original's own run: plain-list matrices, model matrices and LOD).
    void draw_variant(const Variant& v, std::array<int16_t, 3> pos, const Trace& tr) {
        std::vector<const TraceEvent*> plains, models;
        for (const TraceEvent& e : tr.events) {
            if (e.op == Op::Plain) plains.push_back(&e);
            if (e.op == Op::Model) models.push_back(&e);
        }
        size_t pi = 0, mi = 0;
        std::vector<std::vector<SVert>> all(v.parts.size());
        // Transform every part first: a prim may cull on another part's vertices (windows).
        for (size_t i = 0; i < v.parts.size(); ++i) {
            const Part& p = v.parts[i];
            const Vec3 o{add16(pos[0], p.origin.x), add16(pos[1], p.origin.y), add16(pos[2], p.origin.z)};
            // The original's exact camera-space vertices.
            std::vector<Vec3> ex;
            const TraceEvent* plain = nullptr;
            if (p.source == Part::Source::Packed) {
                const Vec3 oc = game::world_to_camera(o, cam_pos, cam);
                std::vector<V3s> cv;
                unpack_vertices({oc.x, oc.y, oc.z}, p.packed.data(), p.verts.size() - 1, axes, cv);
                for (const V3s& c : cv) ex.push_back({c[0], c[1], c[2]});
            } else if (p.source == Part::Source::Plain) {
                if (pi >= plains.size()) {
                    note("plain list missing from the trace");
                    return;
                }
                plain = plains[pi++];
                const Vec3 t = game::world_to_camera(o, cam_pos, cam);
                if (t.x != plain->translation[0] || t.y != plain->translation[1] || t.z != plain->translation[2]) {
                    note("plain list position differs from the original's");
                }
                for (const Vec3i& q : p.verts) {
                    const Vec3 in{int16_t(q.y), game::wrap_neg(int16_t(q.z)), int16_t(q.x)};
                    ex.push_back(mat_apply(in, plain->matrix, {t.x, t.y, t.z}));
                }
            } else {
                ++mi;
                continue;
            }
            if (!from_world) {
                for (const Vec3& c : ex) all[i].push_back(project(c));
                continue;
            }
            // The World API's float geometry, checked against the exact vertices.
            std::vector<Vec3> fl;
            if (p.source == Part::Source::Packed) {
                for (const Vec3i& q : p.verts) fl.push_back(to_cam(pos, {double(q.x), double(q.y), double(q.z)}));
            } else {
                double yaw = 0, pitch = p.pitch, roll = p.roll;
                switch (p.rotation) {
                case Part::Rotation::Fixed: yaw = p.yaw; break;
                case Part::Rotation::CameraYaw: yaw = cam_yaw + p.yaw; break;
                case Part::Rotation::Animated: yaw = plain->angles[0]; break;  // the animation's current angle
                default: pitch = roll = 0; break;
                }
                for (const Vec3i& q : p.verts) {
                    const Vec3d r = rotate_local({double(q.x), double(q.y), double(q.z)}, yaw, pitch, roll);
                    fl.push_back(to_cam(pos, {p.origin.x + r.x, p.origin.y + r.y, p.origin.z + r.z}));
                }
            }
            deviation(ex, fl, p.source == Part::Source::Packed ? 0 : 1);
            for (const Vec3& c : fl) all[i].push_back(project(c));
        }
        // A painter-order alternative applies when its deciding face points away.
        const Variant::Reorder* reorder = nullptr;
        for (const Variant::Reorder& r : v.reorders) {
            if (!reorder && !visible(r.unless_visible, all)) reorder = &r;
        }
        mi = 0;
        for (size_t i = 0; i < v.parts.size(); ++i) {
            const Part& p = v.parts[i];
            if (p.source == Part::Source::Model) {
                if (mi < models.size()) draw_model(*models[mi], &p, pos);
                ++mi;
                continue;
            }
            if (!reorder) {
                for (const Prim& prim : p.prims) draw_prim(p, prim, all[i], all);
            }
        }
        if (reorder) {
            for (const auto& [part, prim] : reorder->order) {
                draw_prim(v.parts[part], v.parts[part].prims[prim], all[part], all);
            }
        }
    }

    // --- Objects and cells ------------------------------------------------------------------------
    void draw_object(uint16_t routine, std::array<int16_t, 3> pos, int depth = 0) {
        st->calls.push_back({routine, pos[0], pos[1], pos[2]});
        cur_routine = routine;
        if (debug) std::printf("object %04X at %d,%d,%d%c", routine, pos[0], pos[1], pos[2], 10);
        for (int i = 0; i < 3; ++i) set16(static_cast<uint16_t>(addr::kObjPos + 2 * i), static_cast<uint16_t>(pos[static_cast<size_t>(i)]));
        Trace tr;
        if (!tracer->run(routine, pos, tr)) {
            note("routine " + std::to_string(routine) + ": " + tr.error);
        }
        const Routine* r = world.routine(routine);
        ++st->objects;
        if (!r) {
            ++st->unmatched;
            note("routine not in the world");
            return;
        }
        if (r->compound && tr.events.empty()) {
            return;  // already drawn this frame (DS:2AC0)
        }
        const uint64_t sig = tr.signature();
        const Variant* v = nullptr;
        for (const Variant& c : r->variants) {
            if (c.signature == sig) v = &c;
        }
        if (!v) {
            ++st->unmatched;
            char buf[64];
            std::snprintf(buf, sizeof buf, "routine %04X: variant not in the world", routine);
            note(buf);
            return;
        }
        if (!v->calls.empty() && depth == 0) {
            for (const SubCall& c : v->calls) {
                draw_object(c.routine,
                            {add16(pos[0], c.offset.x), add16(pos[1], c.offset.y), add16(pos[2], c.offset.z)}, 1);
            }
            return;
        }
        draw_variant(*v, pos, tr);
    }

    void draw_vehicle(uint16_t routine, std::array<int16_t, 3> pos) {
        st->calls.push_back({routine, pos[0], pos[1], pos[2]});
        cur_routine = routine;
        for (int i = 0; i < 3; ++i) set16(static_cast<uint16_t>(addr::kObjPos + 2 * i), static_cast<uint16_t>(pos[static_cast<size_t>(i)]));
        Trace tr;
        tracer->run(routine, pos, tr);
        ++st->vehicles;
        for (const TraceEvent& e : tr.events) {
            if (e.op == Op::Model) {
                draw_model(e, nullptr, pos);
            } else {
                note("vehicle routine draws something other than a model");
            }
        }
    }

    void draw_cell(const WinCell& w, bool own) {
        ++st->cells;
        set8(addr::kOwnCell, own ? 0xFF : 0);
        const auto draw_x = static_cast<uint16_t>(w.cx + ds16(kCellOriginX));
        const auto draw_y = static_cast<uint16_t>(w.dx + ds16(kCellOriginY));
        set16(kDrawX, draw_x);
        set16(kDrawY, draw_y);
        int row = s16(ds16(kCamRow)), col = s16(ds16(kCamCol));
        int bx = w.bx + s16(ds16(kCamCellX));
        if (bx < 0) {
            bx += 16;
            if (row != 0) --row;
        } else if (bx >= 16) {
            bx -= 16;
            if (++row >= rows) --row;
        }
        int ax = w.ax + s16(ds16(kCamCellY));
        if (ax < 0) {
            ax += 16;
            if (col != 0) --col;
        } else if (ax >= 16) {
            ax -= 16;
            if (++col >= cols) --col;
        }
        set16(kCellX, static_cast<uint16_t>(bx));
        set16(kCellY, static_cast<uint16_t>(ax));
        const auto bt2 = static_cast<uint16_t>((row * cols + col) * 2);
        set16(kBigTileIndex2, bt2);
        const Cell& cell = world.cell(row * kBigTileCells + bx, col * kBigTileCells + ax);
        const auto z = static_cast<uint16_t>(cell.elevation * kElevationStep);
        set16(kDrawZ, z);
        set16(kCellType, cell.type);
        const CellType& ct = world.types[cell.type];
        set16(kCellRecord, ct.address);  // vehicles read the ground class from it (3009:285B)
        for (const ListEntry& e : ct.list1) {
            draw_object(e.routine, {add16(draw_x, e.dx), add16(draw_y, e.dy), add16(z, e.dz)});
        }
        // Sortables: the World's list2, then the vehicles and pedestrians the original collects.
        set16(kListAPtr, ds16(static_cast<uint16_t>(kListA + bt2)));
        set16(kListBPtr, ds16(static_cast<uint16_t>(kListB + bt2)));
        uint16_t n = 0;
        if (ds8(addr::kNoBuildings) == 0) {
            for (const ListEntry& e : ct.list2) {
                const auto at = static_cast<uint16_t>(kRecords + 8 * n);
                set16(at, e.routine);
                set16(static_cast<uint16_t>(at + 2), static_cast<uint16_t>(add16(draw_x, e.dx)));
                set16(static_cast<uint16_t>(at + 4), static_cast<uint16_t>(add16(draw_y, e.dy)));
                set16(static_cast<uint16_t>(at + 6), static_cast<uint16_t>(add16(z, e.dz)));
                ++n;
            }
        }
        set16(kStaticCount, n);
        set16(kCellIndex, static_cast<uint16_t>(bx * 16 + ax));
        host::Registers regs;
        regs.r[host::DI] = static_cast<uint16_t>(kRecords + 8 * n);
        regs.r[host::CX] = n;
        for (const uint16_t routine : {kCollectVehicles, kCollectPedestrians}) {
            if (!tracer->call(routine, regs)) {
                char buf[96];
                std::snprintf(buf, sizeof buf, "collecting vehicles (%04X) did not return: %04X:%04X", routine,
                              regs.s[host::CS], regs.ip);
                note(buf);
                return;
            }
        }
        set16(regs.r[host::DI], 0xFFFF);
        const uint16_t total = regs.r[host::CX];
        if (total == 0) return;

        // cull_sortables (4562 / 44D0): depth Z and lateral X of the reference point.
        struct Rec {
            uint16_t code;
            std::array<int16_t, 3> pos;
            bool vehicle;
        };
        std::vector<Rec> recs;
        std::vector<int16_t> keys;
        int kept = 0;
        for (uint16_t i = 0; i < total; ++i) {
            const auto at = static_cast<uint16_t>(kRecords + 8 * i);
            Rec rec{ds16(at), {s16(ds16(static_cast<uint16_t>(at + 2))), s16(ds16(static_cast<uint16_t>(at + 4))),
                               s16(ds16(static_cast<uint16_t>(at + 6)))},
                    i >= n};
            const Vec3 d = game::camera_delta({rec.pos[0], rec.pos[1], rec.pos[2]}, cam_pos);
            const int16_t zc = game::q15_dot3(d, cam[2], cam[5], cam[8]);
            const int16_t xc = game::q15_dot3(d, cam[0], cam[3], cam[6]);
            const int lo = rec.vehicle ? -0x80 : -0x400, margin = rec.vehicle ? 0x80 : 0x600;
            int16_t key = -1;
            if (zc >= lo && zc < 0x1400) {
                const int16_t ax_ = static_cast<int16_t>(xc > 0 ? xc : -xc);
                if (static_cast<int16_t>(zc + margin) > ax_) {
                    key = zc >= 0 ? zc : static_cast<int16_t>(rec.vehicle ? 0x400 : 0);
                    ++kept;
                }
            }
            recs.push_back(rec);
            keys.push_back(key);
        }
        // sort_sortables (4686): bubble sort, far first, over every record (rejected ones sink).
        std::vector<uint16_t> idx;
        // The index list (reset from DS:37C5 after every cell) holds 2 x the record number: 4603 reads
        // record 35C5h + 4 * index.
        for (uint16_t i = 0; i < total; ++i) {
            idx.push_back(i < 16 ? static_cast<uint16_t>(ds16(static_cast<uint16_t>(kSortIndexInit + 2 * i)) / 2) : i);
        }
        if (total > 1) {
            bool swapped = true;
            while (swapped) {
                swapped = false;
                for (size_t i = 0; i + 1 < keys.size(); ++i) {
                    if (keys[i] < keys[i + 1]) {
                        std::swap(keys[i], keys[i + 1]);
                        std::swap(idx[i], idx[i + 1]);
                        swapped = true;
                    }
                }
            }
        }
        for (int i = 0; i < kept; ++i) {
            const Rec& rec = recs[idx[static_cast<size_t>(i)]];
            tracer->wr16(addr::kCodeSeg, addr::kSortKey, static_cast<uint16_t>(keys[static_cast<size_t>(i)]));
            if (rec.vehicle) {
                draw_vehicle(rec.code, rec.pos);
            } else {
                draw_object(rec.code, rec.pos);
            }
        }
    }
};

void ReferenceRenderer::Impl::line(int x0, int y0, int x1, int y1, uint8_t colour, bool a1ff) {
    // 4FC1: Cohen-Sutherland against the viewport, the first end first, intersections by IMUL/IDIV.
    const auto code = [this](int x, int y) {
        int c = 0;
        if (x < view.left) c |= 2;
        if (x > view.right) c |= 1;
        if (y < view.top) c |= 4;
        if (y > view.bottom) c |= 8;
        return c;
    };
    // b at a == at on the line (a0, b0) - (a1, b1).
    const auto cross = [](int a0, int b0, int a1, int b1, int at) {
        const int32_t num = int32_t{w16(at - a0)} * w16(b1 - b0);
        return w16(b0 + idiv16(num, w16(a1 - a0)));
    };
    for (int steps = 0; steps < 16; ++steps) {
        const int c0 = code(x0, y0), c1 = code(x1, y1);
        if (c0 & c1) return;
        if (c0) {
            if (c0 & 3) {
                const int x = (c0 & 1) ? view.right : view.left;
                y0 = cross(x0, y0, x1, y1, x);
                x0 = x;
            } else {
                const int y = (c0 & 4) ? view.top : view.bottom;
                x0 = cross(y0, x0, y1, x1, y);
                y0 = y;
            }
        } else if (c1) {
            if (c1 & 3) {
                const int x = (c1 & 1) ? view.right : view.left;
                y1 = cross(x0, y0, x1, y1, x);
                x1 = x;
            } else {
                const int y = (c1 & 4) ? view.top : view.bottom;
                x1 = cross(y0, x0, y1, x1, y);
                y1 = y;
            }
        } else {
            break;
        }
    }
    if (code(x0, y0) | code(x1, y1)) return;
    if (a1ff) {
        raster_a1ff(x0, y0, x1, y1, colour);
        return;
    }
    if (y0 == y1 && (x0 >> 3) == (x1 >> 3)) {
        // 5295: a horizontal line inside one screen byte ANDs the masks the wrong way round
        // (left[x1 & 7] & right[x0 & 7]), so only right-to-left lines (x1 <= x0) show.
        for (int x = x1; x <= x0; ++x) plot(x, y0, colour & 0x0F);
        return;
    }
    // 5170: Bresenham from the first end, error 2 minor - major, a minor step when the error >= 0.
    const int sx = x1 >= x0 ? 1 : -1, sy = y1 >= y0 ? 1 : -1;
    const int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
    const bool x_major = dy <= dx;
    const int major = x_major ? dx : dy, minor = x_major ? dy : dx;
    int e = 2 * minor - major;
    int x = x0, y = y0;
    for (int i = 0; i <= major; ++i) {
        plot(x, y, colour & 0x0F);
        if (e >= 0) {
            if (x_major) {
                y += sy;
            } else {
                x += sx;
            }
            e += 2 * (minor - major);
        } else {
            e += 2 * minor;
        }
        if (x_major) {
            x += sx;
        } else {
            y += sy;
        }
    }
}

// 3009:A1FF: from the first end; per pixel the error grows by the minor delta and, past half of it,
// gives back the major delta and steps the minor axis (the edge walker's scheme).
void ReferenceRenderer::Impl::raster_a1ff(int x0, int y0, int x1, int y1, uint8_t colour) {
    const uint8_t c = colour & 0x0F;
    if (x0 == x1) {
        for (int y = std::min(y0, y1); y <= std::max(y0, y1); ++y) plot(x0, y, c);
        return;
    }
    if (y0 == y1) {
        for (int x = std::min(x0, x1); x <= std::max(x0, x1); ++x) plot(x, y0, c);
        return;
    }
    const int sx = x1 > x0 ? 1 : -1, sy = y1 > y0 ? 1 : -1;
    const int adx = std::abs(x1 - x0), ady = std::abs(y1 - y0);
    int x = x0, y = y0, e = 0;
    if (ady < adx) {
        for (int i = 0; i <= adx; ++i) {
            plot(x, y, c);
            e += ady;
            if (e > (ady >> 1)) {
                e -= adx;
                y += sy;
            }
            x += sx;
        }
    } else {
        for (int i = 0; i <= ady; ++i) {
            plot(x, y, c);
            e += adx;
            if (e > (adx >> 1)) {
                e -= ady;
                x += sx;
            }
            y += sy;
        }
    }
}

ReferenceRenderer::ReferenceRenderer(const World& world) : impl_(std::make_unique<Impl>(world)) {}
ReferenceRenderer::~ReferenceRenderer() = default;

ReferenceStats ReferenceRenderer::render(const uint8_t* ram, std::vector<uint8_t>& pixels, int width) {
    Impl& m = *impl_;
    ReferenceStats stats;
    m.st = &stats;
    m.debug = debug;
    m.px = &pixels;
    m.width = width;
    if (!m.tracer) {
        m.tracer = std::make_unique<Tracer>(ram);
        m.tracer->set_exact_models(true);
    } else {
        m.tracer->load(ram);
    }
    m.view = {s16(m.ds16(kViewLeft)), s16(m.ds16(kViewTop)), s16(m.ds16(kViewRight)), s16(m.ds16(kViewBottom)),
              s16(m.ds16(kCentreX)), s16(m.ds16(kCentreY)), s16(m.ds16(kViewHeight))};
    m.cam_pos = {s16(m.ds16(addr::kCamera)), s16(m.ds16(addr::kCamera + 2)), s16(m.ds16(addr::kCamera + 4))};
    for (int i = 0; i < 9; ++i) m.cam[static_cast<size_t>(i)] = s16(m.ds16(static_cast<uint16_t>(kCamMatrix + 2 * i)));
    for (size_t i = 0; i < 24; ++i) {
        for (size_t c = 0; c < 3; ++c) {
            m.axes.vectors[i][c] = s16(m.ds16(static_cast<uint16_t>(kAxisTable + 6 * i + 2 * c)));
        }
    }
    for (size_t j = 0; j < 3; ++j) m.axes.base[j] = m.ds8(static_cast<uint16_t>(kAxisBase + 2 - j));
    m.mirror = m.ds8(addr::kMirrorPass) != 0;
    m.from_world = from_world;
    {
        // The camera matrix (3F2D's formula) from the negated angles, in floating point.
        constexpr double kDeg = 3.14159265358979323846 / 180.0;
        const double yaw = s16(m.ds16(addr::kCamera + 6)), pitch = s16(m.ds16(addr::kCamera + 8)),
                     roll = s16(m.ds16(addr::kCamera + 10));
        m.cam_yaw = yaw;
        const double sa = std::sin(-yaw * kDeg), ca = std::cos(-yaw * kDeg);
        const double sb = std::sin(-pitch * kDeg), cb = std::cos(-pitch * kDeg);
        const double sc = std::sin(-roll * kDeg), cc = std::cos(-roll * kDeg);
        m.fcam = {sa * sb * sc + ca * cc, sa * sb * cc + ca * sc, -sa * cb,  //
                  -cb * sc,               cb * cc,               sb,        //
                  ca * sb * sc + sa * cc, -ca * sb * cc + sa * sc, ca * cb};
    }
    if (debug) {
        // Does the frame's model data still match what was extracted at startup?
        const ImageView img{ram};
        for (size_t id = 0; id < kModelCount; ++id) {
            const Model& md = m.world.models[id];
            if (!md.present) continue;
            ModelMesh now;
            std::string e;
            decode_model_mesh(img, addr::kModelSeg, md.near_mesh.header, now, e);
            for (size_t f = 0; f < now.faces.size() && f < md.near_mesh.faces.size(); ++f) {
                const ModelFace& a = md.near_mesh.faces[f];
                const ModelFace& b = now.faces[f];
                if (a.flags != b.flags || a.colour.raw != b.colour.raw || a.colour_hi != b.colour_hi) {
                    std::printf("model %zu face %04X: extracted flags %04X colour %02X/%02X, frame %04X %02X/%02X\n", id,
                                a.address, a.flags, a.colour.raw, a.colour_hi, b.flags, b.colour.raw, b.colour_hi);
                }
            }
        }
    }
    m.rows = s16(m.ds16(kBigRows));
    m.cols = s16(m.ds16(kBigCols));

    // cell_lookup (3589) on the camera.
    const auto cx = static_cast<uint16_t>(m.cam_pos.x), cy = static_cast<uint16_t>(m.cam_pos.y);
    m.set16(kCellOriginX, cx & 0xF800);
    m.set16(kCellOriginY, cy & 0xF800);
    m.set16(kCamCellX, cx >> 11);
    m.set16(kCamCellY, cy >> 11);

    // draw_world_cells (30C6): the window for the heading's sector, far to near.
    constexpr WinCell k35F0{0xF800, 0xF800, -1, -1}, k35FF{0xF000, 0xF800, -2, -1}, k360E{0xF800, 0xF000, -1, -2},
        k361D{0xF800, 0, -1, 0}, k362C{0xF000, 0, -2, 0}, k363B{0xF800, 0x800, -1, 1}, k364A{0xF000, 0x800, -2, 1},
        k3659{0xF800, 0x1000, -1, 2}, k3668{0, 0xF800, 0, -1}, k3677{0, 0xF000, 0, -2}, k369C{0, 0x800, 0, 1},
        k36A9{0, 0x1000, 0, 2}, k36B6{0x800, 0xF800, 1, -1}, k36C5{0x800, 0xF000, 1, -2},
        k36D4{0x0FD0, 0xF800, 2, -1}, k36E3{0x800, 0, 1, 0}, k36F0{0x0FD0, 0, 2, 0}, k36FD{0x800, 0x800, 1, 1},
        k370C{0x800, 0x1000, 1, 2}, k371B{0x1000, 0x800, 2, 1}, kOwn{0, 0, 0, 0};
    struct Step {
        WinCell w;
        bool gated;
    };
    std::vector<Step> steps;
    const int yaw = s16(m.ds16(addr::kCamera + 6));
    uint8_t facing = 0;
    const bool hi_y = (cy & 0x7FF) >= 0x400, hi_x = (cx & 0x7FF) >= 0x400;
    if (yaw >= 0x13B || yaw < 0x2D) {
        facing = 0;
        if (hi_y) steps = {{k371B, true}, {k36FD, false}, {k369C, false}};
        else steps = {{k36D4, true}, {k36B6, false}, {k3668, false}};
        steps.insert(steps.end(), {{k36F0, true}, {k36E3, false}});
    } else if (yaw < 0x87) {
        facing = 1;
        if (hi_x) steps = {{k370C, true}, {k36FD, false}, {k36E3, false}};
        else steps = {{k3659, true}, {k363B, false}, {k361D, false}};
        steps.insert(steps.end(), {{k36A9, true}, {k369C, false}});
    } else if (yaw < 0xE1) {
        facing = 0;
        if (hi_y) steps = {{k364A, true}, {k363B, false}, {k369C, false}};
        else steps = {{k35FF, true}, {k35F0, false}, {k3668, false}};
        steps.insert(steps.end(), {{k362C, true}, {k361D, false}});
    } else {
        facing = 1;
        if (hi_x) steps = {{k36C5, true}, {k36B6, false}, {k36E3, false}};
        else steps = {{k360E, false}, {k35F0, false}, {k361D, false}};
        steps.insert(steps.end(), {{k3677, true}, {k3668, false}});
    }
    m.set8(addr::kFacing, facing);
    for (const Step& s : steps) {
        if (s.gated && m.mirror) continue;
        m.draw_cell(s.w, false);
    }
    m.draw_cell(kOwn, true);
    m.set8(addr::kOwnCell, 0);
    m.st = nullptr;
    return stats;
}

} // namespace vette::enhanced
