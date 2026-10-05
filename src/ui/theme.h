#pragma once
// The look shared by the app's own screens (launch menu, sound editor): dark navy, a gold title and
// selection, values in an aligned column. Colours are 0xRRGGBB.

#include <cstdint>

namespace vette::ui::theme {

constexpr uint32_t kBackground = 0x0E1324, kSelection = 0x28345A, kRule = 0x2E3A5C;
constexpr uint32_t kGold = 0xFFC23C, kSubtitle = 0xB9BECB, kLabel = 0xC5CAD6, kValue = 0xEEF0F4;
constexpr uint32_t kDim = 0x5D6475, kHelp = 0xD6DAE4, kHint = 0x7C8396, kGood = 0x62D96B, kBad = 0xFF6464;

}  // namespace vette::ui::theme
