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

namespace vette {

// Every file of the DOS release (see Game/README.md).
inline constexpr std::array<std::string_view, 25> kRequiredGameFiles = {
    "VETTE.EXE",    "BIGVET.BIN",   "CONFIG.BIN",   "CRASH0.BIN",   "CRASH1.BIN",
    "EGAPIC.BIN",   "EGASKILL.BIN", "GARAGE.BIN",   "HIGHSC.BIN",   "HORIZON0.BIN",
    "HORIZON1.BIN", "HORIZON2.BIN", "LOSER0.BIN",   "LOSER1.BIN",   "LOSER2.BIN",
    "LOSER3.BIN",   "MAPPIC.BIN",   "PENALTY.BIN",  "REDVETTE.BIN", "SCORE.BIN",
    "SPETRUM.BIN",  "TICKET.BIN",   "TITLE.BIN",    "VX.BIN",       "WINNER.BIN",
};

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
    std::optional<GameDir> dir;                   // first folder with every required file
    std::vector<std::filesystem::path> searched;  // folders tried, in order
    std::vector<std::string_view> missing;        // from the most complete folder, if none matched
};

// With `override_dir` (--game) only that folder is tried. Otherwise: Game/ next to the executable
// and in up to 4 of its parents (so build trees find the repo's Game/), then Game/ in the CWD.
GameDirSearch find_game_dir(const std::optional<std::filesystem::path>& override_dir);

// Checks the MZ signature (throws if absent) and logs size and CRC32. Returns whether this is the
// known v1.1 build; anything else only gets a warning.
bool identify_vette_exe(std::span<const std::uint8_t> exe);

}  // namespace vette
