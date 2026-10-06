#include "game/smooth.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#include "game/x86.h"

namespace vette::game {
namespace {

using host::Ega;

constexpr uint16_t kCode = emu_seg(0x3009);
constexpr uint16_t kData = kDataSeg;

// Frame loop points (re/notes/02-frame-loop.md, 03-renderer-and-visibility.md).
constexpr uint16_t kDrawStart = 0x02DA;  // the 3D drawing section begins
constexpr uint16_t kCapture = 0x0356;    // after the traffic update, before the cell lookup and world
constexpr uint16_t kDrawEnd = 0x0374;    // world and view border drawn; HUD and mirror follow
constexpr uint16_t kDrawWorld = 0x30C6;  // draw_world_cells (near call from the section)
constexpr uint16_t kHighwayBranch = 0x0342;   // highway mode (DS:2AD4 != 0) jumps from here to 03A1,
constexpr uint16_t kHighwayDrawEnd = 0x03A1;  // past the traffic, the cell window and 0374
constexpr uint16_t kHighwayDrawn = 0x0405;    // after highway_frame and highway_draw_cars: the freeway drawn
constexpr uint16_t kMirrorWorldDrawn = 0x075F;  // draw_mirror_view: its world drawn, its frame (6439) next
constexpr uint16_t kHorizonCopy = 0x6773;  // vram_copy_rows, from blit_horizon: AX:SI -> DX:DI, BP rows of 40 bytes
constexpr uint16_t kPanoramaSeg = 0xA400;  // cs:0013, the off-screen panorama buffer
constexpr uint16_t kTrafficStep = 0xBCFB, kPedestrianStep = 0xBB72;  // simulation inside the section

constexpr uint16_t kBackBufSeg = 0x0011;  // cs: segment the race view is drawn into (A000 / A200)
constexpr uint16_t kHighway = 0x2AD4;     // DS: freeway mode (a separate renderer) when non-zero
constexpr uint16_t kViewLeft = 0x315E, kViewTop = 0x315A, kViewRight = 0x3160, kViewBottom = 0x315C;
constexpr uint16_t kViewOffset = 0x2B87;  // DS: camera yaw offset, 0 ahead, +85 right, -85 left
constexpr uint16_t kMirrorOff = 0x2AC7;   // DS: byte, 0 while the rear-view mirror is drawn (0434-0442)
constexpr uint16_t kChaseDistance = 0x2C7D;  // DS: the helicopter view's distance; the mirror needs 0
constexpr uint16_t kEndOfRoad = 0x8411;   // DS: byte, the end of the freeway is in sight (the city is drawn)

// The mirror's viewport descriptors (re/notes/03, "Viewports") for each view: ahead, right, left.
struct Rect {
    int x0, y0, x1, y1;  // inclusive
};
constexpr uint16_t kMirrorViewports[3] = {0x35A3, 0x3587, 0x3595};
int mirror_view(int16_t view_offset) { return view_offset == 0x55 ? 1 : view_offset == -0x55 ? 2 : 0; }
// Where the mirror's high-resolution view can't be used, the mirror stays as the game drew it, with this
// much on either side of its viewport for its frame (measured; the frame rectangles below fit in it).
constexpr int kMirrorFrameX = 8;

// The mirror's frame (3009:6439): opaque rectangles copied from off-screen video memory (4160:07F4, DI =
// destination offset in the page, 40 bytes a row; BH bytes wide, BP rows). Looking ahead (648A) two,
// as immediates; looking right (64B9) or left (645D) three each, from DS tables {DI words, BP words,
// BH bytes}.
struct Blit {
    uint16_t di;
    uint16_t rows;
    uint8_t bytes;
};
constexpr Blit kMirrorFrameAhead[2] = {{0x0017, 0x24, 1}, {0x04C8, 6, 0x10}};
constexpr uint16_t kMirrorFrameTables[3][3] = {{0, 0, 0}, {0x3C51, 0x3C57, 0x3C5D}, {0x3C3C, 0x3C42, 0x3C48}};

// The freeway (re/notes/04 section 8; highway_frame 3009:775E).
constexpr uint16_t kRouteSegments = 0x8154;  // {b slice type, b slices, w heading}..FFFF
constexpr uint16_t kRingBase = 0x804A;       // x, y of the current slice
constexpr uint16_t kSegment = 0x8158, kSlice = 0x815C, kRingSegment = 0x8232, kRingSlice = 0x8230;
constexpr uint16_t kRing = 0x8234;           // 32 x {x, y, heading}
constexpr uint16_t kHighwayCars = 0x82F4, kCarActive = 0x8420;  // 11 x 16h bytes: +4 x, +6 y, +0C heading
constexpr int kRingSlices = 32, kHighwayCarSlots = 11;

// The camera struct and the car structs share a layout: big-tile-local x/y (0..7FFFh) with the big
// tile's row/column at +22h/+24h. Vehicles and pedestrians in the per-big-tile lists are entities
// whose x/y (+2/+4) are local to a cell (0..7FFh); a car struct is its entity + 2.
constexpr uint16_t kCamera = 0x2C71;
constexpr uint16_t kCars[] = {0x2D35, 0x2F09, 0x3065};  // player, opponent, chase car
constexpr uint16_t kX = 0, kY = 2, kZ = 4, kYaw = 6, kPitch = 8, kRoll = 0x0A, kRow = 0x22, kCol = 0x24;
constexpr uint16_t kListsA = 0xEF5A, kListsB = 0xEF8C;  // vehicles, pedestrians (25 lists each)
constexpr int kBigTiles = 25;

// A value that jumps further than this between two game frames is a teleport or wrap: don't blend it.
constexpr int32_t kStructSnap = 0x800, kEntitySnap = 0x400;
constexpr uint64_t kStaleNs = 250'000'000;     // no game frame for this long: the race view isn't running
constexpr int64_t kReplayBudget = 20'000'000;  // cycles; the section needs a small fraction of this

int16_t lerp_angle(int16_t a, int16_t b, double t) {
    int d = b - a;
    while (d > 180) d -= 360;
    while (d < -180) d += 360;
    int v = a + static_cast<int>(std::lround(d * t));
    if (a >= 0 && a < 360 && b >= 0 && b < 360) {  // headings stay 0..359
        v = ((v % 360) + 360) % 360;
    }
    return static_cast<int16_t>(v);
}

int32_t lerp_int(int32_t a, int32_t b, double t) { return a + static_cast<int32_t>(std::lround((b - a) * t)); }

constexpr uint16_t at(uint16_t base, uint16_t offset) { return static_cast<uint16_t>(base + offset); }

} // namespace

struct SmoothRenderer::Snapshot {
    std::vector<uint8_t> ram = std::vector<uint8_t>(host::Memory::kSize);
    Ega ega;
    Registers regs;
    uint64_t t_ns = 0;
    uint64_t frame = 0;  // 0 = empty

