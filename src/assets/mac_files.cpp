#include "assets/mac_files.h"

#include <algorithm>
#include <cstring>
#include <system_error>

namespace vette::assets {

namespace fs = std::filesystem;

namespace {

// Big-endian reads (the caller checks the bounds).
std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }
std::uint32_t be32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}

constexpr std::uint64_t kMaxBinHexBytes = 64u << 20;
constexpr int kMaxDepth = 8;
constexpr std::size_t kMaxDirEntries = 4096;
constexpr std::size_t kMaxEntries = 50000;

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

bool iends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && ieq(s.substr(s.size() - suffix.size()), suffix);
}

std::string utf8(const fs::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

std::string join(const std::string& dir, const std::string& name) { return dir.empty() ? name : dir + "/" + name; }

// --- Single-file containers ------------------------------------------------------------------------

struct Decoded {
    std::string kind;  // "MacBinary II", "AppleSingle", ...
    std::string name;  // Mac name, UTF-8 ('/' as ':'); empty if the container doesn't keep it
    std::uint32_t type = 0, creator = 0;
    MacFiles::Fork data, rsrc;
};

MacFiles::Fork range(const std::shared_ptr<const ByteSource>& src, std::uint64_t offset, std::uint64_t size) {
    MacFiles::Fork f;
    f.src = src;
    f.offset = offset;
    f.size = size;
    return f;
}

std::string mac_name(const std::uint8_t* p, std::size_t n) {
    std::string s = mac_roman_to_utf8(std::span<const std::uint8_t>(p, n));
    std::replace(s.begin(), s.end(), '/', ':');
    return s;
}

std::uint64_t round128(std::uint64_t n) { return (n + 127) & ~std::uint64_t{127}; }

// MacBinary I/II/III: a 128-byte header (name, type, creator, fork lengths; II and III add a CRC),
// then the data and resource forks, each padded to 128 bytes.
std::optional<Decoded> decode_macbinary(const std::shared_ptr<const ByteSource>& src) {
    std::uint8_t h[128];
    if (!src->read(0, h, sizeof h) || h[0] != 0 || h[74] != 0 || h[1] == 0 || h[1] > 63) return std::nullopt;
    const bool crc_ok = crc16_xmodem(std::span<const std::uint8_t>(h, 124)) == be16(h + 124);
    if (!crc_ok) {  // MacBinary I has no CRC; its unused bytes are zero
        if (h[82] != 0) return std::nullopt;
        for (int i = 99; i <= 125; ++i) {
            if (h[i] != 0) return std::nullopt;
        }
    }
    for (int i = 0; i < h[1]; ++i) {
        if (h[2 + i] == 0) return std::nullopt;
    }
    const std::uint64_t data_len = be32(h + 83), rsrc_len = be32(h + 87);
    if (data_len > 0x7FFFFFFF || rsrc_len > 0x7FFFFFFF || data_len + rsrc_len == 0) return std::nullopt;
    const std::uint64_t data_off = 128 + (crc_ok ? round128(be16(h + 120)) : 0);
    const std::uint64_t rsrc_off = data_off + round128(data_len);
    if (data_off + data_len > src->size() || (rsrc_len && rsrc_off + rsrc_len > src->size())) return std::nullopt;
    Decoded d;
    d.kind = std::memcmp(h + 102, "mBIN", 4) == 0 ? "MacBinary III" : crc_ok ? "MacBinary II" : "MacBinary";
    d.name = mac_name(h + 2, h[1]);
    d.type = be32(h + 65);
    d.creator = be32(h + 69);
    d.data = range(src, data_off, data_len);
    d.rsrc = range(src, rsrc_off, rsrc_len);
    return d;
}

// AppleSingle (0x00051600) and AppleDouble (0x00051607): a header and a table of entries; 1 = data
// fork, 2 = resource fork, 3 = real name, 9 = Finder info (type, creator, ...).
std::optional<Decoded> decode_apple_single(const std::shared_ptr<const ByteSource>& src, bool* is_double = nullptr) {
    std::uint8_t h[26];
    if (!src->read(0, h, sizeof h)) return std::nullopt;
    const std::uint32_t magic = be32(h), version = be32(h + 4);
    if ((magic != 0x00051600 && magic != 0x00051607) || (version != 0x00010000 && version != 0x00020000))
        return std::nullopt;
    const std::uint16_t n = be16(h + 24);
    if (n > 256) return std::nullopt;
    std::vector<std::uint8_t> table(12u * n);
    if (!src->read(26, table.data(), table.size())) return std::nullopt;
    Decoded d;
    d.kind = magic == 0x00051607 ? "AppleDouble" : "AppleSingle";
    if (is_double) *is_double = magic == 0x00051607;
    for (std::uint16_t i = 0; i < n; ++i) {
        const std::uint8_t* e = table.data() + 12 * i;
        const std::uint32_t id = be32(e), off = be32(e + 4), len = be32(e + 8);
        if (std::uint64_t{off} + len > src->size()) continue;
        if (id == 1) d.data = range(src, off, len);
        if (id == 2) d.rsrc = range(src, off, len);
        if (id == 3 && len > 0 && len < 256) {
            std::uint8_t name[256];
            if (src->read(off, name, len)) d.name = mac_name(name, len);
        }
        if (id == 9 && len >= 8) {
            std::uint8_t fi[8];
            if (src->read(off, fi, sizeof fi)) {
                d.type = be32(fi);
                d.creator = be32(fi + 4);
            }
        }
    }
    return d;
}

// BinHex 4.0 (.hqx): 7-bit text between colons, 6 bits per character, with 0x90 run-length coding;
// then a header (name, type, creator, fork lengths, CRC) and each fork followed by its CRC.
std::optional<Decoded> decode_binhex(const ByteSource& src, std::string* problem) {
    static constexpr std::string_view kMarker = "(This file must be converted with BinHex";
    static constexpr std::string_view kAlphabet =
        "!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr";
    const std::vector<std::uint8_t> text = src.read_all(0, src.size(), kMaxBinHexBytes);
    const std::string_view s(reinterpret_cast<const char*>(text.data()), text.size());
    const std::size_t marker = s.find(kMarker);
    if (marker == std::string_view::npos) return std::nullopt;
    std::size_t start = s.find(':', marker + kMarker.size());
    if (start == std::string_view::npos) return std::nullopt;

    std::int8_t value[256];
    std::memset(value, -1, sizeof value);
    for (std::size_t i = 0; i < kAlphabet.size(); ++i)
        value[static_cast<std::uint8_t>(kAlphabet[i])] = static_cast<std::int8_t>(i);
    std::vector<std::uint8_t> packed;  // 6-bit decoded, still run-length coded
    std::uint32_t bits = 0;
    int nbits = 0;
    bool closed = false;
    for (std::size_t i = start + 1; i < s.size(); ++i) {
        const char c = s[i];
        if (c == ':') {
            closed = true;
            break;
        }
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        const int v = value[static_cast<std::uint8_t>(c)];
        if (v < 0) {
            if (problem) *problem = "BinHex: bad character";
            return std::nullopt;
        }
        bits = (bits << 6 | static_cast<std::uint32_t>(v)) & 0xFFFFFF;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            packed.push_back(static_cast<std::uint8_t>(bits >> nbits));
        }
    }
    if (!closed) {
        if (problem) *problem = "BinHex: truncated";
        return std::nullopt;
    }
    std::vector<std::uint8_t> b;  // 0x90 n: repeat the previous byte n-1 more times; 0x90 0: a literal 0x90
    b.reserve(packed.size());
    for (std::size_t i = 0; i < packed.size(); ++i) {
        if (packed[i] != 0x90) {
            b.push_back(packed[i]);
        } else if (++i < packed.size()) {
            if (packed[i] == 0) {
                b.push_back(0x90);
            } else if (!b.empty()) {
                if (b.size() + packed[i] > kMaxBinHexBytes * 2) break;
                b.insert(b.end(), packed[i] - 1u, b.back());
            }
        }
    }
    auto fail = [&](const char* why) -> std::optional<Decoded> {
        if (problem) *problem = why;
        return std::nullopt;
    };
    if (b.empty() || b[0] < 1 || b[0] > 63) return fail("BinHex: bad header");
    const std::size_t n = b[0];
    const std::size_t head = 22 + n;
    if (b.size() < head) return fail("BinHex: truncated header");
    if (crc16_xmodem(std::span<const std::uint8_t>(b.data(), head - 2)) != be16(b.data() + head - 2))
        return fail("BinHex: header CRC mismatch");
    const std::uint64_t data_len = be32(b.data() + 12 + n), rsrc_len = be32(b.data() + 16 + n);
    if (head + data_len + 2 + rsrc_len + 2 > b.size()) return fail("BinHex: truncated forks");
    Decoded d;
    d.kind = "BinHex 4.0";
    d.name = mac_name(b.data() + 1, n);
    d.type = be32(b.data() + 2 + n);
    d.creator = be32(b.data() + 6 + n);
    const std::size_t rsrc_at = head + static_cast<std::size_t>(data_len) + 2;
    auto fork_ok = [&](std::size_t at, std::uint64_t len) {
        return crc16_xmodem(std::span<const std::uint8_t>(b.data() + at, static_cast<std::size_t>(len))) ==
               be16(b.data() + at + len);
    };
    if (!fork_ok(head, data_len) || !fork_ok(rsrc_at, rsrc_len)) {
        if (problem) *problem = "BinHex: fork CRC mismatch (used anyway)";
    }
    auto buf = ByteSource::from_memory(std::move(b));
    d.data = range(buf, head, data_len);
    d.rsrc = range(buf, rsrc_at, rsrc_len);
    return d;
}

