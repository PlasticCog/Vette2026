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
//
// The README's race script reaches the race at ~37 s and drives north on the Great Highway:
//   vette_world --map --validate 37:50 --key 13:39 --key 17:1C --key 21:1C --key 25:1C --key 30:1C
//               --key 37:1E --key 37.3:02 --hold 37.5:49:48 --hold 38.5:39.3:4D
// Right arrow at the course menu (--key 29:4D, before the --key 30:1C) picks course 2 (Vista Point,
// the Golden Gate), twice (29:4D, 29.4:4D) course 3 (the Bay Bridge). F2/F3 (3C/3D) switch to the chase
// and helicopter views, W (11) toggles window detail, B (30) buildings, F6 (40) the mirror.
// Whole-map check: --teleport 10000 with any of these scripts (all views must be identical).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "enhanced/world.h"
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

// --- PNG (stored deflate) ------------------------------------------------------------------------------
uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
        init = true;
    }
    c = ~c;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

bool write_png(const std::filesystem::path& path, int w, int h, const std::vector<uint32_t>& rgb) {
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (static_cast<size_t>(w) * 3 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < w; ++x) {
            const uint32_t c = rgb[static_cast<size_t>(y * w + x)];
            raw.push_back(static_cast<uint8_t>(c >> 16));
            raw.push_back(static_cast<uint8_t>(c >> 8));
            raw.push_back(static_cast<uint8_t>(c));
        }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (const uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t off = 0; off < raw.size() || off == 0;) {
        const size_t n = std::min<size_t>(65535, raw.size() - off);
        const bool last = off + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n));
        z.push_back(static_cast<uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(off), raw.begin() + static_cast<std::ptrdiff_t>(off + n));
        off += n;
        if (last) break;
    }
    const uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back(static_cast<uint8_t>(adler >> s));

    std::ofstream f(path, std::ios::binary);
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    f.write(reinterpret_cast<const char*>(sig), 8);
    const auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        std::vector<uint8_t> buf(type, type + 4);
        buf.insert(buf.end(), data.begin(), data.end());
        const uint32_t len = static_cast<uint32_t>(data.size());
        const uint8_t l[4] = {static_cast<uint8_t>(len >> 24), static_cast<uint8_t>(len >> 16),
                              static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len)};
        f.write(reinterpret_cast<const char*>(l), 4);
        f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const uint32_t c = crc32(buf.data(), buf.size());
        const uint8_t cb[4] = {static_cast<uint8_t>(c >> 24), static_cast<uint8_t>(c >> 16),
                               static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c)};
        f.write(reinterpret_cast<const char*>(cb), 4);
    };
    std::vector<uint8_t> ihdr;
    for (const uint32_t v : {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}) {
        for (int s = 24; s >= 0; s -= 8) ihdr.push_back(static_cast<uint8_t>(v >> s));
    }
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return static_cast<bool>(f);
}

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
        for (int i = 0; i < 400 && !done_; ++i) cpu_->run(50000);
        return done_;
    }

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

int main(int argc, char* argv[]) {
    MachineConfig config;
    config.game_dir = "Game";
    config.start_time = vette::host::RealTime{1989, 10, 23, 12, 0, 0, 0};
    std::filesystem::path out_dir = "re/out/world";
    bool catalogue = false, map = false, manual_check = false;
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
    struct KeyEvent {
        uint64_t at_ms;
        uint8_t scancode;
    };
    std::vector<KeyEvent> keys;
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
        } else if (a == "--recheck") {
            recheck = true;
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
    if (val_from < 0 && teleports == 0) {
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

    Validator val(machine, world);
    setup(val, label);
    val.from = val_from;
    val.to = val_to;
    if (val_from >= 0) val.install();

    // Teleport views: snapshot a race frame at 3009:02DA once the time has come.
    Validator tval(machine, world);
    setup(tval, label + "_teleport");
    Teleporter teleporter(machine, tval);
    if (teleports > 0) {
        machine.cpu().add_watch(Cpu::linear(kCode, 0x02DA), [&](Cpu&) {
            if (!teleporter.ready() && static_cast<double>(machine.emulated_ns()) / 1e9 >= teleport_at &&
                machine.memory().read8(Cpu::linear(kData, 0x2AD4)) == 0) {
                teleporter.snapshot();
            }
        });
    }

    const double end_s = std::max(val_to, teleports > 0 ? teleport_at : 0.0) + 0.5;
    const uint64_t end_ms = static_cast<uint64_t>((seconds > 0 ? seconds : end_s) * 1000);
    size_t next = 0;
    for (; ms < end_ms && !machine.stopped() && !(teleports > 0 && val_from < 0 && teleporter.ready()); ++ms) {
        while (next < keys.size() && keys[next].at_ms <= ms) machine.key(keys[next++].scancode);
        machine.run_for(kNsPerMs);
    }
    val.log.close();
    if (val_from >= 0) summary(val, "validation", "frames");
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
    if (!machine.fault().empty()) std::printf("machine fault: %s\n", machine.fault().c_str());
    return 0;
}