    uint16_t word(uint16_t seg, uint16_t off) const {
        return static_cast<uint16_t>(ram[Cpu::linear(seg, off)] |
                                     ram[Cpu::linear(seg, static_cast<uint16_t>(off + 1))] << 8);
    }
    int16_t data(uint16_t off) const { return static_cast<int16_t>(word(kData, off)); }
    uint8_t byte(uint16_t off) const { return ram[Cpu::linear(kData, off)]; }
    bool highway() const { return byte(kHighway) == 0xFF; }
    // The slice a (segment, slice) pair names, counted from the route's start (7A0E: slice n of a
    // segment n slices long is the next segment's first).
    int route_slice(int segment, int slice) const {
        const uint16_t list = word(kData, kRouteSegments);
        int g = 0;
        for (int k = 0; k < segment && k < 1024; ++k) {
            const uint16_t w = word(kData, static_cast<uint16_t>(list + 4 * k));
            if (w == 0xFFFF) return -1;
            g += w >> 8;
        }
        return g + slice;
    }
    // The ring's slice k: x, y as it holds them.
    int32_t ring_x(int k) const { return word(kData, static_cast<uint16_t>(kRing + 6 * k)); }
    int32_t ring_y(int k) const { return word(kData, static_cast<uint16_t>(kRing + 6 * k + 2)); }
};

// Interpolation across highway_frame's re-centring: where the slice the current ring starts with lies in
// the previous frame's ring, and how the positions in memory moved between the two frames. `ring` moves
// the ring's (and the highway cars') coordinates, `current` the camera's and the player's (the ring in
// memory may still be in the frame before the latest re-centring; DS:804A is not).
bool SmoothRenderer::freeway_shift(const Snapshot& p, const Snapshot& c, int32_t ring[2], int32_t current[2]) {
    if (p.word(kData, kRouteSegments) != c.word(kData, kRouteSegments)) return false;
    const int gp = p.route_slice(p.data(kRingSegment), p.data(kRingSlice));
    const int gc = c.route_slice(c.data(kRingSegment), c.data(kRingSlice));
    const int dp = p.route_slice(p.data(kSegment), p.data(kSlice)) - gp;
    const int dc = c.route_slice(c.data(kSegment), c.data(kSlice)) - gc;
    const int k = gc - gp;
    if (gp < 0 || gc < 0 || k < 0 || k >= kRingSlices || dp < 0 || dp >= kRingSlices || dc < 0 || dc >= kRingSlices) {
        return false;
    }
    const auto d16 = [](int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<int16_t>(static_cast<uint16_t>(a - b))); };
    ring[0] = d16(c.ring_x(0), p.ring_x(k));
    ring[1] = d16(c.ring_y(0), p.ring_y(k));
    // Each frame's ring offset: its current slice's position (DS:804A) less the ring's entry for it.
    const int32_t off_p[2] = {d16(p.word(kData, kRingBase), p.ring_x(dp)), d16(p.word(kData, kRingBase + 2), p.ring_y(dp))};
    const int32_t off_c[2] = {d16(c.word(kData, kRingBase), c.ring_x(dc)), d16(c.word(kData, kRingBase + 2), c.ring_y(dc))};
    current[0] = ring[0] + off_c[0] - off_p[0];
    current[1] = ring[1] + off_c[1] - off_p[1];
    return true;
}

