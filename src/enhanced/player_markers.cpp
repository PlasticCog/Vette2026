#include "enhanced/player_markers.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "game/x86.h"
#include "host/machine.h"
#include "ui/font8x8.h"

namespace vette::enhanced {
namespace {

constexpr float kFocal = 256;  // project_vertices: x * 256 / z (as the scene's)
constexpr float kNear = 1;
constexpr double kDeg = 3.14159265358979323846 / 180.0;
// On top of everything the scene draws (SceneVertex::depth is halved and clipped at 1 on the GPU).
constexpr float kOnTop = 1.9f;

// DS (notes 03, 04, 12).
constexpr uint16_t kCamera = 0x2C71, kCamRow = 0x2C93, kCamCol = 0x2C95;  // x, y, z, yaw, pitch, roll
constexpr uint16_t kViewLeft = 0x315E, kViewTop = 0x315A, kViewRight = 0x3160, kViewBottom = 0x315C;
constexpr uint16_t kCentreX = 0x3169, kCentreY = 0x316B;
constexpr uint16_t kViewOffset = 0x2B87;
constexpr uint16_t kMirrorViewAhead = 0x35A3, kMirrorViewRight = 0x3587, kMirrorViewLeft = 0x3595;
constexpr uint16_t kHighway = 0x2AD4;          // byte: this car on a freeway
constexpr uint16_t kOtherOnFreeway = 0x842B;   // byte: the other car's last packet was a freeway's
constexpr uint16_t kPlayer = 0x2D35, kPlayerRow = 0x2D57, kPlayerCol = 0x2D59;  // x, y, z, heading
constexpr uint16_t kOther = 0x2F09, kOtherRow = 0x2F2B, kOtherCol = 0x2F2D;     // the remote car

// The arrow: on the ground round this car's middle, kRing out, pointing along +u (world units, ~3 in.).
constexpr float kRing = 104;
constexpr float kNearby = 200;  // closer than this, no arrow: the other car is right there
constexpr float kTagHeight = 20;  // the tag's point, just over the other car's roof
// The text's size: a font pixel per this many rows of the picture (the tag small, so it hides little).
constexpr float kTagRows = 1080, kBannerRows = 540;
constexpr int kMirrorFrame = 8;  // the rear-view mirror's frame, beyond its view (race-frame pixels)
struct P2f {
    float u, v;
};
constexpr P2f kShaft[] = {{-30, -7}, {6, -7}, {6, 7}, {-30, 7}};
constexpr P2f kHead[] = {{6, -20}, {32, 0}, {6, 20}};
constexpr float kOutline = 1.22f;  // the dark edge: the shapes scaled up, drawn first

struct Rgba {
    float r, g, b, a;
};
constexpr Rgba kArrow = {1.0f, 1.0f, 0.333f, 1};   // EGA 14
constexpr Rgba kEdge = {0.0f, 0.0f, 0.0f, 0.85f};
constexpr Rgba kTagBack = {0.0f, 0.0f, 0.0f, 0.7f};
constexpr Rgba kTagRim = {1.0f, 1.0f, 0.333f, 1};
constexpr Rgba kText = {1.0f, 1.0f, 1.0f, 1};

struct Ds {
    const uint8_t* p;
    uint8_t u8(uint16_t off) const { return p[off]; }
    uint16_t u16(uint16_t off) const { return static_cast<uint16_t>(p[off] | p[static_cast<uint16_t>(off + 1)] << 8); }
    int16_t s16(uint16_t off) const { return static_cast<int16_t>(u16(off)); }
};

struct V3 {
    float x, y, z;
};

// The race view's camera as SceneBuilder sets it up (scene.cpp, setup): the main view's or the mirror's.
struct Camera {
    double x = 0, y = 0, z = 0;
    float m[9] = {};
    float xs = 1;
    float cx = 160, cy = 60;
    int left = 0, top = 0, right = 319, bottom = 119;

