#pragma once
// What the presenter draws for one displayed frame when replacement art applies: a stack of layers
// in the DOS frame's coordinates (frame_w x frame_h, shown 4:3 like the frame itself).
//
//   1. `backdrop` over the whole picture (the letterbox bars around art of another aspect ratio),
//      only when `base` is empty: a full-screen replacement;
//   2. `base`: DOS pixels under the art (where no replacement covers the frame, e.g. the race's 3D
//      view above a replaced dashboard), kTransparent elsewhere. Transparent pixels show whatever
//      the presenter drew before (the Enhanced 3D view when the frame was SmoothRenderer's `over`);
//   3. `layers`: replacement images, each a source rectangle of an Image drawn into a destination
//      rectangle, smoothly scaled at the output's resolution;
//   4. `over`: the DOS pixels that stay on top (text, highlights, sprites, gauges: whatever the
//      game drew over its picture), kTransparent where the art shows through;
//   5. `pieces`: DOS rectangles moved to where the replacement's layout has them, taken from `moved`.
//
// base, over and moved are palette indices (frame_w * frame_h, row-major) through `palette`.

#include <array>
#include <cstdint>
#include <vector>

#include "graphics/image.h"

namespace vette::graphics {

constexpr std::uint8_t kTransparent = 0xFF;  // same value as Presenter::kTransparentPixel

struct IRect {
    int x = 0, y = 0, w = 0, h = 0;
};
struct FRect {
    float x = 0, y = 0, w = 0, h = 0;
};

struct Composite {
    int frame_w = 0, frame_h = 0;
    std::array<std::uint32_t, 16> palette{};  // 0xRRGGBB for base, over and moved
    std::uint32_t backdrop = 0xFF000000;       // 0xAARRGGBB, used when base is empty
    std::vector<std::uint8_t> base;            // empty: nothing under the art
    struct Layer {
        const Image* image = nullptr;  // owned by the Substitution, valid while it lives (cache by pointer)
        IRect src;                     // in the image's pixels
        FRect dst;                     // in frame pixels
    };
    std::vector<Layer> layers;
    std::vector<std::uint8_t> over;   // empty: nothing on top
    std::vector<std::uint8_t> moved;  // the pixels the pieces draw (same layout as over)
    struct Piece {
        IRect src;  // in frame pixels (of `moved`)
        FRect dst;  // in frame pixels
    };
    std::vector<Piece> pieces;
};

// Reference renderer (tools, tests, and a CPU fallback): the composite's 4:3 picture into an
// out_w x out_h image. Art is scaled bilinearly; DOS pixels are scaled "sharp" (nearest-neighbour).
Image render(const Composite& c, int out_w, int out_h);

// The plain DOS frame at out_w x out_h, nearest-neighbour, for side-by-side comparisons.
Image render_frame(const std::uint8_t* pixels, int frame_w, int frame_h, const std::array<std::uint32_t, 16>& palette,
                   int out_w, int out_h);

}  // namespace vette::graphics