// BinHex files are text (possibly after mail headers); only those are read in full.
bool looks_like_text(const ByteSource& src) {
    std::uint8_t head[64];
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(sizeof head, src.size()));
    if (n == 0 || !src.read(0, head, n)) return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (head[i] < 0x20 && head[i] != '\r' && head[i] != '\n' && head[i] != '\t') return false;
        if (head[i] >= 0x7F) return false;
    }
    return true;
}

bool is_stuffit(std::uint32_t type) {
    return type == fourcc("SIT!") || type == fourcc("SITD") || type == fourcc("SIT5");
}

}  // namespace

// --- Small helpers -----------------------------------------------------------------------------------

std::string fourcc_string(std::uint32_t code) {
    const std::uint8_t b[4] = {static_cast<std::uint8_t>(code >> 24), static_cast<std::uint8_t>(code >> 16),
                               static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(code)};
    return mac_roman_to_utf8(std::span<const std::uint8_t>(b, 4));
}

std::optional<std::uint32_t> fourcc_from_string(std::string_view text) {
    if (text.size() != 4) return std::nullopt;
    std::uint32_t code = 0;
    for (const char c : text) code = code << 8 | static_cast<std::uint8_t>(c);
    return code;
}

std::uint16_t crc16_xmodem(std::span<const std::uint8_t> bytes, std::uint16_t crc) {
    for (const std::uint8_t b : bytes) {
        crc = static_cast<std::uint16_t>(crc ^ b << 8);
        for (int i = 0; i < 8; ++i) crc = static_cast<std::uint16_t>(crc & 0x8000 ? crc << 1 ^ 0x1021 : crc << 1);
    }
    return crc;
}

