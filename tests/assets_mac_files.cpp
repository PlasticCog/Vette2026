// The Mac file readers: resource forks, HFS images (bare, DiskCopy 4.2, Apple partition map), the
// single-file containers and the folder scanner, on synthetic data built here; damaged input must fail
// cleanly. Then the player's own files, when present (Game/Mac, or Vette_Mac_EN in the repo root).

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "assets/hfs.h"
#include "assets/mac_files.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace vette::assets;
using Bytes = std::vector<std::uint8_t>;

namespace {

void put16(Bytes& b, std::size_t at, std::uint32_t v) {
    if (b.size() < at + 2) b.resize(at + 2);
    b[at] = static_cast<std::uint8_t>(v >> 8);
    b[at + 1] = static_cast<std::uint8_t>(v);
}
void put32(Bytes& b, std::size_t at, std::uint32_t v) {
    put16(b, at, v >> 16);
    put16(b, at + 2, v & 0xFFFF);
}
void put(Bytes& b, std::size_t at, const Bytes& src) {
    if (b.size() < at + src.size()) b.resize(at + src.size());
    std::copy(src.begin(), src.end(), b.begin() + static_cast<std::ptrdiff_t>(at));
}
Bytes text(const std::string& s) { return Bytes(s.begin(), s.end()); }

struct Res {
    std::string type;
    std::int16_t id;
    std::string name;  // empty: none
    Bytes data;
};

// A resource fork as the Resource Manager writes it: header, data, map (type list, refs, names).
Bytes resource_fork(const std::vector<Res>& res) {
    Bytes data, names;
    std::vector<std::string> types;
    for (const Res& r : res) {
        if (std::find(types.begin(), types.end(), r.type) == types.end()) types.push_back(r.type);
    }
    Bytes map(28 + 2 + 8 * types.size());
    std::vector<Bytes> refs(types.size());
    for (const Res& r : res) {
        const std::size_t t = static_cast<std::size_t>(std::find(types.begin(), types.end(), r.type) - types.begin());
        Bytes ref(12);
        put16(ref, 0, static_cast<std::uint16_t>(r.id));
        if (r.name.empty()) {
            put16(ref, 2, 0xFFFF);
        } else {
            put16(ref, 2, static_cast<std::uint32_t>(names.size()));
            names.push_back(static_cast<std::uint8_t>(r.name.size()));
            names.insert(names.end(), r.name.begin(), r.name.end());
        }
        put32(ref, 4, static_cast<std::uint32_t>(data.size()));  // attributes 0
        Bytes len(4);
        put32(len, 0, static_cast<std::uint32_t>(r.data.size()));
        data.insert(data.end(), len.begin(), len.end());
        data.insert(data.end(), r.data.begin(), r.data.end());
        refs[t].insert(refs[t].end(), ref.begin(), ref.end());
    }
    put16(map, 24, 28);
    put16(map, 28, static_cast<std::uint32_t>(types.size() - 1));
    std::size_t ref_at = 2 + 8 * types.size();  // from the type list
    for (std::size_t t = 0; t < types.size(); ++t) {
        put(map, 30 + 8 * t, text(types[t]));
        put16(map, 30 + 8 * t + 4, static_cast<std::uint32_t>(refs[t].size() / 12 - 1));
        put16(map, 30 + 8 * t + 6, static_cast<std::uint32_t>(ref_at));
        ref_at += refs[t].size();
    }
    for (const Bytes& r : refs) map.insert(map.end(), r.begin(), r.end());
    put16(map, 26, static_cast<std::uint32_t>(map.size()));
    map.insert(map.end(), names.begin(), names.end());
    Bytes fork(256);  // header, then the Resource Manager's reserved bytes
    put32(fork, 0, 256);
    put32(fork, 4, static_cast<std::uint32_t>(256 + data.size()));
    put32(fork, 8, static_cast<std::uint32_t>(data.size()));
    put32(fork, 12, static_cast<std::uint32_t>(map.size()));
    for (int i = 0; i < 16; ++i) map[static_cast<std::size_t>(i)] = fork[static_cast<std::size_t>(i)];
    fork.insert(fork.end(), data.begin(), data.end());
    fork.insert(fork.end(), map.begin(), map.end());
    return fork;
}

const Bytes kForkA = resource_fork({{"PICT", 128, "Title", text("picture one")},
                                    {"PICT", 129, "", text("picture two")},
                                    {"INST", -5, "Engine", text("12345678abc")},
                                    {"vers", 1, "", text("v")}});

// --- A tiny HFS volume ------------------------------------------------------------------------------
// 512-byte allocation blocks from byte 2048. Blocks: 0-1 extents B-tree (header, leaf), 2-3 catalog
// B-tree (header, leaf), 4 "File A" data, 5-7 its resource fork, 10/12/14/16 "B:C"'s data fork in
// four pieces (the fourth in the extents overflow file).
constexpr std::size_t kBlock = 512, kAlloc = 2048, kBlocks = 24;
const Bytes kDataA = text("hello, data fork");
const std::size_t kSizeC = 4 * kBlock - 100;

Bytes node(std::int8_t kind, const std::vector<Bytes>& recs) {
    Bytes n(kBlock);
    n[8] = static_cast<std::uint8_t>(kind);
    put16(n, 10, static_cast<std::uint32_t>(recs.size()));
    std::size_t at = 14;
    for (std::size_t i = 0; i <= recs.size(); ++i) {
        put16(n, kBlock - 2 * (i + 1), static_cast<std::uint32_t>(at));
        if (i < recs.size()) {
            put(n, at, recs[i]);
            at += recs[i].size();
        }
    }
    return n;
}

Bytes header_node(std::uint32_t leaf_records) {
    Bytes h(106);
    put16(h, 0, leaf_records ? 1 : 0);              // depth
    put32(h, 2, leaf_records ? 1 : 0);              // root
    put32(h, 6, leaf_records);                      // leaf records
    put32(h, 10, leaf_records ? 1 : 0);             // first leaf
    put32(h, 14, leaf_records ? 1 : 0);             // last leaf
    put16(h, 18, kBlock);                           // node size
    put16(h, 20, 37);                               // max key length
    put32(h, 22, 2);                                // nodes
    return node(1, {h});
}

Bytes cat_key(std::uint32_t parent, const std::string& name) {
    Bytes k(7);
    k[0] = static_cast<std::uint8_t>(6 + name.size());
    put32(k, 2, parent);
    k[6] = static_cast<std::uint8_t>(name.size());
    k.insert(k.end(), name.begin(), name.end());
    if (k.size() % 2) k.push_back(0);
    return k;
}

Bytes dir_rec(std::uint32_t parent, const std::string& name, std::uint32_t id) {
    Bytes r = cat_key(parent, name);
    const std::size_t d = r.size();
    r.resize(d + 70);
    r[d] = 1;
    put32(r, d + 6, id);
    return r;
}

struct Ext {
    std::uint16_t start, count;
};

Bytes file_rec(std::uint32_t parent, const std::string& name, std::uint32_t id, std::uint32_t data_len,
               std::vector<Ext> data, std::uint32_t rsrc_len, std::vector<Ext> rsrc) {
    Bytes r = cat_key(parent, name);
    const std::size_t d = r.size();
    r.resize(d + 102);
    r[d] = 2;
    put(r, d + 4, text("TEXT"));
    put(r, d + 8, text("ttxt"));
    put32(r, d + 20, id);
    put32(r, d + 26, data_len);
    put32(r, d + 36, rsrc_len);
    for (std::size_t i = 0; i < data.size() && i < 3; ++i) {
        put16(r, d + 74 + 4 * i, data[i].start);
        put16(r, d + 76 + 4 * i, data[i].count);
    }
    for (std::size_t i = 0; i < rsrc.size() && i < 3; ++i) {
        put16(r, d + 86 + 4 * i, rsrc[i].start);
        put16(r, d + 88 + 4 * i, rsrc[i].count);
    }
    return r;
}

std::uint8_t pattern_c(std::size_t i) { return static_cast<std::uint8_t>(i * 7 + 3); }

Bytes hfs_volume() {
    Bytes img(kAlloc + kBlocks * kBlock);
    Bytes m(162);
    put16(m, 0, 0x4244);
    put16(m, 14, 3);  // volume bitmap
    put16(m, 18, kBlocks);
    put32(m, 20, kBlock);
    put16(m, 28, kAlloc / 512);
    const std::string vn = "Test Vol";
    m[36] = static_cast<std::uint8_t>(vn.size());
    put(m, 37, text(vn));
    put32(m, 130, 2 * kBlock);  // extents file: blocks 0-1
    put16(m, 134, 0);
    put16(m, 136, 2);
    put32(m, 146, 2 * kBlock);  // catalog file: blocks 2-3
    put16(m, 150, 2);
    put16(m, 152, 2);
    put(img, 1024, m);

    Bytes over(8 + 12);  // C's data fork continues at fork block 3 with volume block 16
    over[0] = 7;
    put32(over, 2, 18);
    put16(over, 6, 3);
    put16(over, 8, 16);
    put16(over, 10, 1);
    put(img, kAlloc + 0 * kBlock, header_node(1));
    put(img, kAlloc + 1 * kBlock, node(-1, {over}));

    put(img, kAlloc + 2 * kBlock, header_node(5));
    put(img, kAlloc + 3 * kBlock,
        node(-1, {dir_rec(1, vn, 2), dir_rec(2, "Folder", 16),
                  file_rec(2, "File A", 17, static_cast<std::uint32_t>(kDataA.size()), {{4, 1}},
                           static_cast<std::uint32_t>(kForkA.size()), {{5, 3}}),
                  file_rec(16, "B/C", 18, static_cast<std::uint32_t>(kSizeC), {{10, 1}, {12, 1}, {14, 1}}, 0, {}),
                  cat_key(2, "") /* a thread-like stub with no data: skipped */}));
    put(img, kAlloc + 4 * kBlock, kDataA);
    put(img, kAlloc + 5 * kBlock, kForkA);
    std::size_t i = 0;
    for (const std::size_t b : {10, 12, 14, 16}) {
        for (std::size_t j = 0; j < kBlock && i < kSizeC; ++j, ++i) img[kAlloc + b * kBlock + j] = pattern_c(i);
    }
    return img;
}

Bytes diskcopy(const Bytes& disk) {
    Bytes h(84);
    h[0] = 4;
    put(h, 1, text("Test"));
    put32(h, 64, static_cast<std::uint32_t>(disk.size()));
    h[80] = 1;
    h[81] = 0x22;
    put16(h, 82, 0x0100);
    h.insert(h.end(), disk.begin(), disk.end());
    return h;
}

Bytes partitioned(const Bytes& disk) {
    Bytes img(64 * 512);
    put16(img, 0, 0x4552);  // 'ER'
    put16(img, 2, 512);
    for (std::uint32_t i = 1; i <= 2; ++i) {
        const std::size_t e = 512 * i;
        put16(img, e, 0x504D);  // 'PM'
        put32(img, e + 4, 2);
        put32(img, e + 8, i == 1 ? 1 : 64);
        put32(img, e + 12, i == 1 ? 63 : static_cast<std::uint32_t>(disk.size() / 512));
        put(img, e + 48, text(i == 1 ? "Apple_partition_map" : "Apple_HFS"));
    }
    img.insert(img.end(), disk.begin(), disk.end());
    return img;
}

Bytes macbinary(const std::string& name, const std::string& type, const std::string& creator, const Bytes& data,
                const Bytes& rsrc) {
    Bytes h(128);
    h[1] = static_cast<std::uint8_t>(name.size());
    put(h, 2, text(name));
    put(h, 65, text(type));
    put(h, 69, text(creator));
    put32(h, 83, static_cast<std::uint32_t>(data.size()));
    put32(h, 87, static_cast<std::uint32_t>(rsrc.size()));
    h[122] = 129;  // MacBinary II
    h[123] = 129;
    put16(h, 124, crc16_xmodem(std::span<const std::uint8_t>(h.data(), 124)));
    Bytes out = h;
    out.insert(out.end(), data.begin(), data.end());
    out.resize((out.size() + 127) / 128 * 128);
    out.insert(out.end(), rsrc.begin(), rsrc.end());
    return out;
}

Bytes apple_single(bool is_double, const std::string& name, const Bytes& data, const Bytes& rsrc) {
    std::vector<std::pair<std::uint32_t, Bytes>> entries;
    if (!is_double && !data.empty()) entries.push_back({1, data});
    Bytes finder(32);
    put(finder, 0, text("DATA"));
    put(finder, 4, text("VETT"));
    entries.push_back({9, finder});
    if (!name.empty()) entries.push_back({3, text(name)});
    entries.push_back({2, rsrc});
    Bytes out(26 + 12 * entries.size());
    put32(out, 0, is_double ? 0x00051607 : 0x00051600);
    put32(out, 4, 0x00020000);
    put16(out, 24, static_cast<std::uint32_t>(entries.size()));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        put32(out, 26 + 12 * i, entries[i].first);
        put32(out, 30 + 12 * i, static_cast<std::uint32_t>(out.size()));
        put32(out, 34 + 12 * i, static_cast<std::uint32_t>(entries[i].second.size()));
        out.insert(out.end(), entries[i].second.begin(), entries[i].second.end());
    }
    return out;
}