// The scratch machine's I/O: the EGA only. Nothing else is touched by the drawing code (the game's
// INT 0 handler's EOI to the PIC is simply dropped).
class SmoothRenderer::ScratchIo final : public host::IoBus {
public:
    explicit ScratchIo(Ega& ega) : ega_(ega) {}
    uint8_t in8(uint16_t port) override { return Ega::handles(port) ? ega_.in8(port) : 0xFF; }
    void out8(uint16_t port, uint8_t value) override {
        if (Ega::handles(port)) {
            ega_.out8(port, value);
        }
    }

private:
    Ega& ega_;
};

SmoothRenderer::SmoothRenderer(host::Machine& machine, bool world_layers)
    : machine_(machine),
      world_layers_(world_layers),
      prev_(std::make_unique<Snapshot>()),
      cur_(std::make_unique<Snapshot>()),
      scratch_io_(std::make_unique<ScratchIo>(scratch_ega_)) {
    scratch_mem_.set_video(&scratch_ega_);
    scratch_cpu_ = std::make_unique<Cpu>(scratch_mem_, *scratch_io_);
    scratch_cpu_->set_callback([](Cpu&, uint8_t) {});  // BIOS/DOS: not used by the drawing code
    const auto near_return = [](Cpu& c) { c.regs.ip = c.pop16(); };
    scratch_cpu_->set_code_hook(Cpu::linear(kCode, kTrafficStep), near_return);
    scratch_cpu_->set_code_hook(Cpu::linear(kCode, kPedestrianStep), near_return);
    scratch_cpu_->set_code_hook(Cpu::linear(kCode, kDrawEnd), [](Cpu& c) { c.request_stop(); });
    scratch_cpu_->set_code_hook(Cpu::linear(kCode, kHighwayDrawEnd), [](Cpu& c) { c.request_stop(); });
    scratch_cpu_->add_watch(Cpu::linear(kCode, kHorizonCopy), [this](Cpu& c) {
        if (c.regs.r[host::AX] != kPanoramaSeg) {
            return;
        }
        const auto bp = static_cast<int16_t>(c.regs.r[host::BP]);
        horizon_ = {bp > 0 ? bp : 1, c.regs.r[host::SI], c.regs.r[host::DI]};  // a do-while: one row at least
        horizon_dest_seg_ = c.regs.r[host::DX];
    });
    if (world_layers_) {
        // The world is drawn by the Enhanced renderer: take the memory it needs and skip the original's.
        scratch_cpu_->set_code_hook(Cpu::linear(kCode, kDrawWorld), [this](Cpu& c) {
            if (world_ram_) {
                std::memcpy(world_ram_->data(), scratch_mem_.ram(), host::Memory::kSize);
                world_copied_ = true;
            }
            c.regs.ip = c.pop16();
        });
    }

    Cpu& cpu = machine_.cpu();
    capture_watch_ = cpu.add_watch(Cpu::linear(kCode, kCapture), [this](Cpu&) { capture_frame(); });
    pure_watch_ = cpu.add_watch(Cpu::linear(kCode, kDrawEnd), [this](Cpu&) { capture_pure_image(); });
    if (world_layers_) {
        // Highway mode skips 0356 and 0374 (unless the end of the road is in sight).
        highway_watch_ = cpu.add_watch(Cpu::linear(kCode, kHighwayBranch), [this](Cpu&) {
            Memory& m = machine_.memory();
            if (rd8(m, kData, kHighway) != 0 && rd8(m, kData, kEndOfRoad) == 0) {
                capture_frame();
            }
        });
        highway_pure_watch_ = cpu.add_watch(Cpu::linear(kCode, kHighwayDrawn), [this](Cpu&) {
            if (rd8(machine_.memory(), kData, kHighway) != 0) {
                capture_pure_image();
            }
        });
        mirror_watch_ = cpu.add_watch(Cpu::linear(kCode, kMirrorWorldDrawn), [this](Cpu&) { capture_mirror_image(); });
    }
}

