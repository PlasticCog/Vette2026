#include "enhanced/backdrop.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <span>

namespace vette::enhanced {
namespace {

constexpr int kRows = kPanoramaRows;
constexpr int kWidth = kPanoramaWidth;
constexpr int kDegrees = kPanoramaDegrees;
constexpr int kSize = kRows * kWidth;
constexpr uint8_t kSky = 0x0B;  // cs:57DF, the sky colour (fill_sky_ground)

// The panorama buffer in off-screen video memory (vram_store_seg A400, horizon_offsets DS:3CA7).
constexpr uint32_t kBufferStart = 0x4000;  // A400:0000 as an offset into the planes
constexpr uint32_t kBufferBytes = kPanoramas * kRows * (kWidth / 8);
constexpr int kFrameBytes = 8000;  // render_page's 320 x 200 at 40 bytes a row

constexpr uint16_t colours(std::initializer_list<int> list) {
    uint16_t m = 0;
    for (const int c : list) m = static_cast<uint16_t>(m | (1u << c));
    return m;
}
constexpr bool has(uint16_t mask, int colour) { return colour >= 0 && (mask >> colour & 1) != 0; }
constexpr int wrap(int x) { return ((x % kDegrees) + kDegrees) % kDegrees; }
constexpr int pmod(int a, int m) { return ((a % m) + m) % m; }
int round_half_up(double v) { return static_cast<int>(std::floor(v + 0.5)); }

// --- The retouching, in panorama columns (0..2879; ranges may run past 2879 and wrap) and rows (0 at the top).
// Hill: columns [x0, x1) repainted whole. Their skyline follows `line` (row by column, straight between the
// points; with `rough` rows of value noise every `rough_period` columns, and the roughness of the original
// skyline over [detail_x0, detail_x1)), except that where the original's own top is within `tol` rows of the
// line and of a `keep` colour, it stays, with its `keep_rows` rows (above row `keep_until`). Below the
// skyline: an `outline` pixel along the new edge, `edge_rows` rows taken from the top of the natural skyline
// over [edge_x0, edge_x1), then the interior texture: the block [block_x0, block_x1) x [block_y0, block_y1)
// tiled, or `dots_base` sprinkled with `dots` (per mille). From row `water` down, the water of columns
// [water_x0, water_x1) from their row `water_top`.
struct Point {
    int x, row;
};
struct Dot {
    int colour, permille;
};
constexpr uint16_t kHillColours = colours({0, 8, 2, 10, 3, 6});
struct Hill {
    int x0 = 0, x1 = 0;
    std::span<const Point> line{};
    int tol = -1;
    uint16_t keep = kHillColours;
    int keep_rows = 3, keep_until = kRows;
    int water = -1, water_x0 = 0, water_x1 = 0, water_top = 0;
    int edge_x0 = -1, edge_x1 = 0, edge_rows = 2;
    int outline = -1;
    int rough = 0, rough_period = 1;
    int detail_x0 = -1, detail_x1 = 0;
    int block_x0 = -1, block_x1 = 0, block_y0 = 0, block_y1 = 0;
    int dots_base = 0;
    std::span<const Dot> dots{};
};
// Patch: the pixels of [x0, x1) x [y0, y1) (only those of `colours`, if any) painted over with the nearest
// natural pixels of their row, or sky above the skyline joined across them.
struct Patch {
    int x0, x1, y0, y1;
    uint16_t colours;
};
struct Retouch {
    uint64_t hash;
    std::span<const Hill> hills;
    std::span<const Patch> patches;
};

// The DOS release's panoramas.
// HORIZON0, the bay.
constexpr Point kLine0_0[] = {{1547, 14}, {1562, 12}, {1575, 11}, {1582, 13}, {1600, 13}, {1645, 12}, {1660, 11},
    {1700, 11}, {1760, 12}, {1793, 12}, {1820, 10}, {1835, 8}, {1848, 6}, {1856, 7}, {1868, 10}, {1880, 11},
    {1900, 11}, {1930, 9}, {1960, 8}, {1990, 10}, {2030, 11}, {2080, 9}, {2120, 10}, {2160, 12}, {2220, 11},
    {2260, 9}, {2300, 10}, {2360, 12}, {2418, 13}};
const Hill kHills0[] = {
    // The city as a far skyline (Sutro Tower, the Transamerica Pyramid): hills, with those that show over it.
    {.x0 = 1548, .x1 = 2418, .line = kLine0_0, .tol = 1, .keep = colours({0, 8, 2, 10}), .edge_x0 = 0, .edge_x1 = 400,
     .block_x0 = 2590, .block_x1 = 2630, .block_y0 = 11, .block_y1 = 17},
};
constexpr Patch kPatches0[] = {
    {1290, 1430, 12, 18, colours({15, 12})},  // lights on the far shore
    {1340, 1456, 18, 24, colours({0, 1})},  // a pier's pilings
    {1455, 1518, 17, 24, 0},  // a pier
    {2416, 2440, 17, 24, 0},  // the waterfront's arches
    {2436, 2480, 19, 24, colours({0, 1, 7, 8})},  // pilings
    {2660, 2730, 14, 21, colours({7, 15})},  // Alcatraz's buildings (the island stays)
};

// HORIZON1, the city.
constexpr Point kLine1_0[] = {{2865, 15}, {2960, 15}, {3040, 14}, {3120, 16}, {3200, 15}, {3300, 14}, {3400, 15},
    {3500, 16}, {3600, 15}, {3700, 15}, {3753, 16}};
constexpr Point kLine1_1[] = {{872, 16}, {884, 17}, {900, 16}, {930, 17}, {976, 16}};
constexpr Point kLine1_2[] = {{1039, 16}, {1100, 15}, {1200, 16}, {1300, 15}, {1400, 16}};
constexpr Point kLine1_3[] = {{1400, 16}, {1500, 15}, {1600, 14}, {1690, 15}};
constexpr Dot kDots1[] = {{8, 120}, {2, 30}};
constexpr Point kLine1_4[] = {{1690, 15}, {1737, 13}, {1755, 11}, {1766, 7}, {1772, 9}, {1780, 11}, {1786, 8},
    {1795, 10}, {1810, 12}, {1830, 13}, {1845, 13}, {1870, 15}, {1885, 16}, {1900, 17}};
constexpr Point kLine1_5[] = {{1900, 17}, {1910, 18}, {1920, 20}, {1940, 21}};
constexpr Point kLine1_6[] = {{1940, 21}, {2575, 21}};
constexpr Point kLine1_7[] = {{2575, 21}, {2584, 19}, {2591, 16}, {2600, 13}};
const Hill kHills1[] = {
    // Downtown: the bay and the brown East Bay hills behind it.
    {.x0 = 2866, .x1 = 3753, .line = kLine1_0, .water = 19, .water_x0 = 2887, .water_x1 = 2905, .water_top = 19,
     .outline = 0, .rough = 1, .rough_period = 5, .block_x0 = 45, .block_x1 = 55, .block_y0 = 16, .block_y1 = 18},
    // The Bay Bridge's towers and deck: Yerba Buena Island stays.
    {.x0 = 873, .x1 = 976, .line = kLine1_1, .tol = 1, .keep = colours({0, 2, 8}), .keep_rows = 4, .keep_until = 19,
     .water = 19, .water_x0 = 2887, .water_x1 = 2905, .water_top = 19, .rough = 1, .rough_period = 5, .block_x0 = 928,
     .block_x1 = 978, .block_y0 = 17, .block_y1 = 19},
    // SoMa and its freeways: the bay and the hills.
    {.x0 = 1040, .x1 = 1400, .line = kLine1_2, .water = 19, .water_x0 = 2887, .water_x1 = 2905, .water_top = 19,
     .outline = 0, .rough = 1, .rough_period = 5, .block_x0 = 45, .block_x1 = 55, .block_y0 = 16, .block_y1 = 18},
    // Further south: hills.
    {.x0 = 1400, .x1 = 1690, .line = kLine1_3, .rough = 1, .rough_period = 6, .dots = kDots1},
    // Twin Peaks stays above the houses, without Sutro Tower.
    {.x0 = 1690, .x1 = 1900, .line = kLine1_4, .tol = 1, .keep = colours({0, 2, 8}), .rough = 1, .rough_period = 6,
     .dots = kDots1},
    // Down to the ocean.
    {.x0 = 1900, .x1 = 1940, .line = kLine1_5, .water = 21, .water_x0 = 2887, .water_x1 = 2905, .water_top = 19,
     .rough = 1, .rough_period = 7, .dots = kDots1},
    // The Sunset and the Richmond: the ocean beyond them.
    {.x0 = 1940, .x1 = 2575, .line = kLine1_6, .water = 21, .water_x0 = 2887, .water_x1 = 2905, .water_top = 19},
    // Up to the Marin hills.
    {.x0 = 2575, .x1 = 2600, .line = kLine1_7, .water = 19, .water_x0 = 2887, .water_x1 = 2905, .water_top = 19,
     .rough = 1, .rough_period = 4, .block_x0 = 2748, .block_x1 = 2816, .block_y0 = 16, .block_y1 = 18},
};
constexpr Patch kPatches1[] = {
    {975, 998, 0, 24, 0},  // a tower on the shore
    {1003, 1017, 0, 21, 0},  // a sign on its pole
    {996, 1046, 20, 24, colours({0, 1, 8})},  // piers
    {2600, 2665, 0, 12, colours({0, 4, 6, 8, 14, 10, 2})},  // the Golden Gate Bridge above the Marin hills,
    {2598, 2612, 19, 24, 0},  // its south tower's foot,
    {2598, 2626, 12, 17, colours({0})},  // its deck
    {2600, 2672, 12, 24, colours({4, 14})},  // and its red in front of the hills
    {2608, 2642, 15, 24, 0},  // a building at the water
    {2674, 2688, 0, 24, 0},  // towers on the waterfront
    {2700, 2716, 0, 24, 0},  // towers on the waterfront
    {2728, 2740, 0, 24, 0},  // towers on the waterfront
    {2740, 2836, 19, 24, colours({0, 3, 8})},  // piers
    {2640, 2740, 21, 24, colours({0, 3, 8})},  // piers
    {2836, 2850, 0, 24, 0},  // a tower
    {2846, 2866, 15, 24, 0},  // the Ferry Building and its pier
};

// HORIZON2, the ocean side.
constexpr Point kLine2_0[] = {{290, 12}, {330, 11}, {380, 11}, {420, 12}, {460, 12}, {520, 12}, {560, 10}, {600, 11},
    {641, 12}};
constexpr Point kLine2_1[] = {{700, 15}, {760, 14}, {840, 15}, {920, 14}, {1000, 16}};
constexpr Point kLine2_2[] = {{1000, 16}, {1050, 15}, {1080, 15}, {1096, 13}, {1110, 11}, {1122, 7}, {1127, 9},
    {1135, 11}, {1141, 8}, {1148, 10}, {1160, 12}, {1200, 13}, {1250, 14}, {1300, 14}, {1340, 12}, {1370, 11},
    {1400, 12}, {1440, 14}, {1480, 13}, {1520, 11}, {1545, 9}, {1570, 11}, {1600, 14}};
constexpr Dot kDots2[] = {{8, 120}, {3, 60}, {2, 30}};
const Hill kHills2[] = {
    // The Presidio's trees stay, without the Golden Gate Bridge above them and the houses below.
    {.x0 = 290, .x1 = 641, .line = kLine2_0, .tol = 2, .keep = colours({0, 2, 3, 8, 10}), .keep_rows = 24,
     .keep_until = 18, .detail_x0 = 450, .detail_x1 = 600, .block_x0 = 318, .block_x1 = 366, .block_y0 = 12,
     .block_y1 = 18},
    // Trees, without the houses below them.
    {.x0 = 700, .x1 = 1000, .line = kLine2_1, .tol = 2, .keep = colours({0, 2, 3, 8, 10}), .keep_rows = 24,
     .keep_until = 18, .detail_x0 = 450, .detail_x1 = 600, .block_x0 = 318, .block_x1 = 366, .block_y0 = 12,
     .block_y1 = 18},
    // The Sunset's houses: Twin Peaks and Mount Sutro stay, without Sutro Tower.
    {.x0 = 1000, .x1 = 1600, .line = kLine2_2, .tol = 1, .keep = colours({0, 2, 3, 8}), .rough = 1, .rough_period = 6,
     .dots = kDots2},
};
constexpr Patch kPatches2[] = {
    {162, 179, 13, 23, 0},  // buildings on the shore
    {186, 203, 15, 23, 0},  // buildings on the shore
    {223, 245, 19, 22, colours({0, 7, 8})},  // a boat
    {641, 657, 10, 24, colours({0, 7})},  // a wall by the lake
    {2016, 2034, 0, 23, colours({0, 7, 8})},  // ships
    {2251, 2274, 0, 23, colours({0, 7, 8})},  // ships
};

const Retouch kRetouches[] = {
    {0xDDD8C848C468F2CDull, kHills0, kPatches0},
    {0x61658111DE986D61ull, kHills1, kPatches1},
    {0x59407C9AF545B84Aull, kHills2, kPatches2},
};
// --- end of the retouching tables

// One panorama's working state.
class Retoucher {
public:
    Retoucher(const uint8_t* in, uint8_t* out) : orig_(in), work_(out) {
        std::memcpy(work_, orig_, kSize);
        for (int x = 0; x < kDegrees; ++x) {
            int r = 0;
            while (r < kRows && orig_[r * kWidth + x] == kSky) ++r;
            top_[static_cast<size_t>(x)] = r;
        }
    }

