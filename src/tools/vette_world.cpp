// World extraction tool for the Enhanced renderer (src/enhanced/world.h). Boots the original headlessly,
// extracts the city model from the game's own memory, and validates it.
//
//   vette_world --game Game --out re/out/world [options]
//
// --catalogue          print every routine with its variants
// --map                write a top-down render of the whole map (map_topdown.png)
// --validate A:B       between emulated seconds A and B, redraw every race frame's 3D view from the
//                      extracted world (src/enhanced/world_reference.h) and compare it with the
//                      original's own drawing, captured from the back buffer when draw_world_cells
//                      returns (3009:0371)
// --png-every N        with --validate, write original | reference | diff PNGs for every Nth frame
// --png-over P         ... and for every frame with more than P% of its pixels differing (default 2)
// --label NAME         prefix for the PNGs and the frame log
// --dump-calls N       print the original's and the reference's draw calls of validation frame N
// --key T:SC, --hold A:B:SC   scripted keys as in vette_run (scan codes in hex)
// --seconds N          emulated run length (default: the end of the validation or teleport time + 0.5)
// --manual-check       show the copy-protection question (skipped by default)
// --teleport N         replay the original's 3D drawing (3009:02DA-0371) on a scratch copy of a race frame
//                      for N random camera positions and headings anywhere on the map, and compare each
// --teleport-at T      ... using the race frame at emulated second T (default 40); --seed S for the views
// --float              build the reference image from the World API's float geometry (Part::verts,
//                      rotate_local, model_to_world) instead of the original's fixed-point data
// --dump-routine ADDR  print a routine's variants (parts, primitives, culling)
// --recheck            extract again at the end of the run and check the result is the same world
// --scene-compare A:B  build each race frame's Scene (src/enhanced/scene.h) with the original's window and
//                      LOD (SceneOptions::original_window, the original's routines choosing variants),
//                      rasterize it at 320x200 and compare it with the original's 3D view
// --scene-bench A:B    time SceneBuilder::build() on each race frame (whole map and radius 8, for output at
//                      --scene-scale)
// --scene-shots T,...  original | radius 8 | whole map PNGs of the 3D view at --scene-scale (default 6),
//                      with the rear-view mirror's scene where the original drew its mirror
//                      (in highway mode, all of these work on the freeway: the memory before
//                      highway_frame, 3009:03B4, against the image after highway_draw_cars, 0405;
//                      --scene-compare also compares the mirror's view, from 074F to 075F)
// --bridge-check       at the race frame of --teleport-at (default 40 s), sweep the camera along every
//                      bridge (on the deck both ways and to the sides, and from the helicopter's height)
//                      and compare the original's view with the Enhanced scene where the original drew;
//                      prints the share it doesn't explain and writes the worst views per bridge
// --bridge-view x,y,z,yaw,pitch   ... only that view of the sweep
// --freeway-boxes      list the on-ramp collision boxes (3009:1A12-1AD8) and their cells
// --poke T:OFF:VAL     write DS:OFF at second T (two hex digits: a byte, else a word), e.g. a freeway
//                      route: --poke 39:2AD4:03 --poke 39:8156:0003
// --shot T             save the displayed frame at second T (<label>_frame_T.png)
// --watch OFF[*N],...  print these DS words (N words from OFF) with each --shot
// --sky-views          at the race frame of --teleport-at, the Enhanced view (whole map) from a set of places
//                      and headings (the Great Highway, the bridges, downtown, Twin Peaks, the helicopter's
//                      height), each with the painted skyline above the Hills skyline (enhanced/backdrop.h),
//                      at --scene-scale and at the original's 320x200 enlarged (<label>_sky_NAME.png)
// --sky-sweep         ... and from three places all the way round (every 40 degrees)
// --sky-view x,y,z,yaw,pitch,NAME   ... this view instead (repeatable; z above the ground)
// --replicas           ... with traffic and pedestrians in every cell of their pattern (SceneOptions::replicas,
//                      as the game's Maximum draw distance draws them)
// --horizon-dump       write the three panoramas from video memory and their Hills versions
//                      (<label>_horizon{0,1,2}_{painted,hills}.bin: 24 rows x 3200 EGA colour indices) and
//                      PNGs of both
//
// The README's race script reaches the race at ~37 s and drives north on the Great Highway:
//   vette_world --map --validate 37:50 --key 13:39 --key 17:1C --key 21:1C --key 25:1C --key 30:1C
//               --key 37:1E --key 37.3:02 --hold 37.5:49:48 --hold 38.5:39.3:4D
// Right arrow at the course menu (--key 29:4D, before the --key 30:1C) picks course 2 (Vista Point,
// the Golden Gate), twice (29:4D, 29.4:4D) course 3 (the Bay Bridge). F1/F2/F3 (3B/3C/3D) look left,
// ahead and right from the car, F4 (3E) is the helicopter view (handlers 3009:092B/094C/090A/0A49),
// W (11) toggles window detail, B (30) buildings, F6 (40) the mirror. Course 3 held straight on (no
// steering) reaches the 280 freeway's on-ramp (cell 18,49) after about 290 s.
// Whole-map check: --teleport 10000 with any of these scripts (all views must be identical).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "enhanced/scene.h"
#include "assets/png.h"
#include "enhanced/backdrop.h"
#include "enhanced/world.h"
#include "enhanced/world_probe.h"
#include "enhanced/world_reference.h"
#include "game/options.h"
#include "host/machine.h"

namespace {

using vette::enhanced::World;
using vette::host::Cpu;
using vette::host::Ega;
using vette::host::Machine;
using vette::host::MachineConfig;
namespace en = vette::enhanced;

constexpr uint64_t kNsPerMs = 1'000'000;
constexpr uint16_t kCode = 0x4009, kData = 0x224A;

// Default EGA palette (the race view's attribute registers are the defaults; notes 05 open question 7).
constexpr uint32_t kEga[16] = {0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
                               0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};

using vette::assets::write_png;

// --- Top-down map -----------------------------------------------------------------------------------
class TopDown {
public:
    TopDown(const World& w, int px_per_cell) : world_(w), scale_(px_per_cell / double(en::kCellSize)) {
        width_ = w.cells_y() * px_per_cell;
        height_ = w.cells_x() * px_per_cell;
        img_.assign(static_cast<size_t>(width_ * height_), 0);
    }

    void render() {
        const int pc = static_cast<int>(scale_ * en::kCellSize);
        for (int cx = 0; cx < world_.cells_x(); ++cx) {
            for (int cy = 0; cy < world_.cells_y(); ++cy) {
                const int bt = (cx / 16) * world_.big_cols + cy / 16;
                const uint32_t c = kEga[world_.big_tile_ground[static_cast<size_t>(bt)] & 15];
                for (int v = 0; v < pc; ++v) {
                    for (int u = 0; u < pc; ++u) {
                        img_[static_cast<size_t>((height_ - 1 - (cx * pc + v)) * width_ + cy * pc + u)] = c;
                    }
                }
            }
        }
        struct Sortable {
            double z;
            const en::Variant* v;
            en::Vec3i pos;
        };
        std::vector<Sortable> late;
        for (int cx = 0; cx < world_.cells_x(); ++cx) {
            for (int cy = 0; cy < world_.cells_y(); ++cy) {
                const en::Cell& cell = world_.cell(cx, cy);
                const en::CellType& ct = world_.types[cell.type];
                en::DrawState state;
                const en::Vec3i origin{cx * en::kCellSize, cy * en::kCellSize, cell.elevation * en::kElevationStep};
                for (const auto* list : {&ct.list1, &ct.list2}) {
                    for (const en::ListEntry& e : *list) {
                        const en::Routine* r = world_.routine(e.routine);
                        if (!r || r->compound) continue;
                        const en::Variant* v = r->select(state);
                        if (!v) continue;
                        if (v->sets_finish_flag >= 0) state.finish_flag = v->sets_finish_flag != 0;
                        const en::Vec3i pos{origin.x + e.dx, origin.y + e.dy, origin.z + e.dz};
                        if (list == &ct.list1) {
                            draw(*v, pos);
                        } else {
                            late.push_back({double(pos.z), v, pos});
                        }
                    }
                }
            }
        }
        for (const en::CompoundInstance& ci : world_.compounds) {
            const en::Routine* r = world_.routine(ci.routine);
            const en::Variant* v = r ? r->select(en::DrawState{}) : nullptr;
            if (!v) continue;
            for (const en::SubCall& s : v->calls) {
                const en::Routine* sr = world_.routine(s.routine);
                const en::Variant* sv = sr ? sr->select(en::DrawState{}) : nullptr;
                if (sv) {
                    late.push_back({double(ci.position.z + s.offset.z), sv,
                                    {ci.position.x + s.offset.x, ci.position.y + s.offset.y, ci.position.z + s.offset.z}});
                }
            }
        }
        std::stable_sort(late.begin(), late.end(), [](const Sortable& a, const Sortable& b) { return a.z < b.z; });
        for (const Sortable& s : late) draw(*s.v, s.pos);
        // Big-tile grid.
        for (int i = 0; i <= world_.big_rows; ++i) hline(height_ - 1 - std::min(height_ - 1, i * 16 * pc), 0xFFFFFF);
        for (int i = 0; i <= world_.big_cols; ++i) vline(std::min(width_ - 1, i * 16 * pc), 0xFFFFFF);
    }

    bool save(const std::filesystem::path& p) const { return write_png(p, width_, height_, img_); }

private:
    struct P {
        double u, v, z;
    };
    P to_px(double x, double y, double z) const { return {y * scale_, height_ - 1 - x * scale_, z}; }

    void draw(const en::Variant& v, en::Vec3i pos) {
        for (const en::Part& p : v.parts) {
            std::vector<P> pts;
            if (p.source == en::Part::Source::Model) {
                const en::Model& m = world_.models[p.model];
                const double yaw = p.rotation == en::Part::Rotation::None ? 0 : p.yaw;
                std::vector<P> mp;
                for (const en::Vec3i& q : m.near_mesh.verts) {
                    const en::Vec3d w = en::model_to_world(q, yaw, p.pitch, p.roll);
                    mp.push_back(to_px(pos.x + p.origin.x + w.x, pos.y + p.origin.y + w.y, pos.z + p.origin.z + w.z));
                }
                // Octant for a camera high above: up is -y in model axes.
                const int oct = en::model_octant({0, -1e6, 0});
                for (const uint16_t fi : m.near_mesh.order[static_cast<size_t>(oct)]) {
                    const en::ModelFace& f = m.near_mesh.faces[fi];
                    if (f.flags & 0x4000) continue;
                    for (const auto& prim : f.prims) {
                        std::vector<P> poly;
                        for (const uint16_t i : prim) poly.push_back(mp[i]);
                        if (f.lines()) {
                            for (size_t k = 0; k + 1 < poly.size(); ++k) seg(poly[k], poly[k + 1], f.colour.base());
                        } else {
                            fill(poly, f.colour);
                        }
                    }
                }
                continue;
            }
            for (const en::Vec3i& q : p.verts) {
                if (p.source == en::Part::Source::Packed) {
                    pts.push_back(to_px(pos.x + q.x, pos.y + q.y, pos.z + q.z));
                } else {
                    const double yaw = (p.rotation == en::Part::Rotation::Fixed || p.rotation == en::Part::Rotation::Animated) ? p.yaw : 0;
                    const en::Vec3d w = en::rotate_local({double(q.x), double(q.y), double(q.z)}, yaw, p.pitch, p.roll);
                    pts.push_back(to_px(pos.x + p.origin.x + w.x, pos.y + p.origin.y + w.y, pos.z + p.origin.z + w.z));
                }
            }
            for (const en::Prim& prim : p.prims) {
                std::vector<P> poly;
                for (uint32_t k = 0; k < prim.count; ++k) poly.push_back(pts[p.indices[prim.first + k]]);
                if (prim.kind == en::Prim::Kind::Line) {
                    seg(poly[0], poly[1], prim.colour.base());
                } else {
                    fill(poly, prim.colour);
                }
            }
        }
    }

