// The program icon: sizes, opacity, and whole-pixel scaling of its two drawings.

#include "test.h"
#include "ui/app_icon.h"

using vette::ui::app_icon;

TEST(app_icon_sizes_and_opacity) {
    for (const int size : {16, 32, 48, 64, 128, 256}) {
        const auto pixels = app_icon(size);
        CHECK(pixels.size() == static_cast<size_t>(size * size));
        bool opaque = true;
        for (const auto p : pixels)
            opaque = opaque && (p >> 24) == 0xFF;
        CHECK(opaque);
        CHECK(pixels.front() == 0xFF55FFFF);  // sky in the top-left corner
    }
}

TEST(app_icon_scales_by_whole_pixels) {
    const auto small = app_icon(16), large = app_icon(32);
    const auto x2 = app_icon(64), x3 = app_icon(48), x8 = app_icon(256);
    bool same = true;
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
            same = same && x8[y * 256 + x] == large[(y / 8) * 32 + x / 8];
            if (y < 64 && x < 64)
                same = same && x2[y * 64 + x] == large[(y / 2) * 32 + x / 2];
            if (y < 48 && x < 48)
                same = same && x3[y * 48 + x] == small[(y / 3) * 16 + x / 3];  // 48: the 16x16 drawing
        }
    }
    CHECK(same);
}
