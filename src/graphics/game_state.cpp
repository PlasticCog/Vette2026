#include "graphics/game_state.h"

namespace vette::graphics {

namespace {

int u8(const std::uint8_t* ram, std::uint32_t a) { return ram[a & 0xFFFFF]; }
int u16(const std::uint8_t* ram, std::uint32_t a) { return u8(ram, a) | u8(ram, a + 1) << 8; }
int s16(const std::uint8_t* ram, std::uint32_t a) { return static_cast<std::int16_t>(u16(ram, a)); }

}  // namespace

DashState read_dash_state(const std::uint8_t* ram) {
    DashState s;
    if (!ram) return s;
    s.speed_mph = u16(ram, kGameDs + 0x3AA1);
    s.rpm100 = u16(ram, kGameCs + 0x588F);  // the copy the dashboard shows (3009:6148)
    s.gear = u16(ram, kGameDs + 0x2D45);
    s.max_gear = u8(ram, kGameDs + 0x2D47);
    s.automatic = u8(ram, kGameDs + 0x2D50) != 0;
    s.cruise = u8(ram, kGameDs + 0x2AD7) != 0;
    s.shift_up = u8(ram, kGameDs + 0x2D51) != 0;
    s.steering = s16(ram, kGameDs + 0x2B84);
    s.view = s16(ram, kGameDs + 0x2B87);
    return s;
}

int read_map_course(const std::uint8_t* ram) {
    if (!ram) return 0;
    const int c = u16(ram, kGameDs + 0xFD10);
    return c >= 1 && c <= 4 ? c : 0;
}

DosFont read_dos_font(const std::uint8_t* ram) {
    DosFont f;
    if (!ram) return f;
    int set = 0;
    for (std::size_t c = 0; c < f.glyphs.size(); ++c)
        for (std::size_t r = 0; r < 10; ++r) {
            const auto b = static_cast<std::uint8_t>(u8(ram, kGameDs + 0xF3DE + static_cast<std::uint32_t>(c * 10 + r)));
            f.glyphs[c][r] = b;
            set += b != 0;
        }
    // The space must be empty and the letters not: otherwise this isn't the font (not loaded yet).
    bool space_empty = true;
    for (const auto b : f.glyphs[0]) space_empty = space_empty && b == 0;
    f.valid = space_empty && set > 200;
    return f;
}

int draw_text(Image& image, const DosFont& font, std::string_view text, int x, int y, int scale, std::uint32_t argb) {
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c > 0x20 && c < 0x80) {
            const auto& g = font.glyphs[c - 0x20];
            for (int r = 0; r < 10; ++r)
                for (int b = 0; b < 8; ++b) {
                    if (!((g[static_cast<std::size_t>(r)] >> (7 - b)) & 1)) continue;
                    for (int sy = 0; sy < scale; ++sy)
                        for (int sx = 0; sx < scale; ++sx) {
                            const int px = x + b * scale + sx, py = y + r * scale + sy;
                            if (px >= 0 && py >= 0 && px < image.width && py < image.height)
                                image.pixels[static_cast<std::size_t>(py) * image.width + px] = argb;
                        }
                }
        }
        x += 8 * scale;
    }
    return x;
}

}  // namespace vette::graphics
