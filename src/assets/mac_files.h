#pragma once
// The Mac VETTE!'s files, from the player's own copy, whatever form it is in: a disk image (.toast,
// .dsk, .img, .hfv, .image: HFS, bare or behind a DiskCopy 4.2 header or an Apple partition map), or
// files copied off one, with their resource forks as raw dumps ("name.rsrc"), MacBinary (.bin),
// AppleDouble ("._name", "%name", ".AppleDouble/name"), AppleSingle, BinHex 4.0 (.hqx) or, on macOS,
// the file's own resource fork. MacBinary/AppleSingle/BinHex files holding a disk image are opened too.
// Everything is read at runtime; nothing from the game is stored in the repo.
//
// Players' files vary, so all of it fails soft: a file that can't be read is noted and skipped, and a
// damaged resource fork or volume never crashes or hangs the reader.

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "assets/hfs.h"

namespace vette::assets {

// A four-character code ('PICT', 'snd ') as the big-endian number the Mac uses.
constexpr std::uint32_t fourcc(const char (&s)[5]) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[0])) << 24 |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[1])) << 16 |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[2])) << 8 | static_cast<std::uint8_t>(s[3]);
}
std::string fourcc_string(std::uint32_t code);  // printable (Mac Roman as UTF-8)
std::optional<std::uint32_t> fourcc_from_string(std::string_view text);  // 4 Mac Roman/ASCII chars

// CRC-16/XMODEM (CCITT polynomial, initial 0), as MacBinary II and BinHex 4.0 use it.
std::uint16_t crc16_xmodem(std::span<const std::uint8_t> bytes, std::uint16_t crc = 0);

struct Resource {
    std::uint32_t type = 0;
    std::int16_t id = 0;
    std::string name;  // UTF-8; empty when the resource has none
    bool has_name = false;
    std::uint8_t attributes = 0;
    std::uint32_t offset = 0, size = 0;  // the data's position in the fork
};

// A resource fork (Inside Macintosh: More Macintosh Toolbox, ch. 1), parsed from its raw bytes.
class ResourceFork {
public:
    // False (leaving the fork empty) if the header or map is malformed. A resource whose data lies
    // outside the fork is skipped and counted in skipped().
    bool parse(std::vector<std::uint8_t> fork, std::string* error = nullptr);

    bool empty() const { return resources_.empty(); }
    const std::vector<Resource>& resources() const { return resources_; }  // in map order
    std::vector<std::uint32_t> types() const;                             // in map order
    std::vector<const Resource*> of_type(std::uint32_t type) const;
    const Resource* find(std::uint32_t type, std::int16_t id) const;
    // Ignoring case, as GetNamedResource does (the game asks for "engine"; the resource is "Engine").
    const Resource* find(std::uint32_t type, std::string_view name) const;
    std::span<const std::uint8_t> data(const Resource& r) const;
    std::span<const std::uint8_t> get(std::uint32_t type, std::int16_t id) const;  // empty if absent
    int skipped() const { return skipped_; }
    const std::vector<std::uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::vector<Resource> resources_;
    std::map<std::pair<std::uint32_t, std::int16_t>, std::size_t> index_;  // first of each type/id
    int skipped_ = 0;
};

struct MacFile {
    // '/'-separated and UTF-8. Files on a disk image start with the volume's name
    // ("VETTE!/VETTE! Folder/(Folder) Color VETTE!/Color VETTE!"); loose files with their folder
    // relative to the one opened, under their Mac name ("extracted/VETTE! Folder/.../VETTE!.Data").
    std::string path;
    std::string name;                     // the last component
    std::uint32_t type = 0, creator = 0;  // Finder type and creator; 0 if the form doesn't keep them
    std::uint64_t data_size = 0, rsrc_size = 0;
    std::string source;  // where it came from: "HFS image VETTE_1_02.toast", "MacBinary II", "AppleDouble", ...
};

class MacFiles {
public:
    // Opens a folder (searched a few levels deep) or a single file. Never throws: an empty set means
    // nothing Mac-like is there, and notes() says what was found and skipped.
    static MacFiles open(const std::filesystem::path& location);

    bool empty() const { return files_.empty(); }
    const std::vector<MacFile>& files() const { return files_; }  // sorted by path
    const std::vector<std::string>& notes() const { return notes_; }

    // By full path, by trailing path components ("Color VETTE!/VETTE!.Data"), or by name; case is
    // ignored. With several matches, the first in path order. nullptr if there is none.
    const MacFile* find(std::string_view path_or_name) const;
    std::vector<const MacFile*> find_all(std::string_view path_or_name) const;
    // The first file with this type and creator (0 matches any creator).
    const MacFile* find_type(std::uint32_t type, std::uint32_t creator = 0) const;

    // `file` must be one of files(). Empty if the fork is empty or can't be read.
    std::vector<std::uint8_t> data_fork(const MacFile& file) const;
    std::vector<std::uint8_t> resource_fork(const MacFile& file) const;
    // The parsed resource fork, or nullopt if the file has none or it's malformed. By name: the first
    // match whose resource fork parses (e.g. resources("Color VETTE!")->get(fourcc("PICT"), 128)).
    std::optional<ResourceFork> resources(const MacFile& file) const;
    std::optional<ResourceFork> resources(std::string_view path_or_name) const;

    // Fork storage, public for the scanner's helpers.
    struct Fork {
        std::shared_ptr<const ByteSource> src;  // a host file or a decoded buffer...
        std::uint64_t offset = 0, size = 0;
        int volume = -1;                        // ...or a file on volumes_[volume]
        std::size_t entry = 0;
        bool resource = false;
    };

private:
    friend struct MacScanner;
    std::vector<std::uint8_t> read(const Fork& fork) const;

    std::vector<MacFile> files_;
    std::vector<std::pair<Fork, Fork>> forks_;  // data, resource; parallel to files_
    std::vector<HfsVolume> volumes_;
    std::vector<std::string> notes_;
};

}  // namespace vette::assets