// --- ResourceFork ------------------------------------------------------------------------------------

bool ResourceFork::parse(std::vector<std::uint8_t> fork, std::string* error) {
    *this = ResourceFork();
    auto fail = [&](const char* why) {
        if (error) *error = why;
        *this = ResourceFork();
        return false;
    };
    const std::size_t size = fork.size();
    if (size < 16) return fail(size == 0 ? "empty resource fork" : "resource fork too short");
    const std::uint8_t* p = fork.data();
    const std::uint64_t data_off = be32(p), map_off = be32(p + 4), data_len = be32(p + 8), map_len = be32(p + 12);
    if (map_len < 30 || map_off + map_len > size) return fail("resource map outside the fork");
    if (data_off > size) return fail("resource data outside the fork");
    const std::uint64_t data_end = std::min<std::uint64_t>(size, data_off + data_len);
    const std::uint8_t* m = p + map_off;
    const std::uint32_t type_list = be16(m + 24), name_list = be16(m + 26);
    if (type_list + 2u > map_len) return fail("resource type list outside the map");
    const std::uint32_t types = (be16(m + type_list) + 1u) & 0xFFFF;  // stored as count - 1
    if (type_list + 2u + 8u * types > map_len) return fail("resource type list truncated");
    for (std::uint32_t t = 0; t < types; ++t) {
        const std::uint8_t* te = m + type_list + 2 + 8 * t;
        const std::uint32_t type = be32(te), count = be16(te + 4) + 1u;
        const std::uint32_t refs = type_list + be16(te + 6);
        if (refs + 12u * count > map_len) {
            skipped_ += static_cast<int>(count);
            continue;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint8_t* re = m + refs + 12 * i;
            Resource r;
            r.type = type;
            r.id = static_cast<std::int16_t>(be16(re));
            const std::uint32_t name_off = be16(re + 2);
            r.attributes = re[4];
            const std::uint64_t at = data_off + (be32(re + 4) & 0xFFFFFF);
            if (name_off != 0xFFFF) {
                const std::uint64_t n = std::uint64_t{name_list} + name_off;
                if (n < map_len && n + 1 + m[n] <= map_len) {
                    r.name = mac_roman_to_utf8(std::span<const std::uint8_t>(m + n + 1, m[n]));
                    r.has_name = true;
                }
            }
            if (at + 4 > data_end || at + 4 + be32(p + at) > data_end) {
                ++skipped_;
                continue;
            }
            r.offset = static_cast<std::uint32_t>(at + 4);
            r.size = be32(p + at);
            index_.emplace(std::make_pair(r.type, r.id), resources_.size());
            resources_.push_back(std::move(r));
        }
    }
    bytes_ = std::move(fork);
    return true;
}