    void hill(const Hill& op);
    void mark(const Patch& p);
    void fill();
    void finish() {
        for (int r = 0; r < kRows; ++r) {
            std::memcpy(work_ + r * kWidth + kDegrees, work_ + r * kWidth, kWidth - kDegrees);
        }
    }

private:
    uint8_t orig(int r, int x) const { return orig_[r * kWidth + wrap(x)]; }
    int top(int x) const { return top_[static_cast<size_t>(wrap(x))]; }
    double smooth_top(int x) const {
        double s = 0;
        for (int k = -6; k <= 6; ++k) s += top(x + k);
        return s / 13;
    }
    static double line_at(std::span<const Point> line, int x) {
        if (x <= line.front().x) return line.front().row;
        if (x >= line.back().x) return line.back().row;
        for (size_t i = 1; i < line.size(); ++i) {
            if (x <= line[i].x) {
                const Point a = line[i - 1], b = line[i];
                return a.row + double(b.row - a.row) * double(x - a.x) / double(b.x - a.x);
            }
        }
        return line.back().row;
    }
    static double noise(int x, int period, int amp, int seed) {
        const auto v = [&](int i) {
            uint32_t h = static_cast<uint32_t>(i) * 374761393u + static_cast<uint32_t>(seed) * 668265263u;
            h = (h ^ (h >> 13)) * 1274126177u;
            return static_cast<int>((h >> 8) % static_cast<uint32_t>(2 * amp + 1)) - amp;
        };
        const int k = x / period;
        const double f = double(x % period) / period;
        return v(k) * (1 - f) + v(k + 1) * f;
    }
    static int dot(const Hill& op, int r, int x) {
        uint32_t h = (static_cast<uint32_t>(x) * 73856093u) ^ (static_cast<uint32_t>(r) * 19349663u) ^ 0x5bd1e995u;
        h = (h ^ (h >> 15)) * 2246822519u;
        int v = static_cast<int>((h ^ (h >> 13)) % 1000u);
        for (const Dot& d : op.dots) {
            if (v < d.permille) return d.colour;
            v -= d.permille;
        }
        return op.dots_base;
    }

