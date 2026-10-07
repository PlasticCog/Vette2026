#pragma once
// Where a car can drive on the city map, and which places connect (re/notes/05-world-data.md section 3).
// A cell type's collision class lists boxes (DS:C0A6[class] -> {xmin, ymin, xmax, ymax}, cell-local); the
// car stops at a box (or, at water's, the race ends), except at the race's own triggers: the finish
// lines, the freeway on-ramps, the toll booths' speed limit, the bridge decks' camera. Everything else is
// drivable, less the car's half-size around each box. The drivable samples form connected regions.

#include <cstdint>
#include <vector>

#include "game/city_map.h"

namespace vette::host {
class Memory;
}

namespace vette::game {

// Is the box at this DS address one of the race's triggers (not a wall)?
bool collision_box_passable(uint16_t box);
// The freeway route (0..8) whose on-ramp this box is, or -1.
int freeway_ramp_route(uint16_t box);

struct DrivableMap {
    static constexpr int kStep = 32;  // world units between samples
    int n = 0;                        // samples per side
    std::vector<int32_t> region;      // per sample (x / kStep * n + y / kStep): -1 blocked, else its region
    std::vector<int> region_size;     // samples in each region, largest first
    int region_at(int32_t x, int32_t y) const {
        const int i = x / kStep, j = y / kStep;
        return i >= 0 && j >= 0 && i < n && j < n ? region[static_cast<size_t>(i * n + j)] : -1;
    }
};

// `memory`: the unpacked game (for the cell types' collision classes and boxes); `map`: the cells.
// `margin`: the car's half-size (DS:2BC9/2BD9 give 12 to 24 by heading; 12 lets it through anything it
// can pass turned the right way).
DrivableMap find_drivable(host::Memory& memory, const CityMap& map, int margin = 12);

// A collision box in a cell, absolute.
struct PlacedBox {
    int cx = 0, cy = 0;
    uint16_t box = 0;  // its DS address
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
std::vector<PlacedBox> collision_boxes(host::Memory& memory, const CityMap& map);

}  // namespace vette::game
