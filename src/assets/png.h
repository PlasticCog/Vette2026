#pragma once
// PNG writing without a zlib dependency: LZ77 + the fixed Huffman codes, which suit flat-shaded and
// pixel-art pictures (screenshots, the icon, tool output).

#include <cstdint>
#include <filesystem>
#include <vector>

namespace vette::assets {

// `pixels`: w x h, top row first, 0xAARRGGBB (alpha is written only with `alpha`, else 0xRRGGBB).
std::vector<std::uint8_t> encode_png(int w, int h, const std::uint32_t* pixels, bool alpha = false);
bool write_png(const std::filesystem::path& path, int w, int h, const std::vector<std::uint32_t>& pixels,
               bool alpha = false);

// The deflate stream alone (RFC 1951), as used inside the PNG.
std::vector<std::uint8_t> deflate_fixed(const std::vector<std::uint8_t>& in);

}  // namespace vette::assets
