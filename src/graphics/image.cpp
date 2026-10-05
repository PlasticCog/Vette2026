#include "graphics/image.h"

#include <algorithm>

#include "assets/pc98_pic.h"
#include "assets/pict.h"

namespace vette::graphics {

Image from_indexed(const std::uint8_t* pixels, int width, int height, const std::array<std::uint32_t, 16>& palette) {
    Image img;
    img.width = width;
    img.height = height;
    img.pixels.resize(static_cast<std::size_t>(width) * height);
    for (std::size_t i = 0; i < img.pixels.size(); ++i) img.pixels[i] = 0xFF000000u | palette[pixels[i] & 15];
    return img;
}

Image from_pc98(const assets::Pc98Pic& pic, const std::array<std::uint32_t, 16>& palette) {
    return from_indexed(pic.pixels.data(), pic.width, pic.height, palette);
}

Image from_pict(const assets::Pict& pict, std::uint32_t background) {
    Image img;
    img.width = pict.width;
    img.height = pict.height;
    img.pixels = pict.pixels;
    for (auto& p : img.pixels)
        if ((p >> 24) == 0) p = background;
    return img;
}

Image crop(const Image& image, int x, int y, int w, int h) {
    Image out;
    const int x0 = std::clamp(x, 0, image.width), y0 = std::clamp(y, 0, image.height);
    const int x1 = std::clamp(x + w, 0, image.width), y1 = std::clamp(y + h, 0, image.height);
    out.width = x1 - x0;
    out.height = y1 - y0;
    out.pixels.reserve(static_cast<std::size_t>(std::max(out.width, 0)) * std::max(out.height, 0));
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) out.pixels.push_back(image.at(xx, yy));
    return out;
}

Image apply_mask(const Image& color, const Image& mask) {
    Image out = color;
    if (mask.width != color.width || mask.height != color.height) return out;
    for (std::size_t i = 0; i < out.pixels.size(); ++i) {
        const std::uint32_t m = mask.pixels[i];
        const bool opaque = (m >> 24) != 0 && (m & 0xFFFFFF) != 0xFFFFFF;
        if (!opaque) out.pixels[i] = 0;
    }
    return out;
}

}  // namespace vette::graphics
