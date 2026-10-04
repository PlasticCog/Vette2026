#include "ui/draw.h"

#include <algorithm>

#include "ui/font8x8.h"

namespace vette::ui {

void fill_rect(Framebuffer& fb, int x, int y, int w, int h, uint8_t color) {
    const int x0 = std::max(x, 0), x1 = std::min(x + w, fb.width);
    const int y0 = std::max(y, 0), y1 = std::min(y + h, fb.height);
    for (int row = y0; row < y1; ++row) {
        std::fill_n(fb.pixels.begin() + row * fb.width + x0, std::max(x1 - x0, 0), color);
    }
}

void frame_rect(Framebuffer& fb, int x, int y, int w, int h, uint8_t color) {
    fill_rect(fb, x, y, w, 1, color);
    fill_rect(fb, x, y + h - 1, w, 1, color);
    fill_rect(fb, x, y, 1, h, color);
    fill_rect(fb, x + w - 1, y, 1, h, color);
}

void draw_text(Framebuffer& fb, int x, int y, std::string_view text, uint8_t color) {
    for (const char ch : text) {
        const auto code = static_cast<unsigned char>(ch);
        const uint8_t* glyph = kFont8x8[code < 128 ? code : '?'];
        for (int gy = 0; gy < kGlyph; ++gy) {
            const int py = y + gy;
            if (py < 0 || py >= fb.height)
                continue;
            for (int gx = 0; gx < kGlyph; ++gx) {
                const int px = x + gx;
                if (px >= 0 && px < fb.width && ((glyph[gy] >> gx) & 1))
                    fb.pixels[static_cast<size_t>(py * fb.width + px)] = color;
            }
        }
        x += kGlyph;
    }
}

}  // namespace vette::ui