    const uint8_t* orig_;
    uint8_t* work_;
    std::array<int, kDegrees> top_{};
    std::vector<uint8_t> built_ = std::vector<uint8_t>(kRows * kDegrees, 0);
    bool any_built_ = false;
};

void Retoucher::hill(const Hill& op) {
    std::vector<Point> ends;
    std::span<const Point> line = op.line;
    if (line.empty()) {
        ends = {{op.x0 - 1, top(op.x0 - 1)}, {op.x1, top(op.x1)}};
        line = ends;
    }
    // The skyline of each column, and whether it is the original's (snapped), from x0 - 1 to x1.
    const int n = op.x1 - op.x0 + 2;
    std::vector<int> sky(static_cast<size_t>(n));
    std::vector<uint8_t> snapped(static_cast<size_t>(n));
    for (int x = op.x0 - 1; x <= op.x1; ++x) {
        const auto i = static_cast<size_t>(x - op.x0 + 1);
        const double on_line = line_at(line, x);
        double base = on_line;
        if (op.detail_x0 >= 0) {
            const int d = op.detail_x0 + pmod(x - op.x0, op.detail_x1 - op.detail_x0);
            base += top(d) - smooth_top(d);
        }
        if (op.rough) base += noise(x, op.rough_period, op.rough, op.x0);
        const int t = top(x);
        const bool snap = op.tol >= 0 && std::fabs(t - on_line) <= op.tol && t < kRows && has(op.keep, orig(t, x));
        snapped[i] = snap;
        sky[i] = snap || x < op.x0 || x >= op.x1 ? t : std::clamp(round_half_up(base), 0, kRows);
    }
    const int bw = (op.block_x1 - op.block_x0) & ~1, bh = (op.block_y1 - op.block_y0) & ~1;
    for (int x = op.x0; x < op.x1; ++x) {
        const auto i = static_cast<size_t>(x - op.x0 + 1);
        const int X = wrap(x);
        const int h = sky[i];
        const bool snap = snapped[i] != 0;
        int es = -1;
        if (op.edge_x0 >= 0) {
            // A column of the edge texture whose dither phase matches here.
            const int ew = op.edge_x1 - op.edge_x0;
            es = op.edge_x0 + pmod(x - op.x0, ew);
            for (int k = 0; k < 6; ++k) {
                const int c = op.edge_x0 + pmod(es - op.edge_x0 + k, ew);
                if (((h - top(c)) + (X - c)) % 2 == 0) {
                    es = c;
                    break;
                }
            }
        }
        const int side = std::max(sky[i - 1], sky[i + 1]);
        for (int r = 0; r < kRows; ++r) {
            int v;
            if (r < h) {
                v = kSky;
            } else if (snap && r < h + op.keep_rows && r < op.keep_until && has(op.keep, orig(r, X))) {
                v = orig(r, X);
            } else if (op.water >= 0 && r >= op.water) {
                int wx = op.water_x0 + pmod(x - op.x0, op.water_x1 - op.water_x0);
                if ((wx - X) % 2 != 0) wx = wx + 1 < op.water_x1 ? wx + 1 : wx - 1;
                v = orig(std::min(kRows - 1, r - op.water + op.water_top), wx);
            } else if (op.outline >= 0 && !snap && (r == h || r < side)) {
                v = op.outline;
            } else {
                v = -1;
                if (es >= 0 && !snap && r < h + op.edge_rows) {
                    const int rr = top(es) + (r - h);
                    if (rr < kRows && has(op.keep, orig(rr, es))) v = orig(rr, es);
                }
                if (v < 0) {
                    v = op.block_x0 >= 0 ? orig(op.block_y0 + pmod(r - op.block_y0, bh), op.block_x0 + pmod(X - op.block_x0, bw))
                                         : dot(op, r, X);
                }
            }
            work_[r * kWidth + X] = static_cast<uint8_t>(v);
        }
    }
}

void Retoucher::mark(const Patch& p) {
    for (int x = p.x0; x < p.x1; ++x) {
        const int X = wrap(x);
        for (int r = p.y0; r < p.y1; ++r) {
            if (p.colours == 0 || has(p.colours, work_[r * kWidth + X])) {
                built_[static_cast<size_t>(r * kDegrees + X)] = 1;
                any_built_ = true;
            }
        }
    }
}

void Retoucher::fill() {
    if (!any_built_) return;
    const auto at = [](int r, int x) { return static_cast<size_t>(r * kDegrees + x); };
    // The sky: sky-coloured pixels joined to the top row.
    std::vector<uint8_t> sky(static_cast<size_t>(kRows * kDegrees), 0);
    std::vector<int> stack;
    for (int x = 0; x < kDegrees; ++x) {
        if (work_[x] == kSky && !built_[at(0, x)]) {
            sky[at(0, x)] = 1;
            stack.push_back(x);
        }
    }
    while (!stack.empty()) {
        const int p = stack.back();
        stack.pop_back();
        const int r = p / kDegrees, x = p % kDegrees;
        const int next[4][2] = {{r - 1, x}, {r + 1, x}, {r, (x + 1) % kDegrees}, {r, (x + kDegrees - 1) % kDegrees}};
        for (const auto& q : next) {
            if (q[0] < 0 || q[0] >= kRows) continue;
            const size_t k = at(q[0], q[1]);
            if (!sky[k] && !built_[k] && work_[q[0] * kWidth + q[1]] == kSky) {
                sky[k] = 1;
                stack.push_back(q[0] * kDegrees + q[1]);
            }
        }
    }
    // The skyline: where the first pixel below the sky is natural, it stands; elsewhere (something built on
    // it) it is joined straight across from the nearest such columns either side.
    std::vector<int> line(kDegrees, -1), known;
    for (int x = 0; x < kDegrees; ++x) {
        int r = 0;
        while (r < kRows && sky[at(r, x)]) ++r;
        if (r == kRows || !built_[at(r, x)]) {
            line[static_cast<size_t>(x)] = r;
            known.push_back(x);
        }
    }
    if (known.empty()) return;
    for (int x = 0; x < kDegrees; ++x) {
        if (line[static_cast<size_t>(x)] >= 0) continue;
        const auto it = std::lower_bound(known.begin(), known.end(), x);
        const size_t i = static_cast<size_t>(it - known.begin());
        const int xr = known[i % known.size()], xl = known[(i + known.size() - 1) % known.size()];
        const int dl = pmod(x - xl, kDegrees), dr = pmod(xr - x, kDegrees);
        const double t = double(dl) / double(dl + dr);
        line[static_cast<size_t>(x)] =
            round_half_up(line[static_cast<size_t>(xl)] * (1 - t) + line[static_cast<size_t>(xr)] * t);
    }
    // Built pixels below the skyline: the nearest run of natural pixels in their row, either side (water or
    // land, as the run's first pixel is), repeated across with a period no longer than the gap (an even
    // one keeps the dithering in phase); failing that, the pixel two rows down, or the nearest natural one
    // above.
    constexpr int kMaxPeriod = 64, kSearch = 600;
    const std::vector<uint8_t> src(work_, work_ + kSize);
    const auto ok = [&](int r, int x) { return !built_[at(r, x)] && !sky[at(r, x)]; };
    const auto water = [&](int r, int x) {  // 1 water, 0 land, 2 either (teal)
        const uint8_t c = src[static_cast<size_t>(r * kWidth + x)];
        return c == 3 ? 2 : c == 9 || c == 1 || c == 11 || c == 15 ? 1 : 0;
    };
    std::vector<int> gap(kDegrees);
    for (int r = kRows - 1; r >= 0; --r) {
        // The length of the run of built pixels each built pixel of the row is in.
        int start = -1;
        for (int x = 0; x < kDegrees && start < 0; ++x) {
            if (!built_[at(r, x)]) start = x;
        }
        std::fill(gap.begin(), gap.end(), start < 0 ? kDegrees : 0);
        for (int i = 1; start >= 0 && i <= kDegrees; ++i) {
            const int x = (start + i) % kDegrees;
            if (built_[at(r, x)] && !built_[at(r, wrap(x - 1))]) {
                int n = 0;
                while (built_[at(r, wrap(x + n))]) ++n;
                for (int k = 0; k < n; ++k) gap[static_cast<size_t>(wrap(x + k))] = n;
            }
        }
        for (int x = 0; x < kDegrees; ++x) {
            if (!built_[at(r, x)]) continue;
            uint8_t& out = work_[r * kWidth + x];
            if (r < line[static_cast<size_t>(x)]) {
                out = kSky;
                continue;
            }
            int best_d = -1, best_x = 0;
            for (const int side : {-1, 1}) {
                for (int d = 1; d <= kSearch;) {
                    const int xs = wrap(x + side * d);
                    if (!ok(r, xs)) {
                        ++d;
                        continue;
                    }
                    int run = 0;
                    const int kind = water(r, xs);
                    while (run < kMaxPeriod && ok(r, wrap(xs + side * run))) {
                        const int k = water(r, wrap(xs + side * run));
                        if (k != kind && k != 2) break;
                        ++run;
                    }
                    if (run >= 2) {
                        const int g = gap[static_cast<size_t>(x)];
                        const int period = std::min(run & ~1, std::max(2, g + (g & 1)));
                        const int k = (d + period - 1) / period;
                        if (best_d < 0 || d < best_d) {
                            best_d = d;
                            best_x = wrap(x + side * k * period);
                        }
                        break;
                    }
                    d += run;
                }
            }
            if (best_d >= 0) {
                out = src[static_cast<size_t>(r * kWidth + best_x)];
            } else if (r + 2 < kRows) {
                out = work_[(r + 2) * kWidth + x];
            } else {
                out = kSky;
                for (int rr = r - 2; rr >= 0; rr -= 2) {
                    if (ok(rr, x)) {
                        out = src[static_cast<size_t>(rr * kWidth + x)];
                        break;
                    }
                }
            }
        }
    }
}

} // namespace

uint64_t panorama_hash(const uint8_t* pixels) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < kSize; ++i) {
        h ^= pixels[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

bool landscape_panorama(const uint8_t* in, uint8_t* out) {
    const uint64_t h = panorama_hash(in);
    const Retouch* found = nullptr;
    for (const Retouch& r : kRetouches) {
        if (r.hash == h) found = &r;
    }
    Retoucher t(in, out);
    if (!found) return false;
    for (const Hill& op : found->hills) t.hill(op);
    for (const Patch& p : found->patches) t.mark(p);
    t.fill();
    t.finish();
    return true;
}

void Backdrop::load(const host::Ega& ega) {
    // render_page() of a start address in the buffer: 8000 bytes, 40 a row, as pixels.
    original_.assign(static_cast<size_t>(kBufferBytes) * 8, 0);
    host::Ega::Frame f;
    for (uint32_t at = 0; at < kBufferBytes; at += kFrameBytes) {
        ega.render_page(static_cast<uint16_t>(kBufferStart + at), f);
        if (f.width != 320 || f.height != 200) {
            original_.clear();
            return;
        }
        const size_t n = std::min<size_t>(kFrameBytes, kBufferBytes - at) * 8;
        std::memcpy(original_.data() + static_cast<size_t>(at) * 8, f.pixels.data(), n);
    }
    landscape_.resize(original_.size());
    known_ = 0;
    for (int p = 0; p < kPanoramas; ++p) {
        const size_t o = static_cast<size_t>(p) * kSize;
        if (landscape_panorama(original_.data() + o, landscape_.data() + o)) ++known_;
    }
}

int Backdrop::matching(int rows, uint32_t source, uint32_t dest, const uint8_t* pixels) const {
    int n = 0;
    for (int r = 0; r < rows; ++r) {
        const uint8_t* o = original_.data() + static_cast<size_t>(source + static_cast<uint32_t>(r) * (kWidth / 8)) * 8;
        const uint8_t* f = pixels + static_cast<size_t>(dest + static_cast<uint32_t>(r) * 40) * 8;
        for (int i = 0; i < 320; ++i) n += o[i] == f[i];
    }
    return n;
}

bool Backdrop::apply(const host::Ega& ega, int rows, uint32_t source, uint32_t dest, uint8_t* pixels, int width,
                     int height) {
    // The copy must lie within one panorama's rows and within the frame.
    if (rows <= 0 || width != 320 || height <= 0) return false;
    const uint32_t row_bytes = kWidth / 8, panorama_bytes = kRows * row_bytes;
    const uint32_t last = source + static_cast<uint32_t>(rows - 1) * row_bytes;
    if (last + 40 > kBufferBytes || source / panorama_bytes != (last + 39) / panorama_bytes ||
        (source % row_bytes) + 40 > row_bytes ||
        dest + static_cast<uint32_t>(rows) * 40 > static_cast<uint32_t>(width / 8 * height)) {
        return false;
    }
    // Most of those rows must show the panoramas as read (else they have changed: read them again). Pixels
    // that differ were drawn over the panorama afterwards and stay.
    const int enough = rows * 320 * 3 / 4;
    if (reload_wait_ > 0) --reload_wait_;
    if (original_.empty() || matching(rows, source, dest, pixels) < enough) {
        if (reload_wait_ > 0) return false;
        load(ega);
        if (original_.empty() || matching(rows, source, dest, pixels) < enough) {
            reload_wait_ = 60;
            return false;
        }
    }
    for (int r = 0; r < rows; ++r) {
        const size_t s = static_cast<size_t>(source + static_cast<uint32_t>(r) * row_bytes) * 8;
        uint8_t* f = pixels + static_cast<size_t>(dest + static_cast<uint32_t>(r) * 40) * 8;
        for (size_t i = 0; i < 320; ++i) {
            if (f[i] == original_[s + i]) f[i] = landscape_[s + i];
        }
    }
    return true;
}

} // namespace vette::enhanced
