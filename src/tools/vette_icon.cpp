// Writes the program icon (src/ui/app_icon.cpp) for the build: a Windows .ico, compiled into
// vette2026.exe's resources (every size a 32-bit image with an all-opaque mask), or a macOS .icns for
// the app bundle (PNG images). The extension picks the format.
// Usage: vette_icon <out.ico | out.icns>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string_view>
#include <vector>

#include "assets/png.h"
#include "ui/app_icon.h"

namespace {

constexpr int kSizes[] = {16, 32, 48, 64, 128, 256};

void put16(std::vector<std::uint8_t>& out, unsigned v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}

// One icon image: BITMAPINFOHEADER (height doubled for the mask), BGRA rows bottom-up, then the
// 1-bit AND mask, rows padded to 4 bytes (all zero: every pixel opaque).
std::vector<std::uint8_t> icon_image(int size) {
    const std::vector<std::uint32_t> argb = vette::ui::app_icon(size);
    const auto mask_row = static_cast<std::uint32_t>((size + 31) / 32 * 4);
    const auto pixels_bytes = static_cast<std::uint32_t>(size * size * 4);
    std::vector<std::uint8_t> out;
    put32(out, 40);
    put32(out, static_cast<std::uint32_t>(size));
    put32(out, static_cast<std::uint32_t>(size * 2));
    put16(out, 1);   // planes
    put16(out, 32);  // bits per pixel
    put32(out, 0);   // BI_RGB
    put32(out, pixels_bytes + mask_row * static_cast<std::uint32_t>(size));
    for (int i = 0; i < 4; ++i)
        put32(out, 0);  // resolution, palette
    for (int y = size - 1; y >= 0; --y) {
        for (int x = 0; x < size; ++x)
            put32(out, argb[static_cast<size_t>(y) * size + x]);  // little-endian ARGB is BGRA
    }
    out.resize(out.size() + mask_row * static_cast<std::uint32_t>(size), 0);
    return out;
}

std::vector<std::uint8_t> ico() {
    std::vector<std::vector<std::uint8_t>> images;
    for (const int size : kSizes)
        images.push_back(icon_image(size));

    // ICONDIR, one ICONDIRENTRY per image, then the images.
    std::vector<std::uint8_t> ico;
    put16(ico, 0);
    put16(ico, 1);  // icon
    put16(ico, static_cast<unsigned>(images.size()));
    auto offset = static_cast<std::uint32_t>(6 + 16 * images.size());
    for (size_t i = 0; i < images.size(); ++i) {
        const auto dim = static_cast<std::uint8_t>(kSizes[i] & 0xFF);  // 256 is written as 0
        ico.push_back(dim);
        ico.push_back(dim);
        ico.push_back(0);  // palette colours
        ico.push_back(0);
        put16(ico, 1);   // planes
        put16(ico, 32);  // bits per pixel
        put32(ico, static_cast<std::uint32_t>(images[i].size()));
        put32(ico, offset);
        offset += static_cast<std::uint32_t>(images[i].size());
    }
    for (const auto& image : images)
        ico.insert(ico.end(), image.begin(), image.end());
    return ico;
}

// The .icns container: big-endian 'icns' + length, then (type, length, PNG) per image. The "@2x"
// types are the next size up, for retina displays.
std::vector<std::uint8_t> icns() {
    struct Entry {
        const char* type;
        int size;
    };
    constexpr Entry kEntries[] = {{"icp4", 16},  {"icp5", 32},  {"ic11", 32},  {"icp6", 64},
                                  {"ic12", 64},  {"ic07", 128}, {"ic08", 256}, {"ic13", 256},
                                  {"ic09", 512}, {"ic14", 512}, {"ic10", 1024}};
    const auto be32 = [](std::vector<std::uint8_t>& out, std::uint32_t v) {
        for (int s = 24; s >= 0; s -= 8)
            out.push_back(static_cast<std::uint8_t>(v >> s));
    };
    std::vector<std::uint8_t> body;
    for (const Entry& e : kEntries) {
        const std::vector<std::uint32_t> pixels = vette::ui::app_icon(e.size);
        const std::vector<std::uint8_t> png = vette::assets::encode_png(e.size, e.size, pixels.data(), true);
        body.insert(body.end(), e.type, e.type + 4);
        be32(body, static_cast<std::uint32_t>(png.size() + 8));
        body.insert(body.end(), png.begin(), png.end());
    }
    std::vector<std::uint8_t> out = {'i', 'c', 'n', 's'};
    be32(out, static_cast<std::uint32_t>(body.size() + 8));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: vette_icon <out.ico | out.icns>\n");
        return 2;
    }
    const std::string_view path = argv[1];
    const bool mac = path.size() >= 5 && path.substr(path.size() - 5) == ".icns";
    const std::vector<std::uint8_t> data = mac ? icns() : ico();
    std::ofstream file(argv[1], std::ios::binary);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file) {
        std::fprintf(stderr, "vette_icon: couldn't write %s\n", argv[1]);
        return 1;
    }
    return 0;
}