std::vector<std::uint32_t> ResourceFork::types() const {
    std::vector<std::uint32_t> out;
    for (const Resource& r : resources_) {
        if (std::find(out.begin(), out.end(), r.type) == out.end()) out.push_back(r.type);
    }
    return out;
}

std::vector<const Resource*> ResourceFork::of_type(std::uint32_t type) const {
    std::vector<const Resource*> out;
    for (const Resource& r : resources_) {
        if (r.type == type) out.push_back(&r);
    }
    return out;
}

const Resource* ResourceFork::find(std::uint32_t type, std::int16_t id) const {
    const auto it = index_.find({type, id});
    return it == index_.end() ? nullptr : &resources_[it->second];
}

const Resource* ResourceFork::find(std::uint32_t type, std::string_view name) const {
    for (const Resource& r : resources_) {
        if (r.type == type && r.has_name && ieq(r.name, name)) return &r;
    }
    return nullptr;
}

std::span<const std::uint8_t> ResourceFork::data(const Resource& r) const {
    if (std::uint64_t{r.offset} + r.size > bytes_.size()) return {};
    return std::span<const std::uint8_t>(bytes_.data() + r.offset, r.size);
}

std::span<const std::uint8_t> ResourceFork::get(std::uint32_t type, std::int16_t id) const {
    const Resource* r = find(type, id);
    return r ? data(*r) : std::span<const std::uint8_t>();
}

// --- The folder scanner ------------------------------------------------------------------------------

struct MacScanner {
    MacFiles& out;
    fs::path root;
    std::size_t seen = 0;  // directory entries looked at, against pointing this at a whole disk

    void add(MacFile f, MacFiles::Fork data, MacFiles::Fork rsrc) {
        data.resource = false;
        rsrc.resource = true;
        f.data_size = data.volume >= 0 ? f.data_size : data.size;
        f.rsrc_size = rsrc.volume >= 0 ? f.rsrc_size : rsrc.size;
        out.files_.push_back(std::move(f));
        out.forks_.emplace_back(std::move(data), std::move(rsrc));
    }

    // Opens an HFS image and lists its files. False if `src` holds no HFS volume.
    bool mount(const std::shared_ptr<const ByteSource>& src, const std::string& label) {
        std::string why;
        if (!locate_hfs(*src, &why)) {
            if (!why.empty()) out.notes_.push_back(label + ": " + why);
            return !why.empty();  // an HFS+ image is still consumed (noted, not listed as a file)
        }
        auto vol = HfsVolume::open(src, &why);
        if (!vol) {
            out.notes_.push_back(label + ": damaged HFS volume (" + why + ")");
            return true;
        }
        const int index = static_cast<int>(out.volumes_.size());
        std::size_t files = 0;
        for (std::size_t i = 0; i < vol->entries().size(); ++i) {
            const HfsEntry& e = vol->entries()[i];
            if (e.directory) continue;
            MacFile f;
            f.path = vol->name() + "/" + e.path;
            f.name = e.name;
            f.type = e.type;
            f.creator = e.creator;
            f.data_size = e.data_size;
            f.rsrc_size = e.rsrc_size;
            f.source = "HFS image " + label;
            MacFiles::Fork data, rsrc;
            data.volume = rsrc.volume = index;
            data.entry = rsrc.entry = i;
            add(std::move(f), data, rsrc);
            ++files;
        }
        out.notes_.push_back(label + ": HFS volume '" + vol->name() + "' (" + vol->container() + "), " +
                             std::to_string(files) + " files");
        for (const std::string& w : vol->warnings()) out.notes_.push_back(label + ": " + w);
        out.volumes_.push_back(std::move(*vol));
        return true;
    }