std::string binhex(const std::string& name, const Bytes& data, const Bytes& rsrc) {
    Bytes b;
    b.push_back(static_cast<std::uint8_t>(name.size()));
    b.insert(b.end(), name.begin(), name.end());
    b.push_back(0);
    const Bytes t = text("INSTVETT");
    b.insert(b.end(), t.begin(), t.end());
    b.push_back(0);
    b.push_back(0);
    const std::size_t at = b.size();
    put32(b, at, static_cast<std::uint32_t>(data.size()));
    put32(b, at + 4, static_cast<std::uint32_t>(rsrc.size()));
    auto crc = [&](std::size_t from) { put16(b, b.size(), crc16_xmodem(std::span(b.data() + from, b.size() - from))); };
    crc(0);
    std::size_t from = b.size();
    b.insert(b.end(), data.begin(), data.end());
    crc(from);
    from = b.size();
    b.insert(b.end(), rsrc.begin(), rsrc.end());
    crc(from);
    Bytes rle;  // 0x90 must be escaped; also code one run to exercise the decoder
    for (const std::uint8_t c : b) {
        rle.push_back(c);
        if (c == 0x90) rle.push_back(0);
    }
    rle.push_back(0x41);
    rle.push_back(0x90);
    rle.push_back(5);  // trailing junk after the last CRC: ignored
    const std::string alphabet = "!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr";
    std::string out = "Subject: test\r\n\r\n(This file must be converted with BinHex 4.0)\r\n:";
    std::uint32_t bits = 0;
    int nbits = 0, col = 1;
    auto emit = [&](std::uint32_t v) {
        out += alphabet[v & 63];
        if (++col == 64) {
            out += "\r\n";
            col = 0;
        }
    };
    for (const std::uint8_t c : rle) {
        bits = bits << 8 | c;
        nbits += 8;
        while (nbits >= 6) {
            nbits -= 6;
            emit(bits >> nbits);
        }
    }
    if (nbits) emit(bits << (6 - nbits));
    return out + ":\r\n";
}

