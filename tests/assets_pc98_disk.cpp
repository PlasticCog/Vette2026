// The PC-98 disk reader: synthetic FAT12/FAT16 volumes in every supported image format, malformed
// images (loops, bad clusters, truncation, random damage), and the player's own files when present
// (Game/PC98/, Vette_PC-98_JA/VETTE.hdi; VETTE_PC98_DIR overrides the folder).

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "assets/pc98_disk.h"
#include "test.h"

using vette::assets::Pc98Files;
namespace fs = std::filesystem;

namespace {

void put16(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    b[at] = static_cast<uint8_t>(v);
    b[at + 1] = static_cast<uint8_t>(v >> 8);
}
void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    put16(b, at, v & 0xFFFF);
    put16(b, at + 2, v >> 16);
}

std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) {
        v[i] = static_cast<uint8_t>(seed + i * 7 + (i >> 8));
    }
    return v;
}

// A FAT volume written from scratch: files in the root or in one subdirectory.
struct FatSpec {
    uint32_t bps, spc, reserved, fats, root, total, spf;
    uint8_t media;
    uint32_t spt, heads, hidden;
};
constexpr FatSpec kTwoHd{1024, 1, 1, 2, 192, 1232, 2, 0xFE, 8, 2, 0};          // PC-98 1.2 MB floppy
constexpr FatSpec kHardDisk{1024, 1, 1, 2, 512, 5000, 10, 0xF8, 17, 4, 68};    // FAT16 (4970 clusters)

class FatBuilder {
public:
    explicit FatBuilder(const FatSpec& s) : s_(s), img_(size_t{s.total} * s.bps, 0) {
        const uint32_t root_sectors = s.root * 32 / s.bps;
        data_ = (s.reserved + s.fats * s.spf + root_sectors) * s.bps;
        clusters_ = (s.total - s.reserved - s.fats * s.spf - root_sectors) / s.spc;
        fat16_ = clusters_ >= 4085;
        img_[0] = 0xEB;
        img_[1] = 0x3C;
        img_[2] = 0x90;
        std::memcpy(&img_[3], "NEC 6.20", 8);
        put16(img_, 11, s.bps);
        img_[13] = static_cast<uint8_t>(s.spc);
        put16(img_, 14, s.reserved);
        img_[16] = static_cast<uint8_t>(s.fats);
        put16(img_, 17, s.root);
        put16(img_, 19, s.total);
        img_[21] = s.media;
        put16(img_, 22, s.spf);
        put16(img_, 24, s.spt);
        put16(img_, 26, s.heads);
        put32(img_, 28, s.hidden);
        set_fat(0, 0xFFFF00u | s.media);
        set_fat(1, 0xFFFFFFFF);
    }

    void set_fat(uint32_t c, uint32_t v) {
        for (uint32_t f = 0; f < s_.fats; ++f) {
            const size_t base = (s_.reserved + f * s_.spf) * size_t{s_.bps};
            if (fat16_) {
                put16(img_, base + c * 2, v & 0xFFFF);
            } else {
                const size_t i = base + c * 3 / 2;
                const uint32_t old = img_[i] | img_[i + 1] << 8;
                const uint32_t nv = (c & 1) ? ((old & 0x000F) | (v & 0xFFF) << 4) : ((old & 0xF000) | (v & 0xFFF));
                put16(img_, i, nv);
            }
        }
    }
    uint32_t eoc() const { return fat16_ ? 0xFFFF : 0xFFF; }

    // Stores `data` in fresh clusters; returns the first (0 for empty data).
    uint32_t store(const std::vector<uint8_t>& data) {
        const size_t cb = size_t{s_.bps} * s_.spc;
        const size_t n = (data.size() + cb - 1) / cb;
        if (n == 0) {
            return 0;
        }
        const uint32_t first = next_;
        for (size_t k = 0; k < n; ++k) {
            const uint32_t c = next_++;
            const size_t len = std::min(cb, data.size() - k * cb);
            std::memcpy(&img_[data_ + (c - 2) * cb], &data[k * cb], len);
            set_fat(c, k + 1 < n ? c + 1 : eoc());
        }
        return first;
    }

    static std::vector<uint8_t> entry(const char* name83, uint8_t attr, uint32_t cluster, uint32_t size) {
        std::vector<uint8_t> e(32, 0);
        std::memcpy(e.data(), name83, 11);
        e[11] = attr;
        put16(e, 26, cluster);
        put32(e, 28, size);
        return e;
    }