    void put(int u, int v, uint32_t c) {
        if (u >= 0 && v >= 0 && u < width_ && v < height_) img_[static_cast<size_t>(v * width_ + u)] = c;
    }
    void hline(int v, uint32_t c) {
        for (int u = 0; u < width_; ++u) put(u, v, c);
    }
    void vline(int u, uint32_t c) {
        for (int v = 0; v < height_; ++v) put(u, v, c);
    }
    void seg(const P& a, const P& b, uint8_t colour) {
        const int n = static_cast<int>(std::max(std::abs(b.u - a.u), std::abs(b.v - a.v))) + 1;
        for (int i = 0; i <= n; ++i) {
            const double t = double(i) / n;
            put(static_cast<int>(a.u + t * (b.u - a.u)), static_cast<int>(a.v + t * (b.v - a.v)), kEga[colour & 15]);
        }
    }
    void fill(const std::vector<P>& poly, en::Colour c) {
        if (poly.size() < 3) {
            if (poly.size() == 2) seg(poly[0], poly[1], c.base());
            return;
        }
        double vmin = 1e30, vmax = -1e30;
        for (const P& p : poly) {
            vmin = std::min(vmin, p.v);
            vmax = std::max(vmax, p.v);
        }
        for (int v = std::max(0, static_cast<int>(std::ceil(vmin - 0.5))); v <= std::min(height_ - 1, static_cast<int>(vmax)); ++v) {
            const double yc = v + 0.5;
            std::vector<double> xs;
            for (size_t k = 0; k < poly.size(); ++k) {
                const P& a = poly[k];
                const P& b = poly[(k + 1) % poly.size()];
                if ((a.v <= yc) != (b.v <= yc)) xs.push_back(a.u + (yc - a.v) / (b.v - a.v) * (b.u - a.u));
            }
            std::sort(xs.begin(), xs.end());
            for (size_t k = 0; k + 1 < xs.size(); k += 2) {
                for (int u = std::max(0, static_cast<int>(std::ceil(xs[k] - 0.5)));
                     u <= std::min(width_ - 1, static_cast<int>(std::floor(xs[k + 1] - 0.5))); ++u) {
                    const bool second = c.dithered() && ((u + v) & 1) == 0;
                    put(u, v, kEga[second ? c.second() : c.base()]);
                }
            }
        }
    }

    const World& world_;
    double scale_;
    int width_ = 0, height_ = 0;
    std::vector<uint32_t> img_;
};

// --- Validation ---------------------------------------------------------------------------------------
struct Validator {
    Machine& machine;
    const World& world;
    std::filesystem::path out_dir;
    std::string label;
    double from = 0, to = 0;
    int png_every = 0;
    int dump_calls = -1;
    double png_over = 2.0;
    bool from_world = false;

    en::ReferenceRenderer ref{world};
    std::vector<uint8_t> ram;
    Ega::Frame pre, post;
    bool armed = false;
    std::vector<en::ReferenceStats::Call> original_calls;
    std::string where;  // log column: emulated time or view number
    std::ofstream log;
    template <typename... Args>
    void logf(const char* fmt, Args... args) {
        char buf[512];
        std::snprintf(buf, sizeof buf, fmt, args...);
        log << buf;
    }

    struct Totals {
        std::array<en::ReferenceStats::Deviation, 3> deviation{};
        int frames = 0, identical = 0, sequence_mismatch = 0, unmatched = 0;
        uint64_t pixels = 0, differing = 0;
        double worst = 0;
        int worst_frame = -1;
        double ms = 0;
        std::vector<std::string> notes;
    } totals;

    Validator(Machine& m, const World& w) : machine(m), world(w) {}

    uint16_t ds16(uint16_t off) { return machine.memory().read16(Cpu::linear(kData, off)); }
    uint16_t back_page() {
        const uint16_t seg = machine.memory().read16(Cpu::linear(kCode, 0x0011));
        return static_cast<uint16_t>((seg - 0xA000) * 16);
    }
    bool in_window() const {
        const double t = static_cast<double>(machine.emulated_ns()) / 1e9;
        return t >= from && t <= to;
    }

    void install() {
        Cpu& cpu = machine.cpu();
        cpu.add_watch(Cpu::linear(kCode, 0x30C6), [this](Cpu&) {
            armed = false;
            if (!in_window() || machine.memory().read8(Cpu::linear(kData, 0x18)) != 0) return;
            ram.assign(machine.memory().ram(), machine.memory().ram() + vette::host::Memory::kSize);
            machine.ega().render_page(back_page(), pre);
            original_calls.clear();
            char buf[32];
            std::snprintf(buf, sizeof buf, "t=%7.3f", static_cast<double>(machine.emulated_ns()) / 1e9);
            where = buf;
            armed = true;
        });
        const auto record = [this](Cpu& c) {
            if (!armed) return;
            const auto rd = [&](uint16_t off) { return static_cast<int16_t>(ds16(off)); };
            original_calls.push_back({c.regs.r[vette::host::BX], rd(0x3220), rd(0x3222), rd(0x3224)});
        };
        for (const uint16_t at : std::array<uint16_t, 4>{0x32D3, 0x4629, 0x2A9C, 0x2C7A}) {
            cpu.add_watch(Cpu::linear(kCode, at), record);
        }
        cpu.add_watch(Cpu::linear(kCode, 0x0371), [this](Cpu&) {
            if (!armed) return;
            armed = false;
            machine.ega().render_page(back_page(), post);
            compare();
        });
    }

    void compare() {
        const int w = pre.width;
        if (w != 320) return;
        std::vector<uint8_t> mine = pre.pixels;
        const auto t0 = std::chrono::steady_clock::now();
        ref.debug = dump_calls == totals.frames;
        ref.from_world = from_world;
        const en::ReferenceStats st = ref.render(ram.data(), mine, w);
        totals.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const auto rd = [&](uint16_t off) {
            return static_cast<int16_t>(ram[(static_cast<uint32_t>(kData) << 4) + off] | ram[(static_cast<uint32_t>(kData) << 4) + off + 1] << 8);
        };
        const int left = rd(0x315E), top = rd(0x315A), right = rd(0x3160), bottom = rd(0x315C);
        uint64_t diff = 0, total = 0;
        for (int y = top; y <= bottom; ++y) {
            for (int x = left; x <= right; ++x) {
                const size_t i = static_cast<size_t>(y * w + x);
                ++total;
                diff += mine[i] != post.pixels[i] ? 1 : 0;
            }
        }
        const int frame = totals.frames++;
        totals.pixels += total;
        totals.differing += diff;
        totals.identical += diff == 0 ? 1 : 0;
        totals.unmatched += st.unmatched;
        for (size_t k = 0; k < 3; ++k) {
            totals.deviation[k].vertices += st.deviation[k].vertices;
            totals.deviation[k].over4 += st.deviation[k].over4;
            totals.deviation[k].over16 += st.deviation[k].over16;
            if (st.deviation[k].max > totals.deviation[k].max) {
                totals.deviation[k].max = st.deviation[k].max;
                totals.deviation[k].worst_routine = st.deviation[k].worst_routine;
            }
        }
        const bool seq_ok = st.calls == original_calls;
        totals.sequence_mismatch += seq_ok ? 0 : 1;
        const double pct = total ? 100.0 * static_cast<double>(diff) / static_cast<double>(total) : 0;
        if (pct > totals.worst) {
            totals.worst = pct;
            totals.worst_frame = frame;
        }
        for (const auto& n : st.notes) {
            if (std::find(totals.notes.begin(), totals.notes.end(), n) == totals.notes.end()) totals.notes.push_back(n);
        }
        size_t first_bad = 0;
        while (first_bad < std::min(st.calls.size(), original_calls.size()) && st.calls[first_bad] == original_calls[first_bad]) {
            ++first_bad;
        }
        if (log) {
            logf(
                         "frame %4d %s cam %5d %5d %4d yaw %3d pitch %3d bt %d,%d  cells %d objects %3d vehicles %2d "
                         "unmatched %d polys %4d near %3d  diff %5llu / %llu = %.3f%%  calls %s",
                         frame, where.c_str(), rd(0x2C71), rd(0x2C73), rd(0x2C75), rd(0x2C77), rd(0x2C79), rd(0x2C93), rd(0x2C95), st.cells, st.objects, st.vehicles, st.unmatched,
                         st.polygons, st.clipped_near, static_cast<unsigned long long>(diff),
                         static_cast<unsigned long long>(total), pct, seq_ok ? "same" : "DIFFER");
            if (!seq_ok) {
                logf(" (%zu vs %zu, first difference at %zu", st.calls.size(), original_calls.size(), first_bad);
                if (first_bad < original_calls.size()) {
                    const auto& c = original_calls[first_bad];
                    logf(": original %04X %d,%d,%d", c.routine, c.x, c.y, c.z);
                }
                if (first_bad < st.calls.size()) {
                    const auto& c = st.calls[first_bad];
                    logf(" / reference %04X %d,%d,%d", c.routine, c.x, c.y, c.z);
                }
                logf(")");
            }
            logf("\n");
        }
        if (dump_calls == frame) {
            for (size_t k = 0; k < std::max(st.calls.size(), original_calls.size()); ++k) {
                const auto show = [](const std::vector<en::ReferenceStats::Call>& v, size_t i) {
                    char b[48] = "-";
                    if (i < v.size()) std::snprintf(b, sizeof b, "%04X %6d %6d %5d", v[i].routine, v[i].x, v[i].y, v[i].z);
                    return std::string(b);
                };
                std::printf("  %3zu  original %-26s reference %s\n", k, show(original_calls, k).c_str(),
                            show(st.calls, k).c_str());
            }
        }
        if ((png_every > 0 && frame % png_every == 0) || pct > png_over) {
            save_png(frame, mine, top, bottom);
        }
    }

    void save_png(int frame, const std::vector<uint8_t>& mine, int top, int bottom) {
        const int w = 320, h = bottom - top + 1, s = 2;
        const int W = w * 3 * s + 2 * 4, H = h * s;
        std::vector<uint32_t> img(static_cast<size_t>(W * H), 0x202020);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>((y + top) * w + x);
                const uint32_t a = kEga[post.pixels[i] & 15], b = kEga[mine[i] & 15];
                uint32_t d;
                if (post.pixels[i] == mine[i]) {
                    const uint32_t g = ((a >> 16 & 255) + (a >> 8 & 255) + (a & 255)) / 6;
                    d = g << 16 | g << 8 | g;
                } else {
                    d = 0xFF2020;
                }
                for (int dy = 0; dy < s; ++dy) {
                    for (int dx = 0; dx < s; ++dx) {
                        const int yy = y * s + dy;
                        img[static_cast<size_t>(yy * W + x * s + dx)] = a;
                        img[static_cast<size_t>(yy * W + w * s + 4 + x * s + dx)] = b;
                        img[static_cast<size_t>(yy * W + 2 * (w * s + 4) + x * s + dx)] = d;
                    }
                }
            }
        }
        char name[96];
        std::snprintf(name, sizeof name, "%s_frame%04d.png", label.c_str(), frame);
        write_png(out_dir / name, W, H, img);
    }
};

// --- Teleport validation ------------------------------------------------------------------------------
// The original's own 3D drawing section (3009:02DA-0371: camera matrix, axis table, sky, ground and
// horizon, the cell window), replayed on a scratch copy of a race frame with the camera moved
// anywhere on the map. The traffic and pedestrian simulation calls inside it (0350, 0353) are skipped,
// so nothing of the real game changes. Each view is compared like a live frame.
class Teleporter {
public:
    Teleporter(Machine& m, Validator& v) : machine_(m), val_(v), io_(ega_) {
        mem_.set_video(&ega_);
        cpu_ = std::make_unique<Cpu>(mem_, io_);
        cpu_->set_code_hook(Cpu::linear(kCode, 0x0350), [](Cpu& c) { c.regs.ip = static_cast<uint16_t>(c.regs.ip + 3); });
        cpu_->set_code_hook(Cpu::linear(kCode, 0x0353), [](Cpu& c) { c.regs.ip = static_cast<uint16_t>(c.regs.ip + 3); });
        cpu_->set_code_hook(Cpu::linear(kCode, 0x0371), [this](Cpu& c) {
            done_ = true;
            ega_.render_page(back_page(), val_.post);
            c.request_stop();
        });
        cpu_->add_watch(Cpu::linear(kCode, 0x30C6), [this](Cpu&) {
            val_.ram.assign(mem_.ram(), mem_.ram() + vette::host::Memory::kSize);
            ega_.render_page(back_page(), val_.pre);
            val_.original_calls.clear();
        });
        // blit_horizon's copy (vram_copy_rows 3009:6773): AX:SI (the panorama buffer) to DX:DI, BP rows.
        cpu_->add_watch(Cpu::linear(kCode, 0x6773), [this](Cpu& c) {
            if (c.regs.r[vette::host::AX] != 0xA400) return;
            const auto bp = static_cast<int16_t>(c.regs.r[vette::host::BP]);
            horizon_ = {bp > 0 ? bp : 1, c.regs.r[vette::host::SI], c.regs.r[vette::host::DI]};
        });
        cpu_->add_watch(Cpu::linear(kCode, 0xB765), [this](Cpu& c) {
            if (val_.dump_calls == val_.totals.frames) {
                std::printf("original model header %04X at %d,%d,%d%c", c.regs.r[vette::host::SI], s16(0xE0C8), s16(0xE0CA),
                            s16(0xE0CC), 10);
            }
        });
        for (const uint16_t at : std::array<uint16_t, 4>{0x32D3, 0x4629, 0x2A9C, 0x2C7A}) {
            cpu_->add_watch(Cpu::linear(kCode, at), [this](Cpu& c) {
                val_.original_calls.push_back({c.regs.r[vette::host::BX], s16(0x3220), s16(0x3222), s16(0x3224)});
            });
        }
    }

