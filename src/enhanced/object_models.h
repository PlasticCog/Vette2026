#pragma once
// The city's code-drawn objects as models (game/model_pack.h: a set's `objects`): the ordinary
// buildings, street lamps and signs the original draws with routines of its own rather than as
// segment-245A models. Their geometry comes from the extracted world (enhanced/world.h): a routine's most
// detailed variant (window detail on) made into a model the object editor can reshape, which the game
// then draws in the routine's place, and the Enhanced view with it.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "enhanced/world.h"
#include "game/model_pack.h"

namespace vette::host {
class Machine;
}

namespace vette::enhanced {

// A code-drawn object that can be edited as a model: a routine in the cells' sortable lists (list 2),
// not a compound structure, not turned to the camera or animated, and not drawn with a model already
// (those are in the editor as models), with at most the model's 124 vertices.
struct CityObject {
    uint16_t routine = 0;
    int cells = 0;         // the map's cells it stands in
    std::string name;      // "Building", "Tall building", "Street object", by its size
};
std::vector<CityObject> city_objects(const World& world);  // most used first

// The model of a routine's most detailed variant (window detail on): its parts' vertices in the model's
// axes (x east, y down, z north; the reference frame first), its polygons and lines as faces with their
// colours, a culled polygon wound to show its front (and culled the model's way), each face drawn after
// any it's painted on (make_orders). nullopt (with `why`) if it can't be one.
std::optional<game::ModelData> routine_model(const World& world, uint16_t routine, std::string& why);

// Draws `objects` ({routine, model number}, game::object_models) as their models in `world`: each model
// decoded from the machine's memory (written there by game::install_model_pack), each routine's variants
// replaced by that model at the object's place, unrotated. False (with `error`) if a model isn't there.
bool use_object_models(World& world, host::Machine& machine, const std::vector<std::pair<uint16_t, int>>& objects,
                       std::string& error);

}  // namespace vette::enhanced
