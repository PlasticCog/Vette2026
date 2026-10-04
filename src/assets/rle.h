#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace vette {

// PCX-style RLE used by the DOS .BIN pictures: a byte b >= 0xC0 is a run of (b & 0x3F) copies of
// the next byte; any other byte is a literal. Throws std::runtime_error on a truncated run.
std::vector<std::uint8_t> rle_decode(std::span<const std::uint8_t> in);

}  // namespace vette
