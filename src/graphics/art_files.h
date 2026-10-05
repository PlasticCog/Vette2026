#pragma once
// Where the replacement art comes from: the player's PC-98 files and the Mac Color VETTE!
// application's resources, plus the DOS pictures the screens are matched against. Everything is read
// at runtime from the player's own copies; nothing is bundled.
//
// ArtFiles is a set of callbacks, so any file access can be plugged in. from_game_dir() is the
// game's: DOS files in the game folder, the PC-98 version in PC98/ (assets/pc98_disk.h: plain files
// or a disk image), the Mac version in Mac/ (assets/mac_files.h: HFS images, MacBinary, AppleDouble,
// raw forks, ...). from_folders() takes plain files and a raw resource fork, for tools and tests.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace vette::graphics {

struct ArtFiles {
    // Each returns the file or resource, or an empty vector when it's absent.
    std::function<std::vector<std::uint8_t>(const std::string& name)> dos_file;   // "TITLE.BIN"
    std::function<std::vector<std::uint8_t>(const std::string& name)> pc98_file;  // "TITLE.PIC"
    std::function<std::vector<std::uint8_t>(std::int16_t id)> mac_pict;           // Color VETTE!'s PICT id

    // The game folder's layout (Game/, Game/PC98/, Game/Mac/). `notes` receives what was found.
    static ArtFiles from_game_dir(const std::filesystem::path& game_dir, std::vector<std::string>* notes = nullptr);
    // Plain files: `dos_dir` holds the .BIN files, `pc98_dir` the PC-98 files, `mac_rsrc` is Color
    // VETTE!'s resource fork as a raw file. Missing parts are fine.
    static ArtFiles from_folders(const std::filesystem::path& dos_dir, const std::filesystem::path& pc98_dir,
                                 const std::filesystem::path& mac_rsrc);
};

std::vector<std::uint8_t> read_file(const std::filesystem::path& path);  // empty if missing
// Finds `name` in `dir` ignoring case (DOS names). Returns an empty path if absent.
std::filesystem::path find_ci(const std::filesystem::path& dir, const std::string& name);

}  // namespace vette::graphics
