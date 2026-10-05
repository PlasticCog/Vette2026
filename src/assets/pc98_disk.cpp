#include "assets/pc98_disk.h"

#include "core/path_utf8.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <memory>
#include <set>
#include <system_error>
#include <utility>

namespace vette::assets {
namespace fs = std::filesystem;

namespace {

constexpr uint64_t kMaxImageBytes = 4ull << 30;  // bigger than any PC-98 disk: something else
constexpr uint64_t kMaxFloppyBytes = 16u << 20;  // D88/NFD are converted to plain sectors in memory
constexpr uint64_t kScanBytes = 4u << 20;        // boot sectors are looked for this far into a disk
constexpr uint64_t kMaxGameFileBytes = 16u << 20;
constexpr uint64_t kMaxGameBytes = 64u << 20;
constexpr size_t kMaxListed = 65536;  // directory entries per image
constexpr int kMaxDepth = 16;

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0] | p[1] << 8 | p[2] << 16) | static_cast<uint32_t>(p[3]) << 24;
}

char ascii_upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return ascii_upper(x) == ascii_upper(y); });
}

// The extension of a file name, lowercase, without the dot.
std::string extension(std::string_view name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string_view::npos || name.find_first_of("/\\", dot) != std::string_view::npos) {
        return {};
    }
    std::string ext(name.substr(dot + 1));
    for (char& c : ext) {
        c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return ext;
}

std::string hex(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

// Image extensions, most specific first: the order in which a folder's images are tried.
constexpr std::array<std::string_view, 17> kImageExtensions = {
    "hdi", "nhd", "thd", "fdi", "d88", "88d", "d98", "98d", "nfd",
    "hdm", "tfd", "xdf", "dup", "img", "ima", "raw", "hdd",
};

int image_rank(std::string_view ext) {
    const auto it = std::find(kImageExtensions.begin(), kImageExtensions.end(), ext);
    return it == kImageExtensions.end() ? -1 : static_cast<int>(it - kImageExtensions.begin());
}

// --- Image bytes ------------------------------------------------------------------------------------

class Source {
public:
    virtual ~Source() = default;
    virtual uint64_t size() const = 0;
    // False unless the whole range lies inside the source and was read.
    virtual bool read(uint64_t offset, std::span<uint8_t> out) const = 0;
};

class MemorySource final : public Source {
public:
    explicit MemorySource(std::span<const uint8_t> data) : data_(data) {}
    uint64_t size() const override { return data_.size(); }
    bool read(uint64_t offset, std::span<uint8_t> out) const override {
        if (offset > data_.size() || out.size() > data_.size() - offset) {
            return false;
        }
        std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
        return true;
    }

private:
    std::span<const uint8_t> data_;
};

class FileSource final : public Source {
public:
    FileSource(const fs::path& path, uint64_t size) : file_(path, std::ios::binary), size_(size) {}
    bool ok() const { return file_.is_open(); }
    uint64_t size() const override { return size_; }
    bool read(uint64_t offset, std::span<uint8_t> out) const override {
        if (offset > size_ || out.size() > size_ - offset) {
            return false;
        }
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(offset));
        file_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        return file_.gcount() == static_cast<std::streamsize>(out.size());
    }

private:
    mutable std::ifstream file_;
    uint64_t size_;
};

// The sectors of a disk, header stripped: a range of a source, plus what the image format says
// about the geometry (0 = unknown).
struct Disk {
    const Source* src = nullptr;
    uint64_t base = 0, size = 0;
    uint32_t sector_size = 0, heads = 0, sectors = 0;
    bool floppy = false;
    std::string format;
    std::unique_ptr<MemorySource> owned_src;  // converted floppy images
    std::vector<uint8_t> owned;

    bool read(uint64_t offset, std::span<uint8_t> out) const {
        return offset <= size && out.size() <= size - offset && src->read(base + offset, out);
    }
};

// --- Floppy formats with per-sector records (D88, NFD): sectors placed by their own C/H/R ids ------

struct SectorRecord {
    uint32_t c, h, r;
    uint64_t offset;  // of the data in the image file
    uint32_t length;
};