SmoothRenderer::~SmoothRenderer() {
    Cpu& cpu = machine_.cpu();
    for (const host::Cpu::WatchId id : {capture_watch_, pure_watch_, highway_watch_, highway_pure_watch_, mirror_watch_}) {
        if (id) {
            cpu.remove_watch(id);
        }
    }
}

void SmoothRenderer::capture_frame() {
    std::swap(prev_, cur_);
    Snapshot& s = *cur_;
    std::memcpy(s.ram.data(), machine_.memory().ram(), host::Memory::kSize);
    s.ega.copy_state_from(machine_.ega());
    s.regs = machine_.cpu().regs;
    s.t_ns = machine_.emulated_ns();
    s.frame = ++stats_.game_frames;
    scratch_loaded_ = false;
}

void SmoothRenderer::capture_pure_image() {
    const uint16_t seg = rd16(machine_.memory(), kCode, kBackBufSeg);
    if (seg != 0xA000 && seg != 0xA200) {
        return;
    }
    PureImage& p = pure_[seg == 0xA000 ? 0 : 1];
    machine_.ega().render_page(static_cast<uint16_t>((seg - 0xA000) * 16), p.image);
    p.valid = true;
    p.frame = stats_.game_frames;
}

void SmoothRenderer::capture_mirror_image() {
    Memory& mem = machine_.memory();
    const uint16_t seg = rd16(mem, kCode, kBackBufSeg);
    if (seg != 0xA000 && seg != 0xA200) {
        return;
    }
    MirrorImage& m = mirror_[seg == 0xA000 ? 0 : 1];
    machine_.ega().render_page(static_cast<uint16_t>((seg - 0xA000) * 16), m.image);
    m.valid = true;
    m.frame = stats_.game_frames;
    m.view = mirror_view(static_cast<int16_t>(rd16(mem, kData, kViewOffset)));
}

