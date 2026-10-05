#include "enhanced/world.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <map>
#include <set>

#include "enhanced/world_decode.h"
#include "enhanced/world_probe.h"
#include "host/machine.h"

namespace vette::enhanced {

namespace {

using Op = TraceEvent::Op;

// Data addresses (DS) of the map tables (notes 05 section 2, section 3).
constexpr uint16_t kBigRows = 0x856F, kBigCols = 0x8571, kBigTilePtrs = 0x8524, kBigTileGround = 0x8556;
constexpr uint16_t kCellTypes = 0x9D73;
// Segment-245A model table (notes 05 section 6).
constexpr uint16_t kModelTable = 0x6FF8;
// The far box's colour patch in model_select_lod: MOV AX,[SI+6], five MOV [addr],AX, MOV SI,[SI+4].
constexpr uint16_t kFarPatchCode = 0xBA18;

// The entry position every probe draws its object at, in the middle of the 16-bit range so that
// camera offsets of +-3A00h and the compound lists' offsets don't wrap.
constexpr std::array<int16_t, 3> kEntry = {0x4000, 0x4000, 0x0400};

// Camera offsets of the position grid: odd multiples of 200h up to 3A00h. The routines compare
// per-axis distances against multiples of 400h (400h, 800h, C00h, 1000h, 1800h, 2000h), measured
// from points offset by multiples of 400h, so one sample per 400h band reaches every outcome.
constexpr int kGridSteps = 15;
// Camera heights relative to the entry (the routines test camera above/below the object, at the
// object's height plus 20h, minus 3C0h, ...).
constexpr int16_t kHeights[] = {0x40, -0x500, -0x100, 0x10, 0x200, 0x500};

std::string hex4(uint16_t v) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%04X", v);
    return buf;
}

// --- Image check ----------------------------------------------------------------------------------

bool check_image(const ImageView& img, std::string& error) {
    const uint16_t cs = addr::kCodeSeg;
    struct Expect {
        uint16_t off;
        std::vector<uint8_t> bytes;
    };
    // Code the extraction relies on: the packed transform, the cell walk, the model LOD choice and its
    // far colour patch, the compound walkers' `call bx`, and the entry's MOV AX,DS (relocated).
    const Expect expect[] = {
        {0x3879, {0x89, 0x1E, 0x1C, 0x32, 0x83, 0x06, 0x1C, 0x32, 0x06}},
        {0x30C6, {0xBE, 0x71, 0x2C, 0xE8, 0xBD, 0x04}},
        {0xB9F6,
         {0xB9, static_cast<uint8_t>(addr::kModelSeg & 0xFF), static_cast<uint8_t>(addr::kModelSeg >> 8), 0x8E, 0xD9}},
        {0x2A9C, {0xFF, 0xD3}},
        {0x2C7A, {0xFF, 0xD3}},
        {0x002E, {0xB8, static_cast<uint8_t>(addr::kDataSeg & 0xFF), static_cast<uint8_t>(addr::kDataSeg >> 8)}},
        {kFarPatchCode, {0x8B, 0x44, 0x06, 0xA3}},
    };
    for (const Expect& e : expect) {
        for (size_t i = 0; i < e.bytes.size(); ++i) {
            if (img.u8(cs, static_cast<uint16_t>(e.off + i)) != e.bytes[i]) {
                error = "VETTE.EXE image not recognised (or not unpacked yet): code at 3009:" + hex4(e.off) +
                        " differs from DOS 1.1";
                return false;
            }
        }
    }
    return true;
}

// --- Probing ----------------------------------------------------------------------------------------

struct Probe {
    int16_t dx = 0x200, dy = 0x200, dz = 0x40;  // camera minus entry position
    uint8_t facing = 0;
    bool far_key = false;
    bool windows_off = false;
    bool finish = false;
    uint8_t course = 1;
    int16_t cam_yaw = 0;
    uint16_t preset_addr = 0;  // optional: angle words written before the run
    std::array<int16_t, 3> preset{};
};

void apply(Tracer& t, const Probe& p) {
    const uint16_t ds = addr::kDataSeg;
    for (int i = 0; i < 3; ++i) {
        t.wr16(ds, static_cast<uint16_t>(addr::kObjPos + 2 * i), static_cast<uint16_t>(kEntry[static_cast<size_t>(i)]));
    }
    t.wr16(ds, addr::kCamera, static_cast<uint16_t>(kEntry[0] + p.dx));
    t.wr16(ds, addr::kCamera + 2, static_cast<uint16_t>(kEntry[1] + p.dy));
    t.wr16(ds, addr::kCamera + 4, static_cast<uint16_t>(kEntry[2] + p.dz));
    t.wr16(ds, addr::kCamera + 6, static_cast<uint16_t>(p.cam_yaw));
    t.wr16(ds, addr::kCamera + 8, 0);
    t.wr16(ds, addr::kCamera + 10, 0);
    t.wr16(ds, addr::kCamPitchCopy, 0);
    t.wr8(ds, addr::kFacing, p.facing);
    t.wr8(ds, addr::kOwnCell, 0);
    t.wr16(addr::kCodeSeg, addr::kSortKey, p.far_key ? 0x0900 : 0x0100);
    t.wr8(ds, addr::kWindowsOff, p.windows_off ? 0xFF : 0);
    t.wr16(ds, addr::kCourse, p.course);
    t.wr8(ds, addr::kFinishFlag, p.finish ? 0xFF : 0);
    t.wr16(ds, addr::kCompoundDone, 0);
    t.wr8(ds, addr::kMirrorPass, 0);
    t.wr8(ds, addr::kNoBuildings, 0);
    t.wr16(ds, addr::kViewOffset, 0);
    t.wr8(ds, addr::kHighway, 0);
    if (p.preset_addr) {
        for (int i = 0; i < 3; ++i) {
            t.wr16(ds, static_cast<uint16_t>(p.preset_addr + 2 * i),
                   static_cast<uint16_t>(p.preset[static_cast<size_t>(i)]));
        }
    }
}

bool same_event(const Trace& a, const TraceEvent& x, const Trace& b, const TraceEvent& y) {
    if (x.op != y.op || x.colour != y.colour || x.addr != y.addr || x.count != y.count || x.aux != y.aux ||
        x.pos != y.pos || x.data_count != y.data_count) {
        return false;
    }
    return std::equal(a.data.begin() + x.data_first, a.data.begin() + x.data_first + x.data_count,
                      b.data.begin() + y.data_first);
}

// The event that set up the vertices each event draws with (the latest transform or model), or -1.
std::vector<int> contexts(const Trace& t) {
    std::vector<int> ctx(t.events.size(), -1);
    int cur = -1;
    for (size_t i = 0; i < t.events.size(); ++i) {
        const Op op = t.events[i].op;
        ctx[i] = cur;
        if (op == Op::Xform || op == Op::Plain || op == Op::Model) {
            cur = static_cast<int>(i);
        }
    }
    return ctx;
}

// Same event drawing from the same vertices: identical fills of different window quads differ only
// in the transform before them.
bool same_in_context(const Trace& a, const std::vector<int>& ca, size_t i, const Trace& b, const std::vector<int>& cb,
                     size_t j) {
    if (!same_event(a, a.events[i], b, b.events[j])) {
        return false;
    }
    if ((ca[i] < 0) != (cb[j] < 0)) {
        return false;
    }
    return ca[i] < 0 || same_event(a, a.events[static_cast<size_t>(ca[i])], b, b.events[static_cast<size_t>(cb[j])]);
}

struct Found {
    Trace trace;
    Probe first;
    size_t order = 0;
    Conditions when{0, 0, 0, 0, 0, false, INT32_MAX, 0};
    int8_t sets_finish = -1;
};

struct Activity {
    bool facing = false, key = false, windows = false, course = false, finish = false;
    bool xy = false, z = false;
};

class RoutineProber {
public:
    RoutineProber(Tracer& t, World& world) : t_(t), world_(world) {}