bool assemble_floppy(const Source& file, const std::vector<SectorRecord>& sectors, Disk& disk, std::string& error) {
    // The geometry is the common case: the most frequent sector size, the highest sector number among
    // those, and the cylinders and heads seen. Odd sectors (copy protection, other formats) are dropped.
    std::array<size_t, 8> by_size{};
    for (const SectorRecord& s : sectors) {
        for (size_t n = 0; n < by_size.size(); ++n) {
            if (s.length == 128u << n) {
                ++by_size[n];
            }
        }
    }
    const auto best = std::max_element(by_size.begin(), by_size.end());
    if (*best == 0) {
        error = "no sectors of a usable size";
        return false;
    }
    const uint32_t ss = 128u << static_cast<unsigned>(best - by_size.begin());
    uint32_t spt = 0, heads = 0, cyls = 0;
    for (const SectorRecord& s : sectors) {
        if (s.length == ss && s.r >= 1 && s.r <= 64 && s.h <= 1 && s.c <= 99) {
            spt = std::max(spt, s.r);
            heads = std::max(heads, s.h + 1);
            cyls = std::max(cyls, s.c + 1);
        }
    }
    const uint64_t bytes = uint64_t{cyls} * heads * spt * ss;
    if (bytes == 0 || bytes > kMaxFloppyBytes) {
        error = "implausible floppy geometry";
        return false;
    }
    disk.owned.assign(bytes, 0);
    for (const SectorRecord& s : sectors) {
        if (s.length == ss && s.r >= 1 && s.r <= spt && s.h < heads && s.c < cyls) {
            const uint64_t at = ((uint64_t{s.c} * heads + s.h) * spt + (s.r - 1)) * ss;
            if (!file.read(s.offset, std::span(disk.owned).subspan(at, ss))) {
                error = "sector data past the end of the image";
                return false;
            }
        }
    }
    disk.owned_src = std::make_unique<MemorySource>(disk.owned);
    disk.src = disk.owned_src.get();
    disk.base = 0;
    disk.size = bytes;
    disk.sector_size = ss;
    disk.heads = heads;
    disk.sectors = spt;
    disk.floppy = true;
    return true;
}

// D88: a 2B0h-byte header (name, flags, image size, then up to 164 track offsets), each track a run of
// sectors with 16-byte headers (C, H, R, N, sectors in track, ..., data length).
bool open_d88(const Source& file, Disk& disk, std::string& error) {
    std::array<uint8_t, 0x2B0> hdr{};
    if (!file.read(0, hdr)) {
        return false;
    }
    const uint64_t size = file.size();
    if (le32(&hdr[0x1C]) != size || size > kMaxFloppyBytes) {
        return false;
    }
    // The track table ends where the first track begins (some images have fewer than 164 entries).
    uint32_t first = 0x2B0;
    for (size_t i = 0x20; i + 4 <= hdr.size(); i += 4) {
        const uint32_t off = le32(&hdr[i]);
        if (off != 0 && off < first) {
            first = off;
        }
    }
    if (first < 0x20 + 4) {
        return false;
    }
    std::vector<SectorRecord> sectors;
    for (uint32_t i = 0x20; i + 4 <= first; i += 4) {
        uint64_t pos = le32(&hdr[i]);
        if (pos == 0) {
            continue;
        }
        std::array<uint8_t, 16> sh{};
        if (!file.read(pos, sh)) {
            error = "D88 track offset past the end";
            return false;
        }
        const uint32_t count = std::min<uint32_t>(le16(&sh[4]), 64);
        for (uint32_t k = 0; k < count; ++k) {
            if (!file.read(pos, sh)) {
                error = "D88 sector header past the end";
                return false;
            }
            const uint32_t length = le16(&sh[14]);
            if (pos + 16 + length > size) {
                error = "D88 sector data past the end";
                return false;
            }
            sectors.push_back({sh[0], sh[1], sh[2], pos + 16, length});
            pos += 16 + length;
        }
    }
    disk.format = "D88 floppy";
    return assemble_floppy(file, sectors, disk, error);
}

