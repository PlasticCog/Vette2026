#include "assets/rle.h"

#include <cstddef>
#include <stdexcept>

namespace vette {

std::vector<std::uint8_t> rle_decode(std::span<const std::uint8_t> in) {
    std::vector<std::uint8_t> out;
    out.reserve(in.size() * 2);
    for (std::size_t i = 0; i < in.size(); ++i) {
        const std::uint8_t b = in[i];
        if (b < 0xC0) {
            out.push_back(b);
            continue;
        }
        if (++i == in.size())
            throw std::runtime_error("RLE data ends inside a run");
        out.insert(out.end(), static_cast<std::size_t>(b & 0x3F), in[i]);
    }
    return out;
}

}  // namespace vette
