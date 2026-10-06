// The Hills skyline (enhanced/backdrop.h): the horizon panoramas without their painted city.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "enhanced/backdrop.h"
#include "host/ega.h"
#include "test.h"

using namespace vette;
namespace fs = std::filesystem;

namespace {

constexpr int kRows = enhanced::kPanoramaRows, kWidth = enhanced::kPanoramaWidth;
constexpr size_t kSize = static_cast<size_t>(kRows) * kWidth;
constexpr uint8_t kSky = 0x0B;

// A made-up panorama: sky over a hill of colour 2 over water (colour 9), repeating after 2880 columns.
std::vector<uint8_t> made_up_panorama() {
    std::vector<uint8_t> p(kSize);
    for (int r = 0; r < kRows; ++r) {
        for (int x = 0; x < kWidth; ++x) {
            const int xx = x % enhanced::kPanoramaDegrees;
            p[static_cast<size_t>(r * kWidth + x)] = r < 10 + xx % 5 ? kSky : r < 18 ? 2 : 9;
        }
    }
    return p;
}

// The panorama buffer in off-screen video memory (A400:0000, 2580h, 4B00h), written plane by plane.
void put_panoramas(host::Ega& ega, const std::array<const std::vector<uint8_t>*, 3>& panoramas) {
    ega.set_mode(0x0D);
    for (int plane = 0; plane < 4; ++plane) {
        ega.out8(0x3C4, 0x02);
        ega.out8(0x3C5, static_cast<uint8_t>(1 << plane));
        for (int p = 0; p < 3; ++p) {
            const std::vector<uint8_t>& px = *panoramas[static_cast<size_t>(p)];
            for (size_t b = 0; b < kSize / 8; ++b) {
                uint8_t v = 0;
                for (int bit = 0; bit < 8; ++bit) v = static_cast<uint8_t>(v << 1 | (px[b * 8 + static_cast<size_t>(bit)] >> plane & 1));
                ega.vram_write(static_cast<uint32_t>(0x4000 + p * 0x2580 + static_cast<int>(b)), v);
            }
        }
    }
    ega.out8(0x3C4, 0x02);
    ega.out8(0x3C5, 0x0F);
}

// What blit_horizon copies: `rows` rows of 40 bytes from A400:source (400 a row) to frame byte dest (40 a row).
void blit(const std::array<const std::vector<uint8_t>*, 3>& panoramas, int rows, uint32_t source, uint32_t dest,
          std::vector<uint8_t>& frame) {
    for (int r = 0; r < rows; ++r) {
        const uint32_t s = source + static_cast<uint32_t>(r) * 400;
        const std::vector<uint8_t>& px = *panoramas[s / 0x2580];
        const size_t from = static_cast<size_t>(s % 0x2580) * 8, to = static_cast<size_t>(dest + static_cast<uint32_t>(r) * 40) * 8;
        for (size_t i = 0; i < 320; ++i) frame[to + i] = px[from + i];
    }
}

// The player's DOS panoramas (HORIZON0/1/2.BIN: 4 planes x 24 rows x 400 bytes), when the repository has them.
bool dos_panoramas(std::array<std::vector<uint8_t>, 3>& out) {
    const fs::path root = fs::path(__FILE__).parent_path().parent_path() / "Game";
    for (const fs::path& dir : {root, root / "DOS"}) {
        bool all = true;
        for (int n = 0; n < 3 && all; ++n) {
            std::ifstream f(dir / ("HORIZON" + std::to_string(n) + ".BIN"), std::ios::binary);
            const std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (d.size() != 4 * kSize / 8) {
                all = false;
                break;
            }
            std::vector<uint8_t>& px = out[static_cast<size_t>(n)];
            px.assign(kSize, 0);
            for (int plane = 0; plane < 4; ++plane) {
                for (size_t b = 0; b < kSize / 8; ++b) {
                    const uint8_t v = d[static_cast<size_t>(plane) * (kSize / 8) + b];
                    for (int bit = 0; bit < 8; ++bit) {
                        px[b * 8 + static_cast<size_t>(bit)] = static_cast<uint8_t>(px[b * 8 + static_cast<size_t>(bit)] | (v >> (7 - bit) & 1) << plane);
                    }
                }
            }
        }
        if (all) return true;
    }
    return false;
}

bool all_sky(const std::vector<uint8_t>& p, int x0, int x1, int y0, int y1) {
    for (int r = y0; r < y1; ++r) {
        for (int x = x0; x < x1; ++x) {
            if (p[static_cast<size_t>(r * kWidth + x)] != kSky) return false;
        }
    }
    return true;
}

} // namespace

