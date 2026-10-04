#pragma once
// The program's icon: the scene from VETTE!'s title screen (the Golden Gate Bridge under the clouds,
// the Ferry Building at the end of the road, the Transamerica Pyramid), redrawn as original pixel art
// in the EGA palette. Nothing in it comes from the game's files. No SDL, so the build can use it to
// write the Windows .ico (src/tools/vette_icon.cpp).

#include <cstdint>
#include <vector>

namespace vette::ui {

// The icon at size x size pixels, 0xAARRGGBB, top row first. It's drawn twice, at 32x32 and, with
// less detail, at 16x16: sizes that are multiples of 32 scale the larger drawing by whole pixels,
// other sizes the smaller one.
std::vector<std::uint32_t> app_icon(int size);

}  // namespace vette::ui
