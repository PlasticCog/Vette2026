#include "graphics/dos_art.h"

#include "assets/planar.h"

namespace vette::graphics {

namespace {

// RLE decode that stops once `want` bytes are out (the game's unpacker knows the picture's size;
// what follows in a buffer isn't part of it). Returns false if the input ends first.
bool unpack(std::span<const std::uint8_t> in, std::size_t want, std::vector<std::uint8_t>& out) {
    out.clear();
    out.reserve(want);
    std::size_t i = 0;
    while (out.size() < want) {
        if (i >= in.size()) return false;
        const std::uint8_t b = in[i++];
        if (b < 0xC0) {
            out.push_back(b);
            continue;
        }
        if (i >= in.size()) return false;
        out.insert(out.end(), static_cast<std::size_t>(b & 0x3F), in[i++]);
    }
    out.resize(want);
    return true;
}

}  // namespace

bool decode_dos_picture(std::span<const std::uint8_t> file, int header, int width, int height, DosPicture& out,
                        std::string* error) {
    if (width <= 0 || height <= 0 || width % 8 != 0 || header < 0) {
        if (error) *error = "invalid picture size";
        return false;
    }
    if (file.size() < static_cast<std::size_t>(header)) {
        if (error) *error = "file too short";
        return false;
    }
    std::vector<std::uint8_t> planes;
    if (!unpack(file.subspan(static_cast<std::size_t>(header)), planar_size(width, height), planes)) {
        if (error) *error = "packed data ends early";
        return false;
    }
    out.width = width;
    out.height = height;
    out.pixels = decode_planar(planes, width, height);
    return true;
}

bool decode_dos_sprite(std::span<const std::uint8_t> file, DosPicture& out, std::string* error) {
    if (file.size() < 4) {
        if (error) *error = "file too short";
        return false;
    }
    const int row_bytes = file[0] | file[1] << 8, height = file[2] | file[3] << 8;
    const auto plane = static_cast<std::size_t>(row_bytes) * static_cast<std::size_t>(height);
    if (row_bytes <= 0 || height <= 0 || row_bytes > 128 || height > 480 || file.size() < 4 + plane * 5) {
        if (error) *error = "not a sprite";
        return false;
    }
    const int width = row_bytes * 8;
    out.width = width;
    out.height = height;
    out.pixels = decode_planar(file.subspan(4 + plane, plane * 4), width, height);
    out.opaque.assign(static_cast<std::size_t>(width) * height, 0);
    for (std::size_t i = 0; i < out.opaque.size(); ++i)
        out.opaque[i] = static_cast<std::uint8_t>(((file[4 + i / 8] >> (7 - i % 8)) & 1) == 0);
    return true;
}

bool decode_dos_dash(std::span<const std::uint8_t> program_image, DosPicture& out, std::string* error) {
    if (program_image.size() <= kDashImageOffset) {
        if (error) *error = "program image too short";
        return false;
    }
    return decode_dos_picture(program_image.subspan(kDashImageOffset), 0, kDashWidth, kDashHeight, out, error);
}

}  // namespace vette::graphics
