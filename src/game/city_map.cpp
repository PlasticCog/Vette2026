#include "game/city_map.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <vector>

#include "game/x86.h"
#include "host/machine.h"

namespace vette::game {
namespace {

using host::Cpu;
using host::Memory;

constexpr uint16_t kRowsAt = 0x856F, kColsAt = 0x8571;  // big-tile rows and columns (5, 5)
constexpr uint16_t kTilePtrs = 0x8524;                   // 25 near pointers to the designs
constexpr uint16_t kGround = 0x8556;                     // 25 ground colours
constexpr uint16_t kDesign0 = 0x8573, kDesignBytes = 0x200;
constexpr uint16_t kEntrySeg = emu_seg(0x3009), kEntry = 0x0025;  // start: the real entry point

constexpr std::string_view kMagic = "VETTE2026 MAP 1";

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> words(std::string_view s) {
    std::vector<std::string_view> out;
    for (;;) {
        s = trim(s);
        if (s.empty()) break;
        const size_t end = s.find_first_of(" \t");
        out.push_back(s.substr(0, end));
        if (end == std::string_view::npos) break;
        s.remove_prefix(end);
    }
    return out;
}

template <typename T>
bool number(std::string_view s, T& out, int base = 10) {
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out, base);
    return ec == std::errc() && end == s.data() + s.size();
}

}  // namespace

int CityMap::sharing(int design) const {
    return static_cast<int>(std::count(layout.begin(), layout.end(), static_cast<uint8_t>(design)));
}

std::optional<CityMap> CityMap::read(Memory& m, std::string& error) {
    if (rd16(m, kDataSeg, kRowsAt) != kRows || rd16(m, kDataSeg, kColsAt) != kCols) {
        error = "the map isn't the original's 5 x 5 big tiles (or VETTE.EXE hasn't started yet)";
        return std::nullopt;
    }
    CityMap map;
    for (int bt = 0; bt < kBigTiles; ++bt) {
        const uint16_t ptr = rd16(m, kDataSeg, static_cast<uint16_t>(kTilePtrs + 2 * bt));
        const int design = (ptr - kDesign0) / kDesignBytes;
        if (ptr < kDesign0 || (ptr - kDesign0) % kDesignBytes != 0 || design >= kDesigns) {
            error = "big tile " + std::to_string(bt) + " doesn't point at one of the 12 designs";
            return std::nullopt;
        }
        map.layout[static_cast<size_t>(bt)] = static_cast<uint8_t>(design);
        map.ground[static_cast<size_t>(bt)] = rd8(m, kDataSeg, static_cast<uint16_t>(kGround + bt));
    }
    for (int d = 0; d < kDesigns; ++d) {
        for (int i = 0; i < kTileCells * kTileCells; ++i) {
            const auto at = static_cast<uint16_t>(kDesign0 + d * kDesignBytes + 2 * i);
            Cell& c = map.designs[static_cast<size_t>(d)].cells[static_cast<size_t>(i)];
            c.type = rd8(m, kDataSeg, at);
            c.elevation = rd8(m, kDataSeg, static_cast<uint16_t>(at + 1));
        }
    }
    return map;
}

void CityMap::write(Memory& m) const {
    for (int bt = 0; bt < kBigTiles; ++bt) {
        wr16(m, kDataSeg, static_cast<uint16_t>(kTilePtrs + 2 * bt),
             static_cast<uint16_t>(kDesign0 + layout[static_cast<size_t>(bt)] * kDesignBytes));
        wr8(m, kDataSeg, static_cast<uint16_t>(kGround + bt), ground[static_cast<size_t>(bt)]);
    }
    for (int d = 0; d < kDesigns; ++d) {
        for (int i = 0; i < kTileCells * kTileCells; ++i) {
            const auto at = static_cast<uint16_t>(kDesign0 + d * kDesignBytes + 2 * i);
            const Cell& c = designs[static_cast<size_t>(d)].cells[static_cast<size_t>(i)];
            wr8(m, kDataSeg, at, c.type);
            wr8(m, kDataSeg, static_cast<uint16_t>(at + 1), c.elevation);
        }
    }
}