    // Probes routine `r.address` into `r`.
    void probe(Routine& r);
    size_t runs() const { return runs_; }

private:
    uint64_t run(const Probe& p, bool positional);
    bool run_trace(const Probe& p, Trace& out);
    struct Alternative {
        int call, face;
        Trace trace;
    };
    void dependencies(const Found& f, std::vector<std::pair<int, int>>& deps, std::vector<Alternative>& alts);
    void rotations(const Found& f, std::vector<Part>& info);
    bool build(const Found& f, Variant& v, std::string& error);

    Tracer& t_;
    World& world_;
    uint16_t routine_ = 0;
    std::map<uint64_t, Found> found_;
    Trace scratch_;
    std::string error_;
    size_t runs_ = 0;
};

bool RoutineProber::run_trace(const Probe& p, Trace& out) {
    ++runs_;
    t_.begin();
    apply(t_, p);
    const bool ok = t_.run(routine_, kEntry, out);
    t_.end();
    return ok;
}

uint64_t RoutineProber::run(const Probe& p, bool positional) {
    ++runs_;
    t_.begin();
    apply(t_, p);
    const bool ok = t_.run(routine_, kEntry, scratch_);
    const bool wrote_finish = t_.wrote(addr::kDataSeg, addr::kFinishFlag);
    const uint8_t finish_value = t_.rd8(addr::kDataSeg, addr::kFinishFlag);
    t_.end();
    if (!ok && error_.empty()) {
        error_ = scratch_.error;
    }
    const uint64_t sig = scratch_.signature();
    auto [it, fresh] = found_.try_emplace(sig);
    Found& f = it->second;
    if (fresh) {
        f.trace = scratch_;
        f.first = p;
        f.order = found_.size() - 1;
    }
    f.when.facing |= static_cast<uint8_t>(1u << p.facing);
    f.when.lod_key |= static_cast<uint8_t>(1u << (p.far_key ? 1 : 0));
    f.when.windows |= static_cast<uint8_t>(1u << (p.windows_off ? 1 : 0));
    f.when.course |= static_cast<uint8_t>(1u << p.course);
    f.when.finish_flag |= static_cast<uint8_t>(1u << (p.finish ? 1 : 0));
    if (positional) {
        const int32_t d = std::max(std::abs(int32_t{p.dx}), std::abs(int32_t{p.dy}));
        f.when.min_distance = std::min(f.when.min_distance, d);
        f.when.max_distance = std::max(f.when.max_distance, d);
    }
    if (wrote_finish) {
        f.sets_finish = finish_value != 0 ? 1 : 0;
    }
    return sig;
}

void RoutineProber::probe(Routine& r) {
    routine_ = r.address;
    found_.clear();
    error_.clear();

    // 1. Screen: which inputs change the routine's output at all.
    const Probe base;
    const uint64_t base_sig = run(base, false);
    Activity act;
    const auto differs = [&](const Probe& p) { return run(p, false) != base_sig; };
    {
        Probe p = base;
        p.facing = 1;
        act.facing = differs(p);
        p = base;
        p.far_key = true;
        act.key = differs(p);
        p = base;
        p.windows_off = true;
        act.windows = differs(p);
        for (uint8_t c = 2; c <= 4; ++c) {
            p = base;
            p.course = c;
            act.course |= differs(p);
        }
        p = base;
        p.finish = true;
        act.finish = differs(p);
    }
    std::vector<int16_t> grid;
    for (int k = 0; k < kGridSteps; ++k) {
        grid.push_back(static_cast<int16_t>(0x200 + 0x400 * k));
        grid.push_back(static_cast<int16_t>(-(0x200 + 0x400 * k)));
    }
    for (uint8_t facing = 0; facing < 2; ++facing) {
        Probe b = base;
        b.facing = facing;
        const uint64_t fsig = run(b, false);
        for (const int16_t v : grid) {
            Probe p = b;
            p.dx = v;
            act.xy |= run(p, false) != fsig;
            p = b;
            p.dy = v;
            act.xy |= run(p, false) != fsig;
        }
        for (const int16_t z : kHeights) {
            Probe p = b;
            p.dz = z;
            act.z |= run(p, false) != fsig;
        }
        if (facing == 1 && !act.facing) {
            // Facing may matter only away from the base position: compare the sweeps.
            for (const int16_t v : grid) {
                Probe p0 = base, p1 = b;
                p0.dx = p1.dx = v;
                if (run(p0, false) != run(p1, false)) {
                    act.facing = true;
                    break;
                }
                p0 = base;
                p1 = b;
                p0.dy = p1.dy = v;
                if (run(p0, false) != run(p1, false)) {
                    act.facing = true;
                    break;
                }
            }
        }
    }

    // 2. Enumerate. The found map so far holds results of the screen; restart with only the
    // enumeration's probes so that the condition masks are exact.
    found_.clear();
    const std::vector<uint8_t> facings = act.facing ? std::vector<uint8_t>{0, 1} : std::vector<uint8_t>{0};
    std::vector<int16_t> heights = {0x40};
    if (act.z) {
        heights.assign(std::begin(kHeights), std::end(kHeights));
    }
    const bool positional = act.xy || act.z;
    for (const uint8_t facing : facings) {
        for (const int16_t z : heights) {
            if (act.xy) {
                for (const int16_t x : grid) {
                    for (const int16_t y : grid) {
                        Probe p = base;
                        p.facing = facing;
                        p.dx = x;
                        p.dy = y;
                        p.dz = z;
                        run(p, true);
                    }
                }
            } else {
                Probe p = base;
                p.facing = facing;
                p.dz = z;
                run(p, positional);
            }
        }
        // Discrete inputs, at the base position, in every combination of the active ones.
        for (int key = 0; key < (act.key ? 2 : 1); ++key) {
            for (int win = 0; win < (act.windows ? 2 : 1); ++win) {
                for (uint8_t course = 1; course <= (act.course ? 4 : 1); ++course) {
                    for (int fin = 0; fin < (act.finish ? 2 : 1); ++fin) {
                        Probe p = base;
                        p.facing = facing;
                        p.far_key = key != 0;
                        p.windows_off = win != 0;
                        p.course = course;
                        p.finish = fin != 0;
                        run(p, false);
                    }
                }
            }
        }
    }

    // 3. Variants, with the masks of inputs the routine ignores set in full.
    r.camera_dependent = positional || act.facing;
    std::vector<const Found*> order;
    for (auto& [sig, f] : found_) {
        order.push_back(&f);
    }
    std::sort(order.begin(), order.end(), [](const Found* a, const Found* b) { return a->order < b->order; });
    for (const Found* fp : order) {
        Found f = *fp;
        if (!act.facing) f.when.facing = 0x3;
        if (!act.key) f.when.lod_key = 0x3;
        if (!act.windows) f.when.windows = 0x3;
        if (!act.course) f.when.course = 0x1E;
        if (!act.finish) f.when.finish_flag = 0x3;
        f.when.positional = positional && found_.size() > 1;
        if (f.when.min_distance == INT32_MAX) {
            f.when.min_distance = f.when.max_distance = 0;
        }
        Variant v;
        std::string error;
        if (!build(f, v, error)) {
            if (r.error.empty()) {
                r.error = error;
            }
            continue;
        }
        r.variants.push_back(std::move(v));
    }
    if (r.error.empty() && !error_.empty()) {
        r.error = error_;
    }
    // Most detailed first: most primitives, then the nearest.
    std::stable_sort(r.variants.begin(), r.variants.end(), [](const Variant& a, const Variant& b) {
        if (a.primitives != b.primitives) return a.primitives > b.primitives;
        return a.when.min_distance < b.when.min_distance;
    });
    for (size_t i = 0; i < r.variants.size(); ++i) {
        r.variants[i].detail = static_cast<uint8_t>(std::min<size_t>(i, 255));
        r.compound |= !r.variants[i].calls.empty();
    }
}

// Which culling face (vis call, face) each event of the variant depends on: hide one face at a time
// and see which events disappear (the face's own fill, and e.g. the windows drawn on it).
// An event depends on a face if it disappears, wherever it was, when only that face is hidden. If
// the rest comes out in another order, that order is kept as a painter-order alternative.
void RoutineProber::dependencies(const Found& f, std::vector<std::pair<int, int>>& deps,
                                 std::vector<Alternative>& alts) {
    deps.assign(f.trace.events.size(), {-1, -1});
    alts.clear();
    int call = 0;
    Trace hidden;
    const std::vector<int> cf = contexts(f.trace);
    for (const TraceEvent& e : f.trace.events) {
        if (e.op != Op::Vis5 && e.op != Op::Vis6) {
            continue;
        }
        for (int face = 0; face < e.count; ++face) {
            t_.hide_face(call, face);
            run_trace(f.first, hidden);
            t_.hide_face(-1, -1);
            const std::vector<int> ch = contexts(hidden);
            std::vector<bool> used(hidden.events.size(), false);
            for (size_t i = 0; i < f.trace.events.size(); ++i) {
                bool found = false;
                for (size_t j = 0; j < hidden.events.size() && !found; ++j) {
                    if (!used[j] && same_in_context(f.trace, cf, i, hidden, ch, j)) {
                        used[j] = found = true;
                    }
                }
                if (!found && deps[i].first < 0) {
                    deps[i] = {call, face};
                }
            }
            // Same order? Then the hidden trace is a subsequence of the full one (plain content: a
            // removed window changes the next window's context, not the order).
            size_t j = 0;
            for (size_t i = 0; i < f.trace.events.size() && j < hidden.events.size(); ++i) {
                if (same_event(f.trace, f.trace.events[i], hidden, hidden.events[j])) {
                    ++j;
                }
            }
            if (j != hidden.events.size()) {
                alts.push_back({call, face, hidden});
            }
        }
        ++call;
    }
}

// Classifies the rotation of each plain list: camera yaw (billboard), fixed, animated or inherited.
void RoutineProber::rotations(const Found& f, std::vector<Part>& info) {
    info.clear();
    Probe a = f.first;
    a.cam_yaw = 0;
    Probe b = f.first;
    b.cam_yaw = 123;
    Trace ta, tb, tc;
    run_trace(a, ta);
    run_trace(b, tb);
    const auto plains = [](const Trace& t) {
        std::vector<const TraceEvent*> out;
        for (const TraceEvent& e : t.events) {
            if (e.op == Op::Plain) {
                out.push_back(&e);
            }
        }
        return out;
    };
    const auto pa = plains(ta), pb = plains(tb);
    for (size_t i = 0; i < pa.size(); ++i) {
        Part p;
        const TraceEvent& ea = *pa[i];
        if (ea.angle_addr == 0 || i >= pb.size()) {
            p.rotation = Part::Rotation::Inherited;
            info.push_back(p);
            continue;
        }
        Probe c = a;
        c.preset_addr = ea.angle_addr;
        for (size_t k = 0; k < 3; ++k) {
            c.preset[k] = static_cast<int16_t>(ea.angle_prior[k] + 40);
        }
        run_trace(c, tc);
        const auto pc = plains(tc);
        const TraceEvent* ec = i < pc.size() ? pc[i] : nullptr;
        const TraceEvent& eb = *pb[i];
        p.anim_angle_addr = ea.angle_addr;
        const int dyaw = ((eb.angles[0] - ea.angles[0]) % 360 + 360) % 360;
        if (dyaw == 123) {
            p.rotation = Part::Rotation::CameraYaw;
            p.yaw = ea.angles[0];  // offset from the camera's yaw (0 in run a)
        } else if (ec && ec->angles[0] == ea.angles[0]) {
            p.rotation = Part::Rotation::Fixed;
            p.yaw = ea.angles[0];
        } else {
            const int step = ea.angles[0] - ea.angle_prior[0];
            p.rotation = step == 0 ? Part::Rotation::Inherited : Part::Rotation::Animated;
            p.anim_step = static_cast<int16_t>(step);
            p.yaw = step == 0 ? int16_t{0} : ea.angles[0];
        }
        const bool pitch_fixed = ec && ec->angles[1] == ea.angles[1];
        const bool roll_fixed = ec && ec->angles[2] == ea.angles[2];
        p.pitch = pitch_fixed ? ea.angles[1] : int16_t{0};
        p.roll = roll_fixed ? ea.angles[2] : int16_t{0};
        if (!pitch_fixed || !roll_fixed) {
            world_.warnings.push_back("routine " + hex4(routine_) +
                                      ": a plain list's pitch/roll come from stale memory (taken as 0)");
        }
        info.push_back(p);
    }
}

bool RoutineProber::build(const Found& f, Variant& v, std::string& error) {
    const Trace& tr = f.trace;
    v.signature = tr.signature();
    v.when = f.when;
    v.sets_finish_flag = f.sets_finish;
    const auto fail = [&](const std::string& what) {
        error = "routine " + hex4(routine_) + ": " + what;
        return false;
    };
    if (!tr.error.empty()) {
        return fail(tr.error);
    }

    bool has_vis = false, has_plain = false;
    for (const TraceEvent& e : tr.events) {
        has_vis |= e.op == Op::Vis5 || e.op == Op::Vis6;
        has_plain |= e.op == Op::Plain;
    }
    std::vector<std::pair<int, int>> deps(tr.events.size(), {-1, -1});
    std::vector<Alternative> alts;
    if (has_vis) {
        dependencies(f, deps, alts);
    }
    // Where each event's primitives went: (part, first prim, prim count), for the alternatives.
    struct Placed {
        int part = -1;
        uint16_t first = 0, count = 0;
    };
    std::vector<Placed> placed(tr.events.size());
    std::vector<Part> rot;
    if (has_plain) {
        rotations(f, rot);
    }

    struct Vis {
        int part;
        bool six;
        const int16_t* bytes;
        uint16_t count;
    };
    std::vector<Vis> vis;
    int cur = -1;
    size_t plain_index = 0;
    const AxisTable ideal = ideal_axis_table();
    for (size_t i = 0; i < tr.events.size(); ++i) {
        const TraceEvent& e = tr.events[i];
        const int16_t* w = tr.data.data() + e.data_first;
        switch (e.op) {
        case Op::Xform: {
            Part p;
            p.source = Part::Source::Packed;
            p.origin = {e.pos[0], e.pos[1], e.pos[2]};
            p.data_addr = e.addr;
            p.packed.assign(w, w + e.data_count);
            std::vector<V3s> verts;
            if (!unpack_vertices({e.pos[0], e.pos[1], e.pos[2]}, w, e.count, ideal, verts)) {
                return fail("bad packed vertex word in block " + hex4(e.addr));
            }
            for (const V3s& q : verts) {
                p.verts.push_back({q[0], q[1], q[2]});
            }
            v.parts.push_back(std::move(p));
            cur = static_cast<int>(v.parts.size() - 1);
            break;
        }
        case Op::Plain: {
            Part p = plain_index < rot.size() ? rot[plain_index] : Part{};
            ++plain_index;
            p.source = Part::Source::Plain;
            p.origin = {e.pos[0], e.pos[1], e.pos[2]};
            p.data_addr = e.addr;
            for (uint32_t k = 0; k + 2 < e.data_count; k += 3) {
                p.verts.push_back({w[k], w[k + 1], w[k + 2]});
            }
            v.parts.push_back(std::move(p));
            cur = static_cast<int>(v.parts.size() - 1);
            break;
        }
        case Op::Model: {
            Part p;
            p.source = Part::Source::Model;
            p.origin = {e.pos[0], e.pos[1], e.pos[2]};
            p.model = static_cast<uint8_t>(e.addr);
            if (e.addr >= kModelCount || !world_.models[e.addr].present) {
                return fail("draws missing model " + std::to_string(e.addr));
            }
            p.rotation = e.aux == 2 ? Part::Rotation::None : Part::Rotation::Fixed;
            p.yaw = e.angles[0];
            p.pitch = e.angles[1];
            p.roll = e.angles[2];
            v.parts.push_back(std::move(p));
            cur = -1;
            ++v.primitives;
            break;
        }
        case Op::Vis5:
        case Op::Vis6:
            vis.push_back({cur, e.op == Op::Vis6, w, e.count});
            break;
        case Op::Fill:
        case Op::FillAlt:
        case Op::Lines: {
            if (cur < 0) {
                return fail("draws without a vertex set");
            }
            Part& p = v.parts[static_cast<size_t>(cur)];
            placed[i] = {cur, static_cast<uint16_t>(p.prims.size()), 0};
            Cull cull;
            if (deps[i].first >= 0) {
                const Vis& vi = vis[static_cast<size_t>(deps[i].first)];
                const auto face = static_cast<size_t>(deps[i].second);
                const int16_t* rec = vi.bytes + face * (vi.six ? 6 : 5);
                cull.kind = vi.six ? Cull::Kind::Plane : Cull::Kind::Behind;
                cull.part = static_cast<uint16_t>(vi.part);
                for (size_t k = 0; k < (vi.six ? 3u : 2u); ++k) {
                    cull.v[k] = static_cast<uint16_t>(rec[2 + k] / 6);
                }
                if (vi.part < 0) {
                    return fail("culls without a vertex set");
                }
            }
            const auto nverts = p.verts.size();
            const auto add_index = [&](int idx) {
                if (idx < 0 || static_cast<size_t>(idx) >= nverts) {
                    return false;
                }
                p.indices.push_back(static_cast<uint16_t>(idx));
                return true;
            };
            if (e.op == Op::Lines) {
                for (uint32_t k = 0; k < e.data_count; ++k) {
                    const auto word = static_cast<uint16_t>(w[k]);
                    Prim prim;
                    prim.kind = Prim::Kind::Line;
                    prim.colour.raw = e.colour;
                    prim.first = static_cast<uint32_t>(p.indices.size());
                    prim.count = 2;
                    prim.cull = cull;
                    if (!add_index((word & 0xFF) / 4) || !add_index((word >> 8) / 4)) {
                        return fail("line list " + hex4(e.addr) + " indexes past its vertices");
                    }
                    p.prims.push_back(prim);
                    ++v.primitives;
                }
                placed[i].count = static_cast<uint16_t>(p.prims.size() - placed[i].first);
            } else {
                uint32_t k = 0;
                while (k < e.data_count) {
                    const int n = w[k];
                    Prim prim;
                    prim.kind = e.op == Op::FillAlt ? Prim::Kind::PolygonAlt : Prim::Kind::Polygon;
                    prim.colour.raw = e.colour;
                    prim.first = static_cast<uint32_t>(p.indices.size());
                    prim.count = static_cast<uint16_t>(n);
                    prim.cull = cull;
                    for (int q = 0; q < n; ++q) {
                        if (!add_index(static_cast<uint16_t>(w[k + 1 + static_cast<uint32_t>(q)]) / 4)) {
                            return fail("polygon list " + hex4(e.addr) + " indexes past its vertices");
                        }
                    }
                    p.prims.push_back(prim);
                    ++v.primitives;
                    k += static_cast<uint32_t>(n) + 2;
                }
            }
            placed[i].count = static_cast<uint16_t>(p.prims.size() - placed[i].first);
            break;
        }
        case Op::SubCall:
            v.calls.push_back({e.addr, {e.pos[0], e.pos[1], e.pos[2]}});
            break;
        case Op::Bad:
            return fail("unhooked renderer path 3009:" + hex4(e.addr));
        }
    }
    for (const Alternative& alt : alts) {
        Variant::Reorder ro;
        // The deciding face, with the vertices of its own culling call.
        int call = 0;
        for (size_t i = 0; i < tr.events.size(); ++i) {
            const TraceEvent& e = tr.events[i];
            if (e.op != Op::Vis5 && e.op != Op::Vis6) continue;
            if (call++ != alt.call) continue;
            const bool six = e.op == Op::Vis6;
            const int16_t* rec = tr.data.data() + e.data_first + static_cast<size_t>(alt.face) * (six ? 6 : 5);
            ro.unless_visible.kind = six ? Cull::Kind::Plane : Cull::Kind::Behind;
            for (size_t k = 0; k < (six ? 3u : 2u); ++k) {
                ro.unless_visible.v[k] = static_cast<uint16_t>(rec[2 + k] / 6);
            }
            // The part current at that call.
            int part = -1;
            for (size_t j = 0; j <= i; ++j) {
                if (tr.events[j].op == Op::Xform || tr.events[j].op == Op::Plain) ++part;
                if (tr.events[j].op == Op::Model) ++part;
            }
            ro.unless_visible.part = static_cast<uint16_t>(std::max(part, 0));
        }
        std::vector<bool> used(tr.events.size(), false);
        const std::vector<int> ct = contexts(tr), ca = contexts(alt.trace);
        for (size_t h = 0; h < alt.trace.events.size(); ++h) {
            for (size_t i = 0; i < tr.events.size(); ++i) {
                if (!used[i] && placed[i].part >= 0 && same_in_context(alt.trace, ca, h, tr, ct, i)) {
                    used[i] = true;
                    for (uint16_t k = 0; k < placed[i].count; ++k) {
                        ro.order.push_back({static_cast<uint16_t>(placed[i].part), static_cast<uint16_t>(placed[i].first + k)});
                    }
                    break;
                }
            }
        }
        v.reorders.push_back(std::move(ro));
    }
    return true;
}

// --- Map, cell types, models -------------------------------------------------------------------------

bool decode_map(const ImageView& img, World& w, std::string& error) {
    const uint16_t ds = addr::kDataSeg;
    w.big_rows = img.u16(ds, kBigRows);
    w.big_cols = img.u16(ds, kBigCols);
    if (w.big_rows < 1 || w.big_cols < 1 || w.big_rows * w.big_cols > 25) {
        error = "map size " + std::to_string(w.big_rows) + " x " + std::to_string(w.big_cols) + " big tiles";
        return false;
    }
    w.cells.assign(static_cast<size_t>(w.cells_x() * w.cells_y()), Cell{});
    for (int r = 0; r < w.big_rows; ++r) {
        for (int c = 0; c < w.big_cols; ++c) {
            const int bt = r * w.big_cols + c;
            const uint16_t tile = img.u16(ds, static_cast<uint16_t>(kBigTilePtrs + 2 * bt));
            w.big_tile_ground[static_cast<size_t>(bt)] = img.u8(ds, static_cast<uint16_t>(kBigTileGround + bt));
            for (int x = 0; x < kBigTileCells; ++x) {
                for (int y = 0; y < kBigTileCells; ++y) {
                    const auto at = static_cast<uint16_t>(tile + x * 32 + y * 2);
                    Cell& cell = w.cells[static_cast<size_t>((r * kBigTileCells + x) * w.cells_y() + c * kBigTileCells + y)];
                    cell.type = img.u8(ds, at);
                    cell.elevation = img.u8(ds, static_cast<uint16_t>(at + 1));
                }
            }
        }
    }
    return true;
}

bool decode_types(const ImageView& img, World& w, std::string& error) {
    const uint16_t ds = addr::kDataSeg;
    for (const Cell& c : w.cells) {
        w.types[c.type].used = true;
    }
    for (size_t t = 0; t < 256; ++t) {
        CellType& ct = w.types[t];
        ct.address = img.u16(ds, static_cast<uint16_t>(kCellTypes + 2 * t));
        if (!ct.used) {
            continue;  // unused slots point at garbage
        }
        const uint16_t header = img.u16(ds, ct.address);
        ct.ground_class = static_cast<uint8_t>(header);
        ct.collision_class = static_cast<uint8_t>(header >> 8);
        const uint16_t l2 = read_entry_list(img, ds, static_cast<uint16_t>(ct.address + 2), ct.list1);
        if (l2 == 0 || read_entry_list(img, ds, l2, ct.list2) == 0) {
            error = "cell type " + std::to_string(t) + ": list not terminated";
            return false;
        }
        ++w.stats.types_used;
    }
    return true;
}

bool decode_models(const ImageView& img, World& w, std::string& error) {
    const uint16_t ms = addr::kModelSeg;
    // The far box's colour patch addresses, read from the code (five MOV [addr],AX).
    std::vector<uint16_t> patched;
    for (int i = 0; i < 5; ++i) {
        const auto at = static_cast<uint16_t>(kFarPatchCode + 3 + 3 * i);
        if (img.u8(addr::kCodeSeg, at) != 0xA3) {
            error = "model far colour patch not recognised";
            return false;
        }
        patched.push_back(img.u16(addr::kCodeSeg, static_cast<uint16_t>(at + 1)));
    }
    for (size_t id = 0; id < kModelCount; ++id) {
        Model& m = w.models[id];
        const auto entry = static_cast<uint16_t>(kModelTable + 8 * id);
        const uint16_t near_hdr = img.u16(ms, entry);
        const uint16_t far_hdr = img.u16(ms, static_cast<uint16_t>(entry + 4));
        m.far_colour = img.u16(ms, static_cast<uint16_t>(entry + 6));
        if (near_hdr == 0) {
            continue;
        }
        std::string e;
        if (!decode_model_mesh(img, ms, near_hdr, m.near_mesh, e) ||
            !decode_model_mesh(img, ms, far_hdr, m.far_mesh, e, m.far_colour, patched)) {
            error = "model " + std::to_string(id) + ": " + e;
            return false;
        }
        m.present = true;
        ++w.stats.models;
    }
    return true;
}

// --- Compound instances -----------------------------------------------------------------------------

void build_compounds(World& w) {
    std::map<std::vector<int64_t>, size_t> index;
    for (int cx = 0; cx < w.cells_x(); ++cx) {
        for (int cy = 0; cy < w.cells_y(); ++cy) {
            const Cell& cell = w.cell(cx, cy);
            const CellType& ct = w.types[cell.type];
            for (const auto* list : {&ct.list1, &ct.list2}) {
                for (const ListEntry& e : *list) {
                    const Routine* r = w.routine(e.routine);
                    if (!r || !r->compound) {
                        continue;
                    }
                    const Variant* v = r->select(DrawState{});
                    if (!v) {
                        continue;
                    }
                    const Vec3i pos{cx * kCellSize + e.dx, cy * kCellSize + e.dy,
                                    cell.elevation * kElevationStep + e.dz};
                    std::vector<int64_t> key;
                    for (const SubCall& s : v->calls) {
                        key.push_back(s.routine);
                        key.push_back(pos.x + s.offset.x);
                        key.push_back(pos.y + s.offset.y);
                        key.push_back(pos.z + s.offset.z);
                    }
                    auto [it, fresh] = index.try_emplace(key, w.compounds.size());
                    if (fresh) {
                        w.compounds.push_back({e.routine, pos, {}});
                    }
                    auto& cells = w.compounds[it->second].cells;
                    const auto id = static_cast<uint16_t>(cx * w.cells_y() + cy);
                    if (cells.empty() || cells.back() != id) {
                        cells.push_back(id);
                    }
                }
            }
        }
    }
}

} // namespace