// NFD r0 (T98-Next): signature, comment, header size, then 163 tracks x 26 sector ids (C, H, R, N, ...)
// of 16 bytes; the data of every used id (C != FFh) follows the header in table order.
bool open_nfd(const Source& file, Disk& disk, std::string& error) {
    constexpr size_t kTable = 0x120, kIds = 163 * 26;
    std::vector<uint8_t> hdr(kTable + kIds * 16);
    if (!file.read(0, hdr)) {
        error = "NFD header truncated";
        return false;
    }
    const uint64_t size = file.size();
    uint64_t pos = le32(&hdr[0x110]);
    if (pos < hdr.size() || pos > size || size > kMaxFloppyBytes) {
        error = "NFD header size out of range";
        return false;
    }
    std::vector<SectorRecord> sectors;
    for (size_t i = 0; i < kIds; ++i) {
        const uint8_t* id = &hdr[kTable + i * 16];
        if (id[0] == 0xFF) {
            continue;
        }
        if (id[3] > 7) {
            error = "NFD sector size code out of range";
            return false;
        }
        const uint32_t length = 128u << id[3];
        if (pos + length > size) {
            error = "NFD sector data past the end";
            return false;
        }
        sectors.push_back({id[0], id[1], id[2], pos, length});
        pos += length;
    }
    disk.format = "NFD floppy";
    return assemble_floppy(file, sectors, disk, error);
}

// Anex86 HDI (hard disk) and FDI (floppy) share a header: reserved, type, header size, data size,
// sector size, sectors, heads, cylinders (little-endian dwords).
bool open_anex86(const Source& file, std::string_view ext, Disk& disk) {
    std::array<uint8_t, 32> h{};
    if (!file.read(0, h)) {
        return false;
    }
    const uint32_t header = le32(&h[8]), data = le32(&h[12]), ss = le32(&h[16]);
    const uint32_t spt = le32(&h[20]), heads = le32(&h[24]), cyls = le32(&h[28]);
    const bool ss_ok = ss == 128 || ss == 256 || ss == 512 || ss == 1024 || ss == 2048;
    const bool geometry_ok = spt >= 1 && spt <= 255 && heads >= 1 && heads <= 255 && cyls >= 1 && cyls <= 65535;
    if (le32(&h[0]) != 0 || header < 32 || header > file.size() || !ss_ok || !geometry_ok || data == 0 ||
        data > file.size() - header) {
        return false;
    }
    // Without the extension, insist on a consistent geometry: zeros at the start of a raw image
    // shouldn't pass for a header.
    const bool exact = uint64_t{ss} * spt * heads * cyls == data;
    if (ext != "hdi" && ext != "fdi" && !exact) {
        return false;
    }
    disk.base = header;
    disk.size = data;
    disk.sector_size = ss;
    disk.heads = heads;
    disk.sectors = spt;
    disk.floppy = ext == "fdi" || (ext != "hdi" && heads <= 2 && data <= 4u << 20);
    disk.format = disk.floppy ? "FDI floppy" : "HDI hard disk";
    return true;
}

// NHD (T98-Next hard disk): signature, comment, then header size, cylinders, heads, sectors, sector size.
bool open_nhd(const Source& file, Disk& disk, std::string& error) {
    std::array<uint8_t, 0x120> h{};
    if (!file.read(0, h)) {
        error = "NHD header truncated";
        return false;
    }
    const uint32_t header = le32(&h[0x110]), cyls = le32(&h[0x114]);
    const uint32_t heads = le16(&h[0x118]), spt = le16(&h[0x11A]), ss = le16(&h[0x11C]);
    if (header < 0x120 || header > file.size() || ss == 0 || ss > 4096 || heads == 0 || spt == 0) {
        error = "NHD header out of range";
        return false;
    }
    disk.base = header;
    disk.size = std::min<uint64_t>(file.size() - header, uint64_t{cyls} * heads * spt * ss);
    disk.sector_size = ss;
    disk.heads = heads;
    disk.sectors = spt;
    disk.format = "NHD hard disk";
    return true;
}

