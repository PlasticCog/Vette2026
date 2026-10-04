#pragma once

#include <cstdint>
#include <span>

namespace vette {

// CRC-32 as used by zip/zlib (reflected polynomial 0xEDB88320). Pass a previous result as `crc`
// to continue a running checksum.
std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc = 0);

}  // namespace vette
