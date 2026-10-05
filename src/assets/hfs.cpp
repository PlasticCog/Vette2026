#include "assets/hfs.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <unordered_map>

namespace vette::assets {

namespace {

// Big-endian reads (the caller checks the bounds).
std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }
std::uint32_t be32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}

// --- Byte sources ------------------------------------------------------------------------------------

// Opens the file for each read: a folder of loose files would otherwise hold hundreds of handles open
// (the C runtime's limit can be 512), and reads are few and large.
class FileSource final : public ByteSource {
public:
    explicit FileSource(std::filesystem::path path) : path_(std::move(path)) {
        std::error_code ec;
        ok_ = std::filesystem::is_regular_file(path_, ec);
        const auto size = ok_ ? std::filesystem::file_size(path_, ec) : 0;
        if (ec) ok_ = false;
        size_ = ok_ ? size : 0;
    }
    bool ok() const { return ok_ && std::ifstream(path_, std::ios::binary).is_open(); }
    std::uint64_t size() const override { return size_; }
    bool read(std::uint64_t offset, void* out, std::size_t len) const override {
        if (offset > size_ || len > size_ - offset) return false;
        if (len == 0) return true;
        std::ifstream file(path_, std::ios::binary);
        if (!file.seekg(static_cast<std::streamoff>(offset))) return false;
        file.read(static_cast<char*>(out), static_cast<std::streamsize>(len));
        return file.gcount() == static_cast<std::streamsize>(len);
    }

private:
    std::filesystem::path path_;
    std::uint64_t size_ = 0;
    bool ok_ = false;
};

class MemorySource final : public ByteSource {
public:
    explicit MemorySource(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}
    std::uint64_t size() const override { return bytes_.size(); }
    bool read(std::uint64_t offset, void* out, std::size_t len) const override {
        if (offset > bytes_.size() || len > bytes_.size() - offset) return false;
        if (len) std::memcpy(out, bytes_.data() + offset, len);
        return true;
    }

private:
    std::vector<std::uint8_t> bytes_;
};

class SliceSource final : public ByteSource {
public:
    SliceSource(std::shared_ptr<const ByteSource> base, std::uint64_t offset, std::uint64_t size)
        : base_(std::move(base)), offset_(offset), size_(size) {}
    std::uint64_t size() const override { return size_; }
    bool read(std::uint64_t offset, void* out, std::size_t len) const override {
        if (offset > size_ || len > size_ - offset) return false;
        return base_->read(offset_ + offset, out, len);
    }

private:
    std::shared_ptr<const ByteSource> base_;
    std::uint64_t offset_, size_;
};

// --- Mac Roman ---------------------------------------------------------------------------------------

constexpr std::uint16_t kMacRomanHigh[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5,
    0x00E7, 0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4,
    0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6,
    0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8, 0x221E, 0x00B1, 0x2264, 0x2265,
    0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF,
    0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5,
    0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044,
    0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02, 0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
    0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4, 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9,
    0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

void append_utf8(std::string& out, std::uint32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | c >> 6);
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | c >> 12);
        out += static_cast<char>(0x80 | (c >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// --- HFS structures ----------------------------------------------------------------------------------

constexpr std::uint16_t kSigHfs = 0x4244;       // 'BD'
constexpr std::uint16_t kSigHfsPlus = 0x482B;   // 'H+'
constexpr std::uint16_t kSigHfsX = 0x4858;      // 'HX'
constexpr std::size_t kMdbSize = 162;
constexpr std::uint32_t kRootId = 2;
constexpr std::uint32_t kExtentsFileId = 3, kCatalogFileId = 4;
constexpr std::uint32_t kMaxBtreeBytes = 64u << 20;
constexpr std::uint32_t kMaxForkBytes = 512u << 20;

enum class MdbKind { None, Hfs, HfsPlus, Wrapper };

MdbKind check_mdb(const ByteSource& src, std::uint64_t volume) {
    std::uint8_t m[kMdbSize];
    if (!src.read(volume + 1024, m, sizeof m)) return MdbKind::None;
    const std::uint16_t sig = be16(m);
    if (sig == kSigHfsPlus || sig == kSigHfsX) return MdbKind::HfsPlus;
    if (sig != kSigHfs) return MdbKind::None;
    const std::uint32_t block_size = be32(m + 20);
    if (block_size == 0 || block_size % 512 != 0 || be16(m + 18) == 0 || m[36] > 27) return MdbKind::None;
    if (be16(m + 124) == kSigHfsPlus) return MdbKind::Wrapper;  // an HFS+ volume embedded in an HFS wrapper
    return MdbKind::Hfs;
}

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        unsigned char x = static_cast<unsigned char>(a[i]), y = static_cast<unsigned char>(b[i]);
        if (x >= 'A' && x <= 'Z') x = static_cast<unsigned char>(x + 32);
        if (y >= 'A' && y <= 'Z') y = static_cast<unsigned char>(y + 32);
        if (x != y) return false;
    }
    return true;
}

