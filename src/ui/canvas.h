#pragma once
// A true-colour drawing surface for the app's own screens (the launch menu): square pixels at a
// small logical resolution, shown by the presenter at an integer scale for crisp, chunky text.

#include <cstdint>
#include <string_view>
#include <vector>

namespace vette::ui {

constexpr int kGlyph = 8;  // kFont8x8 cell size

struct Canvas {
    int width = 0;
    int height = 0;
    int scale = 1;                // window pixels per canvas pixel
    uint32_t background = 0;      // 0xRRGGBB, also used for the window around the canvas
    std::vector<uint32_t> pixels;  // 0xRRGGBB, row-major

    void reset(int w, int h, int s, uint32_t bg);
    void fill_rect(int x, int y, int w, int h, uint32_t color);
    // ASCII text, top-left at (x, y). `size` scales each font pixel; `bold` doubles strokes sideways.
    void text(int x, int y, std::string_view s, uint32_t color, int size = 1, bool bold = false);
};

constexpr int text_width(std::string_view s, int size = 1) { return static_cast<int>(s.size()) * kGlyph * size; }

}  // namespace vette::ui
