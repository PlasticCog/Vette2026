// Two-player markers (enhanced/player_markers.h): the other player's tag and the arrow to them, on a made-up
// frame's data segment: the camera at eye height over this car, looking north, the main view's viewport.

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
    // `north` and `east` of it.
    Frame(int north, int east) {
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
// The vertices of one colour (r, g, b) below `min_y`: the tag's white text, or the yellow of the arrow (on
// the ground, below the horizon at y 60) and of the tag's rim.
Bounds bounds(const en::Scene& s, float r, float g, float b, float min_y = -1e9f) {
    Bounds out;
    for (const en::SceneVertex& v : s.vertices) {
        if (std::fabs(v.r - r) > 0.01f || std::fabs(v.g - g) > 0.01f || std::fabs(v.b - b) > 0.01f || v.a < 0.99f) continue;
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