// Calls `fn` for every record of every leaf node of a B-tree file, following the leaf chain from the
// header's first leaf. Damage ends the walk with a warning; what was read so far stays.
bool walk_leaves(const std::vector<std::uint8_t>& tree, const std::function<void(std::span<const std::uint8_t>)>& fn,
                 std::vector<std::string>& warnings, const char* what, std::string* error) {
    if (tree.size() < 512 || static_cast<std::int8_t>(tree[8]) != 1) {
        if (error) *error = std::string(what) + " B-tree has no header node";
        return false;
    }
    const std::uint8_t* h = tree.data() + 14;
    const std::uint32_t first_leaf = be32(h + 10);
    const std::uint32_t node_size = be16(h + 18);
    if (node_size < 512 || node_size > 32768 || (node_size & (node_size - 1)) != 0 || node_size > tree.size()) {
        if (error) *error = std::string(what) + " B-tree has a bad node size";
        return false;
    }
    const std::size_t nodes = tree.size() / node_size;
    std::uint32_t n = first_leaf;
    std::size_t steps = 0;
    while (n != 0) {
        if (n >= nodes || ++steps > nodes) {
            warnings.push_back(std::string(what) + " B-tree: broken leaf chain");
            break;
        }
        const std::uint8_t* node = tree.data() + static_cast<std::size_t>(n) * node_size;
        if (static_cast<std::int8_t>(node[8]) != -1) {
            warnings.push_back(std::string(what) + " B-tree: leaf chain reaches a non-leaf node");
            break;
        }
        const std::uint32_t records = be16(node + 10);
        if (14 + 2 * (records + 1) > node_size) {
            warnings.push_back(std::string(what) + " B-tree: bad record count");
            break;
        }
        const std::uint8_t* table = node + node_size;  // offsets grow down from the end
        for (std::uint32_t i = 0; i < records; ++i) {
            const std::uint32_t start = be16(table - 2 * (i + 1));
            const std::uint32_t end = be16(table - 2 * (i + 2));
            if (start < 14 || end <= start || end > node_size - 2 * (records + 1)) {
                warnings.push_back(std::string(what) + " B-tree: bad record offset");
                continue;
            }
            fn(std::span<const std::uint8_t>(node + start, end - start));
        }
        n = be32(node);
    }
    return true;
}

}  // namespace

// --- ByteSource --------------------------------------------------------------------------------------

std::vector<std::uint8_t> ByteSource::read_all(std::uint64_t offset, std::uint64_t len, std::uint64_t limit) const {
    if (len > limit || offset > size() || len > size() - offset) return {};
    std::vector<std::uint8_t> out(static_cast<std::size_t>(len));
    if (!read(offset, out.data(), out.size())) return {};
    return out;
}

std::shared_ptr<const ByteSource> ByteSource::from_file(const std::filesystem::path& path) {
    auto src = std::make_shared<FileSource>(path);
    if (!src->ok()) return nullptr;
    return src;
}