    // Snapshot of the live machine at 3009:02DA of a race frame.
    void snapshot() {
        base_ram_.assign(machine_.memory().ram(), machine_.memory().ram() + vette::host::Memory::kSize);
        base_ega_.copy_state_from(machine_.ega());
        base_regs_ = machine_.cpu().regs;
        ready_ = true;
    }
    bool ready() const { return ready_; }

    // Draws the view from absolute camera position (x, y, z) with the given yaw and pitch; false if the
    // original didn't finish.
    bool view(int32_t x, int32_t y, int32_t z, int yaw, int pitch) {
        std::copy(base_ram_.begin(), base_ram_.end(), mem_.ram());
        ega_.copy_state_from(base_ega_);
        const auto set = [this](uint16_t off, int v) { mem_.write16(Cpu::linear(kData, off), static_cast<uint16_t>(v)); };
        set(0x2C71, x & 0x7FFF);
        set(0x2C73, y & 0x7FFF);
        set(0x2C75, z);
        set(0x2C77, yaw);
        set(0x2C79, pitch);
        set(0x2C7B, 0);
        set(0x2C93, x >> 15);
        set(0x2C95, y >> 15);
        mem_.write8(Cpu::linear(kData, 0x2AD4), 0);  // not on a freeway
        mem_.write8(Cpu::linear(kData, 0x18), 0);    // not the mirror pass
        cpu_->regs = base_regs_;
        cpu_->regs.s[vette::host::CS] = kCode;
        cpu_->regs.ip = 0x02DA;
        done_ = false;
        horizon_ = {};
        for (int i = 0; i < 400 && !done_; ++i) cpu_->run(50000);
        return done_;
    }

    // The last view's horizon copy: rows of 40 bytes from A400:source (400 a row) to the page's byte dest.
    struct Horizon {
        int rows = 0;
        uint16_t source = 0, dest = 0;
    };
    const Horizon& horizon() const { return horizon_; }

private:
    class EgaIo final : public vette::host::IoBus {
    public:
        explicit EgaIo(Ega& e) : ega_(e) {}
        uint8_t in8(uint16_t port) override { return Ega::handles(port) ? ega_.in8(port) : 0xFF; }
        void out8(uint16_t port, uint8_t v) override {
            if (Ega::handles(port)) ega_.out8(port, v);
        }

    private:
        Ega& ega_;
    };

    int16_t s16(uint16_t off) { return static_cast<int16_t>(mem_.read16(Cpu::linear(kData, off))); }
    uint16_t back_page() { return static_cast<uint16_t>((mem_.read16(Cpu::linear(kCode, 0x0011)) - 0xA000) * 16); }

    Machine& machine_;
    Validator& val_;
    vette::host::Memory mem_;
    Ega ega_, base_ega_;
    EgaIo io_;
    std::unique_ptr<Cpu> cpu_;
    std::vector<uint8_t> base_ram_;
    vette::host::Registers base_regs_;
    bool ready_ = false, done_ = false;
    Horizon horizon_;
};

// --- Scene validation ----------------------------------------------------------------------------------------
// A software rasterizer for en::Scene (what the Presenter's GPU does): pixel-centre sampling, the top-left
// rule, scissored to the scene's view rect, painter's order, alpha blending.
class SceneRaster {
public:
    // The scene scaled by `scale` output pixels per race-frame pixel, into `img` (w x h, RGB), whose
    // origin is the race-frame point (ox, oy).
    static void draw_rgb(const en::Scene& s, float scale, int w, int h, float ox, float oy, std::vector<uint32_t>& img) {
        const int cx0 = std::max(0, static_cast<int>(std::lround((s.view_x0 - ox) * scale)));
        const int cy0 = std::max(0, static_cast<int>(std::lround((s.view_y0 - oy) * scale)));
        const int cx1 = std::min(w, static_cast<int>(std::lround((s.view_x1 - ox) * scale)));
        const int cy1 = std::min(h, static_cast<int>(std::lround((s.view_y1 - oy) * scale)));
        for (size_t t = 0; t + 2 < s.indices.size(); t += 3) {
            const en::SceneVertex& a = s.vertices[static_cast<size_t>(s.indices[t])];
            const en::SceneVertex& b = s.vertices[static_cast<size_t>(s.indices[t + 1])];
            const en::SceneVertex& c = s.vertices[static_cast<size_t>(s.indices[t + 2])];
            const uint32_t sr = static_cast<uint32_t>(std::lround(a.r * 255)), sg = static_cast<uint32_t>(std::lround(a.g * 255)),
                           sb = static_cast<uint32_t>(std::lround(a.b * 255));
            const float alpha = a.a;
            triangle((a.x - ox) * scale, (a.y - oy) * scale, (b.x - ox) * scale, (b.y - oy) * scale, (c.x - ox) * scale,
                     (c.y - oy) * scale, cx0, cy0, cx1, cy1, [&](int x, int y) {
                         uint32_t& d = img[static_cast<size_t>(y * w + x)];
                         if (alpha >= 1) {
                             d = sr << 16 | sg << 8 | sb;
                         } else {
                             const auto mix = [&](uint32_t dst, uint32_t src) {
                                 return static_cast<uint32_t>(std::lround(alpha * float(src) + (1 - alpha) * float(dst)));
                             };
                             d = mix(d >> 16 & 255, sr) << 16 | mix(d >> 8 & 255, sg) << 8 | mix(d & 255, sb);
                         }
                     });
        }
    }

    // At 1x: per pixel, the set of EGA colours it may show (a dithered pair; screen-door adds the base
    // colour to what was under it). `masks` starts as the background's colours.
    static void draw_masks(const en::Scene& s, std::vector<uint16_t>& masks) {
        for (size_t t = 0; t + 2 < s.indices.size(); t += 3) {
            const en::SceneVertex& a = s.vertices[static_cast<size_t>(s.indices[t])];
            const en::SceneVertex& b = s.vertices[static_cast<size_t>(s.indices[t + 1])];
            const en::SceneVertex& c = s.vertices[static_cast<size_t>(s.indices[t + 2])];
            const uint16_t m = colour_mask(a.r, a.g, a.b);
            const bool door = a.a < 1;
            triangle(a.x, a.y, b.x, b.y, c.x, c.y, s.view_x0, s.view_y0, s.view_x1, s.view_y1, [&](int x, int y) {
                uint16_t& d = masks[static_cast<size_t>(y * 320 + x)];
                d = door ? static_cast<uint16_t>(d | m) : m;
            });
        }
    }

    static uint16_t colour_mask(float r, float g, float b) {
        uint16_t m = 0;
        for (int raw = 0; raw < 256; ++raw) {
            en::Colour c;
            c.raw = static_cast<uint8_t>(raw);
            const en::SceneColour sc = en::scene_colour(c);
            if (sc.r == r && sc.g == g && sc.b == b) {
                m = static_cast<uint16_t>(m | (1u << c.base()) | (c.dithered() ? 1u << c.second() : 0u));
            }
        }
        return m;
    }

private:
    template <typename Plot>
    static void triangle(float ax, float ay, float bx, float by, float cx, float cy, int x0, int y0, int x1, int y1, Plot plot) {
        const auto edge = [](float px, float py, float qx, float qy, float rx, float ry) {
            return (qx - px) * (ry - py) - (qy - py) * (rx - px);
        };
        float area = edge(ax, ay, bx, by, cx, cy);
        if (area == 0) return;
        if (area < 0) {
            std::swap(bx, cx);
            std::swap(by, cy);
        }
        const int minx = std::max(x0, static_cast<int>(std::floor(std::min({ax, bx, cx}))));
        const int maxx = std::min(x1 - 1, static_cast<int>(std::ceil(std::max({ax, bx, cx}))));
        const int miny = std::max(y0, static_cast<int>(std::floor(std::min({ay, by, cy}))));
        const int maxy = std::min(y1 - 1, static_cast<int>(std::ceil(std::max({ay, by, cy}))));
        // Top-left rule for triangles with a positive edge function (clockwise on a y-down screen).
        const auto top_left = [](float px, float py, float qx, float qy) { return qy < py || (qy == py && qx > px); };
        const bool t0 = top_left(bx, by, cx, cy), t1 = top_left(cx, cy, ax, ay), t2 = top_left(ax, ay, bx, by);
        for (int y = miny; y <= maxy; ++y) {
            const float py = float(y) + 0.5f;
            for (int x = minx; x <= maxx; ++x) {
                const float px = float(x) + 0.5f;
                const float w0 = edge(bx, by, cx, cy, px, py);
                const float w1 = edge(cx, cy, ax, ay, px, py);
                const float w2 = edge(ax, ay, bx, by, px, py);
                if ((w0 > 0 || (w0 == 0 && t0)) && (w1 > 0 || (w1 == 0 && t1)) && (w2 > 0 || (w2 == 0 && t2))) plot(x, y);
            }
        }
    }
};

// The hook of the comparison mode: picks each object's variant by running the original routine on a
// copy of the frame, in draw order, as world_reference.h does.
class OriginalChooser final : public en::SceneHook {
public:
    explicit OriginalChooser(const World& w) : world_(w) {}
    void frame(const uint8_t* ram) {
        if (!tracer_) {
            tracer_ = std::make_unique<en::Tracer>(ram);
        } else {
            tracer_->load(ram);
        }
    }
    int unmatched = 0;

    const en::Variant* choose(const Object& o, std::vector<std::array<int16_t, 3>>& angles) override {
        en::Tracer& t = *tracer_;
        for (int i = 0; i < 3; ++i) {
            t.wr16(kData, static_cast<uint16_t>(0x3220 + 2 * i), static_cast<uint16_t>(o.position[static_cast<size_t>(i)]));
        }
        if (o.sortable) t.wr16(kCode, 0x259E, o.sort_key);
        t.wr8(kData, 0x35C3, o.facing);
        t.wr8(kData, 0x35C4, o.own_cell ? 0xFF : 0);
        t.run(o.routine, o.position, trace_);
        const en::Routine* r = world_.routine(o.routine);
        if (!r) return nullptr;
        if (r->compound && trace_.events.empty()) return nullptr;  // drawn already this frame (DS:2AC0)
        const uint64_t sig = trace_.signature();
        for (const en::Variant& v : r->variants) {
            if (v.signature != sig) continue;
            for (const en::TraceEvent& e : trace_.events) {
                if (e.op == en::TraceEvent::Op::Plain) angles.push_back(e.angles);
            }
            return &v;
        }
        ++unmatched;
        return nullptr;
    }

private:
    const World& world_;
    std::unique_ptr<en::Tracer> tracer_;
    en::Trace trace_;
};

// --scene-compare, --scene-shots, --scene-bench: SceneBuilder against the original, frame by frame.
struct SceneCheck {
    Machine& machine;
    const World& world;
    std::filesystem::path out_dir;
    std::string label = "scene";
    double from = -1, to = -1;           // compare window
    std::vector<double> shots;           // emulated seconds
    double bench_from = -1, bench_to = -1;
    int scale = 6;

    en::SceneBuilder builder{world};
    OriginalChooser chooser{world};
    en::Scene scene;
    std::vector<uint8_t> ram;
    Ega::Frame pre, post, final_frame;
    bool armed = false;
    bool highway = false;        // the frame being checked is a freeway frame (captured at 03B4 / 0405)
    bool mirror_drawn = false;   // this frame's mirror pass drew (0686)
    int pending_shot = -1;       // a shot waiting for the mirror to be drawn (or not, 0445)
    // The mirror's compare: its memory (for the builder: the main view's camera angles restored, as
    // SceneOptions::mirror derives the mirror's; for the chooser: as the mirror pass has it), and its
    // image before and after its world.
    std::vector<uint8_t> mram, mram_chooser;
    Ega::Frame mpre, mpost;
    bool mirror_armed = false;
    uint16_t main_yaw = 0, main_pitch = 0;
    size_t next_shot = 0;
    std::ofstream log;

