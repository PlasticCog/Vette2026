#pragma once
// Classic Mac disk images, read-only: the HFS volume format (Inside Macintosh: Files, ch. 2) and the
// ways an image wraps it (a bare volume as in .toast/.hfv/.dsk, a DiskCopy 4.2 header, or an Apple
// partition map). Players' images vary and may be damaged, so every structure is bounds-checked, the
// B-tree walks are limited, and bad data gives an error, never a crash or a hang. HFS+ isn't read (the
// 1989-91 releases shipped on HFS floppies); it is recognised and reported.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace vette::assets {

// Random-access bytes: a file read on demand (CD images can be large), a buffer, or a window into
// another source (a disk image inside a MacBinary file, a partition).
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual std::uint64_t size() const = 0;
    // False if [offset, offset + len) isn't inside the source or the read fails.
    virtual bool read(std::uint64_t offset, void* out, std::size_t len) const = 0;
    // The whole range as a vector (empty if unreadable or larger than `limit`).
    std::vector<std::uint8_t> read_all(std::uint64_t offset, std::uint64_t len,
                                       std::uint64_t limit = 256u << 20) const;

    static std::shared_ptr<const ByteSource> from_file(const std::filesystem::path& path);  // null if unopenable
    static std::shared_ptr<const ByteSource> from_memory(std::vector<std::uint8_t> bytes);
    // [offset, offset + size) of `base`, clipped to it.
    static std::shared_ptr<const ByteSource> slice(std::shared_ptr<const ByteSource> base, std::uint64_t offset,
                                                   std::uint64_t size);
};

// Mac Roman text (file and resource names) as UTF-8. Control characters become U+2400 + c, so names
// stay printable and distinct ("Icon\r" is "Icon␍").
std::string mac_roman_to_utf8(std::span<const std::uint8_t> text);
std::string mac_roman_to_utf8(std::string_view text);

// Where a disk image keeps its HFS volume.
struct HfsLocation {
    std::uint64_t offset = 0;  // the volume's first byte (its master directory block is 1024 bytes in)
    std::uint64_t size = 0;    // bytes from there to the end of the volume or image
    std::string container;     // "bare volume", "DiskCopy 4.2", "Apple partition map"
};

// Looks for an HFS volume in an image. Returns nullopt with the reason in `why_not` (e.g. "HFS+
// volume (not supported)", or empty when the bytes don't look like a disk image at all).
std::optional<HfsLocation> locate_hfs(const ByteSource& image, std::string* why_not = nullptr);

struct HfsEntry {
    std::string path;  // from the volume root, '/'-separated, UTF-8; a '/' inside a Mac name becomes ':'
    std::string name;  // the last component
    bool directory = false;
    std::uint32_t id = 0;      // catalog node id (the root folder is 2)
    std::uint32_t parent = 0;  // the containing folder's id
    std::uint32_t type = 0, creator = 0;  // files: Finder type and creator ('APPL', 'VETT')
    std::uint16_t finder_flags = 0;
    std::uint32_t data_size = 0, rsrc_size = 0;  // logical fork lengths
};

class HfsVolume {
public:
    // Opens the HFS volume in an image (see locate_hfs). Returns nullopt and sets `error` if there is
    // none or its catalog can't be read.
    static std::optional<HfsVolume> open(std::shared_ptr<const ByteSource> image, std::string* error = nullptr);

    const std::string& name() const { return name_; }
    const std::string& container() const { return container_; }
    // Every file and folder except the root, sorted by path.
    const std::vector<HfsEntry>& entries() const { return entries_; }
    // By path from the root, ignoring ASCII case as HFS does. nullptr if absent.
    const HfsEntry* find(std::string_view path) const;
    // Reads a fork in full. False (with `error`) if its extents are damaged or point outside the volume.
    bool read_fork(const HfsEntry& entry, bool resource, std::vector<std::uint8_t>& out,
                   std::string* error = nullptr) const;
    // Volume problems that didn't stop it opening (records skipped, names clashing).
    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    struct Extent {
        std::uint16_t start = 0, count = 0;
    };
    using ExtentRecord = std::array<Extent, 3>;
    struct Forks {
        ExtentRecord data{}, rsrc{};
    };

    HfsVolume() = default;
    bool read_extents(std::uint32_t file_id, bool resource, std::uint32_t size, const ExtentRecord& first,
                      std::vector<std::uint8_t>& out, std::string* error) const;
    bool load_btree(std::uint32_t file_id, std::uint32_t size, const ExtentRecord& first,
                    std::vector<std::uint8_t>& out, std::string* error) const;

    std::shared_ptr<const ByteSource> src_;
    std::uint64_t base_ = 0;         // volume offset in src_
    std::uint64_t alloc_start_ = 0;  // allocation block 0, relative to base_
    std::uint32_t block_size_ = 0;
    std::uint32_t num_blocks_ = 0;
    std::string name_, container_;
    std::vector<HfsEntry> entries_;
    std::vector<Forks> forks_;  // parallel to entries_
    // Extents overflow file: (file id, 0 data / 0xFF resource, first allocation block) -> 3 more extents.
    std::map<std::tuple<std::uint32_t, std::uint8_t, std::uint16_t>, ExtentRecord> overflow_;
    std::vector<std::string> warnings_;
};

}  // namespace vette::assets
