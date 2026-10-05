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
constexpr uint16_t kTrafficStep = 0xBCFB, kPedestrianStep = 0xBB72;  // simulation inside the section

constexpr uint16_t kBackBufSeg = 0x0011;  // cs: segment the race view is drawn into (A000 / A200)
constexpr uint16_t kHighway = 0x2AD4;     // DS: freeway mode (a separate renderer) when non-zero
constexpr uint16_t kViewLeft = 0x315E, kViewTop = 0x315A, kViewRight = 0x3160, kViewBottom = 0x315C;
constexpr uint16_t kMirrorOff = 0x2AC7;   // DS: byte, 0 while the rear-view mirror is drawn
constexpr uint16_t kViewOffset = 0x2B87;  // DS: camera yaw offset, 0 ahead, +85 right, -85 left

// The mirror's viewports (re/notes/03, "Viewports") for each view, and how far its frame reaches
// to their left and right (measured). The mirror is drawn after the 3D view, all of it from the game's
// own frame.
struct Rect {
    int x0, y0, x1, y1;  // inclusive
};
constexpr Rect kMirrorAhead{192, 0, 319, 35}, kMirrorRight{80, 96, 127, 119}, kMirrorLeft{200, 84, 272, 119};
constexpr int kMirrorFrameX = 8;

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
};

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
    if (world_layers_) {
        // The world is drawn by the Enhanced renderer: take the memory it needs and skip the original's.
        scratch_cpu_->set_code_hook(Cpu::linear(kCode, kDrawWorld), [this](Cpu& c) {
            if (world_ram_) {
                std::memcpy(world_ram_->data(), scratch_mem_.ram(), host::Memory::kSize);
            }
            c.regs.ip = c.pop16();
        });
    }

    Cpu& cpu = machine_.cpu();
    capture_watch_ = cpu.add_watch(Cpu::linear(kCode, kCapture), [this](Cpu&) { capture_frame(); });
    pure_watch_ = cpu.add_watch(Cpu::linear(kCode, kDrawEnd), [this](Cpu&) { capture_pure_image(); });
}

SmoothRenderer::~SmoothRenderer() {
    machine_.cpu().remove_watch(capture_watch_);
    machine_.cpu().remove_watch(pure_watch_);
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

void SmoothRenderer::interpolate(double alpha) {
    const Snapshot& p = *prev_;
    const Snapshot& c = *cur_;
    if (alpha >= 1.0 || p.frame == 0 || p.frame + 1 != c.frame) {
        return;  // show the latest game frame as it is
    }
    Memory& m = scratch_mem_;
    const auto put = [&](uint16_t off, int32_t v) { wr16(m, kData, off, static_cast<uint16_t>(v)); };
    const auto angle = [&](uint16_t off) { put(off, lerp_angle(p.data(off), c.data(off), alpha)); };

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
    cpu.run(kReplayBudget);
    const bool finished = cpu.regs.s[host::CS] == kCode && cpu.regs.ip == kDrawEnd;
    if (finished) {
        const uint16_t seg = rd16(scratch_mem_, kCode, kBackBufSeg);
        scratch_ega_.render_page(static_cast<uint16_t>((seg - 0xA000) * 16), out);
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
    if (!world_layers_ || broken_ || c.frame == 0 || now_ns - c.t_ns > kStaleNs || c.data(kHighway) != 0) {
        return false;
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

    // The displayed frame on top, except where it shows the original's own 3D view. The mirror stays
    // whole: its picture often matches the 3D view behind it pixel for pixel (sky, road).
    out.over = shown_;
    const Ega::Frame& pure = pure_[page].image;
    const int x0 = std::max(0, static_cast<int>(c.data(kViewLeft)));
    const int x1 = std::min(shown_.width - 1, static_cast<int>(c.data(kViewRight)));
    const int y0 = std::max(0, static_cast<int>(c.data(kViewTop)));
    const int y1 = std::min(shown_.height - 1, static_cast<int>(c.data(kViewBottom)));
    Rect mirror{-1, -1, -2, -2};
    if ((c.data(kMirrorOff) & 0xFF) == 0) {
        const int16_t offset = c.data(kViewOffset);
        const Rect r = offset == 0 ? kMirrorAhead : offset > 0 ? kMirrorRight : kMirrorLeft;
        mirror = {r.x0 - kMirrorFrameX, r.y0, r.x1 + kMirrorFrameX, r.y1};
    }
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const size_t i = static_cast<size_t>(y) * static_cast<size_t>(shown_.width) + static_cast<size_t>(x);
            const bool in_mirror = x >= mirror.x0 && x <= mirror.x1 && y >= mirror.y0 && y <= mirror.y1;
            if (!in_mirror && shown_.pixels[i] == pure.pixels[i]) {
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