// Works out the image format and strips its header. False with an empty error: not an image we know.
bool identify(const Source& file, std::string_view name, Disk& disk, std::string& error) {
    const std::string ext = extension(name);
    std::array<uint8_t, 16> sig{};
    disk.src = &file;
    disk.base = 0;
    disk.size = file.size();
    if (file.read(0, sig)) {
        const std::string_view s(reinterpret_cast<const char*>(sig.data()), 14);
        if (s == "T98FDDIMAGE.R0") {
            return open_nfd(file, disk, error);
        }
        if (s == "T98FDDIMAGE.R1") {
            error = "NFD r1 images aren't supported (convert to D88 or NFD r0)";
            return false;
        }
        if (s == "T98HDDIMAGE.R0") {
            return open_nhd(file, disk, error);
        }
    }
    if (open_d88(file, disk, error) || !error.empty()) {
        return error.empty();
    }
    if (open_anex86(file, ext, disk)) {
        return true;
    }
    if (ext == "thd") {
        // T98: a 256-byte header (cylinders), then 8 heads x 33 sectors x 256 bytes per cylinder.
        std::array<uint8_t, 2> h{};
        if (file.size() < 256 || !file.read(0, h)) {
            error = "THD header truncated";
            return false;
        }
        disk.base = 256;
        disk.size = std::min<uint64_t>(file.size() - 256, uint64_t{le16(h.data())} * 8 * 33 * 256);
        disk.sector_size = 256;
        disk.heads = 8;
        disk.sectors = 33;
        disk.format = "THD hard disk";
        return true;
    }
    disk.floppy = file.size() <= 2u << 20;
    disk.format = disk.floppy ? "raw floppy image" : "raw hard disk image";
    return true;
}

// --- FAT ----------------------------------------------------------------------------------------------

struct Bpb {
    uint32_t bps, spc, reserved, fats, root_entries, total, fat_sectors, media, spt, heads, hidden;
};

std::optional<Bpb> parse_bpb(const uint8_t* s) {
    Bpb b{};
    b.bps = le16(s + 11);
    b.spc = s[13];
    b.reserved = le16(s + 14);
    b.fats = s[16];
    b.root_entries = le16(s + 17);
    b.total = le16(s + 19) ? le16(s + 19) : le32(s + 32);
    b.media = s[21];
    b.fat_sectors = le16(s + 22);
    b.spt = le16(s + 24);
    b.heads = le16(s + 26);
    b.hidden = le32(s + 28);
    const bool pow2_bps = b.bps >= 128 && b.bps <= 4096 && (b.bps & (b.bps - 1)) == 0;
    const bool pow2_spc = b.spc >= 1 && (b.spc & (b.spc - 1)) == 0;
    if (!pow2_bps || !pow2_spc || b.reserved == 0 || b.fats == 0 || b.fats > 2 || b.root_entries == 0 ||
        b.root_entries > 4096 || b.fat_sectors == 0 || b.media < 0xF0 || b.total == 0) {
        return std::nullopt;
    }
    return b;
}

// The PC-98 2HD floppy format (1.2 MB, 1024-byte sectors) for disks formatted without a BPB.
constexpr Bpb kPc98TwoHd{1024, 1, 1, 2, 192, 1232, 2, 0xFE, 8, 2, 0};

class FatVolume {
public:
    // Checks the layout against the disk; the first FAT must start with a FAT id (F0h-FFh, then FFh).
    // PC-98 hard disks have id FEh where the BPB says F8h, so the two only have to agree when the BPB
    // is assumed (`exact_media`).
    static std::optional<FatVolume> mount(const Disk& disk, uint64_t offset, const Bpb& b, bool exact_media) {
        FatVolume v;
        v.disk_ = &disk;
        v.offset_ = offset;
        v.bpb_ = b;
        const uint64_t root_sectors = (uint64_t{b.root_entries} * 32 + b.bps - 1) / b.bps;
        const uint64_t meta = b.reserved + uint64_t{b.fats} * b.fat_sectors + root_sectors;
        if (meta >= b.total) {
            return std::nullopt;
        }
        v.clusters_ = static_cast<uint32_t>((b.total - meta) / b.spc);
        if (v.clusters_ == 0 || v.clusters_ >= 65525) {
            return std::nullopt;  // FAT32 isn't a PC-98 DOS file system
        }
        v.fat16_ = v.clusters_ >= 4085;
        const uint64_t fat_bytes = uint64_t{b.fat_sectors} * b.bps;
        const uint64_t needed = v.fat16_ ? (v.clusters_ + 2) * 2ull : ((v.clusters_ + 2) * 3ull + 1) / 2;
        if (fat_bytes < needed) {
            return std::nullopt;
        }
        v.fat_.resize(static_cast<size_t>(needed));
        v.root_off_ = offset + (b.reserved + uint64_t{b.fats} * b.fat_sectors) * b.bps;
        v.data_off_ = offset + meta * b.bps;
        v.cluster_bytes_ = b.bps * b.spc;
        if (!disk.read(offset + uint64_t{b.reserved} * b.bps, v.fat_) || v.fat_[0] < 0xF0 || v.fat_[1] != 0xFF ||
            (exact_media && v.fat_[0] != b.media)) {
            return std::nullopt;
        }
        return v;
    }