std::shared_ptr<const ByteSource> ByteSource::from_memory(std::vector<std::uint8_t> bytes) {
    return std::make_shared<MemorySource>(std::move(bytes));
}

std::shared_ptr<const ByteSource> ByteSource::slice(std::shared_ptr<const ByteSource> base, std::uint64_t offset,
                                                    std::uint64_t size) {
    const std::uint64_t total = base->size();
    offset = std::min(offset, total);
    size = std::min(size, total - offset);
    return std::make_shared<SliceSource>(std::move(base), offset, size);
}

// --- Mac Roman ---------------------------------------------------------------------------------------

std::string mac_roman_to_utf8(std::span<const std::uint8_t> text) {
    std::string out;
    out.reserve(text.size());
    for (const std::uint8_t c : text) {
        if (c < 0x20) {
            append_utf8(out, 0x2400u + c);
        } else if (c == 0x7F) {
            append_utf8(out, 0x2421);
        } else if (c < 0x80) {
            out += static_cast<char>(c);
        } else {
            append_utf8(out, kMacRomanHigh[c - 0x80]);
        }
    }
    return out;
}

std::string mac_roman_to_utf8(std::string_view text) {
    return mac_roman_to_utf8(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

// --- Locating the volume -----------------------------------------------------------------------------

std::optional<HfsLocation> locate_hfs(const ByteSource& image, std::string* why_not) {
    bool saw_plus = false;
    auto try_at = [&](std::uint64_t offset, std::uint64_t size, const char* container) -> std::optional<HfsLocation> {
        switch (check_mdb(image, offset)) {
        case MdbKind::Hfs:
            return HfsLocation{offset, std::min(size, image.size() - std::min(offset, image.size())), container};
        case MdbKind::HfsPlus:
        case MdbKind::Wrapper:
            saw_plus = true;
            break;
        case MdbKind::None:
            break;
        }
        return std::nullopt;
    };

    if (auto loc = try_at(0, image.size(), "bare volume")) return loc;

    // DiskCopy 4.2: an 84-byte header (name, sizes, checksums, format, 0x0100), then the disk's bytes.
    std::uint8_t dc[84];
    if (image.read(0, dc, sizeof dc) && dc[0] <= 63 && be16(dc + 82) == 0x0100) {
        const std::uint32_t data_size = be32(dc + 64);
        if (auto loc = try_at(84, data_size, "DiskCopy 4.2")) return loc;
    }

    // Apple partition map: a driver descriptor ('ER') in block 0, then 'PM' entries from block 1. The
    // entries count in the device's block size, which CD images set to 2048; try both readings.
    std::uint8_t block[512];
    std::uint32_t dev_block = 512;
    if (image.read(0, block, sizeof block) && be16(block) == 0x4552) {
        const std::uint32_t b = be16(block + 2);
        if (b >= 512 && b <= 8192 && (b & (b - 1)) == 0) dev_block = b;
    }
    std::uint32_t entries = 1;
    for (std::uint32_t i = 1; i <= entries && i <= 256; ++i) {
        if (!image.read(static_cast<std::uint64_t>(i) * 512, block, sizeof block) || be16(block) != 0x504D) break;
        entries = std::max<std::uint32_t>(entries, be32(block + 4));
        char type[33] = {};
        std::memcpy(type, block + 48, 32);
        if (!ieq(type, "Apple_HFS")) continue;
        const std::uint64_t start = be32(block + 8), count = be32(block + 12);
        for (const std::uint64_t unit : {std::uint64_t{512}, std::uint64_t{dev_block}}) {
            if (auto loc = try_at(start * unit, count * unit, "Apple partition map")) return loc;
        }
    }

    if (why_not) *why_not = saw_plus ? "HFS+ volume (not supported; copy the files out with a Mac tool)" : "";
    return std::nullopt;
}

// --- HfsVolume ---------------------------------------------------------------------------------------

std::optional<HfsVolume> HfsVolume::open(std::shared_ptr<const ByteSource> image, std::string* error) {
    auto fail = [&](const std::string& why) -> std::optional<HfsVolume> {
        if (error) *error = why;
        return std::nullopt;
    };
    if (!image) return fail("no image");
    std::string why;
    const auto loc = locate_hfs(*image, &why);
    if (!loc) return fail(why.empty() ? "no HFS volume found" : why);

    HfsVolume v;
    v.src_ = image;
    v.base_ = loc->offset;
    v.container_ = loc->container;
    std::uint8_t m[kMdbSize];
    if (!image->read(v.base_ + 1024, m, sizeof m)) return fail("truncated volume header");
    v.num_blocks_ = be16(m + 18);
    v.block_size_ = be32(m + 20);
    v.alloc_start_ = static_cast<std::uint64_t>(be16(m + 28)) * 512;
    v.name_ = mac_roman_to_utf8(std::span<const std::uint8_t>(m + 37, std::min<std::size_t>(m[36], 27)));
    auto extents = [](const std::uint8_t* p) {
        ExtentRecord r;
        for (int i = 0; i < 3; ++i) r[static_cast<std::size_t>(i)] = {be16(p + 4 * i), be16(p + 4 * i + 2)};
        return r;
    };

    // The extents overflow file first: the catalog's own extents may continue there.
    std::vector<std::uint8_t> tree;
    if (!v.load_btree(kExtentsFileId, be32(m + 130), extents(m + 134), tree, error)) return std::nullopt;
    if (!walk_leaves(
            tree,
            [&](std::span<const std::uint8_t> rec) {
                if (rec.size() < 8 + 12 || rec[0] < 7) return;
                const std::size_t data = (1u + rec[0] + 1u) & ~1u;
                if (data + 12 > rec.size()) return;
                v.overflow_[{be32(rec.data() + 2), rec[1], be16(rec.data() + 6)}] = extents(rec.data() + data);
            },
            v.warnings_, "extents", error))
        return std::nullopt;

    if (!v.load_btree(kCatalogFileId, be32(m + 146), extents(m + 150), tree, error)) return std::nullopt;
    struct Node {
        HfsEntry entry;
        Forks forks;
    };
    std::vector<Node> nodes;
    if (!walk_leaves(
            tree,
            [&](std::span<const std::uint8_t> rec) {
                const std::size_t key_len = rec.size() > 0 ? rec[0] : 0;
                if (key_len < 6 || key_len + 1 > rec.size()) return;
                const std::size_t name_len = rec[6];
                if (7 + name_len > key_len + 1) return;
                const std::size_t data = (key_len + 2) & ~std::size_t{1};
                if (data + 2 > rec.size()) return;
                const std::uint8_t* d = rec.data() + data;
                const std::size_t avail = rec.size() - data;
                Node n;
                n.entry.parent = be32(rec.data() + 2);
                n.entry.name = mac_roman_to_utf8(std::span<const std::uint8_t>(rec.data() + 7, name_len));
                std::replace(n.entry.name.begin(), n.entry.name.end(), '/', ':');
                if (d[0] == 1 && avail >= 70) {
                    n.entry.directory = true;
                    n.entry.id = be32(d + 6);
                } else if (d[0] == 2 && avail >= 102) {
                    n.entry.id = be32(d + 20);
                    n.entry.type = be32(d + 4);
                    n.entry.creator = be32(d + 8);
                    n.entry.finder_flags = be16(d + 12);
                    n.entry.data_size = be32(d + 26);
                    n.entry.rsrc_size = be32(d + 36);
                    n.forks.data = extents(d + 74);
                    n.forks.rsrc = extents(d + 86);
                } else {
                    return;  // thread records (3, 4) only repeat the key
                }
                nodes.push_back(std::move(n));
            },
            v.warnings_, "catalog", error))
        return std::nullopt;

    // Paths: follow the parent links up to the root folder.
    std::unordered_map<std::uint32_t, std::size_t> dirs;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].entry.directory) dirs.emplace(nodes[i].entry.id, i);
    }
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        HfsEntry& e = nodes[i].entry;
        if (e.directory && e.id == kRootId) continue;
        std::string path = e.name;
        std::uint32_t parent = e.parent;
        int depth = 0;
        while (parent != kRootId) {
            const auto it = dirs.find(parent);
            if (it == dirs.end() || ++depth > 100) {
                v.warnings_.push_back("catalog: '" + e.name + "' has no parent folder");
                break;
            }
            path = nodes[it->second].entry.name + "/" + path;
            parent = nodes[it->second].entry.parent;
        }
        e.path = std::move(path);
        order.push_back(i);
    }
    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b) { return nodes[a].entry.path < nodes[b].entry.path; });
    for (const std::size_t i : order) {
        v.entries_.push_back(std::move(nodes[i].entry));
        v.forks_.push_back(nodes[i].forks);
    }
    if (v.entries_.empty()) v.warnings_.push_back("the volume is empty");
    return v;
}