std::uint32_t lcg(std::uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
}

void write(const fs::path& p, const Bytes& b) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary)
        .write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

struct TempDir {
    fs::path path;
    explicit TempDir(const char* tag) {
        // Unique per run, so concurrent test runs (other build folders) don't share it.
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / (std::string("vette_mac_test_") + tag + "_" + std::to_string(stamp));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string env(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&value, &len, name) == 0 && value) out = value;
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}

fs::path repo_root() { return fs::path(__FILE__).parent_path().parent_path(); }

}  // namespace

// --- Resource forks ---------------------------------------------------------------------------------

TEST(assets_mac_resource_fork) {
    ResourceFork fork;
    std::string error;
    CHECK(fork.parse(kForkA, &error));
    CHECK_EQ(fork.resources().size(), std::size_t{4});
    CHECK_EQ(fork.skipped(), 0);
    CHECK_EQ(fork.types().size(), std::size_t{3});
    CHECK_EQ(fork.of_type(fourcc("PICT")).size(), std::size_t{2});
    const Resource* r = fork.find(fourcc("PICT"), 129);
    CHECK(r && !r->has_name);
    const auto d = fork.get(fourcc("PICT"), 128);
    CHECK_EQ(std::string(d.begin(), d.end()), std::string("picture one"));
    r = fork.find(fourcc("INST"), "engine");  // case ignored, as GetNamedResource
    CHECK(r && r->id == -5 && r->name == "Engine");
    CHECK(fork.find(fourcc("INST"), "Engines") == nullptr);
    CHECK(fork.get(fourcc("snd "), 1).empty());
    CHECK_EQ(fourcc_string(fourcc("snd ")), std::string("snd "));
    CHECK(fourcc_from_string("PICT") == fourcc("PICT"));
    CHECK_EQ(crc16_xmodem(text("123456789")), std::uint16_t{0x31C3});  // the CRC-16/XMODEM check value
}

