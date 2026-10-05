#pragma once
// The PC-98 release (VETTE! 1.02J) from the player's own copy in Game/PC98/: its files as they are, or
// a disk image holding them. Hard disk images: HDI (Anex86), NHD (T98-Next), THD (T98) or raw sectors;
// floppy images: FDI (Anex86), D88, NFD r0 (T98-Next) or raw. The file system is MS-DOS FAT12/FAT16;
// on a hard disk its partitions are found through the PC-98 partition table and by looking for boot
// sectors. The game is the directory holding VETTE.EXE (\VETTE on the shipped image).
// Images are untrusted input: every offset is bounds-checked, cluster chains and directory trees are
// walked with loop and size limits, and a malformed image is rejected with a message. No SDL.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vette::assets {

class Pc98Files {
public:
    // A file or directory in a disk image, for listings.
    struct ImageEntry {
        std::string path;  // from the volume's root, '\'-separated, e.g. "VETTE\TITLE.PIC"
        uint32_t size = 0;
        bool directory = false;
        int volume = 0;  // which FAT volume (partition) of the image, from 0
    };

    // Finds the game in `folder` (Game/PC98/): VETTE.EXE there means plain files; otherwise each disk
    // image there (by extension) is tried until one holds VETTE.EXE. Failing both, the same is tried
    // in each subfolder (a release unpacked into a folder of its own).
    static std::optional<Pc98Files> open(const std::filesystem::path& folder, std::string& error);
    // One disk image file. Its format comes from its contents; the extension only settles ambiguity.
    static std::optional<Pc98Files> open_image(const std::filesystem::path& image, std::string& error);
    // A disk image in memory. `name` is a file name, for messages and the extension hint.
    static std::optional<Pc98Files> open_image(std::span<const uint8_t> image, std::string_view name,
                                               std::string& error);

    // What was opened, e.g. "VETTE.hdi: HDI hard disk, FAT12 volume 0, \VETTE".
    const std::string& description() const { return description_; }
    // The game folder's files (the directory of VETTE.EXE), names as stored, sorted.
    const std::vector<std::string>& names() const { return names_; }
    bool contains(std::string_view name) const;  // case-insensitive, like DOS
    // Reads a game file (case-insensitive name). False if it's missing or unreadable.
    bool read(std::string_view name, std::vector<uint8_t>& out) const;
    std::optional<std::vector<uint8_t>> read(std::string_view name) const;
    // Every file and directory in the image, all volumes (empty when the game came as plain files).
    const std::vector<ImageEntry>& image_listing() const { return listing_; }

private:
    friend struct Pc98FilesBuilder;  // pc98_disk.cpp: reads images into this

    std::string description_;
    std::vector<std::string> names_;
    std::vector<std::vector<uint8_t>> data_;   // per name, from an image
    std::vector<std::filesystem::path> paths_;  // per name, plain files (read on demand)
    std::vector<ImageEntry> listing_;

    int index(std::string_view name) const;
};

}  // namespace vette::assets