TEST(enhanced_backdrop_unknown_panorama_left_as_is) {
    const std::vector<uint8_t> in = made_up_panorama();
    std::vector<uint8_t> out(kSize, 0xEE);
    CHECK(!enhanced::landscape_panorama(in.data(), out.data()));
    CHECK(out == in);
}

TEST(enhanced_backdrop_apply_checks_the_copy) {
    // Three made-up panoramas in video memory; the frame shows rows of the second one, as blit_horizon
    // copies them. Unknown pictures: apply() accepts the copy but has nothing to change.
    std::vector<uint8_t> a = made_up_panorama(), b = a, c = a;
    for (uint8_t& v : b) v = v == 2 ? 6 : v;
    for (uint8_t& v : c) v = v == 2 ? 8 : v;
    const std::array<const std::vector<uint8_t>*, 3> pans{&a, &b, &c};
    host::Ega ega;
    put_panoramas(ega, pans);
    std::vector<uint8_t> frame(320 * 200, 7);
    const uint32_t source = 0x2580 + 3 * 400 + 123, dest = 40 * 50;
    blit(pans, 21, source, dest, frame);
    const std::vector<uint8_t> before = frame;
    enhanced::Backdrop backdrop;
    CHECK(backdrop.apply(ega, 21, source, dest, frame.data(), 320, 200));
    CHECK(frame == before);
    CHECK_EQ(backdrop.known(), 0);
    // A frame that doesn't show those panorama rows, a copy past its panorama's last row, or out of the
    // frame: left alone.
    std::vector<uint8_t> other(320 * 200, 4);
    CHECK(!backdrop.apply(ega, 21, source, dest, other.data(), 320, 200));
    CHECK(other == std::vector<uint8_t>(320 * 200, 4));
    CHECK(!backdrop.apply(ega, 24, source, dest, frame.data(), 320, 200));
    CHECK(!backdrop.apply(ega, 21, source, 40 * 190, frame.data(), 320, 200));
    CHECK(!backdrop.apply(ega, 0, source, dest, frame.data(), 320, 200));
}