TEST(assets_mac_resource_fork_damaged) {
    ResourceFork fork;
    CHECK(!fork.parse({}));
    CHECK(!fork.parse(Bytes(15, 0)));
    CHECK(!fork.parse(Bytes(kForkA.begin(), kForkA.begin() + 300)));  // map cut off
    Bytes bad = kForkA;
    put32(bad, 8, 2);  // data length too short: every resource's data now lies outside
    CHECK(fork.parse(bad));
    CHECK(fork.empty());
    CHECK_EQ(fork.skipped(), 4);
    // Random damage: never crashes, never returns a span outside the bytes.
    std::uint32_t seed = 1;
    for (int round = 0; round < 3000; ++round) {
        Bytes b = kForkA;
        const int hits = 1 + static_cast<int>(lcg(seed) % 6);
        for (int i = 0; i < hits; ++i) b[lcg(seed) % b.size()] = static_cast<std::uint8_t>(lcg(seed));
        if (round % 7 == 0) b.resize(lcg(seed) % b.size());
        if (fork.parse(b)) {
            for (const Resource& res : fork.resources()) {
                const auto s = fork.data(res);
                CHECK(s.size() == res.size && res.offset + res.size <= fork.bytes().size());
            }
        }
    }
}

// --- HFS --------------------------------------------------------------------------------------------

