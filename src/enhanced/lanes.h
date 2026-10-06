#pragma once
// The road's lane markings from the extracted city, for Lane Centering (game/driving.h): the white
// dashed lane lines (colour 0Fh) and yellow centre lines (0Eh) the ground layer draws (list1), in
// absolute world coordinates (re/notes/11-driving.md).

#include <cstddef>
#include <memory>
#include <vector>

#include "enhanced/world.h"
#include "game/driving.h"

namespace vette::enhanced {

std::vector<game::LaneLine> lane_lines(const World& world);
std::shared_ptr<const game::LaneMap> lane_map(const World& world);

} // namespace vette::enhanced