    struct Totals {
        int frames = 0, identical = 0;
        uint64_t pixels = 0, differing = 0, structural = 0;
        double worst = 0;
        int worst_frame = -1;
        int unmatched = 0;
    } cmp, mcmp;
    struct Bench {
        int frames = 0;
        double ms_map = 0, max_map = 0, ms_r8 = 0, max_r8 = 0, ms_mirror = 0, max_mirror = 0;
        int mirror_frames = 0;
        uint64_t tris_map = 0, verts_map = 0, tris_r8 = 0, lines_map = 0, objects_map = 0, cells_map = 0, vehicles_map = 0;
        int max_tris_map = 0;
    } bench;

    SceneCheck(Machine& m, const World& w) : machine(m), world(w) {}

    double now() const { return static_cast<double>(machine.emulated_ns()) / 1e9; }
    uint16_t back_page() {
        const uint16_t seg = machine.memory().read16(Cpu::linear(kCode, 0x0011));
        return static_cast<uint16_t>((seg - 0xA000) * 16);
    }
    bool wanted() const {
        const double t = now();
        return (t >= from && t <= to) || (t >= bench_from && t <= bench_to) || (next_shot < shots.size() && t >= shots[next_shot]);
    }

    bool highway_mode() { return machine.memory().read8(Cpu::linear(kData, 0x2AD4)) == 0xFF; }

    void install() {
        Cpu& cpu = machine.cpu();
        // City frames: the memory at draw_world_cells' entry, the image before and after it.
        cpu.add_watch(Cpu::linear(kCode, 0x30C6), [this](Cpu&) {
            if (machine.memory().read8(Cpu::linear(kData, 0x18)) != 0) return;  // the mirror's pass
            note_main_camera();
            armed = false;
            mirror_drawn = false;
            if (!wanted() || highway_mode()) return;
            ram.assign(machine.memory().ram(), machine.memory().ram() + vette::host::Memory::kSize);
            machine.ega().render_page(back_page(), pre);
            highway = false;
            armed = true;
        });
        cpu.add_watch(Cpu::linear(kCode, 0x0371), [this](Cpu&) {
            if (!armed || highway) return;
            armed = false;
            machine.ega().render_page(back_page(), post);
            checks();
        });
        // Freeway frames: the memory before highway_frame (03B4), the image after highway_draw_cars (0405).
        cpu.add_watch(Cpu::linear(kCode, 0x03B4), [this](Cpu&) {
            note_main_camera();
            armed = false;
            mirror_drawn = false;
            if (!wanted() || !highway_mode()) return;
            ram.assign(machine.memory().ram(), machine.memory().ram() + vette::host::Memory::kSize);
            machine.ega().render_page(back_page(), pre);
            highway = true;
            armed = true;
        });
        cpu.add_watch(Cpu::linear(kCode, 0x0405), [this](Cpu&) {
            if (!armed || !highway) return;
            armed = false;
            machine.ega().render_page(back_page(), post);
            checks();
        });
        // The mirror (draw_mirror_view 0666): drawn this frame; the frame as shown, at its end.
        cpu.add_watch(Cpu::linear(kCode, 0x0686), [this](Cpu&) { mirror_drawn = true; });
        // The mirror's world: after its sky and ground (074F), before its frame (075F).
        cpu.add_watch(Cpu::linear(kCode, 0x074F), [this](Cpu&) {
            mirror_armed = false;
            const double t = now();
            if (t < from || t > to) return;
            mram_chooser.assign(machine.memory().ram(), machine.memory().ram() + vette::host::Memory::kSize);
            mram = mram_chooser;
            const uint32_t ds = static_cast<uint32_t>(kData) << 4;
            mram[ds + 0x2C77] = static_cast<uint8_t>(main_yaw);
            mram[ds + 0x2C78] = static_cast<uint8_t>(main_yaw >> 8);
            mram[ds + 0x2C79] = static_cast<uint8_t>(main_pitch);
            mram[ds + 0x2C7A] = static_cast<uint8_t>(main_pitch >> 8);
            machine.ega().render_page(back_page(), mpre);
            mirror_armed = true;
        });
        cpu.add_watch(Cpu::linear(kCode, 0x075F), [this](Cpu&) {
            if (!mirror_armed) return;
            mirror_armed = false;
            machine.ega().render_page(back_page(), mpost);
            compare(mram, mram_chooser, mpre, mpost, true, mcmp, "_mirror");
        });
        cpu.add_watch(Cpu::linear(kCode, 0x0445), [this](Cpu&) {  // past the mirror (0434-0442), on or off
            if (pending_shot < 0) return;
            machine.ega().render_page(back_page(), final_frame);
            shot(static_cast<size_t>(pending_shot));
            pending_shot = -1;
        });
    }

    void note_main_camera() {
        main_yaw = machine.memory().read16(Cpu::linear(kData, 0x2C77));
        main_pitch = machine.memory().read16(Cpu::linear(kData, 0x2C79));
    }

    void checks() {
        const double t = now();
        if (t >= from && t <= to) compare(ram, ram, pre, post, false, cmp, "");
        if (t >= bench_from && t <= bench_to) time_builds();
        if (next_shot < shots.size() && t >= shots[next_shot]) {
            pending_shot = static_cast<int>(next_shot);  // taken at the frame's end, with the mirror
            while (next_shot < shots.size() && shots[next_shot] <= t) ++next_shot;
        }
    }

    void compare(const std::vector<uint8_t>& rb, const std::vector<uint8_t>& rc, const Ega::Frame& fpre,
                 const Ega::Frame& fpost, bool mirror, Totals& tot, const char* tag) {
        if (fpre.width != 320) return;
        en::SceneOptions o;
        o.original_window = true;
        o.hook = &chooser;
        o.line_width = 1;
        o.min_line_length = 0;
        o.mirror = mirror;
        chooser.frame(rc.data());
        const int unmatched_before = chooser.unmatched;
        builder.build(rb.data(), o, scene);
        std::vector<uint16_t> masks(320 * 200);
        for (size_t i = 0; i < masks.size(); ++i) masks[i] = static_cast<uint16_t>(1u << (fpre.pixels[i] & 15));
        SceneRaster::draw_masks(scene, masks);
        uint64_t diff = 0, structural = 0, total = 0;
        std::vector<uint8_t> bad(320 * 200, 0);
        const int y_end = highway && !mirror ? scene.view_y1 - 1 : scene.view_y1;  // freeway frames: not the view border
        for (int y = scene.view_y0; y < y_end; ++y) {
            for (int x = scene.view_x0; x < scene.view_x1; ++x) {
                const size_t i = static_cast<size_t>(y * 320 + x);
                ++total;
                const uint8_t p = fpost.pixels[i] & 15;
                if (masks[i] & (1u << p)) continue;
                ++diff;
                bad[i] = 1;
                // Structural: nothing within a pixel explains it, either way.
                bool near_ok = false;
                for (int dy = -1; dy <= 1 && !near_ok; ++dy) {
                    for (int dx = -1; dx <= 1 && !near_ok; ++dx) {
                        const int xx = x + dx, yy = y + dy;
                        if (xx < scene.view_x0 || yy < scene.view_y0 || xx >= scene.view_x1 || yy >= scene.view_y1) continue;
                        const size_t j = static_cast<size_t>(yy * 320 + xx);
                        near_ok = (masks[j] & (1u << p)) || (masks[i] & (1u << (fpost.pixels[j] & 15)));
                    }
                }
                if (!near_ok) {
                    ++structural;
                    bad[i] = 2;
                }
            }
        }
        const auto rs16 = [&](uint16_t off) {
            const uint32_t a = (static_cast<uint32_t>(kData) << 4) + off;
            return static_cast<int16_t>(rb[a] | rb[a + 1] << 8);
        };
        const int frame = tot.frames++;
        tot.pixels += total;
        tot.differing += diff;
        tot.structural += structural;
        tot.identical += diff == 0 ? 1 : 0;
        tot.unmatched += chooser.unmatched - unmatched_before;
        const double pct = total ? 100.0 * static_cast<double>(diff) / static_cast<double>(total) : 0;
        const double spct = total ? 100.0 * static_cast<double>(structural) / static_cast<double>(total) : 0;
        if (spct > tot.worst) {
            tot.worst = spct;
            tot.worst_frame = frame;
        }
        if (log) {
            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "compare%s %4d t=%7.3f%s cam %d %d %d yaw %d pitch %d roll %d triangles %5d  differ %5llu (%.2f%%)  "
                          "structural %4llu (%.3f%%)\n",
                          tag, frame, now(), highway ? " freeway" : "", rs16(0x2C71), rs16(0x2C73), rs16(0x2C75), rs16(0x2C77), rs16(0x2C79),
                          rs16(0x2C7B), scene.stats.triangles, static_cast<unsigned long long>(diff), pct,
                          static_cast<unsigned long long>(structural), spct);
            log << buf;
        }
        if (spct > 0.25 || frame % 50 == 0) {
            // Original | scene (1x) | differences: red structural, yellow edge.
            std::vector<uint32_t> mine(320 * 200);
            for (size_t i = 0; i < mine.size(); ++i) mine[i] = kEga[fpre.pixels[i] & 15];
            SceneRaster::draw_rgb(scene, 1, 320, 200, 0, 0, mine);
            const int x0 = scene.view_x0, y0 = scene.view_y0, w = scene.view_x1 - x0, h = scene.view_y1 - y0, s = 2;
            const int W = 3 * w * s + 8, H = h * s;
            std::vector<uint32_t> img(static_cast<size_t>(W * H), 0x202020);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = static_cast<size_t>((y + y0) * 320 + x + x0);
                    const uint32_t a = kEga[fpost.pixels[i] & 15];
                    const uint32_t g = ((a >> 16 & 255) + (a >> 8 & 255) + (a & 255)) / 6;
                    const uint32_t d = bad[i] == 2 ? 0xFF2020 : bad[i] == 1 ? 0xE0C020 : (g << 16 | g << 8 | g);
                    for (int dy = 0; dy < s; ++dy) {
                        for (int dx = 0; dx < s; ++dx) {
                            const size_t row = static_cast<size_t>((y * s + dy) * W);
                            img[row + static_cast<size_t>(x * s + dx)] = a;
                            img[row + static_cast<size_t>(w * s + 4 + x * s + dx)] = mine[i];
                            img[row + static_cast<size_t>(2 * (w * s + 4) + x * s + dx)] = d;
                        }
                    }
                }
            }
            char name[96];
            std::snprintf(name, sizeof name, "%s_compare%s%04d.png", label.c_str(), tag, frame);
            write_png(out_dir / name, W, H, img);
        }
    }

    void time_builds() {
        en::SceneOptions o;
        o.pixel_w = o.pixel_h = static_cast<float>(scale);  // as drawn at the PNGs' resolution
        builder.build(ram.data(), o, scene);  // warm
        double best = 1e9;
        for (int k = 0; k < 5; ++k) {
            builder.build(ram.data(), o, scene);
            best = std::min(best, scene.stats.milliseconds);
        }
        ++bench.frames;
        bench.ms_map += best;
        bench.max_map = std::max(bench.max_map, best);
        bench.tris_map += static_cast<uint64_t>(scene.stats.triangles);
        bench.max_tris_map = std::max(bench.max_tris_map, scene.stats.triangles);
        bench.verts_map += scene.vertices.size();
        bench.lines_map += static_cast<uint64_t>(scene.stats.lines);
        bench.objects_map += static_cast<uint64_t>(scene.stats.objects);
        bench.cells_map += static_cast<uint64_t>(scene.stats.cells);
        bench.vehicles_map += static_cast<uint64_t>(scene.stats.vehicles);
        o.radius = 8;
        best = 1e9;
        for (int k = 0; k < 5; ++k) {
            builder.build(ram.data(), o, scene);
            best = std::min(best, scene.stats.milliseconds);
        }
        bench.ms_r8 += best;
        bench.max_r8 = std::max(bench.max_r8, best);
        bench.tris_r8 += static_cast<uint64_t>(scene.stats.triangles);
        if (ram[(static_cast<uint32_t>(kData) << 4) + 0x2AC7] == 0) {  // the mirror is on: its view, whole map
            o.radius = en::kMapCells;
            o.mirror = true;
            best = 1e9;
            for (int k = 0; k < 5; ++k) {
                builder.build(ram.data(), o, scene);
                best = std::min(best, scene.stats.milliseconds);
            }
            ++bench.mirror_frames;
            bench.ms_mirror += best;
            bench.max_mirror = std::max(bench.max_mirror, best);
        }
    }

    // Original | radius 8 | whole map, each the 3D view at `scale` x.
    void shot(size_t index) {
        if (pre.width != 320) return;
        const int x0 = ram_s16(0x315E), y0 = ram_s16(0x315A), x1 = ram_s16(0x3160) + 1, y1 = ram_s16(0x315C) + 1;
        const int w = (x1 - x0) * scale, h = (y1 - y0) * scale;
        std::vector<uint32_t> img(static_cast<size_t>(w) * static_cast<size_t>(h) * 3, 0);
        const auto upscale = [&](const Ega::Frame& f, size_t at) {
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    img[at + static_cast<size_t>(y * w + x)] = kEga[f.pixels[static_cast<size_t>((y0 + y / scale) * 320 + x0 + x / scale)] & 15];
                }
            }
        };
        upscale(final_frame.width == 320 ? final_frame : post, 0);
        const size_t plane = static_cast<size_t>(w) * static_cast<size_t>(h);
        int tris[2] = {0, 0}, mirror_tris[2] = {0, 0};
        double ms[2] = {0, 0}, mirror_ms[2] = {0, 0};
        for (int k = 0; k < 2; ++k) {
            upscale(pre, plane * static_cast<size_t>(k + 1));
            en::SceneOptions o;
            o.radius = k == 0 ? 8 : en::kMapCells;
            o.pixel_w = o.pixel_h = static_cast<float>(scale);
            std::vector<uint32_t> part(img.begin() + static_cast<std::ptrdiff_t>(plane * static_cast<size_t>(k + 1)),
                                       img.begin() + static_cast<std::ptrdiff_t>(plane * static_cast<size_t>(k + 2)));
            builder.build(ram.data(), o, scene);
            tris[k] = scene.stats.triangles;
            ms[k] = scene.stats.milliseconds;
            SceneRaster::draw_rgb(scene, static_cast<float>(scale), w, h, static_cast<float>(x0), static_cast<float>(y0), part);
            if (mirror_drawn) {
                o.mirror = true;
                builder.build(ram.data(), o, scene);
                mirror_tris[k] = scene.stats.triangles;
                mirror_ms[k] = scene.stats.milliseconds;
                SceneRaster::draw_rgb(scene, static_cast<float>(scale), w, h, static_cast<float>(x0), static_cast<float>(y0), part);
            }
            std::copy(part.begin(), part.end(), img.begin() + static_cast<std::ptrdiff_t>(plane * static_cast<size_t>(k + 1)));
        }
        char name[96];
        std::snprintf(name, sizeof name, "%s_shot%zu_t%.1f.png", label.c_str(), index, now());
        write_png(out_dir / name, w, h * 3, img);
        std::printf("scene shot %s%s: radius 8 %d triangles %.2f ms, whole map %d triangles %.2f ms", name,
                    highway ? " (freeway)" : "", tris[0], ms[0], tris[1], ms[1]);
        if (mirror_drawn) {
            std::printf("; mirror %d triangles %.2f ms / %d triangles %.2f ms", mirror_tris[0], mirror_ms[0], mirror_tris[1],
                        mirror_ms[1]);
        }
        std::printf("\n");
    }

    int ram_s16(uint16_t off) const {
        const size_t a = (static_cast<size_t>(kData) << 4) + off;
        return static_cast<int16_t>(ram[a] | ram[a + 1] << 8);
    }

    void report() {
        for (const auto* t : {&cmp, &mcmp}) {
            if (!t->frames) continue;
            std::printf("scene compare %s%s: %d frames (original window, original LOD), %d identical, %.3f%% of view pixels "
                        "differ, %.4f%% structurally (no match within 1 px; worst frame %d: %.3f%%), %d objects without "
                        "a matching variant\n",
                        label.c_str(), t == &mcmp ? " mirror" : "", t->frames, t->identical,
                        100.0 * static_cast<double>(t->differing) / static_cast<double>(std::max<uint64_t>(t->pixels, 1)),
                        100.0 * static_cast<double>(t->structural) / static_cast<double>(std::max<uint64_t>(t->pixels, 1)),
                        t->worst_frame, t->worst, t->unmatched);
        }
        if (bench.frames) {
            const double n = bench.frames;
            std::printf("scene bench %s: %d frames; whole map %.2f ms avg, %.2f max, %.0f triangles avg (max %d), %.0f vertices, "
                        "%.0f line quads, %.0f objects, %.0f cells, %.1f vehicles; radius 8 %.2f ms avg, %.2f max, %.0f triangles\n",
                        label.c_str(), bench.frames, bench.ms_map / n, bench.max_map, double(bench.tris_map) / n,
                        bench.max_tris_map, double(bench.verts_map) / n, double(bench.lines_map) / n,
                        double(bench.objects_map) / n, double(bench.cells_map) / n, double(bench.vehicles_map) / n,
                        bench.ms_r8 / n, bench.max_r8, double(bench.tris_r8) / n);
            if (bench.mirror_frames) {
                std::printf("scene bench %s: mirror (whole map) %.2f ms avg, %.2f max over %d frames\n", label.c_str(),
                            bench.ms_mirror / bench.mirror_frames, bench.max_mirror, bench.mirror_frames);
            }
        }
    }
};