namespace {

void check_volume(const std::shared_ptr<const ByteSource>& src, const char* container) {
    std::string error;
    const auto v = HfsVolume::open(src, &error);
    CHECK(v.has_value());
    if (!v) {
        std::printf("  %s: %s\n", container, error.c_str());
        return;
    }
    CHECK_EQ(v->name(), std::string("Test Vol"));
    CHECK_EQ(v->container(), std::string(container));
    CHECK_EQ(v->entries().size(), std::size_t{3});
    const HfsEntry* a = v->find("file a");
    const HfsEntry* c = v->find("Folder/B:C");  // '/' in a Mac name reads as ':'
    CHECK(a && c && v->find("Folder") && v->find("Folder")->directory);
    if (!a || !c) return;
    CHECK(a->type == fourcc("TEXT") && a->creator == fourcc("ttxt"));
    Bytes out;
    CHECK(v->read_fork(*a, false, out, &error));
    CHECK(out == kDataA);
    CHECK(v->read_fork(*a, true, out, &error));
    CHECK(out == kForkA);
    CHECK(v->read_fork(*c, false, out, &error));  // three extents, then one from the overflow file
    CHECK_EQ(out.size(), kSizeC);
    bool same = out.size() == kSizeC;
    for (std::size_t i = 0; same && i < out.size(); ++i) same = out[i] == pattern_c(i);
    CHECK(same);
    CHECK(v->read_fork(*c, true, out, &error) && out.empty());
}

}  // namespace

