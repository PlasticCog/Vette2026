#include "graphics/composite.h"

#include <algorithm>
#include <cmath>

namespace vette::graphics {

namespace {

// Source-over blend of a straight-alpha 0xAARRGGBB colour onto an opaque pixel.
std::uint32_t blend(std::uint32_t dst, std::uint32_t src) {
    const std::uint32_t a = src >> 24;
    if (a == 255) return src;
    if (a == 0) return dst;
    const auto mix = [a](std::uint32_t d, std::uint32_t s) { return (s * a + d * (255 - a) + 127) / 255; };
    const std::uint32_t r = mix((dst >> 16) & 255, (src >> 16) & 255);
    const std::uint32_t g = mix((dst >> 8) & 255, (src >> 8) & 255);
    const std::uint32_t b = mix(dst & 255, src & 255);
    return 0xFF000000u | r << 16 | g << 8 | b;
}

// Bilinear sample with premultiplied alpha, clamped to the rectangle `r` of the image.
std::uint32_t sample(const Image& img, const IRect& r, float u, float v) {
    u -= 0.5f;
    v -= 0.5f;
    const int x0 = static_cast<int>(std::floor(u)), y0 = static_cast<int>(std::floor(v));
    const float fx = u - static_cast<float>(x0), fy = v - static_cast<float>(y0);
    const auto px = [&](int x, int y) {
        x = std::clamp(x, r.x, r.x + r.w - 1);
        y = std::clamp(y, r.y, r.y + r.h - 1);
        return img.at(x, y);
    };
    float acc[4] = {0, 0, 0, 0};
    const std::uint32_t c[4] = {px(x0, y0), px(x0 + 1, y0), px(x0, y0 + 1), px(x0 + 1, y0 + 1)};
    const float w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    for (int i = 0; i < 4; ++i) {
        const float a = static_cast<float>(c[i] >> 24) / 255.0f * w[i];
        acc[0] += a;
        acc[1] += static_cast<float>((c[i] >> 16) & 255) * a;
        acc[2] += static_cast<float>((c[i] >> 8) & 255) * a;
        acc[3] += static_cast<float>(c[i] & 255) * a;
    }
    if (acc[0] <= 0.0f) return 0;
    const auto ch = [&](float v2) { return static_cast<std::uint32_t>(std::clamp(v2 / acc[0] + 0.5f, 0.0f, 255.0f)); };
    const auto alpha = static_cast<std::uint32_t>(std::clamp(acc[0] * 255.0f + 0.5f, 0.0f, 255.0f));
    return alpha << 24 | ch(acc[1]) << 16 | ch(acc[2]) << 8 | ch(acc[3]);
}

}  // namespace

Image render(const Composite& c, int out_w, int out_h) {
    Image out;
    out.width = out_w;
    out.height = out_h;
    out.pixels.assign(static_cast<std::size_t>(out_w) * out_h, c.base.empty() ? 0xFF000000u | c.backdrop : 0xFF000000u);
    if (c.frame_w <= 0 || c.frame_h <= 0 || out_w <= 0 || out_h <= 0) return out;
    const float sx = static_cast<float>(out_w) / static_cast<float>(c.frame_w);
    const float sy = static_cast<float>(out_h) / static_cast<float>(c.frame_h);
    const auto frame_size = static_cast<std::size_t>(c.frame_w) * c.frame_h;

    const auto indexed = [&](const std::vector<std::uint8_t>& px) {
        if (px.size() != frame_size) return;
        for (int y = 0; y < out_h; ++y) {
            const int fy = std::min(static_cast<int>(static_cast<float>(y) / sy), c.frame_h - 1);
            for (int x = 0; x < out_w; ++x) {
                const int fx = std::min(static_cast<int>(static_cast<float>(x) / sx), c.frame_w - 1);
                const std::uint8_t v = px[static_cast<std::size_t>(fy) * c.frame_w + fx];
                if (v != kTransparent) out.pixels[static_cast<std::size_t>(y) * out_w + x] = 0xFF000000u | c.palette[v & 15];
            }
        }
    };
    indexed(c.base);

    for (const auto& layer : c.layers) {
        if (!layer.image || layer.image->empty() || layer.src.w <= 0 || layer.src.h <= 0 || layer.dst.w <= 0 ||
            layer.dst.h <= 0)
            continue;
        const float x0 = layer.dst.x * sx, y0 = layer.dst.y * sy;
        const float x1 = (layer.dst.x + layer.dst.w) * sx, y1 = (layer.dst.y + layer.dst.h) * sy;
        const float ku = static_cast<float>(layer.src.w) / (x1 - x0), kv = static_cast<float>(layer.src.h) / (y1 - y0);
        for (int y = std::max(0, static_cast<int>(y0)); y < std::min(out_h, static_cast<int>(std::ceil(y1))); ++y) {
            const float cyp = static_cast<float>(y) + 0.5f;
            if (cyp < y0 || cyp >= y1) continue;
            const float v = static_cast<float>(layer.src.y) + (cyp - y0) * kv;
            for (int x = std::max(0, static_cast<int>(x0)); x < std::min(out_w, static_cast<int>(std::ceil(x1))); ++x) {
                const float cxp = static_cast<float>(x) + 0.5f;
                if (cxp < x0 || cxp >= x1) continue;
                const float u = static_cast<float>(layer.src.x) + (cxp - x0) * ku;
                auto& d = out.pixels[static_cast<std::size_t>(y) * out_w + x];
                d = blend(d, sample(*layer.image, layer.src, u, v));
            }
        }
    }

    indexed(c.over);

    if (c.moved.size() == frame_size) {
        for (const auto& piece : c.pieces) {
            if (piece.src.w <= 0 || piece.src.h <= 0 || piece.dst.w <= 0 || piece.dst.h <= 0) continue;
            const float x0 = piece.dst.x * sx, y0 = piece.dst.y * sy;
            const float x1 = (piece.dst.x + piece.dst.w) * sx, y1 = (piece.dst.y + piece.dst.h) * sy;
            for (int y = std::max(0, static_cast<int>(y0)); y < std::min(out_h, static_cast<int>(std::ceil(y1))); ++y) {
                const float cyp = static_cast<float>(y) + 0.5f;
                if (cyp < y0 || cyp >= y1) continue;
                const int fy = piece.src.y + std::min(static_cast<int>((cyp - y0) / (y1 - y0) * static_cast<float>(piece.src.h)), piece.src.h - 1);
                for (int x = std::max(0, static_cast<int>(x0)); x < std::min(out_w, static_cast<int>(std::ceil(x1))); ++x) {
                    const float cxp = static_cast<float>(x) + 0.5f;
                    if (cxp < x0 || cxp >= x1) continue;
                    const int fx = piece.src.x + std::min(static_cast<int>((cxp - x0) / (x1 - x0) * static_cast<float>(piece.src.w)), piece.src.w - 1);
                    if (fx < 0 || fy < 0 || fx >= c.frame_w || fy >= c.frame_h) continue;
                    const std::uint8_t v = c.moved[static_cast<std::size_t>(fy) * c.frame_w + fx];
                    if (v != kTransparent) out.pixels[static_cast<std::size_t>(y) * out_w + x] = 0xFF000000u | c.palette[v & 15];
                }
            }
        }
    }
    return out;
}

Image render_frame(const std::uint8_t* pixels, int frame_w, int frame_h, const std::array<std::uint32_t, 16>& palette,
                   int out_w, int out_h) {
    Composite c;
    c.frame_w = frame_w;
    c.frame_h = frame_h;
    c.palette = palette;
    c.base.assign(pixels, pixels + static_cast<std::size_t>(frame_w) * frame_h);
    return render(c, out_w, out_h);
}

}  // namespace vette::graphics