// Whether two extractions agree (the world must not depend on when it is extracted).
std::string compare_worlds(const World& a, const World& b) {
    if (a.big_rows != b.big_rows || a.big_cols != b.big_cols) return "map size";
    for (size_t i = 0; i < a.cells.size(); ++i) {
        if (a.cells[i].type != b.cells[i].type || a.cells[i].elevation != b.cells[i].elevation) return "cells";
    }
    if (a.routines.size() != b.routines.size()) return "routine count";
    for (size_t i = 0; i < a.routines.size(); ++i) {
        const auto& ra = a.routines[i];
        const auto& rb = b.routines[i];
        if (ra.address != rb.address || ra.variants.size() != rb.variants.size()) return "routine " + std::to_string(ra.address);
        for (size_t k = 0; k < ra.variants.size(); ++k) {
            const auto& va = ra.variants[k];
            const auto& vb = rb.variants[k];
            if (va.signature != vb.signature || va.primitives != vb.primitives || va.reorders.size() != vb.reorders.size()) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "routine %04X variant %zu", ra.address, k);
                return buf;
            }
        }
    }
    for (size_t id = 0; id < a.models.size(); ++id) {
        for (int far = 0; far < 2; ++far) {
            const auto& ma = far ? a.models[id].far_mesh : a.models[id].near_mesh;
            const auto& mb = far ? b.models[id].far_mesh : b.models[id].near_mesh;
            if (ma.verts != mb.verts || ma.faces.size() != mb.faces.size() || ma.order != mb.order) {
                return "model " + std::to_string(id);
            }
            for (size_t f = 0; f < ma.faces.size(); ++f) {
                if (ma.faces[f].flags != mb.faces[f].flags || ma.faces[f].colour.raw != mb.faces[f].colour.raw ||
                    ma.faces[f].prims != mb.faces[f].prims) {
                    return "model " + std::to_string(id) + " face";
                }
            }
        }
    }
    if (a.compounds.size() != b.compounds.size()) return "compounds";
    return "";
}

int usage() {
    std::fprintf(stderr, "usage: vette_world --game <dir> [--out dir] [--catalogue] [--map] [--validate A:B]\n"
                         "       [--png-every N] [--png-over P] [--label NAME] [--key T:SC]... [--hold A:B:SC]...\n"
                         "       [--seconds N] [--manual-check] [--teleport N] [--teleport-at T] [--seed S]\n"
                         "       [--dump-calls N]\n");
    return 2;
}

} // namespace

// --bridge-check: the camera swept along every bridge (the compound structures) on the deck, both ways
// and looking to either side (F1/F3), and from above (the helicopter view's height and pitch); at each point the original's own view
// (replayed by the Teleporter) against the Enhanced scene (radius 8). Where the original drew
// something, the scene must show the same: anything else in front of it there is a painter's error or a
// wrong variant. Prints the worst views and writes PNGs of them (original | scene | differences).
int run_bridge_check(const World& world, Teleporter& teleporter, Validator& tval, const std::filesystem::path& out_dir,
                     const std::string& label, const std::vector<int>& only) {
    en::SceneBuilder builder(world);
    en::Scene scene;
    struct View {
        double structural = 0;  // % of the original's drawn pixels the scene doesn't explain
        int compound = 0, x = 0, y = 0, z = 0, yaw = 0, pitch = 0;
        std::vector<uint32_t> img;
        int w = 0, h = 0;
    };
    std::vector<View> worst;
    int views = 0, failed = 0, bad = 0;
    double total = 0;
    for (size_t ci = 0; ci < world.compounds.size(); ++ci) {
        const auto& c = world.compounds[ci];
        const en::Routine* r = world.routine(c.routine);
        if (!r || r->variants.empty()) continue;
        int32_t x0 = INT32_MAX, x1 = INT32_MIN, y0 = INT32_MAX, y1 = INT32_MIN;
        for (const auto& v : r->variants) {
            for (const auto& sc : v.calls) {
                x0 = std::min(x0, c.position.x + sc.offset.x);
                x1 = std::max(x1, c.position.x + sc.offset.x);
                y0 = std::min(y0, c.position.y + sc.offset.y);
                y1 = std::max(y1, c.position.y + sc.offset.y);
            }
        }
        if (x0 > x1) continue;
        const bool along_y = y1 - y0 >= x1 - x0;
        const int32_t a0 = along_y ? y0 : x0, a1 = along_y ? y1 : x1;
        const int32_t centre = along_y ? (x0 + x1) / 2 : (y0 + y1) / 2;
        const int fwd = along_y ? 90 : 0;
        for (int32_t a = a0; a <= a1; a += 384) {
            for (int lateral = -1200; lateral <= 1200; lateral += 200) {
                for (const int dir : {0, 180, 85, 265}) {  // along the bridge both ways, and to either side
                    for (const bool heli : {false, true}) {
                        if (heli && dir % 180 != 0) continue;
                        const int32_t x = along_y ? centre + lateral : a, y = along_y ? a : centre + lateral;
                        const int32_t z = heli ? 146 : 10;
                        const int yaw = (fwd + dir) % 360, pitch = heli ? -17 : 0;
                        if (only.size() == 5 && (x != only[0] || y != only[1] || z != only[2] || yaw != only[3] || pitch != only[4])) {
                            continue;
                        }
                        if (!teleporter.view(x, y, z, yaw, pitch)) {
                            ++failed;
                            continue;
                        }
                        ++views;
                        if (only.size() == 5) {
                            for (const auto& call : tval.original_calls) {
                                std::printf("  original draws %04X at %d,%d,%d\n", call.routine, call.x, call.y, call.z);
                            }
                        }
                        en::SceneOptions o;
                        o.radius = 8;
                        o.line_width = 1;
                        o.min_line_length = 0;
                        builder.build(tval.ram.data(), o, scene);
                        std::vector<uint16_t> masks(320 * 200);
                        for (size_t i = 0; i < masks.size(); ++i) masks[i] = static_cast<uint16_t>(1u << (tval.pre.pixels[i] & 15));
                        SceneRaster::draw_masks(scene, masks);
                        int drawn = 0, wrong = 0;
                        std::vector<uint8_t> flag(320 * 200, 0);
                        for (int py = scene.view_y0; py < scene.view_y1; ++py) {
                            for (int px = scene.view_x0; px < scene.view_x1; ++px) {
                                const size_t i = static_cast<size_t>(py * 320 + px);
                                const uint8_t o_px = tval.post.pixels[i] & 15;
                                if (o_px == (tval.pre.pixels[i] & 15)) continue;  // the original drew nothing here
                                ++drawn;
                                bool ok = false;
                                for (int dy = -1; dy <= 1 && !ok; ++dy) {
                                    for (int dx = -1; dx <= 1 && !ok; ++dx) {
                                        const int xx = px + dx, yy = py + dy;
                                        if (xx < 0 || yy < 0 || xx >= 320 || yy >= 200) continue;
                                        ok = (masks[static_cast<size_t>(yy * 320 + xx)] & (1u << o_px)) != 0;
                                    }
                                }
                                if (!ok) {
                                    ++wrong;
                                    flag[i] = 1;
                                }
                            }
                        }
                        if (drawn < 3000) continue;  // the original drew too little here to judge (open water)
                        // On the deck: the original shows road (grey) under the car, not water.
                        const size_t under_car = static_cast<size_t>((scene.view_y1 - 3) * 320 + 160);
                        if (!heli && (tval.post.pixels[under_car] & 15) != 8) continue;
                        const double pct = 100.0 * wrong / drawn;
                        total += pct;
                        if (pct > 1.0) ++bad;
                        size_t mine_n = 0;
                        const View* mine_least = nullptr;
                        for (const View& w : worst) {
                            if (w.compound != static_cast<int>(ci)) continue;
                            ++mine_n;
                            if (!mine_least || w.structural < mine_least->structural) mine_least = &w;
                        }
                        if (mine_n < 3 || pct > mine_least->structural) {
                            View v;
                            v.structural = pct;
                            v.compound = static_cast<int>(ci);
                            v.x = x;
                            v.y = y;
                            v.z = z;
                            v.yaw = yaw;
                            v.pitch = pitch;
                            // Original | scene | wrong pixels in red, 2x.
                            const int vx0 = scene.view_x0, vy0 = scene.view_y0, w = scene.view_x1 - vx0, h = scene.view_y1 - vy0;
                            std::vector<uint32_t> mine(320 * 200);
                            for (size_t i = 0; i < mine.size(); ++i) mine[i] = kEga[tval.pre.pixels[i] & 15];
                            SceneRaster::draw_rgb(scene, 1, 320, 200, 0, 0, mine);
                            v.w = 3 * w * 2 + 8;
                            v.h = h * 2;
                            v.img.assign(static_cast<size_t>(v.w * v.h), 0x202020);
                            for (int yy = 0; yy < h * 2; ++yy) {
                                for (int xx = 0; xx < w * 2; ++xx) {
                                    const size_t i = static_cast<size_t>((yy / 2 + vy0) * 320 + xx / 2 + vx0);
                                    const size_t row = static_cast<size_t>(yy * v.w);
                                    v.img[row + static_cast<size_t>(xx)] = kEga[tval.post.pixels[i] & 15];
                                    v.img[row + static_cast<size_t>(w * 2 + 4 + xx)] = mine[i];
                                    v.img[row + static_cast<size_t>(2 * (w * 2 + 4) + xx)] = flag[i] ? 0xFF2020 : (mine[i] >> 1 & 0x7F7F7F);
                                }
                            }
                            if (mine_n >= 3) {
                                worst.erase(worst.begin() + (mine_least - worst.data()));
                            }
                            worst.push_back(std::move(v));
                            std::sort(worst.begin(), worst.end(), [](const View& p, const View& q) {
                                return p.compound != q.compound ? p.compound < q.compound : p.structural > q.structural;
                            });
                        }
                    }
                }
            }
        }
    }
    std::printf("bridge check %s: %d views over %zu bridges, %.3f%% of the original's drawn pixels unexplained on "
                "average, %d views over 1%%", label.c_str(), views, world.compounds.size(), views ? total / views : 0, bad);
    if (failed) std::printf(", %d views did not finish in the original", failed);
    std::printf("\n");
    for (size_t k = 0; k < worst.size(); ++k) {
        const View& v = worst[k];
        char name[96];
        std::snprintf(name, sizeof name, "%s_bridge%zu.png", label.c_str(), k);
        write_png(out_dir / name, v.w, v.h, v.img);
        std::printf("  %s: %.2f%% (compound %04X, camera %d,%d,%d yaw %d pitch %d)\n", name, v.structural,
                    world.compounds[static_cast<size_t>(v.compound)].routine, v.x, v.y, v.z, v.yaw, v.pitch);
    }
    return 0;
}

