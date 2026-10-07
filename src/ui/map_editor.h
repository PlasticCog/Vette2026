#pragma once
// The map editor (launch menu > Map, Enter): the city from above, north up, to change cell by cell and
// save as a map of one's own (game/city_map.h), which the launch menu's Map setting then plays. Built
// from the original's own cell types (the 161 the map uses); big tiles that share a design change
// together. Maps are files in the save folder's maps folder; the game's own files are never changed.
// Mouse and keyboard.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/game_dir.h"
#include "game/city_map.h"

namespace vette {
class Gamepad;
class Presenter;
}  // namespace vette

namespace vette::ui {

// The maps saved in `maps_dir`, by name (the file's name without ".vmap"), sorted.
std::vector<std::string> list_maps(const std::filesystem::path& maps_dir);
// Map `name` from `maps_dir`; nullopt with the reason in `error`.
std::optional<game::CityMap> load_map(const std::filesystem::path& maps_dir, const std::string& name, std::string& error);

// Edits map `name` (empty: a copy of the original). Returns the map to play afterwards: the one last
// saved, else `name` as it was.
std::string run_map_editor(Presenter& presenter, Gamepad& gamepad, const GameDir& game,
                           const std::filesystem::path& maps_dir, const std::string& name);

}  // namespace vette::ui