    uint64_t offset() const { return offset_; }
    uint64_t end() const { return offset_ + uint64_t{bpb_.total} * bpb_.bps; }
    bool fat16() const { return fat16_; }
    uint32_t clusters() const { return clusters_; }

    // A directory's raw entries: the root region, or a subdirectory's cluster chain.
    bool read_root(std::vector<uint8_t>& out) const {
        out.resize(size_t{bpb_.root_entries} * 32);
        return disk_->read(root_off_, out);
    }
    bool read_chain(uint32_t first, uint64_t max_bytes, std::vector<uint8_t>& out) const {
        out.clear();
        std::vector<bool> visited(size_t{clusters_} + 2);
        uint32_t c = first;
        while (out.size() < max_bytes) {
            if (c < 2 || c >= clusters_ + 2 || visited[c]) {
                return false;  // free, reserved, bad or out-of-range cluster, or a loop
            }
            visited[c] = true;
            const size_t at = out.size();
            out.resize(at + cluster_bytes_);
            if (!disk_->read(data_off_ + uint64_t{c - 2} * cluster_bytes_, std::span(out).subspan(at))) {
                return false;
            }
            c = next(c);
            if (c >= (fat16_ ? 0xFFF8u : 0xFF8u)) {
                break;  // end of chain
            }
        }
        if (out.size() > max_bytes) {
            out.resize(static_cast<size_t>(max_bytes));
        }
        return true;
    }
    bool read_file(uint32_t first, uint32_t size, std::vector<uint8_t>& out) const {
        if (size == 0) {
            out.clear();
            return true;
        }
        return read_chain(first, size, out) && out.size() == size;
    }

private:
    uint32_t next(uint32_t c) const {
        if (fat16_) {
            return le16(&fat_[size_t{c} * 2]);
        }
        const size_t i = size_t{c} * 3 / 2;
        const uint32_t v = le16(&fat_[i]);
        return (c & 1) ? v >> 4 : v & 0xFFF;
    }

    const Disk* disk_ = nullptr;
    Bpb bpb_{};
    uint64_t offset_ = 0, root_off_ = 0, data_off_ = 0;
    uint32_t clusters_ = 0, cluster_bytes_ = 0;
    bool fat16_ = false;
    std::vector<uint8_t> fat_;
};

std::optional<Bpb> bpb_at(const Disk& disk, uint64_t offset) {
    std::array<uint8_t, 64> s{};
    return disk.read(offset, s) ? parse_bpb(s.data()) : std::nullopt;
}

