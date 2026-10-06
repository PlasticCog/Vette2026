#include "enhanced/lanes.h"

#include <cstdlib>

namespace vette::enhanced {
namespace {

constexpr uint8_t kWhite = 0x0F, kYellow = 0x0E;

} // namespace

std::vector<game::LaneLine> lane_lines(const World& world) {
    std::vector<game::LaneLine> out;
    const DrawState state;
    for (int cx = 0; cx < world.cells_x(); ++cx) {
        for (int cy = 0; cy < world.cells_y(); ++cy) {
            const CellType& type = world.types[world.cell(cx, cy).type];
            for (const ListEntry& e : type.list1) {
                const Routine* r = world.routine(e.routine);
                const Variant* v = r && !r->compound ? r->select(state) : nullptr;
                if (!v) {
                    continue;
                }
                const Vec3i at = world.entry_position(cx, cy, e);
                for (const Part& part : v->parts) {
                    if (part.source != Part::Source::Packed) {
                        continue;
                    }
                    for (const Prim& p : part.prims) {
                        if (p.kind != Prim::Kind::Line || p.count < 2 ||
                            (p.colour.raw != kWhite && p.colour.raw != kYellow)) {
                            continue;
                        }
                        const Vec3i& a = part.verts[part.indices[p.first]];
                        const Vec3i& b = part.verts[part.indices[p.first + 1]];
                        // On the ground: level or along a ramp (7 degrees), not up a wall.
                        const int run = std::abs(b.x - a.x) + std::abs(b.y - a.y);
                        if (std::abs(b.z - a.z) * 4 > run) {
                            continue;
                        }
                        out.push_back({at.x + a.x, at.y + a.y, at.x + b.x, at.y + b.y});
                    }
                }
            }
        }
    }
    return out;
}

std::shared_ptr<const game::LaneMap> lane_map(const World& world) {
    return std::make_shared<const game::LaneMap>(lane_lines(world));
}

} // namespace vette::enhanced
