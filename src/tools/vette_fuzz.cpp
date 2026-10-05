// Differential fuzzer for the native ports (docs/PORTING.md). It boots the game into a race, then
// calls each ported routine directly with random register and memory inputs under NativeRunner
// Verify mode. That reaches cases normal play never does: DF=1, ES != DS, overlapping buffers,
// saturation, out-of-range angles, divide overflows.
//
//   vette_fuzz [--game <dir>] [--trials N]     exit code 4 on any mismatch, 5 if a routine didn't return
//
// It uses a fixed seed, so a failure reproduces exactly.
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "game/natives.h"
#include "host/machine.h"
#include "host/native.h"

using namespace vette::host;

static constexpr uint16_t kCode = 0x4009, kData = 0x224A;
static constexpr uint16_t kStubIp = 0x0025;  // start: never executed again after boot

std::mt19937 rng(12345);
uint32_t rnd(uint32_t n) { return std::uniform_int_distribution<uint32_t>(0, n - 1)(rng); }
uint16_t rword() {
    switch (rnd(8)) {
    case 0: { static const uint16_t sp[] = {0, 1, 2, 0x7FFF, 0x8000, 0x8001, 0xFFFF, 0xFFFE, 0x4000, 0xC000, 0x00FF, 0x0100, 0x0080, 0xFF80};
              return sp[rnd(sizeof sp / 2)]; }
    case 1: case 2: return static_cast<uint16_t>(static_cast<int16_t>(rnd(1001)) - 500);
    case 3: return static_cast<uint16_t>(static_cast<int16_t>(rnd(65)) - 32);
    default: return static_cast<uint16_t>(rnd(65536));
    }
}
int16_t q15() {  // plausible sin/cos-like values with extremes
    switch (rnd(4)) {
    case 0: { static const int16_t sp[] = {0, 0x7FFF, -0x7FFF, -0x8000, 0x4000, -0x4000, 1, -1}; return sp[rnd(8)]; }
    default: return static_cast<int16_t>(rnd(65536));
    }
}

struct Ctx {
    Machine& m;
    NativeRunner& runner;
    Registers base;
    bool done = false;
    Memory& mem() { return m.memory(); }
    void w16(uint16_t seg, uint16_t off, uint16_t v) {
        mem().write8(Cpu::linear(seg, off), static_cast<uint8_t>(v));
        mem().write8(Cpu::linear(seg, static_cast<uint16_t>(off + 1)), static_cast<uint8_t>(v >> 8));
    }
    uint16_t r16(uint16_t seg, uint16_t off) {
        return static_cast<uint16_t>(mem().read8(Cpu::linear(seg, off)) | mem().read8(Cpu::linear(seg, static_cast<uint16_t>(off + 1))) << 8);
    }
    // Sets up a near call to `entry` with random registers; returns the register file to tweak.
    Registers& prepare(uint16_t entry) {
        Registers& r = m.cpu().regs;
        r = base;
        for (int i = 0; i < 8; ++i) if (i != SP) r.r[i] = static_cast<uint16_t>(rnd(65536));
        r.s[DS] = kData;
        r.s[ES] = kData;
        r.s[CS] = kCode;
        r.ip = entry;
        uint16_t f = static_cast<uint16_t>(rnd(65536)) & (0x0001 | 0x0004 | 0x0010 | 0x0040 | 0x0080 | 0x0800);
        if (rnd(8) == 0) f |= 0x0400;  // DF
        r.flags = static_cast<uint16_t>(0x0002 | f);  // IF = 0, TF = 0
        r.r[SP] = static_cast<uint16_t>(base.r[SP] - 0x300);
        r.r[SP] = static_cast<uint16_t>(r.r[SP] - 2);
        w16(r.s[SS], r.r[SP], kStubIp);
        return r;
    }
    bool run() {
        done = false;
        for (int i = 0; i < 2000 && !done; ++i) {
            m.cpu().run(10000);
            runner.poll();
        }
        return done;
    }
};

