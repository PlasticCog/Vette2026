#pragma once
// QuickDraw pictures (PICT, versions 1 and 2), as the Mac VETTE!'s screens and sprites are stored.
// The decoder plays the picture onto a canvas the size of its frame and covers what VETTE! uses:
// BitsRect / PackBitsRect bitmaps and indexed pixmaps (1/2/4/8 bits with a colour table),
// DirectBitsRect (16/32-bit), srcCopy/srcOr/transparent modes, rectangular clipping, and the simple
// pen shapes (lines, rectangles, ovals). Text needs the Mac's fonts, so it is recorded, not drawn.
// Malformed input fails cleanly: every read is bounds-checked and sizes are limited.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace vette::assets {

struct PictText {
    int x = 0, y = 0;                  // pen position (baseline) in canvas pixels
    int font = 0, size = 0, face = 0;  // QuickDraw font number, point size, style bits
    std::string text;                  // Mac Roman
};

struct Pict {
    int version = 0;                     // 1 or 2
    int frame_left = 0, frame_top = 0;   // picFrame's top-left in the picture's own coordinates
    int width = 0, height = 0;           // picFrame's size = the canvas
    // 0xAARRGGBB, top row first. Alpha is 0 where the picture drew nothing (QuickDraw would leave
    // the destination as it was there); everything drawn is opaque.
    std::vector<std::uint32_t> pixels;
    std::vector<PictText> texts;         // text the picture draws (not rendered)
    int skipped = 0;                     // drawing operations approximated or skipped (diagnostics)
};

// Decodes a PICT resource, or a PICT file with its 512-byte header. Returns false and sets `error`
// (when given) if the data is malformed or uses an opcode whose length can't be known.
bool decode_pict(std::span<const std::uint8_t> data, Pict& out, std::string* error = nullptr);

// PackBits (Apple's RLE): decodes until `out_size` bytes are produced or the input ends. Returns the
// number of input bytes consumed, or -1 if the input runs out first. `unit` is 1 (bytes) or 2 (the
// 16-bit pixmap variant, where runs repeat words).
long unpack_bits(std::span<const std::uint8_t> in, std::uint8_t* out, std::size_t out_size, int unit = 1);

}  // namespace vette::assets