    void add_root(const std::vector<uint8_t>& e) {
        const size_t root = (s_.reserved + s_.fats * s_.spf) * size_t{s_.bps};
        std::memcpy(&img_[root + 32 * root_used_++], e.data(), 32);
    }
    // A file in the root; returns its first cluster.
    uint32_t add_file(const char* name83, const std::vector<uint8_t>& data) {
        const uint32_t c = store(data);
        add_root(entry(name83, 0x20, c, static_cast<uint32_t>(data.size())));
        return c;
    }
    // A subdirectory of the root holding `files`.
    uint32_t add_dir(const char* name83, const std::vector<std::pair<const char*, std::vector<uint8_t>>>& files) {
        std::vector<std::vector<uint8_t>> entries;
        for (const auto& [name, data] : files) {
            entries.push_back(entry(name, 0x20, store(data), static_cast<uint32_t>(data.size())));
        }
        std::vector<uint8_t> dir;
        const uint32_t self = next_;
        const auto append = [&](const std::vector<uint8_t>& e) { dir.insert(dir.end(), e.begin(), e.end()); };
        append(entry(".          ", 0x10, self, 0));
        append(entry("..         ", 0x10, 0, 0));
        for (const auto& e : entries) {
            append(e);
        }
        dir.resize(size_t{s_.bps} * s_.spc, 0);
        const uint32_t c = store(dir);
        add_root(entry(name83, 0x10, c, 0));
        return c;
    }

    std::vector<uint8_t>& bytes() { return img_; }
    size_t data_offset() const { return data_; }

private:
    FatSpec s_;
    std::vector<uint8_t> img_;
    size_t data_ = 0;
    uint32_t clusters_ = 0, next_ = 2;
    size_t root_used_ = 0;
    bool fat16_ = false;
};

const std::vector<uint8_t> kExe = pattern(5000, 0x4D);
const std::vector<uint8_t> kPic = pattern(2600, 0x11);

// The PC-98 floppy with the game in its root (plus a volume label and a deleted file to skip).
std::vector<uint8_t> floppy_image() {
    FatBuilder b(kTwoHd);
    b.add_root(FatBuilder::entry("VETTE98    ", 0x08, 0, 0));
    b.add_file("VETTE   EXE", kExe);
    auto deleted = FatBuilder::entry("\xE5OLD    TXT", 0x20, 0, 0);
    b.add_root(deleted);
    b.add_file("TITLE   PIC", kPic);
    return b.bytes();
}

// A PC-98 hard disk (512-byte sectors, 17 sectors, 4 heads): IPL, partition table, a FAT16 volume at
// cylinder 1 holding AUTOEXEC.BAT and \VETTE\{VETTE.EXE, TITLE.PIC}.
std::vector<uint8_t> hard_disk() {
    FatBuilder b(kHardDisk);
    b.add_file("AUTOEXECBAT", pattern(100, 1));
    b.add_dir("VETTE      ", {{"VETTE   EXE", kExe}, {"TITLE   PIC", kPic}});
    std::vector<uint8_t> disk(68 * 512, 0);
    const uint8_t ipl[] = {0xEB, 0x0A, 0x90, 0x90, 'I', 'P', 'L', '1'};
    std::memcpy(disk.data(), ipl, sizeof ipl);
    disk[512] = 0xA1;  // partition 0: bootable DOS, starts at C1 H0 S0
    disk[513] = 0x81;
    put16(disk, 512 + 6, 1);
    put16(disk, 512 + 10, 1);
    put16(disk, 512 + 14, 80);
    std::memcpy(&disk[512 + 16], "MS-DOS 6.20     ", 16);
    disk.insert(disk.end(), b.bytes().begin(), b.bytes().end());
    disk.resize(153 * 68 * 512, 0);
    return disk;
}

std::vector<uint8_t> anex86(const std::vector<uint8_t>& raw, uint32_t ss, uint32_t spt, uint32_t heads) {
    std::vector<uint8_t> out(4096, 0);
    put32(out, 4, 0x90);
    put32(out, 8, 4096);
    put32(out, 12, static_cast<uint32_t>(raw.size()));
    put32(out, 16, ss);
    put32(out, 20, spt);
    put32(out, 24, heads);
    put32(out, 28, static_cast<uint32_t>(raw.size() / (size_t{ss} * spt * heads)));
    out.insert(out.end(), raw.begin(), raw.end());
    return out;
}

