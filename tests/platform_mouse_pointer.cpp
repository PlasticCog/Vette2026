// The mouse driver's pointer as the frontend overlays it on a frame (platform/mouse_pointer.h).

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "platform/mouse_pointer.h"
#include "test.h"

using vette::draw_mouse_pointer;
using vette::Framebuffer;

namespace {

// The arrow as it should appear: B = color 0 (black outline), W = color 15 (white fill),
// . = the frame shows through.
constexpr const char* kArrow[16] = {
    "BB..............", "BWB.............", "BWWB............", "BWWWB...........",
    "BWWWWB..........", "BWWWWWB.........", "BWWWWWWB........", "BWWWWWWWB.......",
    "BWWWWWWWWB......", "BWWWWWBBBBB.....", "BWWBWWB.........", "BWB.BWWB........",
    "BB..BWWB........", ".....BWWB.......", ".....BWWB.......", "......BB........",
};

constexpr std::uint8_t kBackground = 7;

Framebuffer make_frame(int width) {
    Framebuffer fb;
    fb.width = width;
    fb.height = Framebuffer::kHeight;
    fb.pixels.assign(static_cast<std::size_t>(width * fb.height), kBackground);
    return fb;
}

// Every pixel of the frame must be the arrow's (drawn with its tip at left, top) or untouched.
void check_frame(const Framebuffer& fb, int left, int top) {
    CHECK_EQ(fb.pixels.size(), static_cast<std::size_t>(fb.width * fb.height));
    int mismatches = 0;
    for (int y = 0; y < fb.height; ++y) {
        for (int x = 0; x < fb.width; ++x) {
            const int col = x - left;
            const int row = y - top;
            const char c = (col >= 0 && col < 16 && row >= 0 && row < 16) ? kArrow[row][col] : '.';
            const std::uint8_t want = c == 'B' ? 0 : c == 'W' ? 15 : kBackground;
            if (fb.pixels[static_cast<std::size_t>(y * fb.width + x)] != want && mismatches++ == 0)
                CHECK_EQ(fb.pixels[static_cast<std::size_t>(y * fb.width + x)], want);
        }
    }
    CHECK_EQ(mismatches, 0);
}

} // namespace

TEST(mouse_pointer_shape_640) {
    Framebuffer fb = make_frame(640);
    draw_mouse_pointer(fb, 100, 50);
    check_frame(fb, 100, 50);
    CHECK_EQ(fb.pixels[50 * 640 + 100], 0);   // the hot spot is the arrow's tip
    CHECK_EQ(fb.pixels[61 * 640 + 103], 7);   // the notch between shaft and tail shows through
}

TEST(mouse_pointer_320_wide_halves_x) {
    // Mode 0Dh: the driver's x is twice the pixel column; the arrow is still 16 frame pixels wide.
    Framebuffer fb = make_frame(320);
    draw_mouse_pointer(fb, 101, 20);
    check_frame(fb, 50, 20);
}

TEST(mouse_pointer_clipped_at_edges) {
    Framebuffer fb = make_frame(640);
    draw_mouse_pointer(fb, 639, 199);  // only the tip is on screen
    check_frame(fb, 639, 199);
    CHECK_EQ(fb.pixels[199 * 640 + 639], 0);

    Framebuffer partial = make_frame(640);
    draw_mouse_pointer(partial, 630, 190);
    check_frame(partial, 630, 190);

    Framebuffer above_left = make_frame(320);
    draw_mouse_pointer(above_left, -8, -5);  // pixel column -4
    check_frame(above_left, -4, -5);
}

TEST(mouse_pointer_and_xor_on_palette_index) {
    // Like the real driver: outline = pixel AND 0 = color 0, fill = color 15, whatever is beneath.
    Framebuffer fb = make_frame(640);
    std::fill(fb.pixels.begin(), fb.pixels.end(), std::uint8_t{15});
    draw_mouse_pointer(fb, 0, 0);
    CHECK_EQ(fb.pixels[0], 0);
    CHECK_EQ(fb.pixels[1 * 640 + 1], 15);
    CHECK_EQ(fb.pixels[1 * 640 + 3], 15);  // transparent: unchanged
}
