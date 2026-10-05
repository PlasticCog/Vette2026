// The Mac sounds: INST and 'snd ' decoding on synthetic resources, the use table and the engine's
// pitch; then VETTE!.Data's 16 sounds from the player's files, when present.

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "assets/mac_files.h"
#include "assets/mac_sounds.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace vette::assets;
using Bytes = std::vector<std::uint8_t>;

namespace {

Bytes inst(std::uint16_t loop_start, std::uint16_t loop_end, std::uint8_t note, std::uint16_t count,
           std::size_t len) {
    Bytes b = {static_cast<std::uint8_t>(loop_start >> 8), static_cast<std::uint8_t>(loop_start),
               static_cast<std::uint8_t>(loop_end >> 8),   static_cast<std::uint8_t>(loop_end),
               note,                                        0,
               static_cast<std::uint8_t>(count >> 8),       static_cast<std::uint8_t>(count)};
    for (std::size_t i = 0; i < len; ++i) b.push_back(static_cast<std::uint8_t>(i % 2 ? 0xC0 : 0x40));
    return b;
}

bool near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

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

TEST(assets_mac_inst) {
    // A 22 kHz looped sample (base note 25), as VETTE!'s horn.
    auto s = decode_inst(inst(10, 90, 25, 100, 100), 3523, "horn");
    CHECK(s.has_value());
    if (!s) return;
    CHECK(s->header_valid && s->loops());
    CHECK_EQ(s->samples.size(), std::size_t{100});
    CHECK_EQ(s->samples[0], std::int16_t{-0x4000});
    CHECK_EQ(s->samples[1], std::int16_t{0x4000});
    CHECK(s->loop_start == 10 && s->loop_end == 90);
    CHECK(near(s->native_rate, 22254.545, 0.01) && s->base_note == 25);

    // Unlooped: plays to the count; base note 0 is the driver's default, 37 (11127 Hz).
    s = decode_inst(inst(0, 0, 0, 60, 100), 1, "thud");
    CHECK(s && s->header_valid && !s->loops() && s->samples.size() == 60 && s->base_note == 37);
    CHECK(s && near(s->native_rate, kMacMixHz, 1e-9) && near(kMacMixHz, 11127.27, 0.01));

    // No header: the first 8 bytes are samples (flags byte nonzero); everything after them plays at 11127 Hz.
    Bytes junk(8, 0x7F);
    junk.resize(5000, 0x80);
    s = decode_inst(junk, 1425, "Opening song");
    CHECK(s && !s->header_valid && !s->loops() && s->samples.size() == 4992 && near(s->rate, kMacMixHz, 1e-9));

    std::string error;
    CHECK(!decode_inst(Bytes(7, 0), 1, "short", &error) && !error.empty());
    CHECK(!decode_inst(Bytes{'H', 'C', 'O', 'M', 0, 0, 0, 0, 1}, 1, "packed", &error) &&
          error.find("HCOM") != std::string::npos);
    // Loop points beyond the data are no header either.
    s = decode_inst(inst(10, 500, 37, 0, 100), 1, "x");
    CHECK(s && !s->header_valid && !s->loops());
}

TEST(assets_mac_snd) {
    // Format 1, one bufferCmd pointing at a standard header: 8-bit, 22050 Hz, loop 4-12, base note 60.
    Bytes b = {0, 1, 0, 1, 0, 5, 0, 0, 0, 0, 0, 1, 0x80, 0x51, 0, 0, 0, 0, 0, 20};
    const Bytes header = {0, 0, 0, 0, 0, 0, 0, 16, 0x56, 0x22, 0, 0, 0, 0, 0, 4, 0, 0, 0, 12, 0x00, 60};
    b.insert(b.end(), header.begin(), header.end());
    for (int i = 0; i < 16; ++i) b.push_back(static_cast<std::uint8_t>(0x80 + i));
    auto s = decode_snd(b, 1, "beep");
    CHECK(s.has_value());
    if (!s) return;
    CHECK(s->type == fourcc("snd ") && s->samples.size() == 16 && s->samples[2] == 0x200);
    CHECK(near(s->rate, 22050, 0.01) && s->base_note == 60 && s->loop_start == 4 && s->loop_end == 12);
    b[20 + 20] = 0xFE;  // compressed
    std::string error;
    CHECK(!decode_snd(b, 1, "x", &error) && !error.empty());
    CHECK(!decode_snd(Bytes{0, 3, 0, 0, 0, 0}, 1, "x", &error));
    // Damaged: never reads outside the resource.
    std::uint32_t seed = 11;
    for (int round = 0; round < 2000; ++round) {
        Bytes d = b;
        d[20 + 20] = 0;
        for (int i = 0; i < 3; ++i) {
            seed = seed * 1664525u + 1013904223u;
            d[(seed >> 8) % d.size()] = static_cast<std::uint8_t>(seed >> 20);
        }
        if (auto x = decode_snd(d, 1, "x")) CHECK(x->samples.size() <= d.size());
    }
}

TEST(assets_mac_sound_uses) {
    // Every DOS sound with a Mac counterpart, and the two the Mac lacks.
    for (const char* dos : {"engine", "garage_rev", "skid", "siren", "title_tune", "crash", "crash_car", "crash_rail",
                            "hit_pedestrian"}) {
        CHECK(mac_sound_for_dos(dos) != nullptr);
    }
    CHECK(mac_sound_for_dos("win_tune") == nullptr && mac_sound_for_dos("gear_grind") == nullptr);
    CHECK_EQ(std::string(mac_sound_for_dos("siren")->sound), std::string("police"));
    CHECK_EQ(std::string(mac_sound_for_dos("hit_pedestrian")->sound), std::string("kill"));
    CHECK(mac_sound_use("HORN") && near(mac_sound_use("horn")->rate, kMacSoundBufferHz, 1e-9));
    CHECK(mac_sound_use("signature") && near(mac_sound_use("signature")->max_seconds, 3.99, 0.01));
    // The engine: 27000/65536 of 11127 Hz up to 1200 rpm, then 15000 + 10 * rpm, capped at 85000.
    CHECK(near(mac_engine_rate(800), kMacMixHz * 27000 / 65536, 1e-6));
    CHECK(near(mac_engine_rate(3000), kMacMixHz * 45000 / 65536, 1e-6));
    CHECK(near(mac_engine_rate(9000), kMacMixHz * 85000 / 65536, 1e-6));
    CHECK(mac_engine_rate(2000) < mac_engine_rate(2100));
}

// The real VETTE!.Data: what the sounds are, as the notes (re/notes/08-mac.md) describe them.
TEST(assets_mac_sounds_real) {
    const std::string override_dir = env("VETTE_MAC_DIR");
    std::vector<fs::path> places = {repo_root() / "Game" / "Mac", repo_root() / "Vette_Mac_EN" / "VETTE_1_02.toast"};
    if (!override_dir.empty()) places.insert(places.begin(), fs::path(override_dir));
    std::optional<ResourceFork> data;
    for (const fs::path& p : places) {
        std::error_code ec;
        if (!fs::exists(p, ec)) continue;
        if ((data = find_vette_data(MacFiles::open(p)))) break;
    }
    if (!data) {
        std::printf("  SKIPPED: no Mac VETTE!.Data (Game/Mac or Vette_Mac_EN)\n");
        return;
    }
    std::vector<std::string> problems;
    const auto sounds = decode_mac_sounds(*data, &problems);
    CHECK(problems.empty());
    CHECK_EQ(sounds.size(), std::size_t{16});
    struct Expect {
        const char* name;
        std::int16_t id;
        std::size_t samples;
        bool header;
        int note;
        std::size_t loop_start, loop_end;
        double rate;
    };
    const double k1 = kMacMixHz, k2 = kMacSoundBufferHz;
    const Expect expect[] = {
        {"mic", 28215, 44591, false, 37, 0, 0, k1},          {"Signature", 12083, 44920, false, 37, 0, 0, k1},
        {"Opening song", 1425, 117550, false, 37, 0, 0, k1}, {"splash", 5403, 36561, false, 37, 0, 0, k1},
        {"kill", 28441, 7291, true, 37, 0, 0, k1},           {"beep2", 22909, 4608, true, 25, 0, 0, k1},
        {"beep1", 24157, 8400, true, 25, 0, 0, k1},          {"heli", 17804, 12128, true, 25, 224, 11808, k2},
        {"joel", 17399, 31173, true, 37, 8593, 29693, k1},   {"horn", 3523, 5606, true, 25, 1428, 4989, k2},
        {"police", 27989, 6920, true, 37, 2384, 6344, k1},   {"crash", 2585, 17464, true, 37, 0, 0, k1},
        {"skid", 2052, 17488, true, 37, 0, 0, k1},           {"cable car bell", 19354, 9826, true, 37, 0, 0, k1},
        {"Engine", 11584, 6053, true, 25, 370, 5682, k1},    {"thud", 56, 2704, true, 37, 0, 0, k1},
    };
    for (const Expect& e : expect) {
        const MacSound* s = nullptr;
        for (const MacSound& x : sounds) {
            if (x.name == e.name) s = &x;
        }
        CHECK(s != nullptr);
        if (!s) continue;
        CHECK_EQ(s->id, e.id);
        CHECK_EQ(s->samples.size(), e.samples);
        CHECK_EQ(s->header_valid, e.header);
        CHECK_EQ(s->base_note, e.note);
        CHECK_EQ(s->loop_start, e.loop_start);
        CHECK_EQ(s->loop_end, e.loop_end);
        CHECK(near(s->rate, e.rate, 1e-6));
        CHECK(mac_sound_use(s->name) != nullptr);  // the game uses every one
    }
}