// D88 of a 2HD floppy: sectors stored in reverse order within each track (placed by their ids).
std::vector<uint8_t> d88(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> out(0x2B0, 0);
    std::memcpy(out.data(), "TEST", 4);
    out[0x1B] = 0x20;  // 2HD
    for (uint32_t t = 0; t < 154; ++t) {
        put32(out, 0x20 + 4 * t, static_cast<uint32_t>(out.size()));
        for (uint32_t k = 0; k < 8; ++k) {
            const uint32_t r = 8 - k;
            std::vector<uint8_t> h(16, 0);
            h[0] = static_cast<uint8_t>(t / 2);
            h[1] = static_cast<uint8_t>(t % 2);
            h[2] = static_cast<uint8_t>(r);
            h[3] = 3;
            put16(h, 4, 8);
            put16(h, 14, 1024);
            out.insert(out.end(), h.begin(), h.end());
            const size_t at = (size_t{t} * 8 + (r - 1)) * 1024;
            out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + 1024));
        }
    }
    put32(out, 0x1C, static_cast<uint32_t>(out.size()));
    return out;
}

std::vector<uint8_t> nfd(const std::vector<uint8_t>& raw) {
    constexpr size_t kTable = 0x120, kHeader = kTable + 163 * 26 * 16 + 0x10;
    std::vector<uint8_t> out(kHeader, 0);
    std::memcpy(out.data(), "T98FDDIMAGE.R0", 14);
    put32(out, 0x110, static_cast<uint32_t>(kHeader));
    out[0x115] = 2;
    for (size_t t = 0; t < 163; ++t) {
        for (size_t s = 0; s < 26; ++s) {
            uint8_t* id = &out[kTable + (t * 26 + s) * 16];
            if (t >= 154 || s >= 8) {
                id[0] = id[1] = id[2] = id[3] = 0xFF;
                continue;
            }
            id[0] = static_cast<uint8_t>(t / 2);
            id[1] = static_cast<uint8_t>(t % 2);
            id[2] = static_cast<uint8_t>(s + 1);
            id[3] = 3;
        }
    }
    out.insert(out.end(), raw.begin(), raw.end());
    return out;
}

void check_game(const std::optional<Pc98Files>& f, const std::string& error) {
    CHECK(f.has_value());
    if (!f) {
        std::fprintf(stderr, "  (%s)\n", error.c_str());
        return;
    }
    CHECK(f->names() == (std::vector<std::string>{"TITLE.PIC", "VETTE.EXE"}));
    CHECK(f->read("vette.exe") == kExe);  // DOS names: case-insensitive
    CHECK(f->read("TITLE.PIC") == kPic);
    CHECK(!f->read("MISSING.DAT").has_value());
}

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

fs::path repo() { return fs::path(__FILE__).parent_path().parent_path(); }

fs::path temp_dir(const char* tag) {
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / (std::string("vette_pc98_") + tag + "_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir, ec);
    return dir;
}

void write_file(const fs::path& p, const std::vector<uint8_t>& data) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

}  // namespace

TEST(pc98_disk_raw_floppy) {
    std::string error;
    const auto f = Pc98Files::open_image(floppy_image(), "VETTE.IMG", error);
    check_game(f, error);
    if (f) {
        CHECK(f->description().find("FAT12") != std::string::npos);
        CHECK(f->image_listing().size() == 2);  // the label and the deleted entry aren't files
    }
}

TEST(pc98_disk_floppy_without_bpb) {
    std::vector<uint8_t> img = floppy_image();
    std::fill(img.begin(), img.begin() + 64, uint8_t{0});  // old format: no BPB, 2HD by its media byte
    std::string error;
    check_game(Pc98Files::open_image(img, "old.dup", error), error);
}

TEST(pc98_disk_hdi_fat16_subdirectory) {
    std::string error;
    const auto f = Pc98Files::open_image(anex86(hard_disk(), 512, 17, 4), "VETTE.HDI", error);
    check_game(f, error);
    if (f) {
        CHECK(f->description().find("FAT16") != std::string::npos);
        CHECK(f->description().find("\\VETTE") != std::string::npos);
        bool seen_exe = false, seen_dir = false;
        for (const auto& e : f->image_listing()) {
            seen_exe |= e.path == "VETTE\\VETTE.EXE" && e.size == kExe.size() && !e.directory;
            seen_dir |= e.path == "VETTE" && e.directory;
        }
        CHECK(seen_exe && seen_dir);
    }
}

TEST(pc98_disk_raw_hard_disk) {
    // No header: the volume is found by its boot sector, then the partition table with its geometry.
    std::string error;
    check_game(Pc98Files::open_image(hard_disk(), "VETTE.raw.img", error), error);
}

