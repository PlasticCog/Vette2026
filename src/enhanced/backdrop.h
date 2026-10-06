#pragma once
// The Hills skyline (Settings::Skyline): the horizon backdrop of the Enhanced 3D view without its painted city.
//
// The original draws a painted 360-degree panorama on the horizon of every race frame (blit_horizon
// 3009:6734, re/notes/03 "Sky, ground and horizon"): one of three, HORIZON0/1/2.BIN, by big tile. They
// show the city as seen from afar, with its skyline, towers and bridges. Once the Enhanced view draws
// the real city to the horizon those double up with the geometry in front of them, so the Hills skyline
// keeps only what the 3D world doesn't have: the hills and mountains, the trees, the water and the sky.
//
// The landscape-only panoramas are made from the player's own pictures, read from the game's off-screen
// video memory: the parts of each panorama that show the city (column ranges and rectangles found by
// hand, kept below as data) are painted over with what lies behind them, from the panorama's own hills,
// trees and water. That retouching applies to the DOS release's three panoramas, recognised by their
// content; any other picture is left as it is.

#include <cstdint>
#include <vector>

#include "host/ega.h"

namespace vette::enhanced {

// A panorama: 24 rows of 3200 pixels (400 bytes a row in each plane): 360 degrees at 8 pixels a degree,
// the last 320 pixels repeating the first.
inline constexpr int kPanoramaRows = 24;
inline constexpr int kPanoramaWidth = 3200;
inline constexpr int kPanoramaDegrees = 2880;  // columns before the repeat
inline constexpr int kPanoramas = 3;

// FNV-1a of a panorama's pixels (kPanoramaRows x kPanoramaWidth EGA colour indices, row by row).
uint64_t panorama_hash(const uint8_t* pixels);

// The landscape-only version of a panorama (same layout) into `out`. Returns false, with `out` a copy of
// `in`, for a picture the retouching doesn't know.
bool landscape_panorama(const uint8_t* in, uint8_t* out);

class Backdrop {
public:
    // Redraws the horizon rows of a race frame (`pixels`, `width` x `height` EGA colour indices: the Enhanced
    // view's background, SmoothRenderer::Layers::under) from the landscape-only panoramas. The rows are where
    // blit_horizon's copy put them (3009:6773): `rows` rows of 40 bytes from the panorama buffer at
    // A400:`source` (400 bytes a row) to the frame's byte `dest` (40 a row). The panoramas are read from
    // `ega`'s off-screen memory. Pixels drawn over the panorama afterwards stay. Returns false, leaving the frame
    // alone, if those rows don't (mostly) show the panoramas as `ega` holds them: nothing to redraw from.
    bool apply(const host::Ega& ega, int rows, uint32_t source, uint32_t dest, uint8_t* pixels, int width, int height);

    // How many of the panoramas read last are known to the retouching (0 before the first apply()).
    int known() const { return known_; }

private:
    int matching(int rows, uint32_t source, uint32_t dest, const uint8_t* pixels) const;  // pixels as read
    void load(const host::Ega& ega);

    // The panorama buffer's pixels, original and landscape-only: buffer byte b is pixels 8b..8b+7
    // (kPanoramas x kPanoramaRows x kPanoramaWidth).
    std::vector<uint8_t> original_, landscape_;
    int known_ = 0;
    int reload_wait_ = 0;  // frames before the panoramas are read again after a mismatch
};

} // namespace vette::enhanced