TEST(assets_mac_hfs_volume) {
    const Bytes vol = hfs_volume();
    check_volume(ByteSource::from_memory(vol), "bare volume");
    check_volume(ByteSource::from_memory(diskcopy(vol)), "DiskCopy 4.2");
    check_volume(ByteSource::from_memory(partitioned(vol)), "Apple partition map");

    std::string why;
    CHECK(!locate_hfs(*ByteSource::from_memory(Bytes(4096, 0)), &why) && why.empty());
    Bytes plus = vol;
    put16(plus, 1024, 0x482B);  // 'H+'
    CHECK(!locate_hfs(*ByteSource::from_memory(plus), &why) && why.find("HFS+") != std::string::npos);

    // A fork whose overflow record is missing fails cleanly.
    Bytes lost = vol;
    put16(lost, kAlloc + kBlock + 14 + 6, 2);  // overflow key now says fork block 2
    const auto v = HfsVolume::open(ByteSource::from_memory(lost));
    Bytes out;
    std::string error;
    CHECK(v && v->find("Folder/B:C") && !v->read_fork(*v->find("Folder/B:C"), false, out, &error));
    CHECK(out.empty() && !error.empty());

    CHECK_EQ(mac_roman_to_utf8(std::string_view("Icon\r")), std::string("Icon\xE2\x90\x8D"));
    CHECK_EQ(mac_roman_to_utf8(std::string_view("VETTE!\xAA")), std::string("VETTE!\xE2\x84\xA2"));
}

TEST(assets_mac_hfs_damaged) {
    const Bytes vol = hfs_volume();
    std::uint32_t seed = 7;
    int opened = 0;
    for (int round = 0; round < 3000; ++round) {
        Bytes b = vol;
        const int hits = 1 + static_cast<int>(lcg(seed) % 8);
        for (int i = 0; i < hits; ++i) {
            // Mostly the metadata (header, B-trees), where damage matters.
            const std::size_t at = lcg(seed) % 2 ? 1024 + lcg(seed) % 162 : kAlloc + lcg(seed) % (4 * kBlock);
            b[at] = static_cast<std::uint8_t>(lcg(seed));
        }
        if (round % 5 == 0) b.resize(lcg(seed) % b.size());
        const auto v = HfsVolume::open(ByteSource::from_memory(b));
        if (!v) continue;
        ++opened;
        for (const HfsEntry& e : v->entries()) {
            Bytes out;
            if (v->read_fork(e, false, out)) CHECK(out.size() == e.data_size);
            if (v->read_fork(e, true, out)) CHECK(out.size() == e.rsrc_size);
        }
    }
    CHECK(opened > 100);  // most damage leaves a readable (if wrong) volume
}

// --- Containers and the folder scanner ---------------------------------------------------------------

