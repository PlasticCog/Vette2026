// Headless runner for the hosted VETTE.EXE: boots the game, runs emulated time, injects scripted
// keys and saves screenshots. Used to verify the emulator and, later, for lockstep tests.
//
//   vette_run --game Game --seconds 20 --shot 5 --shot 12 --key 8:1C --out shots/
//
// --shot T       save the screen at emulated second T (as shot_<T>.bmp)
// --key T:SC     press scan code SC (hex, set 1) at second T; released 100 ms later
// --hold A:B:SC  press scan code SC at second A, release at second B
// --watch S:O    print the word at emulator address S:O (hex) at every shot
// --cpu-hz N     emulated CPU clock (default 12000000)
// --verify F     run native function F (or "all") in Verify mode: every call is checked against the
//                original; mismatches are printed and the exit code is 4
// --native F     replace function F (or "all") with its native port, unverified
// --smooth       take screenshots through the smooth renderer (race view interpolated between frames)
// --smooth-check replay every game frame and compare it with the original's own drawing (exactness)
// --idle-skip    skip emulated time spent polling for vertical retrace (the game's default)
// --skip-manual-check  skip the copy-protection question (the game's default; off here so key
//                scripts that type an answer keep working)
// --trace        log every DOS file access and unhandled BIOS/port use

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "game/natives.h"
#include "game/options.h"
#include "game/smooth.h"
#include "host/machine.h"
#include "host/native.h"

namespace {

using vette::host::Ega;
using vette::host::Machine;
using vette::host::MachineConfig;
using vette::host::NativeRunner;

constexpr uint64_t kNsPerMs = 1'000'000;

struct KeyEvent {
    uint64_t at_ms;
    uint8_t scancode;
};

void write_u16(std::ofstream& f, uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); }
void write_u32(std::ofstream& f, uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); }

// 8-bit indexed BMP, bottom-up.
bool save_bmp(const std::filesystem::path& path, const Ega::Frame& frame) {
    if (frame.width == 0) {
        return false;
    }
    const auto w = static_cast<uint32_t>(frame.width), h = static_cast<uint32_t>(frame.height);
    const uint32_t stride = (w + 3) & ~3u;
    const uint32_t data_off = 14 + 40 + 256 * 4;
    std::ofstream f(path, std::ios::binary);
    f.write("BM", 2);
    write_u32(f, data_off + stride * h);
    write_u32(f, 0);
    write_u32(f, data_off);
    write_u32(f, 40);
    write_u32(f, w);
    write_u32(f, h);
    write_u16(f, 1);
    write_u16(f, 8);
    write_u32(f, 0);
    write_u32(f, stride * h);
    write_u32(f, 2835);
    write_u32(f, 2835);
    write_u32(f, 256);
    write_u32(f, 0);
    for (uint32_t i = 0; i < 256; ++i) {
        const uint32_t rgb = i < 16 ? frame.palette[i] : 0;
        write_u32(f, rgb & 0xFFFFFF);  // BGRA little-endian == 0x00RRGGBB
    }
    std::vector<char> row(stride, 0);
    for (uint32_t y = h; y-- > 0;) {
        std::copy_n(frame.pixels.begin() + static_cast<std::ptrdiff_t>(y * w), w, row.begin());
        f.write(row.data(), stride);
    }
    return static_cast<bool>(f);
}

int usage() {
    std::fprintf(stderr, "usage: vette_run --game <dir> [--seconds N] [--shot T]... [--key T:SC]... "
                         "[--hold A:B:SC]... [--watch S:O]... [--cpu-hz N] [--out dir] [--trace]\n"
                         "       [--verify F|all]... [--native F|all]... [--skip-manual-check] [--smooth]\n"
                         "       [--smooth-check] [--idle-skip]\n");
    return 2;
}

} // namespace