void SmoothRenderer::interpolate(double alpha) {
    const Snapshot& p = *prev_;
    const Snapshot& c = *cur_;
    if (alpha >= 1.0 || p.frame == 0 || p.frame + 1 != c.frame) {
        return;  // show the latest game frame as it is
    }
    Memory& m = scratch_mem_;
    const auto put = [&](uint16_t off, int32_t v) { wr16(m, kData, off, static_cast<uint16_t>(v)); };
    const auto angle = [&](uint16_t off) { put(off, lerp_angle(p.data(off), c.data(off), alpha)); };

    if (p.highway() || c.highway()) {
        // The freeway: its own 16-bit frame, re-centred at each new segment. The camera and the player
        // blend after moving the previous frame's positions into the current frame; so do the highway
        // cars, in the frame of the ring they were placed on.
        int32_t ring[2], current[2];
        if (!p.highway() || !c.highway() || !freeway_shift(p, c, ring, current)) {
            return;
        }
        const auto blend_moved = [&](uint16_t off, int32_t shift, int32_t snap) {
            const int32_t a = static_cast<int32_t>(p.word(kData, off)) + shift;
            const int32_t b = c.word(kData, off);
            const int32_t d = static_cast<int16_t>(static_cast<uint16_t>(b - a));
            if (std::abs(d) <= snap) {
                put(off, b - d + static_cast<int32_t>(std::lround(d * alpha)));
            }
        };
        for (const uint16_t base : {kCamera, kCars[0]}) {
            blend_moved(at(base, kX), current[0], kStructSnap);
            blend_moved(at(base, kY), current[1], kStructSnap);
            blend_moved(at(base, kZ), 0, kStructSnap);
            angle(at(base, kYaw));
            angle(at(base, kPitch));
            angle(at(base, kRoll));
        }
        for (int i = 0; i < kHighwayCarSlots; ++i) {
            if (p.byte(static_cast<uint16_t>(kCarActive + i)) == 0 || c.byte(static_cast<uint16_t>(kCarActive + i)) == 0) {
                continue;
            }
            const auto car = static_cast<uint16_t>(kHighwayCars + 0x16 * i);
            blend_moved(at(car, 4), ring[0], kStructSnap);
            blend_moved(at(car, 6), ring[1], kStructSnap);
            angle(at(car, 0x0C));
        }
        return;
    }

    // Camera and car structs: positions blended in whole-map coordinates (big tile * 8000h + local).
    const auto blend_struct = [&](uint16_t base) {
        for (const auto [coord, tile] : {std::pair{kX, kRow}, std::pair{kY, kCol}}) {
            const int32_t a = p.data(at(base, tile)) * 0x8000 + p.data(at(base, coord));
            const int32_t b = c.data(at(base, tile)) * 0x8000 + c.data(at(base, coord));
            if (std::abs(b - a) <= kStructSnap) {
                const int32_t v = lerp_int(a, b, alpha);
                put(at(base, coord), v & 0x7FFF);
                put(at(base, tile), v >> 15);
            }
        }
        if (std::abs(c.data(at(base, kZ)) - p.data(at(base, kZ))) <= kStructSnap) {
            put(at(base, kZ), lerp_int(p.data(at(base, kZ)), c.data(at(base, kZ)), alpha));
        }
        angle(at(base, kYaw));
        angle(at(base, kPitch));
        angle(at(base, kRoll));
    };
    blend_struct(kCamera);
    for (const uint16_t car : kCars) {
        blend_struct(car);
    }

    // Vehicles and pedestrians from the per-big-tile lists (each entity once).
    std::vector<uint16_t> seen;
    for (const uint16_t lists : {kListsA, kListsB}) {
        for (int bt = 0; bt < kBigTiles; ++bt) {
            const auto list = static_cast<uint16_t>(c.data(at(lists, static_cast<uint16_t>(2 * bt))));
            for (int i = 0; i < 128; ++i) {
                const auto e = static_cast<uint16_t>(c.data(at(list, static_cast<uint16_t>(4 * i))));
                if (e == 0xFFFF) {
                    break;
                }
                const bool car = std::find(std::begin(kCars), std::end(kCars), at(e, 2)) != std::end(kCars);
                if (car || std::find(seen.begin(), seen.end(), e) != seen.end()) {
                    continue;
                }
                seen.push_back(e);
                for (const uint16_t off : {uint16_t{2}, uint16_t{4}, uint16_t{6}}) {
                    const uint16_t field = at(e, off);
                    if (std::abs(c.data(field) - p.data(field)) <= kEntitySnap) {
                        put(field, lerp_int(p.data(field), c.data(field), alpha));
                    }
                }
                angle(at(e, 8));
                angle(at(e, 0x0A));
            }
        }
    }
}

bool SmoothRenderer::replay(double alpha, Ega::Frame& out) {
    const auto start = std::chrono::steady_clock::now();
    if (!scratch_loaded_) {
        std::memcpy(scratch_mem_.ram(), cur_->ram.data(), host::Memory::kSize);
        scratch_loaded_ = true;
    }
    scratch_ega_.copy_state_from(cur_->ega);
    std::vector<host::Memory::JournalEntry> journal;
    scratch_mem_.set_journal(&journal);
    interpolate(alpha);

    Cpu& cpu = *scratch_cpu_;
    cpu.regs = cur_->regs;
    cpu.regs.ip = kDrawStart;  // same CS, stack and segments as at the capture point
    world_copied_ = false;
    horizon_ = {};
    cpu.run(kReplayBudget);
    const bool finished =
        cpu.regs.s[host::CS] == kCode && (cpu.regs.ip == kDrawEnd || cpu.regs.ip == kHighwayDrawEnd);
    if (finished && world_ram_ && !world_copied_) {
        std::memcpy(world_ram_->data(), scratch_mem_.ram(), host::Memory::kSize);  // highway mode: no 30C6
    }
    if (finished) {
        const uint16_t seg = rd16(scratch_mem_, kCode, kBackBufSeg);
        scratch_ega_.render_page(static_cast<uint16_t>((seg - 0xA000) * 16), out);
        if (horizon_dest_seg_ != seg) {
            horizon_ = {};  // drawn somewhere else than the page shown
        }
    } else {
        broken_ = true;  // the section didn't end where expected: stop replaying, show the game's frames
    }

    // Undo everything the replay wrote, so the scratch RAM matches the captured frame again.
    uint8_t* ram = scratch_mem_.ram();
    for (auto it = journal.rbegin(); it != journal.rend(); ++it) {
        ram[it->linear] = it->old;
    }
    scratch_mem_.set_journal(nullptr);
    ++stats_.replays;
    stats_.replay_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return finished;
}

