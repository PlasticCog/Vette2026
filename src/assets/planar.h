#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vette {

// Byte size of a 4-plane, 4bpp EGA image.
constexpr std::size_t planar_size(int width, int height) {
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) / 2;
}

// Converts a 4bpp EGA image stored plane after plane (all of plane 0, then plane 1, ...; each plane
// width*height/8 bytes, row-major, MSB = leftmost pixel) to one color index per pixel, row-major:
// index = p0 | p1 << 1 | p2 << 2 | p3 << 3. Width must be a multiple of 8; extra input is ignored.
std::vector<std::uint8_t> decode_planar(std::span<const std::uint8_t> planes, int width, int height);

}  // namespace vette
