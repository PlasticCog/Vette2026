#pragma once
// The city's map as the original keeps it (re/notes/05-world-data.md section 2), for the map editor and
// for playing an edited map. 5 x 5 big tiles, each pointing (DS:8524) to one of 12 designs of 16 x 16
// cells (DS:8573 + 200h each), a cell being {type, elevation}; and each big tile's ground colour
// (DS:8556: 7 land, 9 water). Big tiles that share a design share its cells. The map is static data in
// VETTE.EXE: an edited one is written into the game's memory as it starts, and the player's files are
// never changed. Cell (cx, cy): cx counts north (16 per big-tile row), cy east (16 per big-tile column).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vette::host {
class Machine;
class Memory;
}  // namespace vette::host

namespace vette::game {

struct CityMap {
    static constexpr int kRows = 5, kCols = 5, kBigTiles = kRows * kCols;
    static constexpr int kDesigns = 12, kTileCells = 16;
    static constexpr int kCellsX = kRows * kTileCells, kCellsY = kCols * kTileCells;

    struct Cell {
        uint8_t type = 0;
        uint8_t elevation = 0;  // z = elevation * 224
        bool operator==(const Cell&) const = default;
    };
    struct Design {
        std::array<Cell, kTileCells * kTileCells> cells{};  // [x * 16 + y]
        bool operator==(const Design&) const = default;
    };

    std::string name;                     // shown in the launch menu (the file's name otherwise)
    std::array<uint8_t, kBigTiles> layout{};  // the design of each big tile (row * 5 + col, row 0 south)
    std::array<uint8_t, kBigTiles> ground{};  // each big tile's ground colour
    std::array<Design, kDesigns> designs{};

    bool operator==(const CityMap& o) const {
        return layout == o.layout && ground == o.ground && designs == o.designs;
    }

    int big_tile(int cx, int cy) const { return (cx / kTileCells) * kCols + cy / kTileCells; }
    int design_of(int cx, int cy) const { return layout[static_cast<size_t>(big_tile(cx, cy))]; }
    Cell& cell(int cx, int cy) {
        return designs[design_of(cx, cy)].cells[static_cast<size_t>((cx % kTileCells) * kTileCells + cy % kTileCells)];
    }
    const Cell& cell(int cx, int cy) const { return const_cast<CityMap*>(this)->cell(cx, cy); }
    int sharing(int design) const;  // how many big tiles use `design`

    // From the game's memory once VETTE.EXE has unpacked itself (nullopt, with `error`, before that or for
    // another build: the grid isn't 5 x 5 or a big tile doesn't point at one of the 12 designs).
    static std::optional<CityMap> read(host::Memory& memory, std::string& error);
    void write(host::Memory& memory) const;

    // A text file: "VETTE2026 MAP 1", "name=...", then "layout", "ground" (5 lines of 5 numbers each,
    // the northern row first), and "design N" with 16 lines (cell x 15 down to 0) of 16 "TT:E" cells
    // (type in hex, elevation), west to east. Lines from '#' are comments.
    std::string serialize() const;
    static std::optional<CityMap> parse(std::string_view text, std::string& error);
};

// Plays `map`: it goes into the game's memory at the real entry point (3009:0025), right after the
// EXEPACK stub has unpacked the image and before anything reads the map.
void install_city_map(host::Machine& machine, const CityMap& map);

}  // namespace vette::game
