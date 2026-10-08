#pragma once
// No freeways (Settings::freeways off): the city as one, driven without the freeways. The original's
// city is in parts that only its freeways join (game/drivable.h finds them): the Zoo and the Sunset, the
// central city, the Marina, the Bay Bridge's end of town, the Golden Gate. Roads made of the original's
// own cells join them: a causeway up the coast from the Great Highway to the Golden Gate (the Presidio
// freeway's way, as a road), the Golden Gate's deck carried on to the Marina, Golden Gate Park and the
// Marina opened into the central city, and the diagonal barrier before the Bay Bridge's end of town made
// a street. The on-ramps lead nowhere (their collision boxes do nothing), and the computer opponent
// drives streets where its route took a freeway: its four freeway legs become waypoints of its own,
// laid along those roads.
//
// The traffic goes along. The original's is a pattern: each big tile's list of cars (and of pedestrians)
// repeats every 4 cells, drawn wherever the view's few cells match, and a byte per cell (DS:D2AE's maps)
// says which way its city cars may go there, or hides them (water, parks). The Golden Gate's cars (the
// bridge's list, DS:F0B0) drive straight lanes up and down the approach. With the freeways off they carry
// on down the whole coast road: the deck to the Marina, the causeway and the Great Highway to the Zoo, in
// the city's list as well as the bridge's, and kept moving wherever the car is (the original moves only
// the camera's big tile's traffic). The city's own cars stay off the causeway (they turn back at the
// Great Highway's end, as at any dead end), the new diagonal street gets the old one's rules, and
// pedestrians keep off the water, the deck and its walls.

#include <cstdint>
#include <utility>
#include <vector>

#include "game/city_map.h"

namespace vette::host {
class Cpu;
class Machine;
class Memory;
}

namespace vette::game {

// The roads, into `map` (on top of whatever it is: the original or one of the player's). Some are of cell
// types of their own, made of the original's pieces: add_no_freeway_cell_types puts those in memory.
void add_no_freeway_roads(CityMap& map);
void add_no_freeway_cell_types(host::Memory& memory);

// Plays without the freeways: the roads into the map, the opponent's routes, as the game starts (with
// install_city_map's map, if any, which goes in first), and the on-ramps made inert.
void install_no_freeways(host::Machine& machine);

// For checks (vette_world --drivable --no-freeways): the opponent's new roads, absolute points per leg, as
// it drives them.
std::vector<std::vector<std::pair<int32_t, int32_t>>> no_freeway_opponent_roads();

// Where a traffic car or pedestrian (`entity`: its record in DS) may be drawn, in map cell gx, gy of type
// `type`: Allow (it belongs there, whatever the Enhanced view's layout rule makes of the cell), Deny, or
// Default (the original's rules decide, and the Enhanced view's).
enum class Placement { Default, Allow, Deny };
Placement no_freeway_placement(uint16_t entity, int gx, int gy, uint8_t type);

// The drawing's part of it: the view's car and pedestrian gathers keep to no_freeway_placement. On the
// game's CPU (install_no_freeways does it) and on any that replays its drawing (game::SmoothRenderer's).
void install_no_freeway_drawing(host::Cpu& cpu);

}  // namespace vette::game
