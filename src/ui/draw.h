#pragma once
// Drawing into an indexed Framebuffer: 8x8 text (kFont8x8) and rectangles, clipped to the frame.

#include <cstdint>
#include <string_view>

#include "platform/framebuffer.h"

namespace vette::ui {

constexpr int kGlyph = 8;  // the font's cell size

void fill_rect(Framebuffer& fb, int x, int y, int w, int h, uint8_t color);
void frame_rect(Framebuffer& fb, int x, int y, int w, int h, uint8_t color);  // 1-pixel outline
// Draws ASCII text with its top-left corner at (x, y). Background pixels are left alone.
void draw_text(Framebuffer& fb, int x, int y, std::string_view text, uint8_t color);

}  // namespace vette::ui