// The FAT volumes of a disk: a floppy's (at 0), PC-98 hard disk partitions from the partition table,
// and boot sectors found near the start (images without geometry, or with damaged tables).
std::vector<FatVolume> find_volumes(Disk& disk) {
    std::vector<FatVolume> volumes;
    const auto try_mount = [&](uint64_t offset, const Bpb& b, bool exact_media = false) {
        for (const FatVolume& v : volumes) {
            if (offset >= v.offset() && offset < v.end()) {
                return;
            }
        }
        if (auto v = FatVolume::mount(disk, offset, b, exact_media)) {
            volumes.push_back(std::move(*v));
        }
    };
    if (auto b = bpb_at(disk, 0)) {
        try_mount(0, *b);
    } else if (disk.floppy && disk.size >= uint64_t{kPc98TwoHd.total} * kPc98TwoHd.bps) {
        try_mount(0, kPc98TwoHd, true);
    }
    if (!volumes.empty() && volumes[0].end() >= disk.size) {
        return volumes;
    }

    // Boot sectors near the start; the first one also tells the geometry if the image didn't.
    const uint64_t scan_end = std::min(disk.size, kScanBytes);
    std::vector<uint8_t> chunk;
    constexpr uint64_t kChunk = 64u << 10;
    for (uint64_t at = 0; at + 512 <= scan_end; at += kChunk) {
        chunk.resize(static_cast<size_t>(std::min(kChunk + 64, scan_end - at)));
        if (!disk.read(at, chunk)) {
            break;
        }
        for (size_t i = at == 0 ? 256 : 0; i < kChunk && i + 64 <= chunk.size(); i += 256) {
            const uint64_t off = at + i;
            if (auto b = parse_bpb(&chunk[i])) {
                const size_t before = volumes.size();
                try_mount(off, *b);
                if (volumes.size() > before && disk.sectors == 0 && b->hidden != 0 && off % b->hidden == 0 &&
                    b->spt != 0 && b->heads != 0) {
                    disk.sector_size = static_cast<uint32_t>(off / b->hidden);
                    disk.sectors = b->spt;
                    disk.heads = b->heads;
                }
            }
        }
    }

    // PC-98 partition table: the sector after the IPL, 32-byte entries (boot and system ids, IPL and
    // start/end positions as sector, head, cylinder word).
    if (disk.sector_size >= 256 && disk.sectors != 0 && disk.heads != 0 && !disk.floppy) {
        const uint32_t table_at = std::max<uint32_t>(disk.sector_size, 512);
        std::array<uint8_t, 512> table{};
        if (disk.read(table_at, table)) {
            for (size_t e = 0; e < table.size(); e += 32) {
                const uint8_t* p = &table[e];
                if (p[0] == 0 && p[1] == 0) {
                    continue;
                }
                const uint64_t lba = (uint64_t{le16(p + 10)} * disk.heads + p[9]) * disk.sectors + p[8];
                const uint64_t offset = lba * disk.sector_size;
                if (offset != 0 && offset < disk.size) {
                    if (auto b = bpb_at(disk, offset)) {
                        try_mount(offset, *b);
                    }
                }
            }
        }
    }
    std::sort(volumes.begin(), volumes.end(), [](const FatVolume& a, const FatVolume& b) { return a.offset() < b.offset(); });
    return volumes;
}

struct DirEntry {
    std::string name;
    uint32_t cluster = 0, size = 0;
    bool directory = false;
};

std::vector<DirEntry> parse_directory(std::span<const uint8_t> raw) {
    std::vector<DirEntry> entries;
    for (size_t i = 0; i + 32 <= raw.size(); i += 32) {
        const uint8_t* e = &raw[i];
        if (e[0] == 0x00) {
            break;  // end of the directory
        }
        const uint8_t attr = e[11];
        if (e[0] == 0xE5 || attr == 0x0F || (attr & 0x08)) {
            continue;  // deleted, long-name part, volume label
        }
        std::string base(reinterpret_cast<const char*>(e), 8), ext(reinterpret_cast<const char*>(e + 8), 3);
        if (base[0] == 0x05) {
            base[0] = static_cast<char>(0xE5);  // a Shift-JIS lead byte E5h, escaped
        }
        base.erase(base.find_last_not_of(' ') + 1);  // npos + 1 == 0: all spaces
        ext.erase(ext.find_last_not_of(' ') + 1);
        if (base.empty() || base == "." || base == "..") {
            continue;
        }
        bool printable = true;
        for (const char c : base + ext) {
            printable = printable && (static_cast<uint8_t>(c) >= 0x20 && c != '\\' && c != '/');
        }
        if (!printable) {
            continue;
        }
        entries.push_back({ext.empty() ? base : base + "." + ext, le16(e + 26), le32(e + 28), (attr & 0x10) != 0});
    }
    return entries;
}

}  // namespace