// --- Public API -------------------------------------------------------------------------------------

const Variant* Routine::select(const DrawState& state) const {
    const auto course_bit = static_cast<uint8_t>(1u << std::clamp(state.course, 1, 4));
    const auto windows_bit = static_cast<uint8_t>(state.windows ? 1 : 2);
    const auto finish_bit = static_cast<uint8_t>(state.finish_flag ? 2 : 1);
    for (const Variant& v : variants) {  // most detailed first
        if ((v.when.course & course_bit) && (v.when.windows & windows_bit) && (v.when.finish_flag & finish_bit)) {
            return &v;
        }
    }
    return nullptr;
}

int32_t World::ground_z(int32_t x, int32_t y) const {
    if (cells.empty()) {
        return 0;
    }
    const int cx = std::clamp(x / kCellSize, 0, cells_x() - 1);
    const int cy = std::clamp(y / kCellSize, 0, cells_y() - 1);
    const Cell& c = cell(cx, cy);
    const int u = std::min(x & 0x7FF, 0x700);
    const int v = std::max((y & 0x7FF) - 0x100, 0);
    int h = 0;
    switch (types[c.type].ground_class) {
    case 1: h = std::min(u, v); break;
    case 2: h = u; break;
    case 3: h = std::min(u, 0x700 - v); break;
    case 4: h = v; break;
    case 6: h = 0x700 - v; break;
    case 7: h = std::min(0x700 - u, v); break;
    case 8: h = 0x700 - u; break;
    case 9: h = std::min(0x700 - u, 0x700 - v); break;
    default: break;
    }
    return c.elevation * kElevationStep + (h >> 3);
}