    V3 view(double wx, double wy, double wz) const {
        const auto dx = static_cast<float>(wx - x), dy = static_cast<float>(wy - y), dz = static_cast<float>(wz - z);
        return {m[0] * dx + m[1] * dy + m[2] * dz, m[3] * dx + m[4] * dy + m[5] * dz, m[6] * dx + m[7] * dy + m[8] * dz};
    }
    void screen(const V3& c, float& sx, float& sy) const {
        sx = xs * c.x * kFocal / c.z + cx;
        sy = c.y * kFocal / c.z + cy;
    }
};

Camera read_camera(Ds ds, bool mirror) {
    Camera cam;
    int yaw = ds.s16(kCamera + 6), pitch = ds.s16(kCamera + 8);
    const int roll = ds.s16(kCamera + 10);
    uint16_t viewport = 0;
    if (mirror) {
        const int16_t offset = ds.s16(kViewOffset);
        if (offset == 0x55) {
            yaw = (yaw + 95) % 360;
            viewport = kMirrorViewRight;
        } else if (offset == -0x55) {
            yaw = (yaw - 95 + 360) % 360;
            viewport = kMirrorViewLeft;
        } else {
            yaw = (yaw - 180 + 360) % 360;
            viewport = kMirrorViewAhead;
        }
        pitch = -pitch;
        cam.xs = -1;
    }
    cam.x = double(ds.s16(kCamRow)) * 0x8000 + ds.u16(kCamera);
    cam.y = double(ds.s16(kCamCol)) * 0x8000 + ds.u16(kCamera + 2);
    cam.z = ds.s16(kCamera + 4);
    const double sa = std::sin(-yaw * kDeg), ca = std::cos(-yaw * kDeg);
    const double sb = std::sin(-pitch * kDeg), cb = std::cos(-pitch * kDeg);
    const double sc = std::sin(-roll * kDeg), cc = std::cos(-roll * kDeg);
    const double m[9] = {sa * sb * sc + ca * cc, sa * sb * cc + ca * sc, -sa * cb,  //
                         -cb * sc,               cb * cc,               sb,        //
                         ca * sb * sc + sa * cc, -ca * sb * cc + sa * sc, ca * cb};
    for (int j = 0; j < 3; ++j) {
        cam.m[j * 3 + 0] = static_cast<float>(m[6 + j]);   // dx (north)
        cam.m[j * 3 + 1] = static_cast<float>(m[0 + j]);   // dy (east)
        cam.m[j * 3 + 2] = static_cast<float>(-m[3 + j]);  // dz (up)
    }
    if (viewport) {
        cam.left = ds.s16(viewport);
        cam.top = ds.s16(static_cast<uint16_t>(viewport + 2));
        cam.right = ds.s16(static_cast<uint16_t>(viewport + 4));
        cam.bottom = ds.s16(static_cast<uint16_t>(viewport + 6));
        cam.cy = ds.s16(static_cast<uint16_t>(viewport + 8));
        cam.cx = ds.s16(static_cast<uint16_t>(viewport + 10));
    } else {
        cam.left = ds.s16(kViewLeft);
        cam.top = ds.s16(kViewTop);
        cam.right = ds.s16(kViewRight);
        cam.bottom = ds.s16(kViewBottom);
        cam.cx = ds.s16(kCentreX);
        cam.cy = ds.s16(kCentreY);
    }
    return cam;
}

// Emits triangles into a scene, at one depth.
struct Emitter {
    Scene& out;
    float depth;