int SmoothRenderer::shown_page() {
    machine_.render(shown_);
    const uint16_t start = machine_.ega().display_start();
    const int page = start == 0 ? 0 : start == 0x2000 ? 1 : -1;
    return shown_.width == 320 && page >= 0 && pure_[page].valid ? page : -1;
}

// Where between the last two game frames to show: one game frame back, blended.
double SmoothRenderer::blend_alpha(uint64_t now_ns) const {
    const Snapshot& p = *prev_;
    const Snapshot& c = *cur_;
    if (!interpolation_ || p.frame == 0 || p.frame + 1 != c.frame || c.t_ns <= p.t_ns) {
        return 1.0;
    }
    return std::clamp(static_cast<double>(now_ns - c.t_ns) / static_cast<double>(c.t_ns - p.t_ns), 0.0, 1.0);
}

bool SmoothRenderer::render(uint64_t now_ns, Ega::Frame& out) {
    const Snapshot& c = *cur_;
    if (world_layers_ || broken_ || c.frame == 0 || now_ns - c.t_ns > kStaleNs || c.data(kHighway) != 0) {
        return false;
    }
    const int page = shown_page();
    if (page < 0 || !replay(blend_alpha(now_ns), replay_image_)) {
        return false;
    }

    // The 3D view comes from the replay, except where the displayed page differs from the original's
    // own 3D image of that page: there the game drew something on top (mirror, messages, pictures).
    out = shown_;
    const Ega::Frame& pure = pure_[page].image;
    const int x0 = std::max(0, static_cast<int>(c.data(kViewLeft)));
    const int x1 = std::min(shown_.width - 1, static_cast<int>(c.data(kViewRight)));
    const int y0 = std::max(0, static_cast<int>(c.data(kViewTop)));
    const int y1 = std::min(shown_.height - 1, static_cast<int>(c.data(kViewBottom)));
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const size_t i = static_cast<size_t>(y) * static_cast<size_t>(shown_.width) + static_cast<size_t>(x);
            if (shown_.pixels[i] == pure.pixels[i]) {
                out.pixels[i] = replay_image_.pixels[i];
            }
        }
    }
    return true;
}