TEST(enhanced_backdrop_dos_panoramas) {
    std::array<std::vector<uint8_t>, 3> pans;
    if (!dos_panoramas(pans)) {
        std::printf("  (skipped: no DOS HORIZON0/1/2.BIN in Game)\n");
        return;
    }
    std::array<std::vector<uint8_t>, 3> hills;
    for (int n = 0; n < 3; ++n) {
        hills[static_cast<size_t>(n)].assign(kSize, 0);
        const auto start = std::chrono::steady_clock::now();
        CHECK(enhanced::landscape_panorama(pans[static_cast<size_t>(n)].data(), hills[static_cast<size_t>(n)].data()));
        std::printf("  HORIZON%d: %.1f ms\n", n,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        const std::vector<uint8_t>& h = hills[static_cast<size_t>(n)];
        // Still a panorama: the last 320 columns repeat the first.
        bool wraps = true;
        for (int r = 0; r < kRows; ++r) {
            for (int x = enhanced::kPanoramaDegrees; x < kWidth; ++x) {
                wraps = wraps && h[static_cast<size_t>(r * kWidth + x)] == h[static_cast<size_t>(r * kWidth + x - enhanced::kPanoramaDegrees)];
            }
        }
        CHECK(wraps);
    }
    const std::vector<uint8_t>&p0 = pans[0], &p1 = pans[1], &p2 = pans[2];
    const std::vector<uint8_t>&h0 = hills[0], &h1 = hills[1], &h2 = hills[2];
    // The painted landmarks are gone, the sky above where they stood: Sutro Tower (HORIZON1, HORIZON0),
    // the Transamerica Pyramid and the downtown towers (HORIZON0), the Bay Bridge's towers (HORIZON1),
    // the Golden Gate Bridge's south tower (HORIZON2).
    CHECK(!all_sky(p1, 1850, 1858, 2, 11) && all_sky(h1, 1850, 1858, 2, 11));
    CHECK(!all_sky(p0, 2040, 2048, 3, 6) && all_sky(h0, 2040, 2048, 3, 6));
    CHECK(!all_sky(p0, 2266, 2278, 4, 8) && all_sky(h0, 2266, 2278, 4, 8));
    CHECK(!all_sky(p1, 877, 934, 8, 13) && all_sky(h1, 877, 934, 8, 13));
    CHECK(!all_sky(p2, 415, 445, 2, 9) && all_sky(h2, 415, 445, 2, 9));
    // The city's colours are gone from HORIZON1: windows and walls in light grey, yellow, light red and
    // magenta (a few grey pixels of rock and shore stay).
    int city = 0, city_before = 0;
    for (size_t i = 0; i < kSize; ++i) {
        for (const int c : {7, 12, 13, 14, 5}) {
            city += h1[i] == c;
            city_before += p1[i] == c;
        }
    }
    std::printf("  HORIZON1 city colours: %d pixels, %d before\n", city, city_before);
    CHECK(city_before > 2000 && city < 40);
    // The hills, the water and the sky away from the city stay as painted: the bay's hills (HORIZON0
    // 0-1280) and Marin across the Golden Gate (HORIZON2 0-150).
    bool kept = true;
    for (int r = 0; r < kRows; ++r) {
        for (int x = 0; x < 1280; ++x) kept = kept && h0[static_cast<size_t>(r * kWidth + x)] == p0[static_cast<size_t>(r * kWidth + x)];
        for (int x = 0; x < 150; ++x) kept = kept && h2[static_cast<size_t>(r * kWidth + x)] == p2[static_cast<size_t>(r * kWidth + x)];
    }
    CHECK(kept);
    // Twin Peaks stays (HORIZON1): its summits' tops as painted.
    CHECK(h1[static_cast<size_t>(7 * kWidth + 1766)] == p1[static_cast<size_t>(7 * kWidth + 1766)] &&
          h1[static_cast<size_t>(7 * kWidth + 1766)] != kSky);

    // In the frame: apply() redraws the copied rows from the Hills panoramas.
    const std::array<const std::vector<uint8_t>*, 3> painted{&p0, &p1, &p2}, landscape{&h0, &h1, &h2};
    host::Ega ega;
    put_panoramas(ega, painted);
    std::vector<uint8_t> frame(320 * 200, 7), expected(320 * 200, 7);
    const uint32_t source = 0x2580 + 210, dest = 40 * 96;  // heading 210: Twin Peaks and Sutro Tower
    blit(painted, 24, source, dest, frame);
    blit(landscape, 24, source, dest, expected);
    CHECK(frame != expected);
    // Something drawn over the panorama afterwards (a pixel the Hills version changes) stays.
    size_t over = 0;
    while (frame[over] == expected[over]) ++over;
    frame[over] = expected[over] = 13;
    enhanced::Backdrop backdrop;
    CHECK(backdrop.apply(ega, 24, source, dest, frame.data(), 320, 200));
    CHECK(frame == expected);
    CHECK_EQ(backdrop.known(), 3);
}
