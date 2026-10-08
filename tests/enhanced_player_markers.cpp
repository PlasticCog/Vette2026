// Two-player markers (enhanced/player_markers.h): the other player's tag and the arrow to them, on a made-up
// frame's data segment: the camera at eye height over this car, looking north, the main view's viewport;
// the helicopter view's arrow on the ground round the car, the driver's seat's at the top of the view.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "enhanced/player_markers.h"
#include "test.h"

namespace en = vette::enhanced;

namespace {

struct Frame {
    std::vector<uint8_t> ds = std::vector<uint8_t>(0x10000);
    void w16(uint16_t off, int v) {
        ds[off] = static_cast<uint8_t>(v);
        ds[static_cast<uint16_t>(off + 1)] = static_cast<uint8_t>(v >> 8);
    }
    // This car at the middle of big tile (2, 2), heading north, the camera 10 over it; the other car
    // `north` and `east` of it. `external`: the helicopter view.
    Frame(int north, int east, bool external = true) {
        ds[0x2ACF] = external ? 0xFF : 0;
        w16(0x2C71, 0x4000), w16(0x2C73, 0x4000), w16(0x2C75, 10);  // camera x, y, z
        w16(0x2C93, 2), w16(0x2C95, 2);
        w16(0x315E, 0), w16(0x315A, 0), w16(0x3160, 319), w16(0x315C, 119);  // left, top, right, bottom
        w16(0x3169, 160), w16(0x316B, 60);                                    // centre
        w16(0x2D35, 0x4000), w16(0x2D37, 0x4000), w16(0x2D57, 2), w16(0x2D59, 2);
        w16(0x2F09, 0x4000 + north), w16(0x2F0B, 0x4000 + east), w16(0x2F2B, 2), w16(0x2F2D, 2);
    }
};

struct Bounds {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    int n = 0;
};
// The vertices of one colour (r, g, b, a) below `min_y`: the tag's white text, or the yellow of the arrow
// (on the ground, below the horizon at y 60) and of the tag's rim; the driver's-seat arrow's (a 0.9).
Bounds bounds(const en::Scene& s, float r, float g, float b, float min_y = -1e9f, float a = 1) {
    Bounds out;
    for (const en::SceneVertex& v : s.vertices) {
        if (std::fabs(v.r - r) > 0.01f || std::fabs(v.g - g) > 0.01f || std::fabs(v.b - b) > 0.01f || std::fabs(v.a - a) > 0.01f)
            continue;
        if (v.y < min_y) continue;
        out.x0 = std::min(out.x0, v.x), out.x1 = std::max(out.x1, v.x);
        out.y0 = std::min(out.y0, v.y), out.y1 = std::max(out.y1, v.y);
        ++out.n;
    }
    return out;
}
constexpr float kYellow[3] = {1.0f, 1.0f, 0.333f};

}  // namespace

TEST(player_markers_ahead) {
    const Frame f(1000, 0);
    en::Scene s;
    en::MarkerOptions o;
    o.name = "Bob";
    CHECK(en::add_player_markers(f.ds.data(), o, s));
    CHECK(s.view_x0 == 0 && s.view_y0 == 0 && s.view_x1 == 320 && s.view_y1 == 120);
    // The text over the other car's roof (20 over its base, which is 1000 ahead: y 57.4), centred.
    const Bounds text = bounds(s, 1, 1, 1);
    CHECK(text.n > 0);
    CHECK(text.y1 < 57.4f && text.y0 > 40);
    CHECK(std::fabs((text.x0 + text.x1) / 2 - 160) < 2);
    // The arrow on the ground ahead (74-136 units out, eye height 11: y 80-98), pointing north.
    const Bounds arrow = bounds(s, kYellow[0], kYellow[1], kYellow[2], 60);
    CHECK(arrow.n > 0);
    CHECK(arrow.y0 > 75 && arrow.y1 < 100);
    CHECK(std::fabs((arrow.x0 + arrow.x1) / 2 - 160) < 1);
    // In a frame of palette indices: some yellow (EGA 14) pixels in the view, none below it.
    std::array<uint32_t, 16> ega = {0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
                                    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};
    std::vector<uint8_t> pixels(320 * 200, 3);
    en::draw_scene_paletted(s, pixels.data(), 320, 200, ega);
    int yellow = 0, below = 0;
    for (int y = 0; y < 200; ++y) {
        for (int x = 0; x < 320; ++x) {
            if (pixels[static_cast<size_t>(y * 320 + x)] == 14) (y < 120 ? yellow : below)++;
        }
    }
    CHECK(yellow > 100);
    CHECK_EQ(below, 0);
}

TEST(player_markers_to_the_side) {
    // 20 degrees east of north: the arrow right of the middle; a car behind: no tag.
    const Frame f(940, 342);
    en::Scene s;
    en::MarkerOptions o;
    o.name = "Bob";
    CHECK(en::add_player_markers(f.ds.data(), o, s));
    const Bounds arrow = bounds(s, kYellow[0], kYellow[1], kYellow[2], 60);
    CHECK(arrow.n > 0 && (arrow.x0 + arrow.x1) / 2 > 200);

    const Frame behind(-1000, 0);
    en::Scene b;
    CHECK(en::add_player_markers(behind.ds.data(), o, b));
    CHECK_EQ(bounds(b, 1, 1, 1).n, 0);
    CHECK_EQ(bounds(b, kYellow[0], kYellow[1], kYellow[2], 60).n, 0);  // (behind the camera)
}

