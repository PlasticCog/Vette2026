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

bool decode_dos_dash(std::span<const std::uint8_t> program_image, DosPicture& out, std::string* error) {
    if (program_image.size() <= kDashImageOffset) {
        if (error) *error = "program image too short";
        return false;
    }
    return decode_dos_picture(program_image.subspan(kDashImageOffset), 0, kDashWidth, kDashHeight, out, error);
}

}  // namespace vette::graphics
