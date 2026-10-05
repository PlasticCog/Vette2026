#pragma once
// RGBA images for the replacement art, and the conversions from the decoders' outputs.

#include <array>
#include <cstdint>
#include <vector>

namespace vette::assets {
struct Pict;
struct Pc98Pic;
}  // namespace vette::assets

namespace vette::graphics {

struct Image {
    int width = 0, height = 0;
    std::vector<std::uint32_t> pixels;  // 0xAARRGGBB, top row first; alpha 0 = transparent

    bool empty() const { return width <= 0 || height <= 0; }
    std::uint32_t at(int x, int y) const { return pixels[static_cast<std::size_t>(y) * width + x]; }
};

// Indexed pixels through a 0xRRGGBB palette, opaque.
Image from_indexed(const std::uint8_t* pixels, int width, int height, const std::array<std::uint32_t, 16>& palette);
Image from_pc98(const assets::Pc98Pic& pic, const std::array<std::uint32_t, 16>& palette);
// A decoded PICT. Undrawn pixels (alpha 0) become `background` (QuickDraw erases windows to white);
// pass 0 to keep them transparent.
Image from_pict(const assets::Pict& pict, std::uint32_t background = 0xFFFFFFFF);
// A sub-rectangle (clipped to the image).
Image crop(const Image& image, int x, int y, int w, int h);
// The Mac sprites come as a colour picture and a 1-bit mask picture of the same size: black in the
// mask is opaque. Returns `color` with the mask's white pixels made transparent.
Image apply_mask(const Image& color, const Image& mask);

}  // namespace vette::graphics