std::string CityMap::serialize() const {
    std::string out(kMagic);
    out += "\nname=" + name + "\n";
    const auto rows = [&](const char* title, const std::array<uint8_t, kBigTiles>& v) {
        out += title;
        for (int r = kRows - 1; r >= 0; --r) {
            for (int c = 0; c < kCols; ++c) {
                out += (c ? " " : "") + std::to_string(v[static_cast<size_t>(r * kCols + c)]);
            }
            out += "\n";
        }
    };
    rows("# Each big tile's design, the northern row first\nlayout\n", layout);
    rows("# Each big tile's ground colour (7 land, 9 water)\nground\n", ground);
    out += "# Each design's cells: type (hex):elevation, the northern row (x 15) first, west to east\n";
    for (int d = 0; d < kDesigns; ++d) {
        out += "design " + std::to_string(d) + "\n";
        for (int x = kTileCells - 1; x >= 0; --x) {
            for (int y = 0; y < kTileCells; ++y) {
                const Cell& c = designs[static_cast<size_t>(d)].cells[static_cast<size_t>(x * kTileCells + y)];
                char buf[16];
                std::snprintf(buf, sizeof buf, "%s%02X:%u", y ? " " : "", c.type, c.elevation);
                out += buf;
            }
            out += "\n";
        }
    }
    return out;
}

std::optional<CityMap> CityMap::parse(std::string_view text, std::string& error) {
    CityMap map;
    enum class Part { Header, Top, Layout, Ground, Design } part = Part::Header;
    int row = 0, design = -1;
    bool have_layout = false, have_ground = false;
    std::array<bool, kDesigns> have_design{};
    int line_no = 0;
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        std::string_view line = trim(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        ++line_no;
        const auto fail = [&](const std::string& why) {
            error = "line " + std::to_string(line_no) + ": " + why;
            return std::nullopt;
        };
        if (line.empty() || line.front() == '#') continue;
        if (part == Part::Header) {
            if (line != kMagic) return fail("not a VETTE! 2026 map file");
            part = Part::Top;
            continue;
        }
        if (line.rfind("name=", 0) == 0) {
            map.name = std::string(line.substr(5));
            continue;
        }
        if (line == "layout" || line == "ground") {
            part = line == "layout" ? Part::Layout : Part::Ground;
            row = 0;
            continue;
        }
        if (line.rfind("design ", 0) == 0) {
            if (!number(trim(line.substr(7)), design) || design < 0 || design >= kDesigns) return fail("no such design");
            part = Part::Design;
            row = 0;
            continue;
        }
        const std::vector<std::string_view> w = words(line);
        if (part == Part::Layout || part == Part::Ground) {
            if (row >= kRows || static_cast<int>(w.size()) != kCols) return fail("expected 5 numbers");
            for (int c = 0; c < kCols; ++c) {
                int v = 0;
                if (!number(w[static_cast<size_t>(c)], v) || v < 0 || v > (part == Part::Layout ? kDesigns - 1 : 15))
                    return fail("bad number");
                auto& dest = part == Part::Layout ? map.layout : map.ground;
                dest[static_cast<size_t>((kRows - 1 - row) * kCols + c)] = static_cast<uint8_t>(v);
            }
            if (++row == kRows) (part == Part::Layout ? have_layout : have_ground) = true;
            continue;
        }
        if (part == Part::Design) {
            if (row >= kTileCells || static_cast<int>(w.size()) != kTileCells) return fail("expected 16 cells");
            const int x = kTileCells - 1 - row;
            for (int y = 0; y < kTileCells; ++y) {
                const std::string_view cell = w[static_cast<size_t>(y)];
                const size_t colon = cell.find(':');
                unsigned type = 0, elevation = 0;
                if (colon == std::string_view::npos || !number(cell.substr(0, colon), type, 16) || type > 255 ||
                    !number(cell.substr(colon + 1), elevation) || elevation > 15)
                    return fail("bad cell \"" + std::string(cell) + "\"");
                map.designs[static_cast<size_t>(design)].cells[static_cast<size_t>(x * kTileCells + y)] = {
                    static_cast<uint8_t>(type), static_cast<uint8_t>(elevation)};
            }
            if (++row == kTileCells) have_design[static_cast<size_t>(design)] = true;
            continue;
        }
        return fail("unexpected \"" + std::string(line) + "\"");
    }
    if (part == Part::Header) {
        error = "not a VETTE! 2026 map file";
        return std::nullopt;
    }
    if (!have_layout || !have_ground ||
        std::find(have_design.begin(), have_design.end(), false) != have_design.end()) {
        error = "the file is incomplete (the layout, the ground and all 12 designs are needed)";
        return std::nullopt;
    }
    return map;
}

void install_city_map(host::Machine& machine, const CityMap& map) {
    machine.cpu().add_watch(Cpu::linear(kEntrySeg, kEntry), [map](Cpu& c) {
        std::string error;
        if (CityMap::read(c.memory(), error))  // the original build, unpacked: the map can go in
            map.write(c.memory());
    });
}

}  // namespace vette::game