bool HfsVolume::load_btree(std::uint32_t file_id, std::uint32_t size, const ExtentRecord& first,
                           std::vector<std::uint8_t>& out, std::string* error) const {
    if (size > kMaxBtreeBytes) {
        if (error) *error = "B-tree file too large";
        return false;
    }
    return read_extents(file_id, false, size, first, out, error);
}

bool HfsVolume::read_extents(std::uint32_t file_id, bool resource, std::uint32_t size, const ExtentRecord& first,
                             std::vector<std::uint8_t>& out, std::string* error) const {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        out.clear();
        return false;
    };
    out.clear();
    if (size > kMaxForkBytes || size > std::uint64_t{num_blocks_} * block_size_) return fail("fork too large");
    out.reserve(size);
    std::uint64_t need = size;
    std::uint32_t blocks = 0;  // allocation blocks of the fork read so far: the overflow lookup key
    ExtentRecord rec = first;
    for (int records = 0; need > 0; ++records) {
        if (records > 0) {
            if (records > 4096 || blocks > 0xFFFF) return fail("fork has too many extents");
            const auto it = overflow_.find({file_id, static_cast<std::uint8_t>(resource ? 0xFF : 0),
                                            static_cast<std::uint16_t>(blocks)});
            if (it == overflow_.end()) return fail("fork's extents are missing");
            rec = it->second;
        }
        for (const Extent& e : rec) {
            if (need == 0 || e.count == 0) break;
            if (static_cast<std::uint32_t>(e.start) + e.count > num_blocks_)
                return fail("fork lies outside the volume");
            const std::uint64_t len = std::min<std::uint64_t>(need, std::uint64_t{e.count} * block_size_);
            const std::size_t at = out.size();
            out.resize(at + static_cast<std::size_t>(len));
            if (!src_->read(base_ + alloc_start_ + std::uint64_t{e.start} * block_size_, out.data() + at,
                            static_cast<std::size_t>(len)))
                return fail("image is truncated");
            need -= len;
            blocks += e.count;
        }
    }
    return true;
}

const HfsEntry* HfsVolume::find(std::string_view path) const {
    for (const HfsEntry& e : entries_) {
        if (ieq(e.path, path)) return &e;
    }
    return nullptr;
}

bool HfsVolume::read_fork(const HfsEntry& entry, bool resource, std::vector<std::uint8_t>& out,
                          std::string* error) const {
    if (&entry < entries_.data() || &entry >= entries_.data() + entries_.size() || entry.directory) {
        if (error) *error = "not a file of this volume";
        out.clear();
        return false;
    }
    const Forks& f = forks_[static_cast<std::size_t>(&entry - entries_.data())];
    return read_extents(entry.id, resource, resource ? entry.rsrc_size : entry.data_size,
                        resource ? f.rsrc : f.data, out, error);
}

}  // namespace vette::assets