const Routine* World::routine(uint16_t address) const {
    const auto it = std::lower_bound(routines.begin(), routines.end(), address,
                                     [](const Routine& r, uint16_t a) { return r.address < a; });
    return it != routines.end() && it->address == address ? &*it : nullptr;
}

bool extract_world(host::Machine& machine, World& out, std::string& error) {
    const auto start = std::chrono::steady_clock::now();
    out = World{};
    const ImageView img{machine.memory().ram()};
    if (!check_image(img, error) || !decode_map(img, out, error) || !decode_types(img, out, error) ||
        !decode_models(img, out, error)) {
        return false;
    }

    // Every routine the used cell types reference, then the compound structures' pieces.
    std::set<uint16_t> pending;
    for (const CellType& ct : out.types) {
        if (!ct.used) {
            continue;
        }
        for (const auto* list : {&ct.list1, &ct.list2}) {
            for (const ListEntry& e : *list) {
                pending.insert(e.routine);
            }
        }
    }
    Tracer tracer(img.ram);
    RoutineProber prober(tracer, out);
    std::deque<uint16_t> queue(pending.begin(), pending.end());
    std::set<uint16_t> done;
    while (!queue.empty()) {
        const uint16_t a = queue.front();
        queue.pop_front();
        if (!done.insert(a).second) {
            continue;
        }
        Routine r;
        r.address = a;
        prober.probe(r);
        for (const Variant& v : r.variants) {
            for (const SubCall& s : v.calls) {
                if (!done.count(s.routine)) {
                    queue.push_back(s.routine);
                }
            }
        }
        out.routines.push_back(std::move(r));
    }
    std::sort(out.routines.begin(), out.routines.end(),
              [](const Routine& a, const Routine& b) { return a.address < b.address; });
    build_compounds(out);

    auto& s = out.stats;
    s.routines = static_cast<int>(out.routines.size());
    for (const Routine& r : out.routines) {
        s.routines_failed += r.error.empty() ? 0 : 1;
        s.compound_routines += r.compound ? 1 : 0;
        s.camera_dependent += r.camera_dependent ? 1 : 0;
        s.variants += static_cast<int>(r.variants.size());
    }
    s.probe_runs = prober.runs();
    s.milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::sort(out.warnings.begin(), out.warnings.end());
    out.warnings.erase(std::unique(out.warnings.begin(), out.warnings.end()), out.warnings.end());
    return true;
}