    // A single-file container: a disk image inside is mounted, anything else listed under its Mac name.
    void add_decoded(Decoded d, const std::string& rel_dir, const std::string& host_name, const std::string& label) {
        if (d.data.size >= 1024 + 162 &&
            mount(ByteSource::slice(d.data.src, d.data.offset, d.data.size), label + " (" + d.kind + ")"))
            return;
        if (is_stuffit(d.type)) {
            out.notes_.push_back(label + ": StuffIt archive (not supported; expand it first)");
            return;
        }
        MacFile f;
        f.name = d.name.empty() ? host_name : d.name;
        f.path = join(rel_dir, f.name);
        f.type = d.type;
        f.creator = d.creator;
        f.source = d.kind;
        add(std::move(f), d.data, d.rsrc);
    }

    // What a loose file is. True if it was a disk image or a container (handled); false for plain
    // bytes, which the caller pairs with resource fork files.
    bool classify(const fs::path& path, const std::string& rel_dir, const std::string& host_name) {
        const auto src = ByteSource::from_file(path);
        const std::string label = join(rel_dir, host_name);
        if (!src) {
            out.notes_.push_back(label + ": can't be read");
            return true;
        }
        if (src->size() >= 1024 + 162 && mount(src, label)) return true;
        std::uint8_t head[8] = {};
        src->read(0, head, std::min<std::uint64_t>(sizeof head, src->size()));
        if (std::memcmp(head, "SIT!", 4) == 0 || std::memcmp(head, "StuffIt", 7) == 0) {
            out.notes_.push_back(label + ": StuffIt archive (not supported; expand it first)");
            return true;
        }
        if (std::memcmp(head, "PK\x03\x04", 4) == 0) {
            out.notes_.push_back(label + ": zip archive (unzip it first)");
            return true;
        }
        if (src->size() >= 512) {
            std::uint8_t koly[4];
            if (src->read(src->size() - 512, koly, 4) && std::memcmp(koly, "koly", 4) == 0) {
                out.notes_.push_back(label + ": compressed .dmg (not supported; convert it to a raw image)");
                return true;
            }
        }
        bool is_double = false;
        if (auto d = decode_apple_single(src, &is_double)) {
            if (is_double) return false;  // a companion: the caller pairs it by name
            add_decoded(std::move(*d), rel_dir, host_name, label);
            return true;
        }
        if (auto d = decode_macbinary(src)) {
            add_decoded(std::move(*d), rel_dir, host_name, label);
            return true;
        }
        if (src->size() < kMaxBinHexBytes && looks_like_text(*src)) {
            std::string problem;
            if (auto d = decode_binhex(*src, &problem)) {
                if (!problem.empty()) out.notes_.push_back(label + ": " + problem);
                add_decoded(std::move(*d), rel_dir, host_name, label);
                return true;
            }
            if (!problem.empty()) {
                out.notes_.push_back(label + ": " + problem);
                return true;
            }
        }
        return false;
    }

    struct Group {
        fs::path plain, data_dump, rsrc_dump, apple_double;
    };

    static void add_double(std::map<std::string, Group>& groups, const std::string& base, const fs::path& p) {
        auto src = ByteSource::from_file(p);
        bool is_double = false;
        if (src && decode_apple_single(src, &is_double) && is_double) groups[base].apple_double = p;
    }

