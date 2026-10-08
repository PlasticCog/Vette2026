#include "game/drivable.h"

#include <algorithm>
#include <array>
#include <numeric>

#include "game/x86.h"
#include "host/memory.h"

namespace vette::game {
namespace {

constexpr uint16_t kCellTypes = 0x9D73;   // 256 near pointers to the cell type records
constexpr uint16_t kBoxLists = 0xC0A6;    // collision class -> box list
constexpr int kCellSize = 0x800;

// The on-ramps' boxes and their routes (3009:1A12-1AD8).
struct Ramp {
    uint16_t box;
    int route;
};
constexpr std::array<Ramp, 9> kRamps = {{{0xC294, 0}, {0xC278, 1}, {0xC2BE, 2}, {0xC554, 3}, {0xC5F4, 4}, {0xC5E2, 5},
                                         {0xC610, 6}, {0xC62C, 7}, {0xC64E, 8}}};
// The other triggers (3009:1984-1B6E): finishes, toll booths, bridge decks, and two of unknown use.
constexpr std::array<uint16_t, 12> kTriggers = {0xC5AC, 0xC28A, 0xC5B6, 0xC29C, 0xC2A4, 0xC2AC,
                                                0xC63C, 0xC67C, 0xC66A, 0xC210, 0xC3BC, 0xC358};

}  // namespace

int freeway_ramp_route(uint16_t box) {
    for (const auto& [b, route] : kRamps) {
        if (b == box) return route;
    }
    return -1;
}

bool collision_box_passable(uint16_t box) {
    return freeway_ramp_route(box) >= 0 || std::find(kTriggers.begin(), kTriggers.end(), box) != kTriggers.end();
}

std::vector<PlacedBox> collision_boxes(host::Memory& m, const CityMap& map) {
    std::vector<PlacedBox> out;
    for (int cx = 0; cx < CityMap::kCellsX; ++cx) {
        for (int cy = 0; cy < CityMap::kCellsY; ++cy) {
            const uint16_t record = rd16(m, kDataSeg, static_cast<uint16_t>(kCellTypes + 2 * map.cell(cx, cy).type));
            const uint8_t cls = rd8(m, kDataSeg, static_cast<uint16_t>(record + 1));
            uint16_t box = rd16(m, kDataSeg, static_cast<uint16_t>(kBoxLists + 2 * cls));
            for (int k = 0; k < 64 && rd16(m, kDataSeg, box) != 0xFFFF; ++k, box = static_cast<uint16_t>(box + 8)) {
                const auto v = [&](int off) { return static_cast<int16_t>(rd16(m, kDataSeg, static_cast<uint16_t>(box + off))); };
                out.push_back({cx, cy, box, cx * kCellSize + v(0), cy * kCellSize + v(2), cx * kCellSize + v(4),
                               cy * kCellSize + v(6)});
            }
        }
    }
    return out;
}

bool point_clear(const std::vector<PlacedBox>& boxes, int32_t x, int32_t y, int margin) {
    for (const PlacedBox& b : boxes) {
        if (x > b.x0 - margin && x < b.x1 + margin && y > b.y0 - margin && y < b.y1 + margin && !collision_box_passable(b.box))
            return false;
    }
    return true;
}

DrivableMap find_drivable(host::Memory& m, const CityMap& map, int margin) {
    DrivableMap d;
    constexpr int kStep = DrivableMap::kStep;
    d.n = CityMap::kCellsX * kCellSize / kStep;
    const int n = d.n;
    std::vector<uint8_t> blocked(static_cast<size_t>(n) * static_cast<size_t>(n), 0);
    for (const PlacedBox& b : collision_boxes(m, map)) {
        if (collision_box_passable(b.box)) continue;
        const int i0 = std::max(0, (b.x0 - margin) / kStep), i1 = std::min(n - 1, (b.x1 + margin) / kStep);
        const int j0 = std::max(0, (b.y0 - margin) / kStep), j1 = std::min(n - 1, (b.y1 + margin) / kStep);
        for (int i = i0; i <= i1; ++i) {
            for (int j = j0; j <= j1; ++j) blocked[static_cast<size_t>(i * n + j)] = 1;
        }
    }
    // Connected regions (4-neighbours), then renumbered largest first.
    d.region.assign(blocked.size(), -1);
    std::vector<int> sizes;
    std::vector<int> stack;
    for (int start = 0; start < n * n; ++start) {
        if (blocked[static_cast<size_t>(start)] || d.region[static_cast<size_t>(start)] >= 0) continue;
        const int id = static_cast<int>(sizes.size());
        int size = 0;
        stack.assign(1, start);
        d.region[static_cast<size_t>(start)] = id;
        while (!stack.empty()) {
            const int s = stack.back();
            stack.pop_back();
            ++size;
            const int i = s / n, j = s % n;
            const int next[4][2] = {{i - 1, j}, {i + 1, j}, {i, j - 1}, {i, j + 1}};
            for (const auto& q : next) {
                if (q[0] < 0 || q[1] < 0 || q[0] >= n || q[1] >= n) continue;
                const int t = q[0] * n + q[1];
                if (blocked[static_cast<size_t>(t)] || d.region[static_cast<size_t>(t)] >= 0) continue;
                d.region[static_cast<size_t>(t)] = id;
                stack.push_back(t);
            }
        }
        sizes.push_back(size);
    }
    std::vector<int> order(sizes.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return sizes[static_cast<size_t>(a)] > sizes[static_cast<size_t>(b)]; });
    std::vector<int> rank(sizes.size());
    for (size_t k = 0; k < order.size(); ++k) rank[static_cast<size_t>(order[k])] = static_cast<int>(k);
    for (int32_t& r : d.region) {
        if (r >= 0) r = rank[static_cast<size_t>(r)];
    }
    d.region_size.resize(sizes.size());
    for (size_t k = 0; k < order.size(); ++k) d.region_size[k] = sizes[static_cast<size_t>(order[k])];
    return d;
}

}  // namespace vette::game