// --- Geometry helpers -------------------------------------------------------------------------------

namespace {
constexpr double kDeg = 3.14159265358979323846 / 180.0;

// The original's object matrix (3009:9D60 / 3DF9) in floating point, row-major, for camera-input
// order vectors (east, down, north).
std::array<double, 9> object_matrix(double yaw, double pitch, double roll) {
    const double sa = std::sin(yaw * kDeg), ca = std::cos(yaw * kDeg);
    const double sb = std::sin(pitch * kDeg), cb = std::cos(pitch * kDeg);
    const double sc = std::sin(roll * kDeg), cc = std::cos(roll * kDeg);
    return {sc * sb * sa + cc * ca, sc * cb, sc * sb * ca - cc * sa,  //
            cc * sb * sa - sc * ca, cc * cb, cc * sb * ca + sc * sa,  //
            cb * sa,                -sb,     cb * ca};
}

Vec3d mul_ci(const std::array<double, 9>& m, double e, double d, double n) {
    // Row vector (east, down, north) times m, back to world axes.
    const double oe = e * m[0] + d * m[3] + n * m[6];
    const double od = e * m[1] + d * m[4] + n * m[7];
    const double on = e * m[2] + d * m[5] + n * m[8];
    return {on, oe, -od};
}
} // namespace

