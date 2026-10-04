#include "assets/planar.h"

#include <stdexcept>
#include <string>

namespace vette {

std::vector<std::uint8_t> decode_planar(std::span<const std::uint8_t> planes, int width, int height) {
    if (width <= 0 || height <= 0 || width % 8 != 0)
        throw std::invalid_argument("planar image size must be positive with width a multiple of 8");
    if (planes.size() < planar_size(width, height))
        throw std::runtime_error("planar image " + std::to_string(width) + "x" + std::to_string(height) +
                                 " needs " + std::to_string(planar_size(width, height)) + " bytes, got " +
                                 std::to_string(planes.size()));

    const std::size_t plane_bytes = planar_size(width, height) / 4;
    const auto p0 = planes.subspan(0 * plane_bytes, plane_bytes);
    const auto p1 = planes.subspan(1 * plane_bytes, plane_bytes);
    const auto p2 = planes.subspan(2 * plane_bytes, plane_bytes);
    const auto p3 = planes.subspan(3 * plane_bytes, plane_bytes);

    std::vector<std::uint8_t> out(plane_bytes * 8);
    for (std::size_t i = 0; i < plane_bytes; ++i) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            const unsigned shift = 7 - bit;
            const int b0 = (p0[i] >> shift) & 1;
            const int b1 = (p1[i] >> shift) & 1;
            const int b2 = (p2[i] >> shift) & 1;
            const int b3 = (p3[i] >> shift) & 1;
            out[i * 8 + bit] = static_cast<std::uint8_t>(b0 | b1 << 1 | b2 << 2 | b3 << 3);
        }
    }
    return out;
}

}  // namespace vette