TEST(pc98_disk_fdi_d88_nfd) {
    const std::vector<uint8_t> raw = floppy_image();
    std::string error;
    check_game(Pc98Files::open_image(anex86(raw, 1024, 8, 2), "VETTE.FDI", error), error);
    const auto f = Pc98Files::open_image(d88(raw), "VETTE.D88", error);
    check_game(f, error);
    if (f) {
        CHECK(f->description().find("D88") != std::string::npos);
    }
    check_game(Pc98Files::open_image(nfd(raw), "VETTE.NFD", error), error);
}

TEST(pc98_disk_damaged_files) {
    // TITLE.PIC's chain loops back on itself: skipped, the rest still loads.
    {
        FatBuilder b(kTwoHd);
        b.add_file("VETTE   EXE", kExe);
        const uint32_t c = b.add_file("TITLE   PIC", kPic);
        b.set_fat(c + 1, c);  // c, c+1, c, ...: a revisit, though long enough for the size
        std::string error;
        const auto f = Pc98Files::open_image(b.bytes(), "loop.img", error);
        CHECK(f.has_value());
        if (f) {
            CHECK(f->names() == std::vector<std::string>{"VETTE.EXE"});
            CHECK(f->description().find("skipped") != std::string::npos);
        }
    }
    // VETTE.EXE points at a free cluster: the image is rejected.
    {
        FatBuilder b(kTwoHd);
        const uint32_t c = b.add_file("VETTE   EXE", kExe);
        b.set_fat(c + 1, 0);
        std::string error;
        CHECK(!Pc98Files::open_image(b.bytes(), "free.img", error).has_value());
        CHECK(error.find("damaged") != std::string::npos);
    }
    // A file longer than its chain, and one whose cluster is past the end of the volume.
    {
        FatBuilder b(kTwoHd);
        b.add_file("VETTE   EXE", kExe);
        b.add_root(FatBuilder::entry("LONG    DAT", 0x20, 2, 1u << 20));
        b.add_root(FatBuilder::entry("FAR     DAT", 0x20, 0xFF0, 10));
        std::string error;
        const auto f = Pc98Files::open_image(b.bytes(), "bad.img", error);
        CHECK(f.has_value() && f->names() == std::vector<std::string>{"VETTE.EXE"});
    }
}

TEST(pc98_disk_directory_loops) {
    // LOOP lists itself as a child (walked once); CYCLE's cluster chain loops (left unread).
    FatBuilder b(kTwoHd);
    b.add_file("VETTE   EXE", kExe);
    const uint32_t loop = b.add_dir("LOOP       ", {});
    const uint32_t cycle = b.add_dir("CYCLE      ", {});
    std::vector<uint8_t>& img = b.bytes();
    const auto again = FatBuilder::entry("AGAIN      ", 0x10, loop, 0);
    std::memcpy(&img[b.data_offset() + (loop - 2) * 1024 + 64], again.data(), 32);
    b.set_fat(cycle, cycle);
    std::string error;
    const auto f = Pc98Files::open_image(img, "dirloop.img", error);
    CHECK(f.has_value() && f->contains("VETTE.EXE"));
    if (f) {
        int again_seen = 0;
        for (const auto& e : f->image_listing()) {
            again_seen += e.path.find("AGAIN") != std::string::npos;
        }
        CHECK_EQ(again_seen, 1);
    }
}

TEST(pc98_disk_rejects_garbage) {
    std::string error;
    CHECK(!Pc98Files::open_image(std::vector<uint8_t>{}, "empty.hdi", error).has_value());
    CHECK(!Pc98Files::open_image(pattern(100000, 3), "noise.img", error).has_value());
    CHECK(!Pc98Files::open_image(std::vector<uint8_t>(1261568, 0), "zero.d88", error).has_value());
    // A header that points past the end, and an NFD r1.
    std::vector<uint8_t> hdi(8192, 0);
    put32(hdi, 8, 4096);
    put32(hdi, 12, 0x7FFFFFFF);
    put32(hdi, 16, 512);
    CHECK(!Pc98Files::open_image(hdi, "short.hdi", error).has_value());
    std::vector<uint8_t> r1(0x10000, 0);
    std::memcpy(r1.data(), "T98FDDIMAGE.R1", 14);
    CHECK(!Pc98Files::open_image(r1, "new.nfd", error).has_value());
    CHECK(error.find("r1") != std::string::npos);
    // Truncated images: whatever is left must not be read past.
    const std::vector<uint8_t> disk = anex86(hard_disk(), 512, 17, 4);
    for (const size_t keep : {size_t{100}, size_t{4096 + 600}, size_t{4096 + 68 * 512 + 3000}, disk.size() / 2}) {
        (void)Pc98Files::open_image(std::span(disk).first(keep), "cut.hdi", error);
    }
}