    // One Mac file from the host files that hold its forks.
    void add_group(const std::string& name, const Group& g, const std::string& rel) {
        MacFile f;
        f.name = name;
        f.path = join(rel, name);
        MacFiles::Fork data, rsrc;
        std::vector<std::string> kinds;
        const fs::path& data_path = !g.plain.empty() ? g.plain : g.data_dump;
        if (!data_path.empty()) {
            if (auto src = ByteSource::from_file(data_path)) data = range(src, 0, src->size());
        }
        if (!g.rsrc_dump.empty()) {
            if (auto src = ByteSource::from_file(g.rsrc_dump)) rsrc = range(src, 0, src->size());
            kinds.push_back("raw resource fork");
        }
        if (auto src = g.apple_double.empty() ? nullptr : ByteSource::from_file(g.apple_double)) {
            if (auto d = decode_apple_single(src)) {
                if (!rsrc.src) rsrc = d->rsrc;
                if (!data.src && d->data.src) data = d->data;
                f.type = d->type;
                f.creator = d->creator;
                kinds.push_back("AppleDouble");
            }
        }
#ifdef __APPLE__
        if (!rsrc.src && !g.plain.empty()) {  // the file's own resource fork, copied by the Finder
            if (auto src = ByteSource::from_file(g.plain / "..namedfork" / "rsrc"); src && src->size() > 0) {
                rsrc = range(src, 0, src->size());
                kinds.push_back("resource fork");
            }
        }
#endif
        if (kinds.empty()) kinds.push_back("plain file");
        for (std::size_t i = 0; i < kinds.size(); ++i) f.source += (i ? " + " : "") + kinds[i];
        add(std::move(f), data, rsrc);
    }

    void scan_dir(const fs::path& dir, const std::string& rel, int depth) {
        std::error_code ec;
        std::vector<fs::path> files, dirs;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (files.size() + dirs.size() >= kMaxDirEntries || ++seen > kMaxEntries) {
                out.notes_.push_back((rel.empty() ? "." : rel) + ": too many files, the rest are skipped");
                break;
            }
            std::error_code e2;
            if (it->is_directory(e2)) {
                dirs.push_back(it->path());
            } else if (it->is_regular_file(e2)) {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end());
        std::sort(dirs.begin(), dirs.end());

        std::map<std::string, Group> groups;  // by Mac name
        std::vector<std::string> lower_names;
        for (const fs::path& p : files) {
            std::string n = utf8(p.filename());
            for (char& c : n) c = lower(c);
            lower_names.push_back(std::move(n));
        }
        auto has_file = [&](std::string n) {
            for (char& c : n) c = lower(c);
            return std::find(lower_names.begin(), lower_names.end(), n) != lower_names.end();
        };
        for (const fs::path& p : files) {
            const std::string name = utf8(p.filename());
            if (name == ".DS_Store" || name == "Icon\r") continue;
            const std::string stem = name.size() > 5 ? name.substr(0, name.size() - 5) : std::string();
            if (name.size() > 2 && name.compare(0, 2, "._") == 0) {
                add_double(groups, name.substr(2), p);
            } else if (name.size() > 1 && name[0] == '%') {
                add_double(groups, name.substr(1), p);
            } else if (!stem.empty() && iends_with(name, ".rsrc")) {
                groups[stem].rsrc_dump = p;
            } else if (!stem.empty() && iends_with(name, ".data") && has_file(stem + ".rsrc")) {
                groups[stem].data_dump = p;  // "X.data" is X's data fork only beside "X.rsrc" ("VETTE!.Data" is a name)
            } else if (!classify(p, rel, name)) {
                if (auto src = ByteSource::from_file(p); src && decode_apple_single(src)) {
                    groups[name].apple_double = p;  // an AppleDouble header under its own name
                } else {
                    groups[name].plain = p;
                }
            }
        }
        // AppleDouble headers kept apart: netatalk's .AppleDouble folder, and the __MACOSX tree macOS
        // writes into zip files (which mirrors the folders from the zip's root).
        const fs::path zip_side = root / "__MACOSX" / fs::path(std::u8string(rel.begin(), rel.end()));
        for (const fs::path& d : {dir / ".AppleDouble", zip_side}) {
            if (!fs::is_directory(d, ec)) continue;
            for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
                std::string name = utf8(it->path().filename());
                if (name.size() > 2 && name.compare(0, 2, "._") == 0) name = name.substr(2);
                add_double(groups, name, it->path());
            }
        }

        for (const auto& [name, g] : groups) add_group(name, g, rel);

        if (depth >= kMaxDepth) return;
        for (const fs::path& d : dirs) {
            const std::string name = utf8(d.filename());
            if (name == ".AppleDouble" || name == "__MACOSX" || (name.size() > 1 && name[0] == '.')) continue;
            scan_dir(d, join(rel, name), depth + 1);
        }
    }
};