// --- Pc98Files --------------------------------------------------------------------------------------

namespace {

std::optional<Pc98Files> fail(std::string& error, std::string message) {
    error = std::move(message);
    return std::nullopt;
}

}  // namespace

struct Pc98FilesBuilder {
    static std::optional<Pc98Files> from_source(const Source& file, std::string_view name, std::string& error) {
        Disk disk;
        std::string why;
        if (!identify(file, name, disk, why)) {
            return fail(error, std::string(name) + ": " + (why.empty() ? "not a disk image" : why));
        }
        std::vector<FatVolume> volumes = find_volumes(disk);
        if (volumes.empty()) {
            return fail(error, std::string(name) + ": " + disk.format + " without a FAT file system");
        }

        Pc98Files files;
        struct Found {
            int volume;
            std::string dir;
            std::vector<DirEntry> entries;
        };
        std::optional<Found> game;
        std::vector<uint8_t> raw;
        for (size_t vi = 0; vi < volumes.size(); ++vi) {
            const FatVolume& vol = volumes[vi];
            // Breadth-first, so the shallowest VETTE.EXE wins; each directory cluster is visited once.
            struct Pending {
                std::string path;
                uint32_t cluster;  // 0 = root
                int depth;
            };
            std::vector<Pending> queue{{"", 0, 0}};
            std::set<uint32_t> seen;
            for (size_t qi = 0; qi < queue.size() && files.listing_.size() < kMaxListed; ++qi) {
                const Pending dir = queue[qi];
                const bool ok = dir.cluster == 0 ? vol.read_root(raw) : vol.read_chain(dir.cluster, 2u << 20, raw);
                if (!ok) {
                    continue;  // unreadable directory: list what can be read
                }
                std::vector<DirEntry> entries = parse_directory(raw);
                for (const DirEntry& e : entries) {
                    if (files.listing_.size() >= kMaxListed) {
                        break;
                    }
                    const std::string path = dir.path.empty() ? e.name : dir.path + "\\" + e.name;
                    files.listing_.push_back({path, e.directory ? 0 : e.size, e.directory, static_cast<int>(vi)});
                    if (e.directory && dir.depth + 1 < kMaxDepth && seen.insert(e.cluster).second) {
                        queue.push_back({path, e.cluster, dir.depth + 1});
                    }
                    if (!game && !e.directory && iequals(e.name, "VETTE.EXE")) {
                        game = Found{static_cast<int>(vi), dir.path, entries};
                    }
                }
            }
        }
        if (!game) {
            return fail(error, std::string(name) + ": no VETTE.EXE on the disk");
        }

        // Read the game folder now; the image isn't needed afterwards.
        const FatVolume& vol = volumes[static_cast<size_t>(game->volume)];
        std::sort(game->entries.begin(), game->entries.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
        uint64_t total = 0;
        int skipped = 0;
        for (const DirEntry& e : game->entries) {
            std::vector<uint8_t> data;
            if (e.directory || files.index(e.name) >= 0) {
                continue;
            }
            if (e.size > kMaxGameFileBytes || total + e.size > kMaxGameBytes || !vol.read_file(e.cluster, e.size, data)) {
                if (iequals(e.name, "VETTE.EXE")) {
                    return fail(error, std::string(name) + ": VETTE.EXE is damaged on the disk");
                }
                ++skipped;
                continue;
            }
            total += e.size;
            files.names_.push_back(e.name);
            files.data_.push_back(std::move(data));
        }
        files.description_ = std::string(name) + ": " + disk.format + ", " + (vol.fat16() ? "FAT16" : "FAT12") +
                             " volume at " + hex(vol.offset()) + ", \\" + game->dir;
        if (skipped) {
            files.description_ += " (" + std::to_string(skipped) + " unreadable file(s) skipped)";
        }
        return files;
    }

    // VETTE.EXE in the folder: plain files. Otherwise its disk images, most specific format first.
    // `subfolders` (optional) receives the folder's subfolders.
    static std::optional<Pc98Files> from_folder(const fs::path& folder, std::vector<fs::path>* subfolders,
                                                std::string& error) {
        std::error_code ec;
        std::vector<fs::path> regular;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec)) {
                regular.push_back(it->path());
            } else if (subfolders && it->is_directory(ec) && subfolders->size() < 64) {
                subfolders->push_back(it->path());
            }
        }
        if (ec && regular.empty()) {
            return fail(error, "no PC-98 folder at " + path_to_utf8(folder));
        }
        std::sort(regular.begin(), regular.end());
        for (const fs::path& p : regular) {
            if (iequals(path_to_utf8(p.filename()), "VETTE.EXE")) {
                Pc98Files files;
                files.description_ = path_to_utf8(folder) + ": files";
                for (const fs::path& f : regular) {
                    files.names_.push_back(path_to_utf8(f.filename()));
                    files.paths_.push_back(f);
                }
                return files;
            }
        }
        std::vector<fs::path> images;
        for (const fs::path& p : regular) {
            if (image_rank(extension(path_to_utf8(p.filename()))) >= 0) {
                images.push_back(p);
            }
        }
        std::stable_sort(images.begin(), images.end(), [](const fs::path& a, const fs::path& b) {
            return image_rank(extension(path_to_utf8(a.filename()))) < image_rank(extension(path_to_utf8(b.filename())));
        });
        std::string tried;
        for (const fs::path& p : images) {
            std::string why;
            if (auto files = Pc98Files::open_image(p, why)) {
                return files;
            }
            tried += (tried.empty() ? "" : "; ") + why;
        }
        return fail(error, images.empty() ? "no VETTE.EXE or disk image in " + path_to_utf8(folder) : tried);
    }
};