TEST(pc98_disk_random_damage) {
    // Random bytes over the metadata (boot sector, FATs, directories, partition table, headers): any
    // outcome is fine as long as it's quick and stays in bounds (sanitizers or a crash would tell).
    std::mt19937 rng(98);
    const std::vector<std::vector<uint8_t>> bases = {floppy_image(), anex86(hard_disk(), 512, 17, 4),
                                                     d88(floppy_image())};
    const char* names[] = {"f.img", "h.hdi", "d.d88"};
    int opened = 0;
    for (int round = 0; round < 300; ++round) {
        const size_t which = static_cast<size_t>(round) % bases.size();
        std::vector<uint8_t> img = bases[which];
        const size_t span = std::min<size_t>(img.size(), 64 * 1024);
        for (int k = 0; k < 1 + round % 24; ++k) {
            img[rng() % span] = static_cast<uint8_t>(rng());
        }
        std::string error;
        opened += Pc98Files::open_image(img, names[which], error).has_value();
    }
    CHECK(opened > 0);  // light damage often leaves the game readable
}

TEST(pc98_disk_folders) {
    const std::vector<uint8_t> raw = floppy_image();
    // Plain files.
    const fs::path plain = temp_dir("plain");
    write_file(plain / "vette.exe", kExe);
    write_file(plain / "TITLE.PIC", kPic);
    std::string error;
    auto f = Pc98Files::open(plain, error);
    CHECK(f.has_value() && f->read("VETTE.EXE") == kExe && f->image_listing().empty());
    // An image (after a broken one, which is reported only if nothing works).
    const fs::path images = temp_dir("images");
    write_file(images / "broken.hdi", pattern(5000, 9));
    write_file(images / "game.d88", d88(raw));
    write_file(images / "notes.txt", pattern(10, 1));
    f = Pc98Files::open(images, error);
    check_game(f, error);
    // A release unpacked into a subfolder.
    const fs::path nested = temp_dir("nested");
    fs::create_directories(nested / "Vette_PC-98_JA");
    write_file(nested / "Vette_PC-98_JA" / "VETTE.FDI", anex86(raw, 1024, 8, 2));
    check_game(Pc98Files::open(nested, error), error);
    // Nothing usable.
    const fs::path empty = temp_dir("empty");
    CHECK(!Pc98Files::open(empty, error).has_value());
    CHECK(!Pc98Files::open(empty / "missing", error).has_value());
    std::error_code ec;
    for (const fs::path& p : {plain, images, nested, empty}) {
        fs::remove_all(p, ec);
    }
}

TEST(pc98_disk_player_files) {
    fs::path folder = env("VETTE_PC98_DIR");
    if (folder.empty()) folder = repo() / "Game" / "PC98";
    std::string error;
    std::optional<Pc98Files> game = Pc98Files::open(folder, error);
    const fs::path hdi = repo() / "Vette_PC-98_JA" / "VETTE.hdi";
    std::optional<Pc98Files> image;
    if (fs::exists(hdi)) {
        image = Pc98Files::open_image(hdi, error);
        CHECK(image.has_value());
    }
    if (!game && !image) {
        std::printf("  (skipped: no PC-98 files in %s)\n", folder.string().c_str());
        return;
    }
    for (const auto* f : {game ? &*game : nullptr, image ? &*image : nullptr}) {
        if (!f) continue;
        const auto exe = f->read("VETTE.EXE");
        CHECK(exe && exe->size() == 202350 && (*exe)[0] == 'M' && (*exe)[1] == 'Z');  // 1.02J
        CHECK(f->contains("TITLE.PIC") && f->contains("DASH.PIC") && f->contains("ANIM.DAT"));
    }
    if (game && image) {
        for (const std::string& name : image->names()) {
            const auto a = image->read(name), b = game->read(name);
            if (b) CHECK(a == b);  // the extracted copies match the image
        }
    }
    if (image) {
        CHECK(image->description().find("HDI hard disk") != std::string::npos);
        CHECK(image->description().find("\\VETTE") != std::string::npos);
        // The raw image (the HDI without its header) holds the same.
        const fs::path raw = repo() / "Vette_PC-98_JA" / "VETTE.raw.img";
        if (fs::exists(raw)) {
            const auto r = Pc98Files::open_image(raw, error);
            CHECK(r.has_value() && r->names() == image->names() && r->read("VETTE.EXE") == image->read("VETTE.EXE"));
        }
    }
}
