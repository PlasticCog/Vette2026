#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace vette {

struct Rgb {
    std::uint8_t r, g, b;
};

// The EGA's power-on palette for 16-color modes.
inline constexpr std::array<Rgb, 16> kEgaPalette = {{
    {0x00, 0x00, 0x00}, {0x00, 0x00, 0xAA}, {0x00, 0xAA, 0x00}, {0x00, 0xAA, 0xAA},
    {0xAA, 0x00, 0x00}, {0xAA, 0x00, 0xAA}, {0xAA, 0x55, 0x00}, {0xAA, 0xAA, 0xAA},
    {0x55, 0x55, 0x55}, {0x55, 0x55, 0xFF}, {0x55, 0xFF, 0x55}, {0x55, 0xFF, 0xFF},
    {0xFF, 0x55, 0x55}, {0xFF, 0x55, 0xFF}, {0xFF, 0xFF, 0x55}, {0xFF, 0xFF, 0xFF},
}};

// The game's screen: one palette index (0-15) per pixel, row-major. VETTE! uses EGA 640x200 (menus,
// mode 0Eh) and 320x200 (the race view, mode 0Dh); both fill a 4:3 display.
struct Framebuffer {
    static constexpr int kWidth = 640;
    static constexpr int kHeight = 200;

    int width = kWidth;
    int height = kHeight;
    std::vector<std::uint8_t> pixels = std::vector<std::uint8_t>(kWidth * kHeight);
    std::array<Rgb, 16> palette = kEgaPalette;
};

// Writes an 8-bit indexed BMP. Throws std::runtime_error on failure.
void save_bmp(const Framebuffer& fb, const std::string& path_utf8);

}  // namespace vette
