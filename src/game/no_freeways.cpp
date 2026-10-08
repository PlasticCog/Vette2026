#include "game/no_freeways.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <memory>
#include <span>
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
// A cell type (DS:9D73[type], notes 05) is a header (low byte the ground's shape, high byte the collision
// class: DS:C0A6[class] -> its boxes) and two lists of {w routine, dx, dy, dz} .. FFFF, the pieces drawn at
// those places in the cell (x north, y east). The city's blocks are made of four: 7270 the north-south
// street along the cell's west edge (1792 x 256, its lane lines at 64, 128 and 192), 7318 the east-west
// one along its north edge (256 x 1792), 7238 their corner (256 x 256), 276A the block (1792 x 1792,
// land grey). Three more are made of them, in types no map uses, with collision class 2C (no boxes):
constexpr uint8_t kCoastRoad = 0x50, kCoastCrossing = 0x54, kMarinaStreet = 0x55, kParkStreet = 0x56;
constexpr uint16_t kCellTypes = 0x9D73, kNoBoxes = 0x2C00;
struct Piece {
    uint16_t routine;
    int16_t dx, dy, dz;
};
struct NewType {
    uint8_t type;
    std::vector<Piece> pieces;  // list 1; list 2 is empty
};
const std::vector<NewType>& new_types() {
    static const std::vector<NewType> kTypes = {
        // The coast road: 320 wide along the west edge (2655, the Marina car parks' wide lane, with the
        // street's lines over it: lanes of 64 for the Golden Gate's cars, 20h-120h), the corner squares on
        // north, land beyond.
        {kCoastRoad,
         {{0x276A, 0, 256, 0}, {0x276A, 256, 256, 0}, {0x2655, 0, 0, 0}, {0x7270, 0, 0, 0}, {0x7238, 1792, 0, 0},
          {0x7238, 1792, 64, 0}}},
        // ...where Marina Boulevard crosses it (along the cell's south edge, as the original's waypoints run).
        {kCoastCrossing,
         {{0x276A, 0, 256, 0}, {0x276A, 256, 256, 0}, {0x7318, 0, 256, 0}, {0x2655, 0, 0, 0}, {0x7270, 0, 0, 0},
          {0x7238, 1792, 0, 0}, {0x7238, 1792, 64, 0}}},
        // Marina Boulevard: the street along the cell's south edge, a wide pavement beyond.
        {kMarinaStreet, {{0x276A, 256, 0, 0}, {0x276A, 256, 256, 0}, {0x7318, 0, 0, 0}, {0x7238, 0, 1792, 0}}},
        // ...and through the park (2724 the park's grass, the whole cell).
        {kParkStreet, {{0x2724, 0, 0, 0}, {0x7318, 0, 0, 0}, {0x7238, 0, 1792, 0}}},
    };
    return kTypes;
}

