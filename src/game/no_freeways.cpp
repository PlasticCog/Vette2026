#include "game/no_freeways.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "game/drivable.h"
#include "game/x86.h"
#include "host/machine.h"

namespace vette::game {
namespace {

using host::Cpu;
using host::Memory;

constexpr int32_t kTile = 0x8000;  // a big tile's side, in world units

// --- The roads ------------------------------------------------------------------------------------------
// Cell types used: 29 the Golden Gate approach's deck (posts only), 0F and 10 the walls either side of it,
// 00 nothing (a wall), 34 a Marina car park, 3E park, C1 the diagonal street beside the barrier.
struct CellEdit {
    int cx, cy;
    uint8_t type;
};
constexpr CellEdit kRoads[] = {
    // The causeway up the coast, column 2: the Presidio on-ramp's cell and the water up to the Marina...
    {27, 2, 0x29},
    {28, 1, 0x0F}, {28, 2, 0x29}, {28, 3, 0x10},
    {29, 1, 0x0F}, {29, 2, 0x29}, {29, 3, 0x10},
    {30, 1, 0x0F}, {30, 2, 0x29}, {30, 3, 0x10},
    {31, 1, 0x0F}, {31, 2, 0x29}, {31, 3, 0x10},
    {37, 2, 0x34},  // (a wall between the car parks and the Marina's street)
    // ...and the Golden Gate's deck carried on from the approach down to the Marina's street.
    {39, 1, 0x0F}, {39, 2, 0x29}, {39, 3, 0x00},
    {40, 1, 0x0F}, {40, 2, 0x29}, {40, 3, 0x00},
    {41, 1, 0x0F}, {41, 2, 0x29}, {41, 3, 0x00},
    {42, 1, 0x0F}, {42, 2, 0x29}, {42, 3, 0x00},
    // The Marina's street into the park beside the central city.
    {38, 9, 0x3E},
    // Golden Gate Park into the central city.
    {17, 15, 0x3E}, {18, 15, 0x3E}, {19, 15, 0x3E}, {20, 15, 0x3E},
    // The diagonal barrier before the Bay Bridge's end of town, made a street.
    {16, 37, 0xC1}, {17, 38, 0xC1}, {18, 39, 0xC1}, {19, 40, 0xC1},
    {20, 41, 0xC1}, {21, 42, 0xC1}, {22, 43, 0xC1}, {23, 44, 0xC1},
};

// --- The opponent's routes ---------------------------------------------------------------------------------
// DS:F7B8[course - 1] -> 3 route lists (by level); a route list: pairs {w waypoint list, w freeway, FFFF
// none} until FFFF (notes 04 section 5). A waypoint list: an 8-byte first target, then {w x, w y} points
// until FFFF, 16-bit and relative to the big tile the opponent is in; a point past the tile's edge is still
// in that tile's terms, the points after it in the next tile's (the opponent's targets move by 8000h as it
// crosses). The opponent reads the next point once its target, which runs up to about 2300 units ahead of
// it, nears the current one: a point past an edge has to lie well past it (these lie 6000 or more), or the
// next is read before the opponent has crossed, in the old tile's terms. (One of the original's lists does
// that just before a freeway, where it never mattered.) The freeways' own segment lists (DS:7560's) are
// free for the new lists: nothing reads them.
constexpr uint16_t kRouteTables = 0xF7B8;
constexpr int kCourses = 4, kLevels = 3;
constexpr uint16_t kScratch = 0x7602, kScratchEnd = 0x7A70;

struct Point {
    int32_t x, y;  // absolute
};
struct Leg {
    std::array<uint16_t, 3> lists;  // the waypoint lists whose end the freeway followed (0: none)
    int freeway;
    int keep;                       // their points kept (-1: all)
    int tile_row, tile_col;         // the big tile the opponent is in after those
    std::vector<Point> road;        // on along the roads to where the freeway put it back
};
// Each road starts at its list's last point and turns at street corners; the cells are in kRoads' notes
// and in game/drivable's regions (vette_world --drivable --no-freeways walks them).
const std::vector<Leg>& legs() {
    static const std::vector<Leg> kLegs = {
        // The Presidio freeway (Great Highway, cell 27,2 -> the Golden Gate's approach, 46,2): straight up
        // the causeway (a little west of the Great Highway's line, clear of the car parks' posts).
        {{0xF9E2, 0, 0}, 3, -1, 1, 0, {{63488, 4160}, {71680, 4160}, {77824, 4160}, {86016, 4160}, {94720, 4224}}},
        // Doyle Drive (the approach, 45.9,2 -> the park's corner, 41.9,13): down to the Marina's street,
        // along it into the park, up to the street north of the park, and east. Like the original's, the
        // lines run near the cells' edges (the planner lays its lanes by them), the Marina's 160 inside
        // its street. (The list's last two points are left out: they cross into big tile 2,0 only 2176
        // past its edge.)
        {{0xF9A2, 0, 0},
         0,
         3,
         3,
         0,
         {{86016, 4224}, {77984, 4224}, {77984, 12288}, {77984, 20480}, {77984, 24700}, {82944, 24700},
          {85792, 24700}, {85792, 26624}}},
        // 480 (cell 25.9,42 -> the Bay Bridge, 18.9,51): down the street east of column 41, through the
        // diagonal, then east onto the bridge's lane.
        {{0xF852, 0xF8A4, 0xF8EE},
         1,
         -1,
         1,
         2,
         {{53120, 85936}, {45056, 85936}, {38784, 85936}, {38784, 94208}, {38784, 100352}, {38784, 104448}}},
        // 280 (the bridge's west end, 18.9,48 -> by the Zoo, 1.95,6): west along the street off the bridge,
        // through the diagonal and the city into Golden Gate Park, then south down the Sunset to the Zoo.
        {{0xF94E, 0, 0},
         2,
         -1,
         1,
         3,
         {{38784, 90112}, {38784, 77824}, {38784, 69632}, {38784, 61440}, {38784, 53248}, {38784, 45056},
          {38784, 36864}, {38784, 28672}, {38784, 20480}, {38784, 12200}, {30720, 12200}, {20480, 12200},
          {12288, 12200}, {4000, 12200}, {4000, 12288}}},
    };
    return kLegs;
}

// A list's points, with the frame convention: each point relative to the tile the opponent is in, which
// becomes the point's own tile after it.
void encode(int row, int col, const std::vector<Point>& points, std::vector<uint16_t>& out) {
    for (const Point& p : points) {
        out.push_back(static_cast<uint16_t>(p.x - row * kTile));
        out.push_back(static_cast<uint16_t>(p.y - col * kTile));
        row = p.x / kTile;
        col = p.y / kTile;
    }
}

void route_opponent(Memory& m) {
    uint16_t at = kScratch;
    for (const Leg& leg : legs()) {
        for (const uint16_t list : leg.lists) {
            if (list == 0) continue;
            // The list as it is, then the road, then FFFF.
            std::vector<uint16_t> words;
            for (int i = 0; i < 4; ++i) words.push_back(rd16(m, kDataSeg, static_cast<uint16_t>(list + 2 * i)));
            uint16_t p = static_cast<uint16_t>(list + 8);
            for (int n = 0; n < (leg.keep < 0 ? 200 : leg.keep) && rd16(m, kDataSeg, p) != 0xFFFF;
                 ++n, p = static_cast<uint16_t>(p + 4)) {
                words.push_back(rd16(m, kDataSeg, p));
                words.push_back(rd16(m, kDataSeg, static_cast<uint16_t>(p + 2)));
            }
            encode(leg.tile_row, leg.tile_col, leg.road, words);
            words.push_back(0xFFFF);
            if (at + 2 * words.size() > kScratchEnd) return;  // (it fits: about 460 of 1134 bytes)
            const uint16_t extended = at;
            for (const uint16_t w : words) {
                wr16(m, kDataSeg, at, w);
                at = static_cast<uint16_t>(at + 2);
            }
            // Every route that took the freeway after this list takes the extended list instead (the
            // freeway's id stays: the route goes on to its next list there, see install_no_freeways).
            for (int course = 0; course < kCourses; ++course) {
                const uint16_t table = rd16(m, kDataSeg, static_cast<uint16_t>(kRouteTables + 2 * course));
                for (int level = 0; level < kLevels; ++level) {
                    uint16_t pair = rd16(m, kDataSeg, static_cast<uint16_t>(table + 2 * level));
                    for (int n = 0; n < 64 && rd16(m, kDataSeg, pair) != 0xFFFF; ++n, pair = static_cast<uint16_t>(pair + 4)) {
                        if (rd16(m, kDataSeg, pair) == list &&
                            rd16(m, kDataSeg, static_cast<uint16_t>(pair + 2)) == static_cast<uint16_t>(leg.freeway))
                            wr16(m, kDataSeg, pair, extended);
                    }
                }
            }
        }
    }
}

constexpr uint16_t kEntrySeg = emu_seg(0x3009), kEntry = 0x0025;  // start, after the image unpacks
constexpr uint16_t kRampChecks = 0x1A12, kHandlerEnd = 0x1D1B;      // the collision handler's on-ramp part
// The opponent's route at a list's end, two ways there (the opponent at its last point, 3009:10B3; its
// target there, 112A): FFFF ends the course; a freeway's id takes it onto the freeway (10C3 / 1153, 4021:01B7,
// whose exit starts the next list); course 4's chaining starts the next list straight away (10DB: F7C0 += 2,
// FA22 = the next list, 206E; 116B the same, counting a leg in DS:2D31, which the watch steps past to 1174).
constexpr uint16_t kOntoFreeway = 0x10C3, kNextList = 0x10DB;
constexpr uint16_t kOntoFreeway2 = 0x1153, kNextList2 = 0x1174;
constexpr uint16_t kRoutePos = 0xF7C0;  // the route list's place

}  // namespace

void add_no_freeway_roads(CityMap& map) {
    for (const CellEdit& e : kRoads) map.cell(e.cx, e.cy).type = e.type;
}

std::vector<std::vector<std::pair<int32_t, int32_t>>> no_freeway_opponent_roads() {
    std::vector<std::vector<std::pair<int32_t, int32_t>>> out;
    for (const Leg& leg : legs()) {
        out.emplace_back();
        for (const Point& p : leg.road) out.back().emplace_back(p.x, p.y);
    }
    return out;
}

void install_no_freeways(host::Machine& machine) {
    Cpu& cpu = machine.cpu();
    cpu.add_watch(Cpu::linear(kEntrySeg, kEntry), [](Cpu& c) {
        std::string error;
        auto map = CityMap::read(c.memory(), error);
        if (!map) return;  // (not the build these addresses are for)
        add_no_freeway_roads(*map);
        map->write(c.memory());
        route_opponent(c.memory());
    });
    // The opponent at a list's end that a freeway followed: on to the next list, as course 4 chains them.
    // The extended lists end where the freeway would have put it back.
    cpu.add_watch(Cpu::linear(kEntrySeg, kOntoFreeway), [](Cpu& c) { c.regs.ip = kNextList; });
    cpu.add_watch(Cpu::linear(kEntrySeg, kOntoFreeway2), [](Cpu& c) {
        wr16(c.memory(), kDataSeg, kRoutePos, static_cast<uint16_t>(rd16(c.memory(), kDataSeg, kRoutePos) + 2));
        c.regs.ip = kNextList2;
    });
    // An on-ramp's box does nothing: on to the handler's end, as the original does with a ramp a course
    // keeps closed (1A31, 1A60).
    cpu.add_watch(Cpu::linear(kEntrySeg, kRampChecks), [](Cpu& c) {
        if (freeway_ramp_route(c.regs.r[host::BX]) >= 0) c.regs.ip = kHandlerEnd;
    });
}

}  // namespace vette::game