TEST(assets_mac_files_folder) {
    TempDir tmp("folder");
    const fs::path d = tmp.path;
    const Bytes fork_b = resource_fork({{"INST", 1, "horn", text("hhhhhhhhzzzz")}});
    write(d / "disk.img", diskcopy(hfs_volume()));
    write(d / "wrapped" / "Vol.image.bin", macbinary("Vol.image", "dImg", "dCpy", partitioned(hfs_volume()), {}));
    write(d / "app.bin", macbinary("Color VETTE!", "APPL", "VETT", {}, fork_b));
    write(d / "Loose", text("loose data"));
    write(d / "Loose.rsrc", kForkA);
    write(d / "Doubled", text("doubled data"));
    write(d / "._Doubled", apple_single(true, "", {}, fork_b));
    write(d / "%Percent", apple_single(true, "", {}, kForkA));
    write(d / "single.as", apple_single(false, "Single One", text("single data"), fork_b));
    {
        const std::string hqx = binhex("Hexed", text("hex data \x90 with marker"), kForkA);
        write(d / "sub" / "hexed.hqx", text(hqx));
    }
    write(d / "zipped" / "VETTE!.Data", {});  // a zip from macOS: empty data fork, the rest in __MACOSX
    write(d / "__MACOSX" / "zipped" / "._VETTE!.Data", apple_single(true, "", {}, fork_b));
    write(d / "only.rsrc", fork_b);
    write(d / "archive.sit", text("SIT!rest of a stuffit archive"));

    const MacFiles files = MacFiles::open(d);
    for (const std::string& n : files.notes()) std::printf("  note: %s\n", n.c_str());
    std::size_t images = 0;
    for (const MacFile& f : files.files()) images += f.path.rfind("Test Vol/", 0) == 0;
    CHECK_EQ(images, std::size_t{4});  // two images, two files each

    const MacFile* c = files.find("Folder/B:C");
    CHECK(c && c->data_size == kSizeC && files.data_fork(*c).size() == kSizeC);
    CHECK_EQ(files.find_all("file a").size(), std::size_t{2});

    const MacFile* app = files.find("Color VETTE!");
    CHECK(app && app->type == fourcc("APPL") && app->source == "MacBinary II");
    CHECK(app && files.resources(*app) && files.resources(*app)->find(fourcc("INST"), "HORN"));

    const MacFile* loose = files.find("Loose");
    CHECK(loose && files.data_fork(*loose) == text("loose data") && files.resource_fork(*loose) == kForkA);

    const MacFile* dbl = files.find("Doubled");
    CHECK(dbl && dbl->type == fourcc("DATA") && dbl->creator == fourcc("VETT"));
    CHECK(dbl && files.data_fork(*dbl) == text("doubled data") && files.resource_fork(*dbl) == fork_b);
    CHECK(files.find("Percent") && files.resource_fork(*files.find("Percent")) == kForkA);

    const MacFile* single = files.find("Single One");
    CHECK(single && files.data_fork(*single) == text("single data") && files.resource_fork(*single) == fork_b);

    const MacFile* hexed = files.find("sub/Hexed");
    CHECK(hexed && hexed->type == fourcc("INST") && hexed->source == "BinHex 4.0");
    CHECK(hexed && files.data_fork(*hexed) == text("hex data \x90 with marker") &&
          files.resource_fork(*hexed) == kForkA);

    const MacFile* zipped = files.find("zipped/VETTE!.Data");
    CHECK(zipped && zipped->data_size == 0 && files.resource_fork(*zipped) == fork_b);
    CHECK(files.find("only") && files.find("only")->rsrc_size == fork_b.size());
    CHECK(files.find("archive.sit") == nullptr);
    bool noted = false;
    for (const std::string& n : files.notes()) noted |= n.find("StuffIt") != std::string::npos;
    CHECK(noted);

    CHECK(files.find("nothing here") == nullptr);
    CHECK(files.find_type(fourcc("APPL"), fourcc("VETT")) == app);
    CHECK(files.resources("Loose") && files.resources("Loose")->find(fourcc("PICT"), 128));

    // A single file opens too.
    const MacFiles one = MacFiles::open(d / "disk.img");
    CHECK_EQ(one.files().size(), std::size_t{2});
    CHECK(MacFiles::open(d / "missing").empty());
}