TEST(player_markers_close_by_and_off) {
    // Close by: no arrow (the car is right there), the tag still.
    const Frame f(150, 0);
    en::Scene s;
    en::MarkerOptions o;
    o.name = "Bob";
    CHECK(en::add_player_markers(f.ds.data(), o, s));
    CHECK_EQ(bounds(s, kYellow[0], kYellow[1], kYellow[2], 60).n, 0);
    CHECK(bounds(s, 1, 1, 1).n > 0);
    // On a freeway (this car's or the other's): nothing.
    Frame fw(1000, 0);
    fw.ds[0x2AD4] = 0xFF;
    en::Scene none;
    CHECK(!en::add_player_markers(fw.ds.data(), o, none));
    CHECK(none.vertices.empty());
}

TEST(player_markers_tag_size) {
    // Its font pixels are whole output pixels: one per 1080 rows of the picture.
    const Frame f(1000, 0);
    en::MarkerOptions o;
    o.name = "Bob";
    o.pixel_w = 9, o.pixel_h = 10.8f;  // 4K, 4:3
    en::Scene s;
    CHECK(en::add_player_markers(f.ds.data(), o, s));
    const Bounds text = bounds(s, 1, 1, 1);
    CHECK(text.n > 0);
    const float tall = (text.y1 - text.y0) * o.pixel_h;  // the letters' height, in output pixels
    CHECK(tall >= 12 && tall <= 16.5f);
}

TEST(player_markers_drivers_seat) {
    en::MarkerOptions o;
    o.name = "Bob";
    // The arrow's vertices (a 0.9), for the other car `north` and `east` of this one, in the driver's seat.
    const auto hud = [&](int north, int east) {
        const Frame f(north, east, false);
        en::Scene s;
        CHECK(en::add_player_markers(f.ds.data(), o, s));
        CHECK_EQ(bounds(s, kYellow[0], kYellow[1], kYellow[2], 60).n, 0);  // no arrow on the ground
        std::vector<en::SceneVertex> out;
        for (const en::SceneVertex& v : s.vertices) {
            if (std::fabs(v.a - 0.9f) < 0.01f) out.push_back(v);
        }
        CHECK(!out.empty());
        return out;
    };
    // Its middle, and the vertex farthest along (dx, dy): the tip if it points that way.
    const auto look = [](const std::vector<en::SceneVertex>& v, float dx, float dy, float& mx, float& my, en::SceneVertex& tip) {
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, best = -1e9f;
        for (const en::SceneVertex& p : v) {
            x0 = std::min(x0, p.x), x1 = std::max(x1, p.x), y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
            if (p.x * dx + p.y * dy > best) best = p.x * dx + p.y * dy, tip = p;
        }
        mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
    };
    float x = 0, y = 0;
    en::SceneVertex tip;
    // Ahead: at the top of the view, in the middle, pointing up.
    look(hud(1000, 0), 0, -1, x, y, tip);
    CHECK(std::fabs(x - 160) < 2 && y < 40 && y > 0);
    CHECK(std::fabs(tip.x - x) < 1 && tip.y < y - 8);
    // To the right: down the right of the view, pointing right.
    look(hud(0, 1000), 1, 0, x, y, tip);
    CHECK(x > 230 && y > 30 && y < 70);
    CHECK(std::fabs(tip.y - y) < 2 && tip.x > x + 8);
    // To the left: the same on the left.
    float lx = 0, ly = 0;
    look(hud(0, -1000), -1, 0, lx, ly, tip);
    CHECK(std::fabs((320 - lx) - x) < 2 && std::fabs(ly - y) < 2);
    CHECK(std::fabs(tip.y - ly) < 2 && tip.x < lx - 8);
    // Behind (to the right of it): at the right edge, pointing right (never backwards).
    float bx = 0, by = 0;
    look(hud(-1000, 200), 1, 0, bx, by, tip);
    CHECK(std::fabs(bx - x) < 1 && std::fabs(by - y) < 1);
    CHECK(std::fabs(tip.y - by) < 2 && tip.x > bx + 8);
    look(hud(-1000, -200), -1, 0, bx, by, tip);
    CHECK(std::fabs(bx - lx) < 1 && tip.x < bx - 8);
    // With a scene over everything, it goes there.
    {
        const Frame f(1000, 0, false);
        en::Scene s, top;
        en::MarkerOptions oh = o;
        oh.hud = &top;
        CHECK(en::add_player_markers(f.ds.data(), oh, s));
        CHECK_EQ(bounds(s, kYellow[0], kYellow[1], kYellow[2], -1e9f, 0.9f).n, 0);
        CHECK(bounds(top, kYellow[0], kYellow[1], kYellow[2], -1e9f, 0.9f).n > 0);
        CHECK(top.view_x1 == 320 && top.view_y1 == 120);
    }
    // Close by: none.
    const Frame near(150, 0, false);
    en::Scene s;
    CHECK(en::add_player_markers(near.ds.data(), o, s));
    CHECK_EQ(bounds(s, kYellow[0], kYellow[1], kYellow[2], -1e9f, 0.9f).n, 0);
}

TEST(player_markers_in_pixels) {
    // For the original's frame: the tag's letters 5 pixels tall, the driver's-seat arrow about 16 long, outlined.
    en::MarkerOptions o;
    o.name = "Bob";
    o.pixels = true;
    const Frame f(1000, 0, false);
    en::Scene s;
    CHECK(en::add_player_markers(f.ds.data(), o, s));
    const Bounds text = bounds(s, 1, 1, 1);
    CHECK(text.y1 - text.y0 == 5.0f);
    CHECK(text.x1 - text.x0 == 11.0f);  // B, o, b: 3 wide, a pixel apart
    const Bounds arrow = bounds(s, kYellow[0], kYellow[1], kYellow[2], -1e9f, 0.9f);
    CHECK(arrow.y1 - arrow.y0 > 14 && arrow.y1 - arrow.y0 < 18);
}
