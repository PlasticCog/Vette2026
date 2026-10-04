#include "core/crc32.h"

#include <array>

namespace vette {
namespace {

constexpr auto kTable = [] {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < table.size(); ++i) {
        std::uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit)
            c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        table[i] = c;
    }
    return table;
}();

constexpr std::uint32_t update(std::span<const std::uint8_t> data, std::uint32_t crc) {
    crc = ~crc;
    for (const std::uint8_t byte : data)
        crc = kTable[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// The standard CRC-32 check value.
static_assert(update(std::array<std::uint8_t, 9>{'1', '2', '3', '4', '5', '6', '7', '8', '9'}, 0) ==
              0xCBF43926u);

}  // namespace

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc) {
    return update(data, crc);
}

}  // namespace vette
