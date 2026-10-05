#pragma once
// The versions of VETTE! in the player's game folder (Game/). Each can be in a folder of its own,
// named anything (Game/DOS, Game/PC-98, Game/Vette Mac 1.02, ...) a few levels down, and the DOS files
// can also sit in the game folder itself. The scan tells them apart by what's inside: the DOS game's
// files; the PC-98 version's files or disk image; the Mac version's disk image or files (whatever
// holds VETTE!.Data).

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "assets/mac_files.h"
#include "assets/pc98_disk.h"

namespace vette {

// Every file of the DOS release (see Game/README.md).
inline constexpr std::array<std::string_view, 25> kRequiredGameFiles = {
    "VETTE.EXE",    "BIGVET.BIN",   "CONFIG.BIN",   "CRASH0.BIN",   "CRASH1.BIN",
    "EGAPIC.BIN",   "EGASKILL.BIN", "GARAGE.BIN",   "HIGHSC.BIN",   "HORIZON0.BIN",
    "HORIZON1.BIN", "HORIZON2.BIN", "LOSER0.BIN",   "LOSER1.BIN",   "LOSER2.BIN",
    "LOSER3.BIN",   "MAPPIC.BIN",   "PENALTY.BIN",  "REDVETTE.BIN", "SCORE.BIN",
    "SPETRUM.BIN",  "TICKET.BIN",   "TITLE.BIN",    "VX.BIN",       "WINNER.BIN",
};

struct GameVersions {
    std::filesystem::path root;  // the folder scanned

    std::optional<std::filesystem::path> dos;   // the folder with every DOS file
    std::vector<std::string_view> dos_missing;  // without one: what the most complete folder lacked

    // Where the other versions are (a folder, or the PC-98's disk image file), and briefly what was
    // found there ("files", "VETTE.hdi: HDI hard disk", "HFS image VETTE_1_02.toast"). open_pc98() and
    // open_mac() open them.
    std::optional<std::filesystem::path> pc98;
    std::string pc98_what;
    std::optional<std::filesystem::path> mac;
    std::string mac_what;
};

// Scans `root` and its subfolders, three levels down. Never throws.
GameVersions scan_game_folder(const std::filesystem::path& root);

// The other versions' files where the scan found them (nullopt / empty if it didn't).
std::optional<assets::Pc98Files> open_pc98(const GameVersions& versions, std::string& error);
assets::MacFiles open_mac(const GameVersions& versions);

}  // namespace vette