bool SmoothRenderer::render_layers(uint64_t now_ns, Layers& out) {
    const Snapshot& c = *cur_;
    if (!world_layers_ || broken_ || c.frame == 0 || now_ns - c.t_ns > kStaleNs) {
        return false;
    }
    if (c.byte(kHighway) != 0 && !c.highway()) {
        return false;  // an on-ramp's frame (DS:2AD4 = 1..3): the camera is still in the city, the picture not
    }
    const int page = shown_page();
    if (page < 0) {
        return false;
    }
    world_ram_ = &out.ram;
    const bool replayed = replay(blend_alpha(now_ns), out.under);
    world_ram_ = nullptr;
    if (!replayed) {
        return false;
    }
    out.horizon = horizon_;

    // The displayed frame on top, except where it shows the original's own 3D view, or its mirror's
    // world (the mirror image captured on that page in the same game frame). The mirror's frame stays,
    // by its pixels; until they are known for this view, the mirror stays whole from the game's frame
    // (its picture often matches the 3D view behind it pixel for pixel: sky, road).
    out.over = shown_;
    const Ega::Frame& pure = pure_[page].image;
    const int x0 = std::max(0, static_cast<int>(c.data(kViewLeft)));
    const int x1 = std::min(shown_.width - 1, static_cast<int>(c.data(kViewRight)));
    const int y0 = std::max(0, static_cast<int>(c.data(kViewTop)));
    const int y1 = std::min(shown_.height - 1, static_cast<int>(c.data(kViewBottom)));
    // The mirror. The game drew one on this page in this game frame if its image was captured there;
    // it draws one (0434-0442) while DS:2C7D = 0 and DS:2AC7 = 0. With such a capture, in the view the
    // frame was drawn in, and the renderer's mirror enabled, the mirror's world is left to the renderer
    // (out.mirror) and only its frame (the rectangles 6439 copies) stays. Otherwise, whenever there is a
    // mirror, it stays whole and opaque as the game drew it, frame and all.
    const MirrorImage& mi = mirror_[static_cast<size_t>(page)];
    const bool captured = mi.valid && mi.frame == pure_[page].frame && mi.image.width == shown_.width &&
                          mi.image.height == shown_.height;
    const bool on = c.data(kChaseDistance) == 0 && c.byte(kMirrorOff) == 0;
    const int view_now = mirror_view(c.data(kViewOffset));
    const bool open = mirror_inset_ && captured && mi.view == view_now;
    out.mirror = open;
    Rect mirror{-1, -1, -2, -2};
    std::array<Rect, 3> frame{};
    int frames = 0;
    if (captured || on) {
        const uint16_t d = kMirrorViewports[captured ? mi.view : view_now];
        mirror = {c.data(d), c.data(static_cast<uint16_t>(d + 2)), c.data(static_cast<uint16_t>(d + 4)),
                  c.data(static_cast<uint16_t>(d + 6))};
    }
    if (open) {
        const auto add = [&](uint16_t di, int rows, int bytes) {
            const int fx = di % 40 * 8, fy = di / 40;
            frame[static_cast<size_t>(frames++)] = {fx, fy, fx + bytes * 8 - 1, fy + rows - 1};
        };
        if (mi.view == 0) {
            for (const Blit& b : kMirrorFrameAhead) add(b.di, b.rows, b.bytes);
        } else {
            const uint16_t* t = kMirrorFrameTables[mi.view];
            for (int k = 0; k < 3; ++k) {
                add(c.word(kData, static_cast<uint16_t>(t[0] + 2 * k)), c.data(static_cast<uint16_t>(t[1] + 2 * k)),
                    c.byte(static_cast<uint16_t>(t[2] + k)));
            }
        }
    } else {
        mirror.x0 -= kMirrorFrameX;  // the whole mirror and its frame stay
        mirror.x1 += kMirrorFrameX;
    }
    const auto inside = [](const Rect& r, int x, int y) { return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1; };
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const size_t i = static_cast<size_t>(y) * static_cast<size_t>(shown_.width) + static_cast<size_t>(x);
            bool on_frame = false;
            for (int k = 0; k < frames && !on_frame; ++k) on_frame = inside(frame[static_cast<size_t>(k)], x, y);
            if (on_frame) {
                continue;  // the mirror's frame
            }
            if (inside(mirror, x, y)) {
                if (open && shown_.pixels[i] == mi.image.pixels[i]) {
                    out.over.pixels[i] = kTransparent;
                }
            } else if (shown_.pixels[i] == pure.pixels[i]) {
                out.over.pixels[i] = kTransparent;
            }
        }
    }
    return true;
}

int SmoothRenderer::self_check() {
    const Snapshot& c = *cur_;
    if (world_layers_ || broken_ || c.frame == 0) {
        return -1;
    }
    const uint16_t seg = c.word(kCode, kBackBufSeg);
    const PureImage& pure = pure_[seg == 0xA000 ? 0 : 1];
    if (!pure.valid || pure.frame != c.frame || !replay(1.0, replay_image_)) {
        return -1;
    }
    const int x0 = std::max(0, static_cast<int>(c.data(kViewLeft)));
    const int x1 = std::min(pure.image.width - 1, static_cast<int>(c.data(kViewRight)));
    const int y0 = std::max(0, static_cast<int>(c.data(kViewTop)));
    const int y1 = std::min(pure.image.height - 1, static_cast<int>(c.data(kViewBottom)));
    int diff = 0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const size_t i = static_cast<size_t>(y) * static_cast<size_t>(pure.image.width) + static_cast<size_t>(x);
            diff += replay_image_.pixels[i] != pure.image.pixels[i];
        }
    }
    return diff;
}

} // namespace vette::game