// --- MacFiles ----------------------------------------------------------------------------------------

MacFiles MacFiles::open(const fs::path& location) {
    MacFiles files;
    std::error_code ec;
    if (fs::is_directory(location, ec)) {
        MacScanner scan{files, location};
        scan.scan_dir(location, "", 0);
    } else if (fs::is_regular_file(location, ec)) {
        MacScanner scan{files, location.parent_path()};
        const std::string name = utf8(location.filename());
        if (!scan.classify(location, "", name)) {
            MacScanner::Group g;
            std::string mac = name;
            const auto src = ByteSource::from_file(location);
            bool is_double = false;
            if (src && decode_apple_single(src, &is_double) && is_double) {
                if (mac.size() > 2 && mac.compare(0, 2, "._") == 0) mac = mac.substr(2);
                if (mac.size() > 1 && mac[0] == '%') mac = mac.substr(1);
                g.apple_double = location;
            } else if (iends_with(name, ".rsrc") && name.size() > 5) {
                mac = name.substr(0, name.size() - 5);
                g.rsrc_dump = location;
            } else {
                g.plain = location;
            }
            scan.add_group(mac, g, "");
        }
    } else {
        files.notes_.push_back(utf8(location) + ": not found");
    }

    std::vector<std::size_t> order(files.files_.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return files.files_[a].path < files.files_[b].path; });
    std::vector<MacFile> sorted;
    std::vector<std::pair<Fork, Fork>> sorted_forks;
    for (const std::size_t i : order) {
        sorted.push_back(std::move(files.files_[i]));
        sorted_forks.push_back(std::move(files.forks_[i]));
    }
    files.files_ = std::move(sorted);
    files.forks_ = std::move(sorted_forks);
    return files;
}

std::vector<const MacFile*> MacFiles::find_all(std::string_view query) const {
    while (!query.empty() && query.front() == '/') query.remove_prefix(1);
    std::vector<const MacFile*> exact, tail;
    for (const MacFile& f : files_) {
        if (ieq(f.path, query)) {
            exact.push_back(&f);
        } else if (f.path.size() > query.size() && f.path[f.path.size() - query.size() - 1] == '/' &&
                   iends_with(f.path, query)) {
            tail.push_back(&f);
        }
    }
    exact.insert(exact.end(), tail.begin(), tail.end());
    return exact;
}

const MacFile* MacFiles::find(std::string_view query) const {
    const auto all = find_all(query);
    return all.empty() ? nullptr : all.front();
}

const MacFile* MacFiles::find_type(std::uint32_t type, std::uint32_t creator) const {
    for (const MacFile& f : files_) {
        if (f.type == type && (creator == 0 || f.creator == creator)) return &f;
    }
    return nullptr;
}

std::vector<std::uint8_t> MacFiles::read(const Fork& fork) const {
    if (fork.volume >= 0 && static_cast<std::size_t>(fork.volume) < volumes_.size()) {
        const HfsVolume& v = volumes_[static_cast<std::size_t>(fork.volume)];
        std::vector<std::uint8_t> out;
        if (fork.entry < v.entries().size() && v.read_fork(v.entries()[fork.entry], fork.resource, out)) return out;
        return {};
    }
    if (!fork.src) return {};
    return fork.src->read_all(fork.offset, fork.size);
}

std::vector<std::uint8_t> MacFiles::data_fork(const MacFile& file) const {
    if (&file < files_.data() || &file >= files_.data() + files_.size()) return {};
    return read(forks_[static_cast<std::size_t>(&file - files_.data())].first);
}

std::vector<std::uint8_t> MacFiles::resource_fork(const MacFile& file) const {
    if (&file < files_.data() || &file >= files_.data() + files_.size()) return {};
    return read(forks_[static_cast<std::size_t>(&file - files_.data())].second);
}

std::optional<ResourceFork> MacFiles::resources(const MacFile& file) const {
    ResourceFork fork;
    if (!fork.parse(resource_fork(file))) return std::nullopt;
    return fork;
}

std::optional<ResourceFork> MacFiles::resources(std::string_view path_or_name) const {
    for (const MacFile* f : find_all(path_or_name)) {
        if (f->rsrc_size == 0) continue;
        if (auto fork = resources(*f)) return fork;
    }
    return std::nullopt;
}

}  // namespace vette::assets