std::optional<Pc98Files> Pc98Files::open_image(std::span<const uint8_t> image, std::string_view name, std::string& error) {
    const MemorySource src(image);
    return Pc98FilesBuilder::from_source(src, name, error);
}

std::optional<Pc98Files> Pc98Files::open_image(const fs::path& image, std::string& error) {
    const std::string name = path_to_utf8(image.filename());
    std::error_code ec;
    const uint64_t size = fs::file_size(image, ec);
    if (ec || size == 0 || size > kMaxImageBytes) {
        return fail(error, name + ": " + (ec ? "can't read the file" : "not a disk image (size)"));
    }
    const FileSource src(image, size);
    if (!src.ok()) {
        return fail(error, name + ": can't open the file");
    }
    return Pc98FilesBuilder::from_source(src, name, error);
}

std::optional<Pc98Files> Pc98Files::open(const fs::path& folder, std::string& error) {
    std::vector<fs::path> subfolders;
    if (auto files = Pc98FilesBuilder::from_folder(folder, &subfolders, error)) {
        return files;
    }
    // A copy unpacked into a folder of its own (Game/PC98/<release>/) is found too.
    std::sort(subfolders.begin(), subfolders.end());
    for (const fs::path& sub : subfolders) {
        std::string ignored;
        if (auto files = Pc98FilesBuilder::from_folder(sub, nullptr, ignored)) {
            return files;
        }
    }
    return std::nullopt;
}

int Pc98Files::index(std::string_view name) const {
    for (size_t i = 0; i < names_.size(); ++i) {
        if (iequals(names_[i], name)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool Pc98Files::contains(std::string_view name) const { return index(name) >= 0; }

bool Pc98Files::read(std::string_view name, std::vector<uint8_t>& out) const {
    const int i = index(name);
    if (i < 0) {
        return false;
    }
    if (!data_.empty()) {
        out = data_[static_cast<size_t>(i)];
        return true;
    }
    const fs::path& path = paths_[static_cast<size_t>(i)];
    std::error_code ec;
    const uint64_t size = fs::file_size(path, ec);
    if (ec || size > kMaxGameFileBytes) {
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    out.resize(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return f.gcount() == static_cast<std::streamsize>(out.size());
}

std::optional<std::vector<uint8_t>> Pc98Files::read(std::string_view name) const {
    std::vector<uint8_t> out;
    if (!read(name, out)) {
        return std::nullopt;
    }
    return out;
}

}  // namespace vette::assets
