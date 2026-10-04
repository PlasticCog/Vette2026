#include "ui/canvas.h"

#include <algorithm>

#include "ui/font8x8.h"

namespace vette::ui {

void Canvas::reset(int w, int h, int s, uint32_t bg) {
    width = w;
    height = h;
    scale = s;
    background = bg;
    pixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h), bg);
}

void Canvas::fill_rect(int x, int y, int w, int h, uint32_t color) {
    const int x0 = std::max(x, 0), x1 = std::min(x + w, width);
    const int y0 = std::max(y, 0), y1 = std::min(y + h, height);
    for (int row = y0; row < y1; ++row) {
        std::fill_n(pixels.begin() + row * width + x0, std::max(x1 - x0, 0), color);
    }
}

void Canvas::text(int x, int y, std::string_view s, uint32_t color, int size, bool bold) {
    for (const char ch : s) {
        const auto code = static_cast<unsigned char>(ch);
        const uint8_t* glyph = kFont8x8[code < 128 ? code : '?'];
        for (int gy = 0; gy < kGlyph; ++gy) {
            for (int gx = 0; gx < kGlyph; ++gx) {
                if ((glyph[gy] >> gx) & 1) {
                    fill_rect(x + gx * size, y + gy * size, size + (bold ? 1 : 0), size, color);
                }
            }
        }
        x += kGlyph * size;
    }
}

}  // namespace vette::ui