    void triangle_fan(const std::vector<std::array<float, 2>>& pts, const Rgba& c) {
        if (pts.size() < 3) return;
        const auto base = static_cast<int32_t>(out.vertices.size());
        for (const auto& p : pts) out.vertices.push_back({p[0], p[1], c.r, c.g, c.b, c.a, depth});
        for (size_t i = 1; i + 1 < pts.size(); ++i) {
            out.indices.push_back(base);
            out.indices.push_back(base + static_cast<int32_t>(i));
            out.indices.push_back(base + static_cast<int32_t>(i + 1));
        }
    }
    void rect(float x0, float y0, float x1, float y1, const Rgba& c) { triangle_fan({{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}, c); }
};

// A convex polygon on the ground (camera space), clipped to the near plane, then onto the screen.
void ground_polygon(const Camera& cam, Emitter& e, const std::vector<V3>& poly, const Rgba& c) {
    std::vector<V3> clipped;
    for (size_t i = 0; i < poly.size(); ++i) {
        const V3& a = poly[i];
        const V3& b = poly[(i + 1) % poly.size()];
        const bool ain = a.z >= kNear, bin = b.z >= kNear;
        if (ain) clipped.push_back(a);
        if (ain != bin) {
            const float t = (kNear - a.z) / (b.z - a.z);
            clipped.push_back({a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), kNear});
        }
    }
    if (clipped.size() < 3) return;
    std::vector<std::array<float, 2>> pts;
    for (const V3& v : clipped) {
        float sx = 0, sy = 0;
        cam.screen(v, sx, sy);
        pts.push_back({sx, sy});
    }
    e.triangle_fan(pts, c);
}

// Text in the 8x8 font, its top left at (x, y), each font pixel gx x gy race-frame pixels: a quad per run
// of set pixels in a row.
void text(Emitter& e, float x, float y, std::string_view s, float gx, float gy, const Rgba& c) {
    for (size_t i = 0; i < s.size(); ++i) {
        const auto ch = static_cast<unsigned char>(s[i]);
        const uint8_t* rows = ui::kFont8x8[ch < 128 ? ch : '?'];
        const float x0 = x + static_cast<float>(i) * 8 * gx;
        for (int r = 0; r < 8; ++r) {
            for (int b = 0; b < 8;) {
                if (!(rows[r] >> b & 1)) {
                    ++b;
                    continue;
                }
                int end = b;
                while (end < 8 && (rows[r] >> end & 1)) ++end;
                e.rect(x0 + static_cast<float>(b) * gx, y + static_cast<float>(r) * gy, x0 + static_cast<float>(end) * gx,
                       y + static_cast<float>(r + 1) * gy, c);
                b = end;
            }
        }
    }
}

// Font pixels in whole output pixels: one for every `rows` rows of the picture (at least one).
void font_scale(const MarkerOptions& o, float rows, float& gx, float& gy) {
    const float k = std::max(1.0f, std::round(200 * o.pixel_h / rows));
    gx = k / std::max(o.pixel_w, 1e-3f);
    gy = k / std::max(o.pixel_h, 1e-3f);
}

float snap(float v, float px) { return std::round(v * px) / px; }

// A text in a box with a rim, its top left at (x, y); returns its size.
void tag_box(Emitter& e, float x, float y, std::string_view s, float gx, float gy, const MarkerOptions& o, float& w, float& h) {
    const float pad_x = 4 * gx, pad_y = 3 * gy;
    w = static_cast<float>(s.size()) * 8 * gx + 2 * pad_x;
    h = 8 * gy + 2 * pad_y;
    x = snap(x, o.pixel_w);
    y = snap(y, o.pixel_h);
    e.rect(x - gx, y - gy, x + w + gx, y + h + gy, kTagRim);
    e.rect(x, y, x + w, y + h, kTagBack);
    text(e, x + pad_x, y + pad_y, s, gx, gy, kText);
}

void set_view(const Camera& cam, Scene& scene) {
    scene.view_x0 = cam.left;
    scene.view_y0 = cam.top;
    scene.view_x1 = cam.right + 1;
    scene.view_y1 = cam.bottom + 1;
}

}  // namespace

bool add_player_markers(const uint8_t* data, const MarkerOptions& o, Scene& scene) {
    const Ds ds{data};
    if (ds.u8(kHighway) != 0 || ds.u8(kOtherOnFreeway) != 0) return false;
    const Camera cam = read_camera(ds, o.mirror);
    if (cam.right <= cam.left || cam.bottom <= cam.top) return false;
    set_view(cam, scene);
    Emitter e{scene, o.depth ? kOnTop : 0.0f};

    const double me_x = double(ds.s16(kPlayerRow)) * 0x8000 + ds.u16(kPlayer);
    const double me_y = double(ds.s16(kPlayerCol)) * 0x8000 + ds.u16(kPlayer + 2);
    const double me_z = ds.s16(kPlayer + 4);
    const double other_x = double(ds.s16(kOtherRow)) * 0x8000 + ds.u16(kOther);
    const double other_y = double(ds.s16(kOtherCol)) * 0x8000 + ds.u16(kOther + 2);
    const double other_z = ds.s16(kOther + 4);

    // The arrow, round this car, towards the other (x north, y east).
    const double dx = other_x - me_x, dy = other_y - me_y;
    const double distance = std::sqrt(dx * dx + dy * dy);
    if (distance >= kNearby) {
        const double ux = dx / distance, uy = dy / distance;  // along
        const double vx = -uy, vy = ux;                       // across
        const double cx = me_x + ux * kRing, cy = me_y + uy * kRing, z = me_z + 1;
        const auto shape = [&](std::span<const P2f> pts, float scale) {
            std::vector<V3> poly;
            for (const P2f& p : pts)
                poly.push_back(cam.view(cx + (p.u * ux + p.v * vx) * scale, cy + (p.u * uy + p.v * vy) * scale, z));
            return poly;
        };
        ground_polygon(cam, e, shape(kShaft, kOutline), kEdge);
        ground_polygon(cam, e, shape(kHead, kOutline), kEdge);
        ground_polygon(cam, e, shape(kShaft, 1), kArrow);
        ground_polygon(cam, e, shape(kHead, 1), kArrow);
    }

    // The tag over the other car, if it's in front of the camera and its point or the car in the view (close
    // by, the point is above the view: the tag stays at its top edge).
    const V3 point = cam.view(other_x, other_y, other_z + kTagHeight);
    const V3 base = cam.view(other_x, other_y, other_z);
    if (!o.name.empty() && point.z >= kNear && base.z >= kNear) {
        float sx = 0, sy = 0, bx = 0, by = 0;
        cam.screen(point, sx, sy);
        cam.screen(base, bx, by);
        const auto inside = [&](float x, float y) {
            return x >= cam.left && x <= cam.right + 1 && y >= cam.top && y <= cam.bottom + 1;
        };
        if (inside(sx, sy) || inside(bx, by)) {
            float gx = 1, gy = 1;
            font_scale(o, kTagRows, gx, gy);
            const float w = static_cast<float>(o.name.size()) * 8 * gx + 8 * gx, h = 8 * gy + 6 * gy;
            const float stem = 5 * gy;
            // Kept in the view: beside its point at the edges, under it at the top.
            float x = std::clamp(sx - w / 2, static_cast<float>(cam.left) + gx, static_cast<float>(cam.right + 1) - w - gx);
            float y = std::clamp(sy - stem - h, static_cast<float>(cam.top) + gy, static_cast<float>(cam.bottom + 1) - h - gy);
            if (o.avoid_mirror && !o.mirror) {
                // The mirror's view and its frame round it (3009:6439: up to 8 pixels beyond it).
                const Camera m = read_camera(ds, true);
                const auto mx0 = static_cast<float>(m.left - kMirrorFrame), my0 = static_cast<float>(m.top - kMirrorFrame);
                const auto mx1 = static_cast<float>(m.right + 1 + kMirrorFrame);
                const auto my1 = static_cast<float>(m.bottom + 1 + kMirrorFrame);
                if (x - gx < mx1 && x + w + gx > mx0 && y - gy < my1 && y + h + gy > my0) {
                    if (by > my1 + h + 2 * gy || mx0 - w - 2 * gx < static_cast<float>(cam.left) + gx)
                        y = my1 + gy;          // below it, still over the car
                    else
                        x = mx0 - w - 2 * gx;  // beside it, its point slanting to the car
                }
            }
            x = snap(x, o.pixel_w);
            y = snap(y, o.pixel_h);
            // Its pointer, from the box's bottom to the point (when the box is above it).
            if (y + h <= sy) {
                const float half = 4 * gx;
                const float px = std::clamp(sx, x + half, x + w - half);
                e.triangle_fan({{px - half - gx, y + h}, {px + half + gx, y + h}, {sx, sy + gy}}, kTagRim);
            }
            float bw = 0, bh = 0;
            tag_box(e, x, y, o.name, gx, gy, o, bw, bh);
        }
    }
    return true;
}

void add_banner(const uint8_t* data, std::string_view s, const MarkerOptions& o, Scene& scene) {
    const Camera cam = read_camera(Ds{data}, false);
    if (cam.right <= cam.left || cam.bottom <= cam.top) return;
    set_view(cam, scene);
    Emitter e{scene, o.depth ? kOnTop : 0.0f};
    float gx = 1, gy = 1;
    font_scale(o, kBannerRows, gx, gy);
    const float w = static_cast<float>(s.size()) * 8 * gx + 8 * gx;
    float bw = 0, bh = 0;
    tag_box(e, (static_cast<float>(cam.left + cam.right + 1) - w) / 2, static_cast<float>(cam.top) + 6 * gy, s, gx, gy, o, bw, bh);
}

void draw_scene_paletted(const Scene& scene, uint8_t* pixels, int width, int height, const std::array<uint32_t, 16>& palette,
                         std::array<int, 4> skip) {
    const int vx0 = std::max(scene.view_x0, 0), vy0 = std::max(scene.view_y0, 0);
    const int vx1 = std::min(scene.view_x1, width), vy1 = std::min(scene.view_y1, height);
    const bool skipping = skip[2] > skip[0] && skip[3] > skip[1];
    for (size_t t = 0; t + 2 < scene.indices.size(); t += 3) {
        const SceneVertex& a = scene.vertices[static_cast<size_t>(scene.indices[t])];
        const SceneVertex& b = scene.vertices[static_cast<size_t>(scene.indices[t + 1])];
        const SceneVertex& c = scene.vertices[static_cast<size_t>(scene.indices[t + 2])];
        if (a.a < 0.5f) continue;
        // The nearest palette colour.
        int best = 0;
        float best_d = 1e9f;
        for (int i = 0; i < 16; ++i) {
            const float r = static_cast<float>(palette[static_cast<size_t>(i)] >> 16 & 0xFF) / 255.0f - a.r;
            const float g = static_cast<float>(palette[static_cast<size_t>(i)] >> 8 & 0xFF) / 255.0f - a.g;
            const float bl = static_cast<float>(palette[static_cast<size_t>(i)] & 0xFF) / 255.0f - a.b;
            const float d = r * r + g * g + bl * bl;
            if (d < best_d) {
                best_d = d;
                best = i;
            }
        }
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::fabs(area) < 1e-6f) continue;
        const float s = area > 0 ? 1.0f : -1.0f;
        const int x0 = std::max(vx0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
        const int x1 = std::min(vx1 - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
        const int y0 = std::max(vy0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
        const int y1 = std::min(vy1 - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
        const auto edge = [s](const SceneVertex& p, const SceneVertex& q, float x, float y) {
            return s * ((q.x - p.x) * (y - p.y) - (q.y - p.y) * (x - p.x));
        };
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                if (skipping && x >= skip[0] && x < skip[2] && y >= skip[1] && y < skip[3]) continue;
                const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
                if (edge(a, b, px, py) >= 0 && edge(b, c, px, py) >= 0 && edge(c, a, px, py) >= 0)
                    pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] = static_cast<uint8_t>(best);
            }
        }
    }
}

void append_clipped(Scene& to, const Scene& from) {
    const auto x0 = static_cast<float>(from.view_x0), y0 = static_cast<float>(from.view_y0);
    const auto x1 = static_cast<float>(from.view_x1), y1 = static_cast<float>(from.view_y1);
    std::vector<SceneVertex> poly, next;
    // Keeps the part of `poly` where f(v) >= 0 (f linear along each edge).
    const auto cut = [&](auto f) {
        next.clear();
        for (size_t i = 0; i < poly.size(); ++i) {
            const SceneVertex& a = poly[i];
            const SceneVertex& b = poly[(i + 1) % poly.size()];
            const float fa = f(a), fb = f(b);
            if (fa >= 0) next.push_back(a);
            if ((fa >= 0) != (fb >= 0)) {
                SceneVertex v = a;
                const float t = fa / (fa - fb);
                v.x = a.x + t * (b.x - a.x);
                v.y = a.y + t * (b.y - a.y);
                next.push_back(v);
            }
        }
        poly.swap(next);
    };
    for (size_t t = 0; t + 2 < from.indices.size(); t += 3) {
        poly = {from.vertices[static_cast<size_t>(from.indices[t])], from.vertices[static_cast<size_t>(from.indices[t + 1])],
                from.vertices[static_cast<size_t>(from.indices[t + 2])]};
        cut([&](const SceneVertex& v) { return v.x - x0; });
        cut([&](const SceneVertex& v) { return x1 - v.x; });
        cut([&](const SceneVertex& v) { return v.y - y0; });
        cut([&](const SceneVertex& v) { return y1 - v.y; });
        if (poly.size() < 3) continue;
        const auto base = static_cast<int32_t>(to.vertices.size());
        to.vertices.insert(to.vertices.end(), poly.begin(), poly.end());
        for (size_t i = 1; i + 1 < poly.size(); ++i) {
            to.indices.push_back(base);
            to.indices.push_back(base + static_cast<int32_t>(i));
            to.indices.push_back(base + static_cast<int32_t>(i + 1));
        }
    }
}

// --- The frame on screen (Classic) ----------------------------------------------------------------------

namespace {
constexpr uint16_t kCode = game::emu_seg(0x3009);
constexpr uint16_t kWorldDrawn = 0x036E;  // call draw_world_cells: the camera and viewport set
constexpr uint16_t kMirrorDrawn = 0x0666;  // draw_mirror_view
constexpr uint16_t kShown = 0x0546;        // after crtc_set_start_vsync: the frame is on screen
constexpr uint64_t kStaleNs = 500'000'000;
}  // namespace

struct ShownFrame::State {
    std::vector<uint8_t> pending = std::vector<uint8_t>(0x10000), shown = std::vector<uint8_t>(0x10000);
    bool pending_valid = false, pending_mirror = false, shown_mirror = false;
    uint64_t shown_at = 0;
};

ShownFrame::ShownFrame(host::Machine& machine) : machine_(machine), state_(std::make_shared<State>()) {
    host::Cpu& cpu = machine.cpu();
    auto s = state_;
    watches_.push_back(cpu.add_watch(host::Cpu::linear(kCode, kWorldDrawn), [s](host::Cpu& c) {
        std::memcpy(s->pending.data(), c.memory().ram() + host::Cpu::linear(game::kDataSeg, 0), 0x10000);
        s->pending_valid = true;
        s->pending_mirror = false;
    }));
    watches_.push_back(cpu.add_watch(host::Cpu::linear(kCode, kMirrorDrawn), [s](host::Cpu&) { s->pending_mirror = true; }));
    host::Machine* m = &machine;
    watches_.push_back(cpu.add_watch(host::Cpu::linear(kCode, kShown), [s, m](host::Cpu&) {
        if (!s->pending_valid) {
            s->shown_at = 0;  // a freeway's frame
            return;
        }
        s->pending.swap(s->shown);
        s->shown_mirror = s->pending_mirror;
        s->pending_valid = false;
        s->shown_at = m->emulated_ns();
    }));
}

ShownFrame::~ShownFrame() {
    for (const uint64_t id : watches_) machine_.cpu().remove_watch(id);
}

const uint8_t* ShownFrame::ds(uint64_t now_ns, bool& mirror) const {
    if (state_->shown_at == 0 || now_ns - state_->shown_at > kStaleNs) return nullptr;
    mirror = state_->shown_mirror;
    return state_->shown.data();
}

}  // namespace vette::enhanced