int main(int argc, char** argv) {
    int trials = 5000;
    MachineConfig config;
    config.game_dir = "Game";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--game" && i + 1 < argc) {
            config.game_dir = argv[++i];
        } else if (a == "--trials" && i + 1 < argc) {
            trials = std::atoi(argv[++i]);
        } else {
            std::fprintf(stderr, "usage: vette_fuzz [--game <dir>] [--trials N]\n");
            return 2;
        }
    }
    config.save_dir = std::filesystem::temp_directory_path() / "vette_fuzz_save";  // never the game folder
    config.start_time = RealTime{1989, 10, 23, 12, 0, 0, 0};
    Machine machine(config);
    std::string error;
    if (!machine.boot(error)) { std::fprintf(stderr, "boot: %s\n", error.c_str()); return 1; }
    struct K { double t; uint8_t sc; };
    std::vector<K> keys = {{13, 0x39}, {17, 0x1C}, {21, 0x1C}, {25, 0x1C}, {30, 0x1C}, {31, 0x0A}, {31.3, 0x07},
                           {31.6, 0x34}, {31.9, 0x03}, {32.2, 0x1C}, {35.5, 0x02}, {37, 0x1E}, {37.5, 0x02}};
    std::vector<std::pair<uint64_t, uint8_t>> ev;
    for (auto k : keys) { ev.push_back({uint64_t(k.t * 1000), k.sc}); ev.push_back({uint64_t(k.t * 1000) + 100, uint8_t(k.sc | 0x80)}); }
    ev.push_back({38000, uint8_t{0x48}});
    std::sort(ev.begin(), ev.end());
    size_t next = 0;
    for (uint64_t ms = 0; ms < 45000; ++ms) {
        while (next < ev.size() && ev[next].first <= ms) machine.key(ev[next++].second);
        machine.run_for(1'000'000);
    }
    Memory& mem = machine.memory();
    std::printf("IVT[0] = %04X:%04X\n", mem.read16(2), mem.read16(0));

    NativeRunner runner(machine);
    int reports = 0;
    runner.set_report([&](const std::string& msg) { if (reports++ < 12) std::printf("%s\n", msg.c_str()); });
    for (const auto& fn : vette::game::native_functions()) runner.install(fn, NativeRunner::Mode::Verify);

    Ctx c{machine, runner, machine.cpu().regs};
    machine.cpu().set_code_hook(Cpu::linear(kCode, kStubIp), [&](Cpu& cpu) { c.done = true; cpu.request_stop(); });

    auto seg_es = [&]() -> uint16_t { return rnd(10) == 0 ? static_cast<uint16_t>(0x5100 + rnd(0x3000)) : kData; };
    auto window = [&](uint16_t lo, uint16_t span) { return static_cast<uint16_t>(lo + rnd(span)); };
    int fails = 0;

    for (int t = 0; t < trials; ++t) {
        // sincos_deg, wild angles and outputs near the table / its own reads
        {
            Registers& r = c.prepare(0x4E3C);
            r.r[AX] = rnd(2) ? static_cast<uint16_t>(rnd(65536)) : static_cast<uint16_t>(static_cast<int16_t>(rnd(1200)) - 600);
            r.r[BX] = rnd(2) ? static_cast<uint16_t>(0x3A4A + 4 * rnd(3)) : window(0x3800, 0x300);
            if (!c.run()) { std::printf("sincos: no return\n"); ++fails; }
        }
        // camera_matrix_from_angles
        {
            Registers& r = c.prepare(0x3F2D);
            r.s[ES] = seg_es();
            r.r[SI] = rnd(2) ? 0x2C77 : window(0x9000, 0x40);
            for (int i = 0; i < 3; ++i) {
                uint16_t a = rnd(4) == 0 ? static_cast<uint16_t>(rnd(65536)) : static_cast<uint16_t>(static_cast<int16_t>(rnd(1440)) - 720);
                if (rnd(3) == 0) a = static_cast<uint16_t>(static_cast<int16_t>(rnd(720)) - 360);
                c.w16(kData, static_cast<uint16_t>(r.r[SI] + ((r.flags & 0x400) ? -2 * i : 2 * i)), a);
            }
            if (!c.run()) { std::printf("camera_matrix: no return\n"); ++fails; }
        }
        // vec_mul_mat3 (aliasing windows)
        {
            Registers& r = c.prepare(0x3D51);
            r.s[ES] = rnd(4) ? kData : seg_es();
            r.r[SI] = window(0x9000, 0x30);
            r.r[BX] = window(0x9000, 0x30);
            r.r[DI] = window(0x9000, 0x30);
            for (uint16_t o = 0x8FC0; o < 0x9080; o += 2) c.w16(kData, o, rnd(2) ? static_cast<uint16_t>(q15()) : rword());
            if (!c.run()) { std::printf("vec_mul_mat3: no return\n"); ++fails; }
        }
        // points_rel_camera
        {
            Registers& r = c.prepare(0x3D8C);
            r.r[CX] = static_cast<uint16_t>(1 + rnd(8));
            r.r[SI] = window(0x9000, 0x30);
            r.r[DI] = rnd(4) == 0 ? static_cast<uint16_t>(0x2C71 - 2 * rnd(10)) : window(0x9000, 0x30);
            for (uint16_t o = 0x9000; o < 0x9080; o += 2) c.w16(kData, o, rword());
            for (uint16_t o = 0x2C71; o < 0x2C77; o += 2) c.w16(kData, o, rword());
            if (!c.run()) { std::printf("points_rel_camera: no return\n"); ++fails; }
        }
        // xform_points_to_camera
        {
            Registers& r = c.prepare(0x3D2F);
            r.r[AX] = static_cast<uint16_t>(1 + rnd(8));
            r.r[BX] = rnd(4) == 0 ? window(0x2660, 0x40) : window(0x9000, 0x30);
            for (uint16_t o = 0x9000; o < 0x9080; o += 2) c.w16(kData, o, rword());
            for (uint16_t o = 0x2C71; o < 0x2C77; o += 2) c.w16(kData, o, rword());
            for (uint16_t o = 0x32B1; o < 0x32C3; o += 2) c.w16(kData, o, static_cast<uint16_t>(q15()));
            if (!c.run()) { std::printf("xform_points_to_camera: no return\n"); ++fails; }
        }
        // world_to_camera_point
        {
            Registers& r = c.prepare(0x3917);
            r.s[ES] = rnd(4) ? kData : seg_es();
            r.r[SI] = rnd(3) == 0 ? window(0x2280, 0x10) : window(0x9000, 0x30);
            for (uint16_t o = 0x9000; o < 0x9080; o += 2) c.w16(kData, o, rword());
            for (uint16_t o = 0x2C71; o < 0x2C77; o += 2) c.w16(kData, o, rword());
            for (uint16_t o = 0x32B1; o < 0x32C3; o += 2) c.w16(kData, o, static_cast<uint16_t>(q15()));
            if (!c.run()) { std::printf("world_to_camera_point: no return\n"); ++fails; }
        }
        // build_axis_table
        {
            Registers& r = c.prepare(0x39B9);
            r.s[ES] = rnd(3) == 0 ? seg_es() : kData;
            r.r[SI] = window(0x9000, 0x40);
            r.r[DI] = rnd(3) == 0 ? window(0x9000, 0x40) : window(0x9100, 0x40);
            for (uint16_t o = 0x8F80; o < 0x9100; o += 2) c.w16(kData, o, rword());
            for (uint16_t o = 0x8F80; o < 0x9100; o += 2) c.w16(r.s[ES], o, rword());
            if (!c.run()) { std::printf("build_axis_table: no return\n"); ++fails; }
        }
        // project_vertices (several per trial)
        for (int k = 0; k < 4; ++k) {
            Registers& r = c.prepare(0xA685);
            r.r[BX] = rnd(3) ? 0x266E : window(0x2600, 0x200);
            const uint16_t n = rnd(10) == 0 ? static_cast<uint16_t>(0x100 + rnd(0x100)) : static_cast<uint16_t>(1 + rnd(40));
            r.r[DX] = n;
            for (uint32_t i = 0; i < n && i < 600; ++i) {
                const uint16_t at = static_cast<uint16_t>(r.r[BX] + 6 * i);
                int16_t z;
                switch (rnd(10)) {
                case 0: z = static_cast<int16_t>(-static_cast<int>(rnd(5))); break;
                case 1: case 2: z = static_cast<int16_t>(1 + rnd(4)); break;
                case 3: case 4: case 5: case 6: z = static_cast<int16_t>(5 + rnd(300)); break;
                case 7: z = 256; break;
                case 8: z = static_cast<int16_t>(rnd(0x8000)); break;
                default: z = static_cast<int16_t>(rnd(65536)); break;
                }
                auto coord = [&]() -> uint16_t {
                    switch (rnd(8)) {
                    case 0: return 0x7FFF;
                    case 1: return static_cast<uint16_t>(z * 128 + static_cast<int>(rnd(5)) - 2);
                    case 2: return static_cast<uint16_t>(-(z * 128) + static_cast<int>(rnd(5)) - 2);
                    case 3: return static_cast<uint16_t>(0x80 + rnd(4) - 2);
                    case 4: return static_cast<uint16_t>(static_cast<int16_t>(rnd(1001)) - 500);
                    default: return static_cast<uint16_t>(rnd(65536));
                    }
                };
                c.w16(kData, at, coord());
                c.w16(kData, static_cast<uint16_t>(at + 2), coord());
                c.w16(kData, static_cast<uint16_t>(at + 4), static_cast<uint16_t>(z));
            }
            c.w16(kData, 0x3169, rnd(4) ? 160 : rword());
            c.w16(kData, 0x316B, rnd(4) ? 100 : rword());
            if (!c.run()) { std::printf("project_vertices: no return\n"); ++fails; }
        }
    }
    std::printf("%s", runner.summary().c_str());
    std::printf("no-return failures: %d\n", fails);
    for (const auto& fn : vette::game::native_functions()) if (runner.stats(fn.name).mismatches) return 4;
    return fails ? 5 : 0;
}