struct CellEdit {
    int cx, cy;
    uint8_t type;
};
constexpr CellEdit kRoads[] = {
    // The coast road from the Great Highway's end (the Presidio freeway's ramp) over the water, the
    // Marina's car parks and green, to the Golden Gate's approach. The water either side is the
    // original's (as beside the Great Highway).
    {27, 2, kCoastRoad}, {28, 2, kCoastRoad}, {29, 2, kCoastRoad}, {30, 2, kCoastRoad}, {31, 2, kCoastRoad},
    {32, 2, kCoastRoad}, {33, 2, kCoastRoad}, {34, 2, kCoastRoad}, {35, 2, kCoastRoad}, {36, 2, kCoastRoad},
    {37, 2, kCoastRoad}, {38, 2, kCoastCrossing},
    {39, 2, kCoastRoad}, {40, 2, kCoastRoad}, {41, 2, kCoastRoad}, {42, 2, kCoastRoad},
    // Marina Boulevard, row 38 (the original's: nothing drawn, so the bay showed), on through the barrier
    // and the park beside the central city to its streets.
    {38, 0, kMarinaStreet}, {38, 1, kMarinaStreet}, {38, 3, kMarinaStreet}, {38, 4, kMarinaStreet},
    {38, 5, kMarinaStreet}, {38, 6, kMarinaStreet}, {38, 7, kMarinaStreet}, {38, 8, kMarinaStreet},
    {38, 9, kParkStreet}, {38, 10, kParkStreet}, {38, 11, kParkStreet},
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
constexpr uint16_t kTypeRecords = kScratchEnd - 0x100;  // the new cell types' records, at the end

struct Point {
    int32_t x, y;  // absolute
};
struct Leg {
    std::array<uint16_t, 3> lists;  // the waypoint lists whose end the freeway followed (0: none)
    int freeway;
    int keep;                       // their points kept (-1: all)
    int tile_row, tile_col;         // the big tile the opponent is in after those
    std::vector<Point> road;        // on along the roads to where the freeway put it back
    int32_t drives_east = 0;        // where the opponent drives, east of the road as given (for the checks)
};
// Each road starts at its list's last point and turns at street corners; the cells are in kRoads' notes
// and in game/drivable's regions (vette_world --drivable --no-freeways walks them).
const std::vector<Leg>& legs() {
    static const std::vector<Leg> kLegs = {
        // The Presidio freeway (Great Highway, cell 27,2 -> the Golden Gate's approach, 46,2): straight up
        // the causeway on the Great Highway's line, the bridge's (the planner keeps 32 to the right of it,
        // between the car parks' posts; it can't be moved over without a turn at a corner).
        {{0xF9E2, 0, 0}, 3, -1, 1, 0, {{63488, 4224}, {71680, 4224}, {77824, 4224}, {86016, 4224}, {94720, 4224}}, 30},
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

// Returns where the scratch space is free from.
uint16_t route_opponent(Memory& m) {
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
            if (at + 2 * words.size() > kScratchEnd) return at;  // (it fits: about 460 of 1134 bytes)
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
    return at;
}

// --- Traffic and pedestrians -------------------------------------------------------------------------------
// Lists (notes 04 section 7): DS:EF5A[bt] -> {w entity, w cell (x*16+y in the big tile)} .. FFFF, slots 0-2
// the player, the opponent, the chase car, slot 3 a patrol car; DS:EF8C[bt] the same for pedestrians.
// The city's list F01E (big tiles 0, 5, 6, 7, 11, 12) ends where the bridge's F0B0 begins, so it's copied
// to the scratch space with the Golden Gate's cars added. The Marina's big tile gets a list of its own
// there: the Golden Gate's, with the Bay Bridge's cars (F0E6's) added for Marina Boulevard.
constexpr uint16_t kListsA = 0xEF5A, kListsB = 0xEF8C;
constexpr uint16_t kCityList = 0xF01E, kBridgeList = 0xF0B0, kBayList = 0xF0E6;
constexpr uint16_t kCityPedestrians = 0xEFC0, kNoPedestrians = 0xEFBE;
constexpr int kBigTiles = 25;
// The Golden Gate's cars (F0B0's slots 4-12; slot 3 is the bridge's patrol car), records 2Eh apart, and
// the pedestrians (EFC0's, 20h apart).
constexpr uint16_t kBridgeCars = 0xEB56, kBridgeCarSize = 0x2E;
constexpr int kBridgeCarCount = 9;
constexpr uint16_t kPedestrians = 0xE8E8, kPedestrianSize = 0x20;
constexpr int kPedestrianCount = 16;
bool bridge_car(uint16_t e) {
    return e >= kBridgeCars && e < kBridgeCars + kBridgeCarCount * kBridgeCarSize && (e - kBridgeCars) % kBridgeCarSize == 0;
}
// The Bay Bridge's cars (F0E6's slots 4-12; its patrol car, slot 3, stays on the bridge): never ruled by the
// cells (the bridges' lists aren't), driving straight on, east (west on course 3: BD69 sets their heading).
constexpr uint16_t kBayCars = 0xED68;
bool bay_car(uint16_t e) {
    return e >= kBayCars && e < kBayCars + kBridgeCarCount * kBridgeCarSize && (e - kBayCars) % kBridgeCarSize == 0;
}
constexpr uint16_t kBridgePatrol = 0xEB28;  // the Golden Gate's, F0B0's slot 3
bool golden_gate(uint16_t e) { return bridge_car(e) || e == kBridgePatrol; }
bool pedestrian(uint16_t e) {
    return e >= kPedestrians && e < kPedestrians + kPedestrianCount * kPedestrianSize && (e - kPedestrians) % kPedestrianSize == 0;
}

// The coast road the bridge's cars drive: column 2 (their lanes, 20h-120h into its cells, are the
// approach's, the causeway's and the Great Highway's), from the Zoo (row 0 is the map's southern wall)
// to the approach (43), where the original's own rules take over.
constexpr int kCoastColumn = 2, kCoastFrom = 1, kApproach = 43, kBridgeTiles = 48;

// Marina Boulevard (row 38, cells 0-11), with the Bay Bridge's cars. Its road runs along the cells' south
// edge, 256 wide (x 0-100h); the bridge's six lanes, 64 apart along its cells' north edge (x 6A0h-7E0h),
// go into it 0.56 as far apart, from x 32 to 212 (no_freeway_lane_x), so a bus in the outer ones (44
// wide) keeps to the asphalt (its kerbs about 16 in). The bridge's own big tiles (8, 9: map
// rows 16-31, columns 48-79) keep them as the original has them.
constexpr int kMarinaRow = 38, kMarinaTo = 11;
bool marina_cell(int gx, int gy) { return gx == kMarinaRow && gy >= 0 && gy <= kMarinaTo; }
bool bay_bridge_cell(int gx, int gy) { return gx >= 16 && gx < 32 && gy >= 48 && gy < 80; }
constexpr int kBayLaneFirst = 0x6A0, kBayLaneLast = 0x7E0, kMarinaLaneFirst = 32, kMarinaLaneLast = 212;

// The city cars' rules (traffic_intersection 3009:BF10): a byte per cell, D2AE[bt] -> 256 bytes (x*16+y).
// 0 hides the car there; else bits 0-1 rule heading north or south (1 north only, 2 south only, 3 both,
// on the right: a car on the wrong side turns round), bits 4-5 east or west (1 west only, 2 east only,
// 3 both); a car the rule forbids turns round.
constexpr uint16_t kCellRules = 0xD2AE;
struct RuleEdit {
    int cx, cy;
    uint8_t rule;
};
// The bytes also choose the dashboard's one-way and turn signs (3009:65E9), and the computer opponent
// steers round the city's cars it meets (3009:D8FD): a corner where they turn back is one it can't get
// through.
constexpr RuleEdit kRuleEdits[] = {
    // Beyond the Great Highway's end, the water the city's cars drove onto as onto the Presidio
    // freeway's ramp (out of sight at 30,2): now the causeway, the coast road's cars'. The city's go out
    // of sight as they come onto it.
    {28, 2, 0x00},
    {29, 2, 0x00},
    // The new diagonal street: the old one's rule beside it (as its last cell, 23,42): north and west
    // only, so the city's cars turn back there rather than go on into the blocks beyond, which have none.
    {16, 37, 0x11}, {17, 38, 0x11}, {18, 39, 0x11}, {19, 40, 0x11},
    {20, 41, 0x11}, {21, 42, 0x11}, {22, 43, 0x11}, {23, 44, 0x11},
};

// The traffic step (traffic_step 3009:BCFB): the camera's big tile's list ([843A] = bt * 2), each car from
// slot 3 (BD10, the entity at BD24), returning at BDAB. It keeps the list in EF50, the slot count in EF4E,
// slot 3's moving flag in EF4C, the step and cell in F002/F004.
constexpr uint16_t kTrafficStep = 0xBCFB, kTrafficNext = 0xBD10, kTrafficEntity = 0xBD24, kTrafficEnd = 0xBDAB;
constexpr uint16_t kCameraTile = 0x843A, kStepList = 0xEF50;
constexpr uint16_t kStepState[] = {0xEF4C, 0xEF4E, 0xEF50, 0xF002, 0xF004};
// The bridges' cars are stepped every frame, wherever the camera is: after the camera's list, a pass for
// each bridge's (unless it was the camera's), as if the camera were there. The Marina's list (its tile's) is
// the Golden Gate's to the game: no cell rules for it (BF22), the bridges' police (1257); the Golden Gate's
// cars and patrol car are stepped with it while the camera is in the Marina.
constexpr uint16_t kMarinaTile = 10;  // the Marina's big tile (the Golden Gate's list in the original)
constexpr uint16_t kGoldenGateTile = 15, kBayBridgeTile = 8;  // the passes', the bridges' own tiles
constexpr uint16_t kRulesSkip = 0xBF22;   // cmp ax, F0B0: the list being stepped (AX) is the Golden Gate's?
constexpr uint16_t kPoliceStart = 0x1257;  // cmp bp, F0B0: the camera's list (BP) is?

// The view's gathers (collect_vehicles 32F8, collect_pedestrians 34D6), each car or pedestrian against the
// drawn cell (big tile * 2 in DS:3556, cell in 2CC9): the pattern's match for a car from slot 3, away from
// the camera's cell (3382) and in it (3414, where the collision test follows), and for a pedestrian (34FC,
// 3531); and where each goes on to the next entry.
constexpr uint16_t kDrawnTile = 0x3556, kDrawnCell = 0x2CC9;
struct Gather {
    uint16_t match, next;
};
constexpr Gather kCarGathers[] = {{0x3382, 0x3391}, {0x3414, 0x341D}};
// ...and where a car's x in its cell has been read (AX, masked next; SI past it: the entity + 4), before the
// cell's base is added: in the window (33AE) and in the camera's own cell (348E, the collision test after).
constexpr uint16_t kCarX[] = {0x33AE, 0x348E};
constexpr Gather kPedestrianGathers[] = {{0x34FC, 0x3515}, {0x3531, 0x353B}};
// The computer opponent's look ahead for traffic (3009:D8FD, on once it has been in view) gathers the
// cars of its big tile's list in its pattern cell as if they were in its own (D94C; a match at D95B, SI the
// entry, on to the next at D965), and for one in its 64-unit column ahead moves its target 64 to the side
// (DA40..DAAF; west going north, across the oncoming lanes). The coast road's cars count only on the
// approach and the bridge, as in the original: on the Great Highway and the causeway, with a corner every
// cell, a sidestep sends the planner round the block (once into the water beside the causeway, half a
// minute lost), so there it overtakes them in their lane. The opponent: big tile DS:2F2B/2F2D, place
// 2F09/2F0B.
constexpr Gather kOpponentLook = {0xD95B, 0xD965};
constexpr uint16_t kLookX = 0xD971;  // its car's x read (AX), about to be masked; BX the entity + 2
constexpr uint16_t kOpponentX = 0x2F09, kOpponentY = 0x2F0B, kOpponentRow = 0x2F2B, kOpponentCol = 0x2F2D;

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

void add_no_freeway_cell_types(Memory& m) {
    uint16_t at = kTypeRecords;
    for (const NewType& t : new_types()) {
        wr16(m, kDataSeg, static_cast<uint16_t>(kCellTypes + 2 * t.type), at);
        std::vector<uint16_t> words = {kNoBoxes};
        for (const Piece& p : t.pieces) {
            for (const int16_t w : {static_cast<int16_t>(p.routine), p.dx, p.dy, p.dz}) words.push_back(static_cast<uint16_t>(w));
        }
        words.push_back(0xFFFF);
        words.push_back(0xFFFF);
        for (const uint16_t w : words) {
            wr16(m, kDataSeg, at, w);
            at = static_cast<uint16_t>(at + 2);
        }
    }
}

void add_no_freeway_roads(CityMap& map) {
    for (const CellEdit& e : kRoads) map.cell(e.cx, e.cy).type = e.type;
}

std::vector<std::vector<std::pair<int32_t, int32_t>>> no_freeway_opponent_roads() {
    std::vector<std::vector<std::pair<int32_t, int32_t>>> out;
    for (const Leg& leg : legs()) {
        out.emplace_back();
        for (const Point& p : leg.road) out.back().emplace_back(p.x, p.y + leg.drives_east);
    }
    return out;
}

Placement no_freeway_placement(uint16_t entity, int gx, int gy, uint8_t type) {
    if (bridge_car(entity)) {
        if (gy == kCoastColumn) return gx < kCoastFrom ? Placement::Deny : gx < kApproach ? Placement::Allow : Placement::Default;
        return gx < kBridgeTiles ? Placement::Deny : Placement::Default;  // (the city's and the Marina's streets)
    }
    if (bay_car(entity)) {
        if (marina_cell(gx, gy)) return Placement::Allow;
        return bay_bridge_cell(gx, gy) ? Placement::Default : Placement::Deny;
    }
    if (pedestrian(entity)) {
        // Not on the water (the Marina's harbour too: 2B, 2D, 2E), the coast road (as on a freeway), the
        // bridge's deck (29, 2A, 11) or the walls and barriers.
        switch (type) {
        case 0x00: case 0x01: case 0x0F: case 0x10: case 0x11: case 0x29: case 0x2A: case 0x2B: case 0x2D: case 0x2E:
        case 0xF6: case 0xF7: case 0xF8: case 0xF9: case 0xFA: case 0xFF: case kCoastRoad: case kCoastCrossing:
            return Placement::Deny;
        default:
            return Placement::Default;
        }
    }
    return Placement::Default;
}

void install_no_freeway_drawing(Cpu& cpu) {
    const auto denied = [](Cpu& c) {
        Memory& m = c.memory();
        const int bt = rd16(m, kDataSeg, kDrawnTile) / 2;
        const uint16_t cell = rd16(m, kDataSeg, kDrawnCell);
        if (bt < 0 || bt >= kBigTiles) return false;
        const int cx = cell >> 4 & 15, cy = cell & 15;
        const uint16_t design = rd16(m, kDataSeg, static_cast<uint16_t>(0x8524 + 2 * bt));
        const uint8_t type = rd8(m, kDataSeg, static_cast<uint16_t>(design + cx * 32 + cy * 2));
        return no_freeway_placement(c.regs.r[host::SI], bt / 5 * 16 + cx, bt % 5 * 16 + cy, type) == Placement::Deny;
    };
    for (const auto& gathers : {std::span<const Gather>(kCarGathers), std::span<const Gather>(kPedestrianGathers)}) {
        for (const Gather& g : gathers) {
            cpu.add_watch(Cpu::linear(kEntrySeg, g.match), [denied, next = g.next](Cpu& c) {
                if (denied(c)) c.regs.ip = next;
            });
        }
    }
    for (const uint16_t at : kCarX) {
        cpu.add_watch(Cpu::linear(kEntrySeg, at), [](Cpu& c) {
            Memory& m = c.memory();
            const int bt = rd16(m, kDataSeg, kDrawnTile) / 2;
            const uint16_t cell = rd16(m, kDataSeg, kDrawnCell);
            if (bt < 0 || bt >= kBigTiles) return;
            const int gx = bt / 5 * 16 + (cell >> 4 & 15), gy = bt % 5 * 16 + (cell & 15);
            const auto entity = static_cast<uint16_t>(c.regs.r[host::SI] - 4);
            c.regs.r[host::AX] = static_cast<uint16_t>(no_freeway_lane_x(entity, gx, gy, c.regs.r[host::AX] & 0x7FF));
        });
    }
}

int no_freeway_lane_x(uint16_t entity, int gx, int gy, int x) {
    if (!bay_car(entity) || !marina_cell(gx, gy)) return x;
    const int lane = std::clamp(x, kBayLaneFirst, kBayLaneLast) - kBayLaneFirst;
    return kMarinaLaneFirst + lane * (kMarinaLaneLast - kMarinaLaneFirst) / (kBayLaneLast - kBayLaneFirst);
}

namespace {

void opponent_cell(Memory& m, int& gx, int& gy) {
    gx = rd16(m, kDataSeg, kOpponentRow) * 16 + (rd16(m, kDataSeg, kOpponentX) >> 11 & 15);
    gy = rd16(m, kDataSeg, kOpponentCol) * 16 + (rd16(m, kDataSeg, kOpponentY) >> 11 & 15);
}

void install_opponent_look(Cpu& cpu) {
    cpu.add_watch(Cpu::linear(kEntrySeg, kOpponentLook.match), [](Cpu& c) {
        Memory& m = c.memory();
        const uint16_t entity = rd16(m, kDataSeg, c.regs.r[host::SI]);
        if (!bridge_car(entity) && !bay_car(entity)) return;
        int gx = 0, gy = 0;
        opponent_cell(m, gx, gy);
        if ((bridge_car(entity) && gx < kApproach) || no_freeway_placement(entity, gx, gy, 0xFF) == Placement::Deny)
            c.regs.ip = kOpponentLook.next;
    });
    // On Marina Boulevard it sees the Bay Bridge's cars in the boulevard's lanes, where they're drawn.
    cpu.add_watch(Cpu::linear(kEntrySeg, kLookX), [](Cpu& c) {
        int gx = 0, gy = 0;
        opponent_cell(c.memory(), gx, gy);
        const auto entity = static_cast<uint16_t>(c.regs.r[host::BX] - 2);
        c.regs.r[host::AX] = static_cast<uint16_t>(no_freeway_lane_x(entity, gx, gy, c.regs.r[host::AX] & 0x7FF));
    });
}

}  // namespace

namespace {

// The city's list again, with the Golden Gate's cars (their cells kept as the bridge's list has them, see
// install_no_freeways), at `at`; every big tile that had the city's list gets it. Returns the new list's
// address (0: the lists aren't the ones expected).
uint16_t add_coast_traffic(Memory& m, uint16_t at, uint16_t end) {
    std::vector<uint16_t> words;
    uint16_t p = kCityList;
    for (int n = 0; n < 64 && rd16(m, kDataSeg, p) != 0xFFFF; ++n, p = static_cast<uint16_t>(p + 4)) {
        words.push_back(rd16(m, kDataSeg, p));
        words.push_back(rd16(m, kDataSeg, static_cast<uint16_t>(p + 2)));
    }
    int added = 0;
    p = static_cast<uint16_t>(kBridgeList + 4 * 4);  // from slot 4
    for (int n = 0; n < 16 && rd16(m, kDataSeg, p) != 0xFFFF; ++n, p = static_cast<uint16_t>(p + 4)) {
        const uint16_t e = rd16(m, kDataSeg, p);
        if (!bridge_car(e)) return 0;
        words.push_back(e);
        words.push_back(rd16(m, kDataSeg, static_cast<uint16_t>(p + 2)));
        ++added;
    }
    if (added != kBridgeCarCount || words.size() < 8) return 0;
    words.push_back(0xFFFF);
    at = static_cast<uint16_t>((at + 1) & ~1);
    if (at + 2 * words.size() > end) return 0;
    for (size_t i = 0; i < words.size(); ++i) wr16(m, kDataSeg, static_cast<uint16_t>(at + 2 * i), words[i]);
    for (int bt = 0; bt < kBigTiles; ++bt) {
        if (rd16(m, kDataSeg, static_cast<uint16_t>(kListsA + 2 * bt)) == kCityList)
            wr16(m, kDataSeg, static_cast<uint16_t>(kListsA + 2 * bt), at);
    }
    // The Marina's big tile gets the city's pedestrians (the original's has none: nobody went there), on
    // its car parks, green, streets and the city's edge (no_freeway_placement keeps them off the rest).
    if (rd16(m, kDataSeg, static_cast<uint16_t>(kListsB + 2 * kMarinaTile)) == kNoPedestrians)
        wr16(m, kDataSeg, static_cast<uint16_t>(kListsB + 2 * kMarinaTile), kCityPedestrians);
    return at;
}

// The Marina's list: the Golden Gate's (its slots 0-3 and its cars), and the Bay Bridge's cars for Marina
// Boulevard, at `at`. Returns its address (0: it doesn't fit, or the lists aren't the ones expected).
uint16_t add_marina_traffic(Memory& m, uint16_t at, uint16_t end) {
    if (rd16(m, kDataSeg, static_cast<uint16_t>(kListsA + 2 * kMarinaTile)) != kBridgeList) return 0;
    std::vector<uint16_t> words;
    for (uint16_t p = kBridgeList; rd16(m, kDataSeg, p) != 0xFFFF && words.size() < 64; p = static_cast<uint16_t>(p + 4)) {
        words.push_back(rd16(m, kDataSeg, p));
        words.push_back(rd16(m, kDataSeg, static_cast<uint16_t>(p + 2)));
    }
    int added = 0;
    for (uint16_t p = static_cast<uint16_t>(kBayList + 4 * 4); rd16(m, kDataSeg, p) != 0xFFFF && added < 16;
         p = static_cast<uint16_t>(p + 4)) {
        if (!bay_car(rd16(m, kDataSeg, p))) return 0;
        words.push_back(rd16(m, kDataSeg, p));
        words.push_back(rd16(m, kDataSeg, static_cast<uint16_t>(p + 2)));
        ++added;
    }
    if (added != kBridgeCarCount) return 0;
    words.push_back(0xFFFF);
    at = static_cast<uint16_t>((at + 1) & ~1);
    if (at + 2 * words.size() > end) return 0;
    for (size_t i = 0; i < words.size(); ++i) wr16(m, kDataSeg, static_cast<uint16_t>(at + 2 * i), words[i]);
    wr16(m, kDataSeg, static_cast<uint16_t>(kListsA + 2 * kMarinaTile), at);
    return at;
}

void edit_cell_rules(Memory& m) {
    for (const RuleEdit& e : kRuleEdits) {
        const int bt = e.cx / 16 * 5 + e.cy / 16;
        const uint16_t rules = rd16(m, kDataSeg, static_cast<uint16_t>(kCellRules + 2 * bt));
        wr8(m, kDataSeg, static_cast<uint16_t>(rules + (e.cx % 16) * 16 + e.cy % 16), e.rule);
    }
}

// The cells of `which` cars in list `to`, from list `from` (the one that moved them this frame).
void copy_cells(Memory& m, uint16_t from, uint16_t to, bool (*which)(uint16_t)) {
    if (from == to || to == 0) return;
    for (uint16_t q = to; rd16(m, kDataSeg, q) != 0xFFFF; q = static_cast<uint16_t>(q + 4)) {
        const uint16_t e = rd16(m, kDataSeg, q);
        if (!which(e)) continue;
        for (uint16_t p = from; rd16(m, kDataSeg, p) != 0xFFFF; p = static_cast<uint16_t>(p + 4)) {
            if (rd16(m, kDataSeg, p) == e) {
                wr16(m, kDataSeg, static_cast<uint16_t>(q + 2), rd16(m, kDataSeg, static_cast<uint16_t>(p + 2)));
                break;
            }
        }
    }
}

}  // namespace

void install_no_freeways(host::Machine& machine) {
    Cpu& cpu = machine.cpu();
    struct Traffic {
        uint16_t city_list = 0;    // the city's list with the coast road's cars (0: not installed)
        uint16_t marina_list = 0;  // the Marina's, with Marina Boulevard's
        size_t pass = 0;           // 0: the camera's list; else the bridges' passes made so far
        std::vector<uint16_t> passes;  // this frame's: the bridges' tiles to step as well
        uint16_t camera_list = 0;      // this frame's camera's list
        uint16_t saved[std::size(kStepState) + 1] = {};
        // The lists that moved the Golden Gate's cars this frame, and so on: the others take their cells.
        void sync(Memory& m) const {
            const uint16_t gg = camera_list == marina_list && marina_list ? marina_list : kBridgeList;
            for (const uint16_t to : {city_list, marina_list, kBridgeList}) copy_cells(m, gg, to, golden_gate);
            for (const uint16_t to : {city_list, marina_list}) copy_cells(m, kBayList, to, bay_car);
        }
    };
    auto traffic = std::make_shared<Traffic>();
    cpu.add_watch(Cpu::linear(kEntrySeg, kEntry), [traffic](Cpu& c) {
        std::string error;
        auto map = CityMap::read(c.memory(), error);
        if (!map) return;  // (not the build these addresses are for)
        add_no_freeway_cell_types(c.memory());
        add_no_freeway_roads(*map);
        map->write(c.memory());
        uint16_t free = route_opponent(c.memory());
        traffic->city_list = add_coast_traffic(c.memory(), free, kTypeRecords);
        if (traffic->city_list) {
            edit_cell_rules(c.memory());
            for (free = traffic->city_list; rd16(c.memory(), kDataSeg, free) != 0xFFFF; free = static_cast<uint16_t>(free + 4)) {}
            traffic->marina_list = add_marina_traffic(c.memory(), static_cast<uint16_t>(free + 2), kTypeRecords);
        }
    });
    // The traffic step. The bridges' cars move with their own lists only (the Golden Gate's, the Marina's
    // as well): the others leave them alone.
    cpu.add_watch(Cpu::linear(kEntrySeg, kTrafficEntity), [traffic](Cpu& c) {
        Memory& m = c.memory();
        const uint16_t e = c.regs.r[host::SI], list = rd16(m, kDataSeg, kStepList);
        const Traffic& t = *traffic;
        const bool skip = golden_gate(e) ? list != kBridgeList && (list != t.marina_list || !t.marina_list)
                                         : bay_car(e) && list != kBayList;
        if (t.city_list && skip) {
            c.regs.r[host::BX] = static_cast<uint16_t>(c.regs.r[host::BX] + 4);
            c.regs.ip = kTrafficNext;
        }
    });
    // The Marina's list is the Golden Gate's where the game asks.
    for (const uint16_t at : {kRulesSkip, kPoliceStart}) {
        cpu.add_watch(Cpu::linear(kEntrySeg, at), [traffic, at](Cpu& c) {
            const int reg = at == kRulesSkip ? host::AX : host::BP;
            if (traffic->marina_list && c.regs.r[reg] == traffic->marina_list) c.regs.r[reg] = kBridgeList;
        });
    }
    // ...which are stepped every frame, wherever the camera is: after the camera's own list, a pass for
    // each bridge's list it wasn't. Then the lists they were added to take their cells.
    cpu.add_watch(Cpu::linear(kEntrySeg, kTrafficEnd), [traffic](Cpu& c) {
        Memory& m = c.memory();
        if (!traffic->city_list) return;
        Traffic& t = *traffic;
        if (t.pass == 0) {
            const uint16_t camera = rd16(m, kDataSeg, kCameraTile);
            if (camera >= 2 * kBigTiles) return;
            const uint16_t list = rd16(m, kDataSeg, static_cast<uint16_t>(kListsA + camera));
            t.camera_list = list;
            t.passes.clear();
            if (list != kBridgeList && (list != t.marina_list || !t.marina_list)) t.passes.push_back(kGoldenGateTile);
            if (list != kBayList && t.marina_list) t.passes.push_back(kBayBridgeTile);
            if (t.passes.empty()) {
                t.sync(m);
                return;
            }
            t.saved[0] = camera;
            for (size_t i = 0; i < std::size(kStepState); ++i) t.saved[i + 1] = rd16(m, kDataSeg, kStepState[i]);
        }
        if (t.pass < t.passes.size()) {
            wr16(m, kDataSeg, kCameraTile, static_cast<uint16_t>(2 * t.passes[t.pass]));
            ++t.pass;
            c.regs.ip = kTrafficStep;
            return;
        }
        t.pass = 0;
        wr16(m, kDataSeg, kCameraTile, t.saved[0]);
        for (size_t i = 0; i < std::size(kStepState); ++i) wr16(m, kDataSeg, kStepState[i], t.saved[i + 1]);
        t.sync(m);
    });
    install_no_freeway_drawing(cpu);
    install_opponent_look(cpu);
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