Vec3d rotate_local(Vec3d v, double yaw_deg, double pitch_deg, double roll_deg) {
    return mul_ci(object_matrix(yaw_deg, pitch_deg, roll_deg), v.y, -v.z, v.x);
}

Vec3d model_to_world(const Vec3i& v, double yaw_deg, double pitch_deg, double roll_deg) {
    return mul_ci(object_matrix(yaw_deg, pitch_deg, roll_deg), v.x, v.y, v.z);
}

int model_octant(Vec3d camera_local) {
    return (-camera_local.x > 0 ? 1 : 0) | (camera_local.z > 0 ? 2 : 0) | (camera_local.y > 0 ? 4 : 0);
}

bool face_visible(const Cull& cull, const Vec3d* v, Vec3d cam) {
    switch (cull.kind) {
    case Cull::Kind::None:
        return true;
    case Cull::Kind::Behind: {
        const Vec3d& a = v[cull.v[0]];
        const Vec3d& b = v[cull.v[1]];
        return (b.x - a.x) * (a.x - cam.x) + (b.y - a.y) * (a.y - cam.y) + (b.z - a.z) * (a.z - cam.z) > 0;
    }
    case Cull::Kind::Plane: {
        // In camera-input order (east, down, north), as the original's cross product.
        const auto ci = [](const Vec3d& p) { return Vec3d{p.y, -p.z, p.x}; };
        const Vec3d p0 = ci(v[cull.v[0]]), p1 = ci(v[cull.v[1]]), p2 = ci(v[cull.v[2]]), c = ci(cam);
        const Vec3d b{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
        const Vec3d a{p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
        const Vec3d n{b.y * a.z - b.z * a.y, b.z * a.x - b.x * a.z, b.x * a.y - b.y * a.x};
        return n.x * (p0.x - c.x) + n.y * (p0.y - c.y) + n.z * (p0.z - c.z) > 0;
    }
    }
    return true;
}

} // namespace vette::enhanced
