#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/versions.h"

namespace vette {

// The supported VETTE.EXE: DOS v1.1, English (EXEPACK-packed as shipped).
inline constexpr std::size_t kVetteExeSize = 248173;
inline constexpr std::uint32_t kVetteExeCrc32 = 0xC013A5B5;

// A folder holding the user's game files. Names match case-insensitively: the DOS names are
// uppercase, but copies on case-sensitive file systems often aren't.
class GameDir {
public:
    explicit GameDir(std::filesystem::path root);

    const std::filesystem::path& root() const { return root_; }
    std::optional<std::filesystem::path> find(std::string_view name) const;
    // Throws std::runtime_error if the file is missing or unreadable.
    std::vector<std::uint8_t> read(std::string_view name) const;

private:
    std::filesystem::path root_;
    std::vector<std::string> names_;  // regular files in root_, UTF-8
};

struct GameDirSearch {
    std::optional<GameDir> dir;                   // the DOS game's files
    GameVersions versions;                        // every version found in the game folder
    std::vector<std::filesystem::path> searched;  // game folders tried, in order
    std::vector<std::string_view> missing;        // DOS files missing from the most complete folder
};

// Finds the game folder and scans it for the versions of VETTE! (core/versions.h): the DOS files in
// it or in a folder of their own below it, the PC-98 and Mac versions in theirs. With `override_dir`
// (--game, the launch menu's choice) only that folder is tried. Otherwise: Game/ next to the
// executable and in up to 4 of its parents (so build trees find the repo's Game/), then Game/ in the
// CWD.
GameDirSearch find_game_dir(const std::optional<std::filesystem::path>& override_dir);

// A subfolder of `dir` by name, ignoring case (Game/Mac, Game/PC98 for the other versions' files).
std::optional<std::filesystem::path> find_subfolder(const std::filesystem::path& dir, std::string_view name);

// Checks the MZ signature (throws if absent) and logs size and CRC32. Returns whether this is the
// known v1.1 build; anything else only gets a warning.
bool identify_vette_exe(std::span<const std::uint8_t> exe);

}  // namespace vette