// --sky-views: the Enhanced view with the painted and the Hills skyline, from places around the city.
struct SkyView {
    int32_t x = 0, y = 0, z = 10;  // z above the ground
    int yaw = 0, pitch = 0;
    std::string name;
};
std::vector<SkyView> default_sky_views(bool sweep) {
    // Map coordinates: x north, y east (big tiles of 8000h; the city is in the south-west 3 x 3); yaw 0 north,
    // 90 east. The places are on the courses' roads.
    std::vector<SkyView> v = {
        {16000, 4200, 10, 0, 0, "great_highway_north"},
        {24448, 4224, 10, 90, 0, "great_highway_east"},
        {38816, 153000, 10, 270, 0, "bay_bridge_west"},
        {37000, 120000, 10, 270, 0, "bay_west"},
        {110000, 4750, 10, 180, 0, "golden_gate_south"},
        {49024, 76800, 10, 90, 0, "downtown_east"},
        {42000, 39040, 10, 200, 0, "twin_peaks"},
        {36736, 45000, 10, 270, 0, "city_west"},
        {49024, 76800, 146, 270, -17, "helicopter_west"},
    };
    if (sweep) {
        // Every panorama all the way round: from the west side (HORIZON2), the city (HORIZON1), the bay (HORIZON0).
        const struct {
            int32_t x, y;
            const char* name;
        } places[] = {{16000, 4200, "west"}, {42000, 39040, "city"}, {38816, 147000, "bay"}};
        for (const auto& p : places) {
            for (int yaw = 0; yaw < 360; yaw += 40) {
                char name[48];
                std::snprintf(name, sizeof name, "sweep_%s_%03d", p.name, yaw);
                v.push_back({p.x, p.y, 10, yaw, 0, name});
            }
        }
    }
    return v;
}

int run_sky_views(const World& world, Machine& machine, Teleporter& teleporter, Validator& tval,
                  const std::filesystem::path& out_dir, const std::string& label, const std::vector<SkyView>& views,
                  int scale, bool replicas) {
    en::SceneBuilder builder(world);
    en::Scene scene;
    en::Backdrop backdrop;
    for (const SkyView& v : views) {
        const int32_t z = world.ground_z(v.x, v.y) + v.z;
        if (!teleporter.view(v.x, v.y, z, v.yaw, v.pitch)) {
            std::printf("sky view %s: the original did not finish\n", v.name.c_str());
            continue;
        }
        const Teleporter::Horizon& hz = teleporter.horizon();
        Ega::Frame hills = tval.pre;
        const bool applied = hz.rows > 0 && backdrop.apply(machine.ega(), hz.rows, hz.source, hz.dest, hills.pixels.data(),
                                                           hills.width, hills.height);
        // The scene at `scale` (the display's resolution) and at 1 (the original's, enlarged).
        en::SceneOptions o;
        o.pixel_w = o.pixel_h = static_cast<float>(scale);
        o.replicas = replicas;
        builder.build(tval.ram.data(), o, scene);
        std::printf("sky view %s: %d vehicles and pedestrians\n", v.name.c_str(), scene.stats.vehicles);
        en::Scene low;
        en::SceneOptions o1;
        o1.line_width = 1;
        o1.replicas = replicas;
        builder.build(tval.ram.data(), o1, low);
        const int vy0 = scene.view_y0, vy1 = scene.view_y1;
        const int w = 320 * scale, h = (vy1 - vy0) * scale, gap = 6;
        // Painted | Hills side by side, display resolution above the original's.
        const int W = 2 * w + gap, H = 2 * h + gap;
        std::vector<uint32_t> img(static_cast<size_t>(W) * static_cast<size_t>(H), 0x202020);
        for (int col = 0; col < 2; ++col) {
            const Ega::Frame& under = col == 0 ? tval.pre : hills;
            std::vector<uint32_t> hi(static_cast<size_t>(w) * static_cast<size_t>(h));
            for (int yy = 0; yy < h; ++yy) {
                for (int xx = 0; xx < w; ++xx) {
                    hi[static_cast<size_t>(yy * w + xx)] = kEga[under.pixels[static_cast<size_t>((yy / scale + vy0) * 320 + xx / scale)] & 15];
                }
            }
            SceneRaster::draw_rgb(scene, static_cast<float>(scale), w, h, 0, static_cast<float>(vy0), hi);
            std::vector<uint32_t> lo(static_cast<size_t>(320 * (vy1 - vy0)));
            for (int yy = 0; yy < vy1 - vy0; ++yy) {
                for (int xx = 0; xx < 320; ++xx) {
                    lo[static_cast<size_t>(yy * 320 + xx)] = kEga[under.pixels[static_cast<size_t>((yy + vy0) * 320 + xx)] & 15];
                }
            }
            SceneRaster::draw_rgb(low, 1, 320, vy1 - vy0, 0, static_cast<float>(vy0), lo);
            const int ox = col * (w + gap);
            for (int yy = 0; yy < h; ++yy) {
                for (int xx = 0; xx < w; ++xx) {
                    img[static_cast<size_t>(yy * W + ox + xx)] = hi[static_cast<size_t>(yy * w + xx)];
                    img[static_cast<size_t>((yy + h + gap) * W + ox + xx)] = lo[static_cast<size_t>((yy / scale) * 320 + xx / scale)];
                }
            }
        }
        char name[160];
        std::snprintf(name, sizeof name, "%s_sky_%s.png", label.c_str(), v.name.c_str());
        write_png(out_dir / name, W, H, img);
        int band = 0;
        for (size_t i = 0; i < hills.pixels.size(); ++i) band += hills.pixels[i] != tval.pre.pixels[i];
        std::printf("sky view %s: camera %d,%d,%d yaw %d pitch %d, big tile %d, panorama rows %d at %04X, %s (%d pixels "
                    "changed), %d cells, %d objects, %s\n",
                    v.name.c_str(), v.x, v.y, z, v.yaw, v.pitch, (v.x >> 15) * 5 + (v.y >> 15), hz.rows, hz.source,
                    applied ? "hills" : "not applied", band, scene.stats.cells, scene.stats.objects, name);
    }
    std::printf("sky views: %d of %d panoramas known to the retouching\n", backdrop.known(), en::kPanoramas);
    return 0;
}

// --horizon-dump: the panoramas as the game holds them, and their Hills versions.
int dump_horizons(Machine& machine, const std::filesystem::path& out_dir, const std::string& label) {
    std::vector<uint8_t> buffer(static_cast<size_t>(en::kPanoramas * en::kPanoramaRows * en::kPanoramaWidth));
    Ega::Frame f;
    const size_t bytes = buffer.size() / 8;
    for (size_t at = 0; at < bytes; at += 8000) {
        machine.ega().render_page(static_cast<uint16_t>(0x4000 + at), f);
        std::memcpy(buffer.data() + at * 8, f.pixels.data(), std::min<size_t>(8000, bytes - at) * 8);
    }
    const size_t n = static_cast<size_t>(en::kPanoramaRows * en::kPanoramaWidth);
    for (int p = 0; p < en::kPanoramas; ++p) {
        const uint8_t* in = buffer.data() + static_cast<size_t>(p) * n;
        std::vector<uint8_t> out(n);
        const bool known = en::landscape_panorama(in, out.data());
        for (const bool hills : {false, true}) {
            const uint8_t* px = hills ? out.data() : in;
            char name[96];
            std::snprintf(name, sizeof name, "%s_horizon%d_%s", label.c_str(), p, hills ? "hills" : "painted");
            std::ofstream(out_dir / (std::string(name) + ".bin"), std::ios::binary)
                .write(reinterpret_cast<const char*>(px), static_cast<std::streamsize>(n));
            std::vector<uint32_t> img(n);
            for (size_t i = 0; i < n; ++i) img[i] = kEga[px[i] & 15];
            write_png(out_dir / (std::string(name) + ".png"), en::kPanoramaWidth, en::kPanoramaRows, img);
        }
        std::printf("horizon %d: hash %016llX, %s\n", p, static_cast<unsigned long long>(en::panorama_hash(in)),
                    known ? "retouched" : "not known (left as it is)");
    }
    return 0;
}

