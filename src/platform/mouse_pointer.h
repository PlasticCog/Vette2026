#pragma once
// The INT 33h mouse driver's pointer, drawn by the frontend. The hosted driver tracks the pointer
// but never draws it into video memory, so it is overlaid on the copy of each frame that goes to
// the screen, at frame-pixel scale like the rest of the image. Header-only and SDL-free, so the
// unit tests can use it.

#include "platform/framebuffer.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace vette {

// The Microsoft mouse driver's default graphics-mode cursor: a 16x16 arrow with its hot spot at the
// tip (0, 0). One word per row, bit 15 is the leftmost pixel. Each pixel becomes
// (pixel AND screen) XOR cursor with the mask bit applied to all four planes, so the outline is
// color 0 and the fill color 15: black and white in the default EGA palette, which VETTE! keeps.
inline constexpr std::array<std::uint16_t, 16> kPointerScreenMask = {
    0x3FFF, 0x1FFF, 0x0FFF, 0x07FF, 0x03FF, 0x01FF, 0x00FF, 0x007F,
    0x003F, 0x001F, 0x01FF, 0x10FF, 0x30FF, 0xF87F, 0xF87F, 0xFCFF,
};
inline constexpr std::array<std::uint16_t, 16> kPointerCursorMask = {
    0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7C00, 0x7E00, 0x7F00,
    0x7F80, 0x7C00, 0x6C00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0000,
};

// The driver's virtual screen is 640 wide in every mode VETTE! uses, so in 320-wide modes x is
// twice the pixel column. Its height matches the frame's (200 lines).
inline constexpr int kMouseVirtualWidth = 640;

// Draws the pointer with its hot spot at driver position (x, y), clipped to the frame.
inline void draw_mouse_pointer(Framebuffer& fb, int x, int y) {
    const int left = x * fb.width / kMouseVirtualWidth;
    for (int row = 0; row < 16; ++row) {
        const int py = y + row;
        if (py < 0 || py >= fb.height)
            continue;
        for (int col = 0; col < 16; ++col) {
            const int px = left + col;
            if (px < 0 || px >= fb.width)
                continue;
            const unsigned bit = 0x8000u >> col;
            const unsigned and_mask = (kPointerScreenMask[row] & bit) ? 0x0Fu : 0u;
            const unsigned xor_mask = (kPointerCursorMask[row] & bit) ? 0x0Fu : 0u;
            std::uint8_t& pixel = fb.pixels[static_cast<std::size_t>(py) * static_cast<std::size_t>(fb.width) +
                                            static_cast<std::size_t>(px)];
            pixel = static_cast<std::uint8_t>((pixel & and_mask) ^ xor_mask);
        }
    }
}

}  // namespace vette