TEST(assets_mac_files_damaged_containers) {
    TempDir tmp("damaged");
    const Bytes sources[] = {macbinary("Color VETTE!", "APPL", "VETT", text("data"), kForkA),
                             apple_single(false, "Single", text("data"), kForkA),
                             text(binhex("Hexed", text("data"), kForkA)), diskcopy(hfs_volume())};
    std::uint32_t seed = 3;
    for (int round = 0; round < 240; ++round) {
        Bytes b = sources[round % 4];
        const int hits = 1 + static_cast<int>(lcg(seed) % 4);
        for (int i = 0; i < hits; ++i) {
            const std::size_t at = lcg(seed) % std::min<std::size_t>(b.size(), 2048);
            b[at] = static_cast<std::uint8_t>(lcg(seed));
        }
        if (round % 3 == 0) b.resize(lcg(seed) % b.size());
        const fs::path p = tmp.path / ("f" + std::to_string(round));
        write(p, b);
        const MacFiles files = MacFiles::open(p);
        for (const MacFile& f : files.files()) {
            files.data_fork(f);
            if (auto fork = files.resources(f)) CHECK(fork->bytes().size() == files.resource_fork(f).size());
        }
    }
}

// --- The player's files -------------------------------------------------------------------------------

TEST(assets_mac_files_real_image) {
    const fs::path toast = repo_root() / "Vette_Mac_EN" / "VETTE_1_02.toast";
    std::error_code ec;
    if (!fs::exists(toast, ec)) {
        std::printf("  SKIPPED: no %s\n", toast.string().c_str());
        return;
    }
    const MacFiles files = MacFiles::open(toast);
    CHECK_EQ(files.files().size(), std::size_t{15});
    const MacFile* color = files.find("VETTE! Folder/(Folder) Color VETTE!/Color VETTE!");
    CHECK(color && color->type == fourcc("APPL") && color->creator == fourcc("VETT"));
    const auto app = color ? files.resources(*color) : std::nullopt;
    CHECK(app && app->of_type(fourcc("PICT")).size() == 192 && app->of_type(fourcc("CODE")).size() == 11);
    const auto data = files.resources("(Folder) B&W VETTE!/VETTE!.Data");
    CHECK(data && data->of_type(fourcc("INST")).size() == 16 && data->find(fourcc("BGAS"), 128));
    // The forks match the copies extracted with another tool, where present.
    const fs::path extracted = repo_root() / "Vette_Mac_EN" / "extracted" / "VETTE! Folder";
    for (const char* name : {"(Folder) Color VETTE!/Color VETTE!", "(Folder) B&W VETTE!/VETTE!.Data",
                             "VETTE! MouseStick Sets"}) {
        const std::string rel = std::string(name) + ".rsrc";
        std::ifstream in(extracted / fs::path(std::u8string(rel.begin(), rel.end())), std::ios::binary);
        if (!in) continue;
        const Bytes dumped((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const MacFile* f = files.find(name);
        CHECK(f && files.resource_fork(*f) == dumped);
    }
}

TEST(assets_mac_files_game_folder) {
    const std::string override_dir = env("VETTE_MAC_DIR");
    const fs::path dir = !override_dir.empty() ? fs::path(override_dir) : repo_root() / "Game" / "Mac";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::printf("  SKIPPED: no %s\n", dir.string().c_str());
        return;
    }
    const MacFiles files = MacFiles::open(dir);
    for (const std::string& n : files.notes()) std::printf("  note: %s\n", n.c_str());
    const auto data = files.resources("VETTE!.Data");
    const auto app = files.resources("Color VETTE!");
    CHECK(data && data->of_type(fourcc("INST")).size() == 16);
    CHECK(app && app->of_type(fourcc("PICT")).size() == 192);
}