int main(int argc, char* argv[]) {
    MachineConfig config;
    config.game_dir = "Game";
    config.start_time = vette::host::RealTime{1989, 10, 23, 12, 0, 0, 0};
    std::filesystem::path out_dir = "re/out/world";
    bool catalogue = false, map = false, manual_check = false, freeway_boxes = false, bridge_check = false;
    double val_from = -1, val_to = -1, seconds = -1;
    int png_every = 0, dump_calls = -1;
    double png_over = 2.0;
    int teleports = 0;
    int dump_routine = -1;
    bool from_world = false;
    double teleport_at = 40;
    uint32_t seed = 1989;
    bool recheck = false;
    std::string label = "drive";
    double scene_from = -1, scene_to = -1, bench_from = -1, bench_to = -1;
    std::vector<double> scene_shots;
    int scene_scale = 6;
    struct KeyEvent {
        uint64_t at_ms;
        uint8_t scancode;
    };
    std::vector<KeyEvent> keys;
    struct Poke {
        uint64_t at_ms;
        uint16_t offset, value;
        bool byte;
    };
    std::vector<Poke> pokes;
    std::vector<double> frame_shots;
    std::vector<uint16_t> watch_words;
    std::vector<int> bridge_view;  // --bridge-view x,y,z,yaw,pitch: only this view of the sweep
    bool sky_views = false, sky_sweep = false, horizon_dump = false, replicas = false;
    std::vector<SkyView> sky_list;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool v = i + 1 < argc;
        if (a == "--game" && v) {
            config.game_dir = argv[++i];
        } else if (a == "--out" && v) {
            out_dir = argv[++i];
        } else if (a == "--catalogue") {
            catalogue = true;
        } else if (a == "--map") {
            map = true;
        } else if (a == "--freeway-boxes") {
            freeway_boxes = true;
        } else if (a == "--bridge-check") {
            bridge_check = true;
        } else if (a == "--sky-views") {
            sky_views = true;
        } else if (a == "--sky-sweep") {
            sky_views = sky_sweep = true;
        } else if (a == "--horizon-dump") {
            horizon_dump = true;
        } else if (a == "--sky-view" && v) {
            sky_views = true;
            std::string r = argv[++i];
            std::vector<std::string> f;
            for (size_t p0 = 0; p0 <= r.size();) {
                const size_t p1 = std::min(r.find(',', p0), r.size());
                f.push_back(r.substr(p0, p1 - p0));
                p0 = p1 + 1;
            }
            if (f.size() != 6) return usage();
            sky_list.push_back({std::atoi(f[0].c_str()), std::atoi(f[1].c_str()), std::atoi(f[2].c_str()),
                                std::atoi(f[3].c_str()), std::atoi(f[4].c_str()), f[5]});
        } else if (a == "--bridge-view" && v) {
            bridge_check = true;
            std::string r = argv[++i];
            for (size_t p0 = 0; p0 <= r.size();) {
                const size_t p1 = std::min(r.find(',', p0), r.size());
                bridge_view.push_back(std::atoi(r.substr(p0, p1 - p0).c_str()));
                p0 = p1 + 1;
            }
        } else if (a == "--poke" && v) {
            const std::string s = argv[++i];
            const size_t c1 = s.find(':'), c2 = s.find(':', c1 + 1);
            if (c1 == std::string::npos || c2 == std::string::npos) return usage();
            pokes.push_back({static_cast<uint64_t>(std::atof(s.substr(0, c1).c_str()) * 1000),
                             static_cast<uint16_t>(std::strtoul(s.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr, 16)),
                             static_cast<uint16_t>(std::strtoul(s.substr(c2 + 1).c_str(), nullptr, 16)),
                             s.size() - c2 - 1 <= 2});
        } else if (a == "--shot" && v) {
            frame_shots.push_back(std::atof(argv[++i]));
        } else if (a == "--watch" && v) {
            std::string r = argv[++i];
            for (size_t p0 = 0; p0 <= r.size();) {
                const size_t p1 = std::min(r.find(',', p0), r.size());
                const std::string item = r.substr(p0, p1 - p0);  // OFF or OFF*COUNT (words)
                const size_t star = item.find('*');
                const auto off = static_cast<uint16_t>(std::strtoul(item.substr(0, star).c_str(), nullptr, 16));
                const int count = star == std::string::npos ? 1 : std::atoi(item.substr(star + 1).c_str());
                for (int k = 0; k < count; ++k) watch_words.push_back(static_cast<uint16_t>(off + 2 * k));
                p0 = p1 + 1;
            }
        } else if (a == "--manual-check") {
            manual_check = true;
        } else if (a == "--validate" && v) {
            const std::string s = argv[++i];
            const size_t c = s.find(':');
            if (c == std::string::npos) return usage();
            val_from = std::atof(s.substr(0, c).c_str());
            val_to = std::atof(s.substr(c + 1).c_str());
        } else if (a == "--png-every" && v) {
            png_every = std::atoi(argv[++i]);
        } else if (a == "--png-over" && v) {
            png_over = std::atof(argv[++i]);
        } else if ((a == "--scene-compare" || a == "--scene-bench") && v) {
            const std::string r = argv[++i];
            const size_t c = r.find(':');
            if (c == std::string::npos) return usage();
            (a == "--scene-compare" ? scene_from : bench_from) = std::atof(r.substr(0, c).c_str());
            (a == "--scene-compare" ? scene_to : bench_to) = std::atof(r.substr(c + 1).c_str());
        } else if (a == "--scene-shots" && v) {
            std::string r = argv[++i];
            for (size_t p0 = 0; p0 <= r.size();) {
                const size_t p1 = std::min(r.find(',', p0), r.size());
                scene_shots.push_back(std::atof(r.substr(p0, p1 - p0).c_str()));
                p0 = p1 + 1;
            }
            std::sort(scene_shots.begin(), scene_shots.end());
        } else if (a == "--scene-scale" && v) {
            scene_scale = std::max(1, std::atoi(argv[++i]));
        } else if (a == "--recheck") {
            recheck = true;
        } else if (a == "--replicas") {
            replicas = true;
        } else if (a == "--float") {
            from_world = true;
        } else if (a == "--dump-routine" && v) {
            dump_routine = static_cast<int>(std::strtoul(argv[++i], nullptr, 16));
        } else if (a == "--teleport" && v) {
            teleports = std::atoi(argv[++i]);
        } else if (a == "--teleport-at" && v) {
            teleport_at = std::atof(argv[++i]);
        } else if (a == "--seed" && v) {
            seed = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--dump-calls" && v) {
            dump_calls = std::atoi(argv[++i]);
        } else if (a == "--label" && v) {
            label = argv[++i];
        } else if (a == "--seconds" && v) {
            seconds = std::atof(argv[++i]);
        } else if (a == "--key" && v) {
            const std::string s = argv[++i];
            const size_t c = s.find(':');
            if (c == std::string::npos) return usage();
            const auto at = static_cast<uint64_t>(std::atof(s.substr(0, c).c_str()) * 1000);
            const auto sc = static_cast<uint8_t>(std::strtoul(s.substr(c + 1).c_str(), nullptr, 16));
            keys.push_back({at, sc});
            keys.push_back({at + 100, static_cast<uint8_t>(sc | 0x80)});
        } else if (a == "--hold" && v) {
            const std::string s = argv[++i];
            const size_t c1 = s.find(':'), c2 = s.find(':', c1 + 1);
            if (c1 == std::string::npos || c2 == std::string::npos) return usage();
            const auto f = static_cast<uint64_t>(std::atof(s.substr(0, c1).c_str()) * 1000);
            const auto t = static_cast<uint64_t>(std::atof(s.substr(c1 + 1, c2 - c1 - 1).c_str()) * 1000);
            const auto sc = static_cast<uint8_t>(std::strtoul(s.substr(c2 + 1).c_str(), nullptr, 16));
            keys.push_back({f, sc});
            keys.push_back({t, static_cast<uint8_t>(sc | 0x80)});
        } else {
            return usage();
        }
    }
    std::sort(keys.begin(), keys.end(), [](const KeyEvent& a, const KeyEvent& b) { return a.at_ms < b.at_ms; });
    config.save_dir = out_dir / "save";
    std::filesystem::create_directories(out_dir);

    Machine machine(config);
    std::string error;
    if (!machine.boot(error)) {
        std::fprintf(stderr, "boot failed: %s\n", error.c_str());
        return 1;
    }
    if (!manual_check) {
        vette::game::install_skip_manual_check(machine.cpu());
    }

    // Extract as soon as the EXEPACK stub has unpacked the image.
    World world;
    bool ok = false;
    uint64_t ms = 0;
    for (; ms < 5000 && !ok; ms += 50) {
        machine.run_for(50 * kNsPerMs);
        ok = en::extract_world(machine, world, error);
    }
    if (!ok) {
        std::fprintf(stderr, "extraction failed: %s\n", error.c_str());
        return 1;
    }
    const auto& s = world.stats;
    std::printf("extracted at emulated %.2f s: map %d x %d big tiles (%d x %d cells), %d cell types used\n",
                static_cast<double>(ms) / 1000, world.big_rows, world.big_cols, world.cells_x(), world.cells_y(),
                s.types_used);
    std::printf("  %d routines (%d failed, %d compound, %d camera-dependent), %d variants; %d models; %zu compound "
                "instances\n",
                s.routines, s.routines_failed, s.compound_routines, s.camera_dependent, s.variants, s.models,
                world.compounds.size());
    std::printf("  extraction %.1f ms (%zu probe runs)\n", s.milliseconds, s.probe_runs);
    for (const auto& r : world.routines) {
        if (!r.error.empty()) std::printf("  routine %04X: %s\n", r.address, r.error.c_str());
    }
    for (const auto& w : world.warnings) std::printf("  note: %s\n", w.c_str());
    for (const auto& c : world.compounds) {
        std::printf("  compound %04X at %d,%d,%d from %zu cells\n", c.routine, c.position.x, c.position.y, c.position.z,
                    c.cells.size());
    }
    {
        // What the Maximum draw distance draws: every cell's most detailed variants, compounds once.
        size_t polys = 0, lines = 0, models = 0, objects = 0, verts = 0;
        const auto add = [&](const en::Variant& v) {
            ++objects;
            for (const auto& p : v.parts) {
                if (p.source == en::Part::Source::Model) {
                    ++models;
                    const auto& m = world.models[p.model].near_mesh;
                    verts += m.verts.size();
                    for (const auto& f : m.faces) {
                        for (const auto& pr : f.prims) (f.lines() ? lines : polys) += f.lines() ? pr.size() - 1 : 1;
                    }
                    continue;
                }
                verts += p.verts.size();
                for (const auto& pr : p.prims) (pr.kind == en::Prim::Kind::Line ? lines : polys) += 1;
            }
        };
        for (int cx = 0; cx < world.cells_x(); ++cx) {
            for (int cy = 0; cy < world.cells_y(); ++cy) {
                const auto& ct = world.types[world.cell(cx, cy).type];
                en::DrawState st;
                for (const auto* list : {&ct.list1, &ct.list2}) {
                    for (const auto& e : *list) {
                        const en::Routine* r = world.routine(e.routine);
                        if (!r || r->compound) continue;
                        if (const en::Variant* v = r->select(st)) {
                            if (v->sets_finish_flag >= 0) st.finish_flag = v->sets_finish_flag != 0;
                            add(*v);
                        }
                    }
                }
            }
        }
        for (const auto& c : world.compounds) {
            if (const en::Variant* v = world.routine(c.routine)->select(en::DrawState{})) {
                for (const auto& sc : v->calls) {
                    if (const en::Variant* sv = world.routine(sc.routine)->select(en::DrawState{})) add(*sv);
                }
            }
        }
        std::printf("  whole map, most detailed (course 1, windows on): %zu objects, %zu polygons, %zu line segments, "
                    "%zu placed models, %zu vertices\n",
                    objects, polys, lines, models, verts);
    }
    if (freeway_boxes) {
        // The on-ramp boxes (3009:1A12-1AD8): collision class DS:C0A6[class] -> {xmin, ymin, xmax, ymax}
        // boxes; the handler compares the box's address. Route DS:8156 and DS:2AD4 value per box.
        struct Ramp {
            uint16_t box;
            int route, mode;
            const char* note;
        };
        static constexpr Ramp kRamps[] = {{0xC294, 0, 1, "not course 1"}, {0xC278, 1, 1, ""},
                                          {0xC2BE, 2, 2, "not course 2"}, {0xC554, 3, 3, ""},
                                          {0xC5F4, 4, 1, ""}, {0xC5E2, 5, 1, ""}, {0xC610, 6, 1, ""},
                                          {0xC62C, 7, 1, "not course 1"}, {0xC64E, 8, 1, ""}};
        auto& mem = machine.memory();
        const auto d16 = [&](uint16_t off) { return mem.read16(Cpu::linear(kData, off)); };
        for (int cx = 0; cx < world.cells_x(); ++cx) {
            for (int cy = 0; cy < world.cells_y(); ++cy) {
                const auto& ct = world.types[world.cell(cx, cy).type];
                uint16_t box = d16(static_cast<uint16_t>(0xC0A6 + 2 * ct.collision_class));
                for (int n = 0; n < 32 && d16(box) != 0xFFFF; ++n, box = static_cast<uint16_t>(box + 8)) {
                    for (const Ramp& r : kRamps) {
                        if (r.box != box) continue;
                        std::printf("freeway box %04X route %d (2AD4=%d%s%s) cell %d,%d type %d: x %d..%d y %d..%d "
                                    "(absolute %d..%d, %d..%d)\n",
                                    box, r.route, r.mode, *r.note ? ", " : "", r.note, cx, cy,
                                    world.cell(cx, cy).type, static_cast<int16_t>(d16(box)),
                                    static_cast<int16_t>(d16(static_cast<uint16_t>(box + 4))),
                                    static_cast<int16_t>(d16(static_cast<uint16_t>(box + 2))),
                                    static_cast<int16_t>(d16(static_cast<uint16_t>(box + 6))),
                                    cx * en::kCellSize + static_cast<int16_t>(d16(box)),
                                    cx * en::kCellSize + static_cast<int16_t>(d16(static_cast<uint16_t>(box + 4))),
                                    cy * en::kCellSize + static_cast<int16_t>(d16(static_cast<uint16_t>(box + 2))),
                                    cy * en::kCellSize + static_cast<int16_t>(d16(static_cast<uint16_t>(box + 6))));
                    }
                }
            }
        }
    }
    if (catalogue) {
        for (const auto& r : world.routines) {
            std::printf("%04X%s%s %zu variant(s)\n", r.address, r.compound ? " compound" : "",
                        r.camera_dependent ? " camera" : "", r.variants.size());
            for (const auto& v : r.variants) {
                size_t verts = 0;
                for (const auto& p : v.parts) verts += p.verts.size();
                std::printf("   d%u prims %u parts %zu verts %zu calls %zu | facing %X key %X win %X course %02X fin %X"
                            " dist %d..%d%s finish->%d\n",
                            v.detail, v.primitives, v.parts.size(), verts, v.calls.size(), v.when.facing,
                            v.when.lod_key, v.when.windows, v.when.course, v.when.finish_flag, v.when.min_distance,
                            v.when.max_distance, v.when.positional ? " positional" : "", v.sets_finish_flag);
            }
        }
    }
    if (dump_routine >= 0) {
        if (const en::Routine* r = world.routine(static_cast<uint16_t>(dump_routine))) {
            for (const auto& v : r->variants) {
                std::printf("variant d%u signature %016llX, %zu reorders\n", v.detail,
                            static_cast<unsigned long long>(v.signature), v.reorders.size());
                for (size_t pi = 0; pi < v.parts.size(); ++pi) {
                    const auto& p = v.parts[pi];
                    std::printf("  part %zu source %d data %04X origin %d,%d,%d rotation %d yaw %d verts:", pi,
                                static_cast<int>(p.source), p.data_addr, p.origin.x, p.origin.y, p.origin.z,
                                static_cast<int>(p.rotation), p.yaw);
                    for (const auto& q : p.verts) std::printf(" (%d,%d,%d)", q.x, q.y, q.z);
                    std::printf("\n");
                    for (const auto& pr : p.prims) {
                        std::printf("    prim kind %d colour %02X verts", static_cast<int>(pr.kind), pr.colour.raw);
                        for (uint32_t k = 0; k < pr.count; ++k) std::printf(" %u", p.indices[pr.first + k]);
                        std::printf("  cull %d part %u v %u %u %u\n", static_cast<int>(pr.cull.kind), pr.cull.part,
                                    pr.cull.v[0], pr.cull.v[1], pr.cull.v[2]);
                    }
                }
            }
        }
    }
    if (map) {
        const auto t0 = std::chrono::steady_clock::now();
        TopDown td(world, 16);
        td.render();
        td.save(out_dir / "map_topdown.png");
        std::printf("map_topdown.png written (%.0f ms)\n",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    const bool scene_mode = scene_from >= 0 || bench_from >= 0 || !scene_shots.empty();
    const bool at_race_frame = bridge_check || sky_views || horizon_dump;
    if (val_from < 0 && teleports == 0 && !scene_mode && frame_shots.empty() && !at_race_frame) {
        return 0;
    }

    for (const auto& e : std::filesystem::directory_iterator(out_dir)) {
        const std::string n = e.path().filename().string();
        if (n.rfind(label + "_", 0) == 0 && e.path().extension() == ".png") std::filesystem::remove(e.path());
    }
    const auto setup = [&](Validator& v, const std::string& name) {
        v.out_dir = out_dir;
        v.label = name;
        v.png_every = png_every;
        v.dump_calls = dump_calls;
        v.png_over = png_over;
        v.from_world = from_world;
        v.log.open(out_dir / (name + "_frames.txt"));
    };
    const auto summary = [](const Validator& v, const char* what, const char* unit) {
        const auto& t = v.totals;
        std::printf("%s %s: %d %s, %d identical, %.4f%% of view pixels differ (worst %d: %.3f%%), "
                    "%d with a different draw sequence, %d unmatched objects, %.1f ms per reference frame\n",
                    what, v.label.c_str(), t.frames, unit, t.identical,
                    t.pixels ? 100.0 * static_cast<double>(t.differing) / static_cast<double>(t.pixels) : 0,
                    t.worst_frame, t.worst, t.sequence_mismatch, t.unmatched, t.frames ? t.ms / t.frames : 0);
        static const char* kKinds[3] = {"packed", "plain", "model"};
        for (size_t k = 0; k < 3; ++k) {
            const auto& d = t.deviation[k];
            if (d.vertices) {
                std::printf("  float geometry, %s parts: %zu vertices, %zu off by > 4 units, %zu by > 16, max %d (%04X)\n",
                            kKinds[k], d.vertices, d.over4, d.over16, d.max, d.worst_routine);
            }
        }
        for (const auto& n : t.notes) std::printf("  note: %s\n", n.c_str());
    };

    SceneCheck sc(machine, world);
    sc.out_dir = out_dir;
    sc.label = label;
    sc.from = scene_from;
    sc.to = scene_to;
    sc.bench_from = bench_from;
    sc.bench_to = bench_to;
    sc.shots = scene_shots;
    sc.scale = scene_scale;
    if (scene_mode) {
        sc.log.open(out_dir / (label + "_scene.txt"));
        sc.install();
    }

    Validator val(machine, world);
    setup(val, label);
    val.from = val_from;
    val.to = val_to;
    if (val_from >= 0) val.install();

    // Teleport views: snapshot a race frame at 3009:02DA once the time has come.
    Validator tval(machine, world);
    setup(tval, label + "_teleport");
    Teleporter teleporter(machine, tval);
    if (teleports > 0 || at_race_frame) {
        machine.cpu().add_watch(Cpu::linear(kCode, 0x02DA), [&](Cpu&) {
            if (!teleporter.ready() && static_cast<double>(machine.emulated_ns()) / 1e9 >= teleport_at &&
                machine.memory().read8(Cpu::linear(kData, 0x2AD4)) == 0) {
                teleporter.snapshot();
            }
        });
    }

    double end_s = std::max(val_to, teleports > 0 || at_race_frame ? teleport_at : 0.0) + 0.5;
    end_s = std::max({end_s, scene_to + 0.5, bench_to + 0.5, scene_shots.empty() ? 0.0 : scene_shots.back() + 1.0,
                      frame_shots.empty() ? 0.0 : *std::max_element(frame_shots.begin(), frame_shots.end()) + 0.1});
    const uint64_t end_ms = static_cast<uint64_t>((seconds > 0 ? seconds : end_s) * 1000);
    size_t next = 0, next_poke = 0, next_frame_shot = 0;
    std::sort(pokes.begin(), pokes.end(), [](const Poke& a, const Poke& b) { return a.at_ms < b.at_ms; });
    std::sort(frame_shots.begin(), frame_shots.end());
    for (; ms < end_ms && !machine.stopped() && !((teleports > 0 || at_race_frame) && val_from < 0 && teleporter.ready()); ++ms) {
        while (next < keys.size() && keys[next].at_ms <= ms) machine.key(keys[next++].scancode);
        while (next_poke < pokes.size() && pokes[next_poke].at_ms <= ms) {
            const Poke& pk = pokes[next_poke];
            if (pk.byte) {
                machine.memory().write8(Cpu::linear(kData, pk.offset), static_cast<uint8_t>(pk.value));
            } else {
                machine.memory().write16(Cpu::linear(kData, pk.offset), pk.value);
            }
            ++next_poke;
        }
        if (next_frame_shot < frame_shots.size() && static_cast<double>(ms) >= frame_shots[next_frame_shot] * 1000) {
            // The displayed frame as a PNG, and the watched DS words.
            Ega::Frame f;
            machine.render(f);
            std::vector<uint32_t> img(f.pixels.size());
            for (size_t k = 0; k < img.size(); ++k) img[k] = kEga[f.pixels[k] & 15];
            char name[96];
            std::snprintf(name, sizeof name, "%s_frame_%.2f.png", label.c_str(), frame_shots[next_frame_shot]);
            write_png(out_dir / name, f.width, f.height, img);
            std::printf("frame %s", name);
            for (const uint16_t w : watch_words) {
                std::printf("  DS:%04X=%04X", w, machine.memory().read16(Cpu::linear(kData, w)));
            }
            std::printf("\n");
            ++next_frame_shot;
        }
        machine.run_for(kNsPerMs);
    }
    val.log.close();
    if (val_from >= 0) summary(val, "validation", "frames");
    if (scene_mode) sc.report();
    if (recheck) {
        World again;
        if (!en::extract_world(machine, again, error)) {
            std::printf("recheck: extraction failed: %s\n", error.c_str());
            return 1;
        }
        const std::string d = compare_worlds(world, again);
        std::printf("recheck at emulated %.1f s: %s (%.1f ms)\n", static_cast<double>(machine.emulated_ns()) / 1e9,
                    d.empty() ? "same world as at startup" : ("differs: " + d).c_str(), again.stats.milliseconds);
    }

    if (teleports > 0) {
        if (!teleporter.ready()) {
            std::printf("teleport: no race frame reached by %.1f s\n", teleport_at);
            return 1;
        }
        uint32_t rng = seed;
        const auto rand = [&rng](uint32_t n) {
            rng = rng * 1664525u + 1013904223u;
            return (rng >> 8) % n;
        };
        int failed = 0;
        for (int i = 0; i < teleports; ++i) {
            const int32_t x = static_cast<int32_t>(rand(static_cast<uint32_t>(world.cells_x() * en::kCellSize)));
            const int32_t y = static_cast<int32_t>(rand(static_cast<uint32_t>(world.cells_y() * en::kCellSize)));
            const int32_t lift = rand(10) < 6 ? 0 : static_cast<int32_t>(rand(600));
            const int32_t z = world.ground_z(x, y) + 10 + lift;
            const int yaw = static_cast<int>(rand(360));
            const int pitch = rand(10) < 7 ? 0 : static_cast<int>(rand(31)) - 15;
            char where[32];
            std::snprintf(where, sizeof where, "view %4d", i);
            tval.where = where;
            if (!teleporter.view(x, y, z, yaw, pitch)) {
                ++failed;
                continue;
            }
            tval.compare();
        }
        tval.log.close();
        summary(tval, "teleport", "views");
        if (failed) std::printf("  %d views did not finish in the original\n", failed);
    }
    if (horizon_dump) {
        if (!teleporter.ready()) {
            std::printf("horizon dump: no race frame reached by %.1f s\n", teleport_at);
            return 1;
        }
        dump_horizons(machine, out_dir, label);
    }
    if (sky_views) {
        if (!teleporter.ready()) {
            std::printf("sky views: no race frame reached by %.1f s\n", teleport_at);
            return 1;
        }
        run_sky_views(world, machine, teleporter, tval, out_dir, label, sky_list.empty() ? default_sky_views(sky_sweep) : sky_list,
                      scene_scale, replicas);
    }
    if (bridge_check) {
        if (!teleporter.ready()) {
            std::printf("bridge check: no race frame reached by %.1f s\n", teleport_at);
            return 1;
        }
        return run_bridge_check(world, teleporter, tval, out_dir, label, bridge_view);
    }
    if (!machine.fault().empty()) std::printf("machine fault: %s\n", machine.fault().c_str());
    return 0;
}
