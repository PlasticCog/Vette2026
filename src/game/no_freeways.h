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

#include <cstdint>
#include <utility>
#include <vector>

#include "game/city_map.h"

namespace vette::host {
class Machine;
}

namespace vette::game {

// The roads, into `map` (on top of whatever it is: the original or one of the player's).
void add_no_freeway_roads(CityMap& map);

// Plays without the freeways: the roads into the map, the opponent's routes, as the game starts (with
// install_city_map's map, if any, which goes in first), and the on-ramps made inert.
void install_no_freeways(host::Machine& machine);

// For checks (vette_world --drivable --no-freeways): the opponent's new roads, absolute points per leg.
std::vector<std::vector<std::pair<int32_t, int32_t>>> no_freeway_opponent_roads();

}  // namespace vette::game