int main(int argc, char* argv[]) {
    MachineConfig config;
    config.game_dir = "Game";
    config.start_time = vette::host::RealTime{1989, 10, 23, 12, 0, 0, 0};  // deterministic
    double seconds = 10;
    std::vector<double> shots;
    std::vector<KeyEvent> keys;
    std::vector<std::pair<uint16_t, uint16_t>> watches;
    std::vector<std::pair<std::string, NativeRunner::Mode>> natives;
    std::filesystem::path out_dir = ".";
    bool trace = false;
    bool skip_manual_check = false;
    bool smooth_shots = false;  // screenshots through the smooth renderer (interpolated race view)
    bool idle_skip = false;     // skip time spent polling for vertical retrace (the game's default)
    bool smooth_check = false;  // replay every game frame and compare it with the original's drawing

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        if (a == "--game" && has_value) {
            config.game_dir = argv[++i];
        } else if (a == "--seconds" && has_value) {
            seconds = std::atof(argv[++i]);
        } else if (a == "--shot" && has_value) {
            shots.push_back(std::atof(argv[++i]));
        } else if (a == "--key" && has_value) {
            const std::string v = argv[++i];
            const size_t colon = v.find(':');
            if (colon == std::string::npos) {
                return usage();
            }
            const auto at = static_cast<uint64_t>(std::atof(v.substr(0, colon).c_str()) * 1000);
            const auto sc = static_cast<uint8_t>(std::strtoul(v.substr(colon + 1).c_str(), nullptr, 16));
            keys.push_back({at, sc});
            keys.push_back({at + 100, static_cast<uint8_t>(sc | 0x80)});
        } else if (a == "--hold" && has_value) {
            const std::string v = argv[++i];
            const size_t c1 = v.find(':'), c2 = v.find(':', c1 + 1);
            if (c1 == std::string::npos || c2 == std::string::npos) {
                return usage();
            }
            const auto from = static_cast<uint64_t>(std::atof(v.substr(0, c1).c_str()) * 1000);
            const auto to = static_cast<uint64_t>(std::atof(v.substr(c1 + 1, c2 - c1 - 1).c_str()) * 1000);
            const auto sc = static_cast<uint8_t>(std::strtoul(v.substr(c2 + 1).c_str(), nullptr, 16));
            keys.push_back({from, sc});
            keys.push_back({to, static_cast<uint8_t>(sc | 0x80)});
        } else if (a == "--watch" && has_value) {
            const std::string v = argv[++i];
            const size_t colon = v.find(':');
            if (colon == std::string::npos) {
                return usage();
            }
            watches.push_back({static_cast<uint16_t>(std::strtoul(v.substr(0, colon).c_str(), nullptr, 16)),
                               static_cast<uint16_t>(std::strtoul(v.substr(colon + 1).c_str(), nullptr, 16))});
        } else if ((a == "--verify" || a == "--native") && has_value) {
            natives.push_back({argv[++i], a == "--verify" ? NativeRunner::Mode::Verify : NativeRunner::Mode::Native});
        } else if (a == "--cpu-hz" && has_value) {
            config.cpu_hz = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--out" && has_value) {
            out_dir = argv[++i];
        } else if (a == "--trace") {
            trace = true;
        } else if (a == "--skip-manual-check") {
            skip_manual_check = true;
        } else if (a == "--smooth") {
            smooth_shots = true;
        } else if (a == "--idle-skip") {
            idle_skip = true;
        } else if (a == "--smooth-check") {
            smooth_check = true;
        } else {
            return usage();
        }
    }
    config.save_dir = out_dir / "save";
    std::sort(keys.begin(), keys.end(), [](const KeyEvent& a, const KeyEvent& b) { return a.at_ms < b.at_ms; });
    std::sort(shots.begin(), shots.end());
    std::filesystem::create_directories(out_dir);

    Machine machine(config);
    machine.set_log([trace](const std::string& msg) {
        if (trace || msg.rfind("console:", 0) == 0 || msg.find("unhandled") != std::string::npos) {
            std::printf("[%s]\n", msg.c_str());
        }
    });
    std::string error;
    if (!machine.boot(error)) {
        std::fprintf(stderr, "boot failed: %s\n", error.c_str());
        return 1;
    }

    if (skip_manual_check) {
        vette::game::install_skip_manual_check(machine.cpu());
    }
    if (idle_skip) {
        vette::game::install_idle_skip(machine);
    }
    NativeRunner runner(machine);
    runner.set_report([](const std::string& msg) { std::printf("%s\n", msg.c_str()); });
    for (const auto& [name, mode] : natives) {
        if (name == "all") {
            for (const auto& fn : vette::game::native_functions()) {
                runner.install(fn, mode);
            }
        } else if (const auto* fn = vette::game::find_native(name)) {
            runner.install(*fn, mode);
        } else {
            std::fprintf(stderr, "unknown native function '%s'\n", name.c_str());
            return 2;
        }
    }

    std::unique_ptr<vette::game::SmoothRenderer> smooth;
    if (smooth_shots || smooth_check) {
        smooth = std::make_unique<vette::game::SmoothRenderer>(machine);
    }
    uint64_t checked_frames = 0, mismatched_frames = 0, mismatched_pixels = 0, last_checked = 0;

    const auto total_ms = static_cast<uint64_t>(seconds * 1000);
    size_t next_key = 0, next_shot = 0;
    Ega::Frame frame;
    for (uint64_t ms = 0; ms < total_ms && !machine.stopped(); ++ms) {
        while (next_key < keys.size() && keys[next_key].at_ms <= ms) {
            machine.key(keys[next_key++].scancode);
        }
        machine.run_for(kNsPerMs);
        runner.poll();
        // Once per game frame, as soon as the original has finished drawing it (self_check is -1 until then).
        if (smooth_check && smooth->stats().game_frames != last_checked) {
            if (const int diff = smooth->self_check(); diff >= 0) {
                last_checked = smooth->stats().game_frames;
                ++checked_frames;
                if (diff > 0) {
                    ++mismatched_frames;
                    mismatched_pixels += static_cast<uint64_t>(diff);
                }
            }
        }
        while (next_shot < shots.size() && shots[next_shot] * 1000 <= static_cast<double>(ms + 1)) {
            if (!smooth_shots || !smooth->render(machine.emulated_ns(), frame)) {
                machine.render(frame);
            }
            char name[64];
            std::snprintf(name, sizeof name, "shot_%06.2f.bmp", shots[next_shot]);
            const bool saved = save_bmp(out_dir / name, frame);
            std::printf("t=%.2fs %s %s (mode %dx%d)", shots[next_shot], saved ? "saved" : "no frame (text mode)",
                        name, frame.width, frame.height);
            for (const auto& [seg, off] : watches) {
                std::printf("  %04X:%04X=%u", seg, off, machine.memory().read16(vette::host::Cpu::linear(seg, off)));
            }
            std::printf("\n");
            ++next_shot;
        }
    }
    const auto& r = machine.cpu().regs;
    std::printf("ran %.3f emulated s, %llu cycles; CS:IP=%04X:%04X%s%s\n",
                static_cast<double>(machine.emulated_ns()) / 1e9,
                static_cast<unsigned long long>(machine.cpu().total_cycles()), r.s[vette::host::CS], r.ip,
                machine.fault().empty() ? "" : " FAULT: ", machine.fault().c_str());
    if (machine.stopped() && machine.fault().empty()) {
        std::printf("program exited with code %d\n", machine.exit_code());
    }
    if (!natives.empty()) {
        std::printf("native functions:\n%s", runner.summary().c_str());
    }
    if (smooth) {
        const auto& st = smooth->stats();
        std::printf("smooth: %llu game frames, %llu replays, %.3f ms per replay\n",
                    static_cast<unsigned long long>(st.game_frames), static_cast<unsigned long long>(st.replays),
                    st.replays ? st.replay_ms / static_cast<double>(st.replays) : 0.0);
        if (smooth_check) {
            std::printf("smooth check: %llu frames compared, %llu differ (%llu pixels)\n",
                        static_cast<unsigned long long>(checked_frames),
                        static_cast<unsigned long long>(mismatched_frames),
                        static_cast<unsigned long long>(mismatched_pixels));
        }
    }
    if (!machine.fault().empty()) {
        return 3;
    }
    for (const auto& fn : vette::game::native_functions()) {
        if (runner.stats(fn.name).mismatches > 0) {
            return 4;
        }
    }
    return 0;
}
