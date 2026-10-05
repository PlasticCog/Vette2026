// Graphics option tool: runs the hosted game headlessly like vette_run and renders what the player
// would see with each art set (DOS, PC-98, Mac), for reviewing the replacement art.
//
//   vette_gfx --game Game --seconds 30 --key 13:39 --shot 3 --shot 14 --out gfx/
//       writes gfx/shot_<T>_dos.png, _pc98.png and _mac.png at --size (default 1280x960)
//   vette_gfx --game Game --stills --out gfx/
//       every recognised screen from its DOS picture alone (screens a headless run can't reach)
//   vette_gfx --game Game --dump gfx/art
//       every Mac PICT and PC-98 picture as PNG, and an inventory on stdout
//   vette_gfx --game Game --trace-io --seconds 30 --key ...
//       logs the game's file opens and reads and its picture blits (3009:8666) with caller addresses
//
// --game DIR        DOS files (default Game)
// --pc98 DIR        PC-98 files (default <game>/PC98)
// --mac-rsrc FILE   Color VETTE!'s resource fork as a raw file. Without --pc98/--mac-rsrc the game
//                   folder is read like the game does (PC98/, Mac/ in any form), with the extracted
//                   copy under Vette_Mac_EN/ standing in for a missing Mac/.
// --key T:SC, --hold A:B:SC, --shot T, --seconds N, --skip-manual-check, --idle-skip: as vette_run
// --poke A:B:SEG:OFF:BYTE  write BYTE at emulator address SEG:OFF (hex) every millisecond from second A
//                   to B: steers the game to screens a script can't easily reach. The README's race
//                   script with --poke 45:62:224A:2AFF:0 ends in a win: the winner screen at 54 s,
//                   the high scores at 60 s (DS:2AFF is the race-over picture)
// --size WxH        output size of each picture (4:3 recommended)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "assets/pc98_pic.h"
#include "assets/pict.h"
#include "assets/png.h"
#include "game/options.h"
#include "graphics/art_files.h"
#include "graphics/dos_art.h"
#include "graphics/substitution.h"
#include "host/machine.h"

namespace fs = std::filesystem;
using vette::graphics::Art;
using vette::graphics::Composite;
using vette::graphics::FrameView;
using vette::graphics::Image;
using vette::graphics::Substitution;
using vette::host::Cpu;

namespace {

struct KeyEvent {
    uint64_t at_ms;
    uint8_t scancode;
};

struct Poke {
    uint64_t from_ms, to_ms;
    uint32_t linear;
    uint8_t value;
};

// Pictures with transparency (the dumped art) keep their alpha; composites are opaque.
bool save(const fs::path& path, const Image& img, bool alpha = false) {
    return vette::assets::write_png(path, img.width, img.height, img.pixels, alpha);
}

std::string fmt_time(double t) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%06.2f", t);
    return buf;
}

int usage() {
    std::fprintf(stderr,
                 "usage: vette_gfx [--game DIR] [--pc98 DIR] [--mac-rsrc FILE] [--out DIR] [--size WxH]\n"
                 "                 [--seconds N] [--shot T]... [--key T:SC]... [--hold A:B:SC]...\n"
                 "                 [--skip-manual-check] [--idle-skip] [--stills | --dump DIR | --trace-io]\n");
    return 2;
}

// Every Mac PICT and PC-98 picture as PNG.
int dump(const vette::graphics::ArtFiles& files, const fs::path& out) {
    fs::create_directories(out);
    int picts = 0;
    for (int id = -32768; id <= 32767; ++id) {
        const auto data = files.mac_pict(static_cast<std::int16_t>(id));
        if (data.empty()) continue;
        ++picts;
        vette::assets::Pict pict;
        std::string err;
        if (!vette::assets::decode_pict(data, pict, &err)) {
            std::printf("PICT %6d: %s\n", id, err.c_str());
            continue;
        }
        std::printf("PICT %6d v%d %4dx%-4d frame at (%d,%d)%s", id, pict.version, pict.width, pict.height,
                    pict.frame_left, pict.frame_top, pict.skipped ? " (approximated)" : "");
        for (const auto& t : pict.texts) std::printf(" [text at %d,%d: \"%s\"]", t.x, t.y, t.text.c_str());
        std::printf("\n");
        save(out / ("mac_pict_" + std::to_string(id) + ".png"), vette::graphics::from_pict(pict, 0), true);
    }
    if (picts == 0) std::printf("no Mac pictures found\n");
    std::array<std::uint32_t, 16> pal{};
    for (std::size_t i = 0; i < 16; ++i) pal[i] = vette::assets::pc98_rgb(vette::assets::kPc98Palette[i]);
    for (const auto& info : vette::assets::pc98_pics()) {
        const auto data = files.pc98_file(info.name);
        vette::assets::Pc98Pic pic;
        std::string err;
        if (data.empty() || !vette::assets::decode_pc98_pic(info.name, data, pic, &err)) {
            std::printf("%-12s %s\n", info.name, data.empty() ? "not found" : err.c_str());
            continue;
        }
        std::printf("%-12s %4dx%-4d %s\n", info.name, pic.width, pic.height, info.what);
        std::string name = info.name;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        save(out / ("pc98_" + name.substr(0, name.find('.')) + ".png"), vette::graphics::from_pc98(pic, pal));
    }
    return 0;
}

// Each screen's composite from its DOS picture alone.
int stills(const vette::graphics::ArtFiles& files, std::vector<std::unique_ptr<Substitution>>& sets, const fs::path& out,
           int out_w, int out_h) {
    fs::create_directories(out);
    const struct {
        const char* file;
        int header, w, h, frame_w, frame_h, x, y;
    } pics[] = {
        {"TITLE.BIN", 0, 640, 200, 640, 200, 0, 0},     {"GARAGE.BIN", 0, 640, 200, 640, 200, 0, 0},
        {"EGAPIC.BIN", 2, 320, 200, 320, 200, 0, 0},    {"HIGHSC.BIN", 0, 640, 200, 640, 200, 0, 0},
        {"WINNER.BIN", 0, 640, 200, 640, 200, 0, 0},    {"CRASH0.BIN", 0, 176, 128, 320, 200, 72, 30},
        {"CRASH1.BIN", 0, 176, 128, 320, 200, 72, 30},  {"LOSER0.BIN", 0, 176, 128, 320, 200, 72, 30},
        {"LOSER1.BIN", 0, 176, 128, 320, 200, 72, 30},  {"LOSER2.BIN", 0, 176, 128, 320, 200, 72, 30},
        {"LOSER3.BIN", 0, 176, 128, 320, 200, 72, 30},
    };
    std::array<std::uint32_t, 16> ega{};
    const std::uint32_t ega_rgb[16] = {0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
                                       0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};
    std::copy(std::begin(ega_rgb), std::end(ega_rgb), ega.begin());
    for (const auto& p : pics) {
        vette::graphics::DosPicture pic;
        if (!vette::graphics::decode_dos_picture(files.dos_file(p.file), p.header, p.w, p.h, pic)) continue;
        std::vector<std::uint8_t> frame(static_cast<std::size_t>(p.frame_w) * p.frame_h, 8);
        for (int y = 0; y < p.h; ++y)
            std::copy_n(pic.pixels.begin() + static_cast<std::ptrdiff_t>(y) * p.w, p.w,
                        frame.begin() + static_cast<std::ptrdiff_t>(p.y + y) * p.frame_w + p.x);
        const FrameView view{frame.data(), p.frame_w, p.frame_h, &ega};
        std::string stem = p.file;
        stem = stem.substr(0, stem.find('.'));
        std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        save(out / ("still_" + stem + "_dos.png"), vette::graphics::render_frame(frame.data(), p.frame_w, p.frame_h, ega, out_w, out_h));
        for (auto& set : sets) {
            Composite c;
            if (!set->compose(view, c)) continue;
            std::string art = vette::graphics::art_name(set->art());
            art.erase(std::remove(art.begin(), art.end(), '-'), art.end());
            std::transform(art.begin(), art.end(), art.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            save(out / ("still_" + stem + "_" + art + ".png"), vette::graphics::render(c, out_w, out_h));
            std::printf("%-11s %-6s", p.file, vette::graphics::art_name(set->art()));
            for (const auto& found : set->found())
                std::printf(" %s (%.2f at %d,%d)", vette::graphics::screen_name(found.screen), found.match, found.rect.x, found.rect.y);
            std::printf("\n");
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    vette::host::MachineConfig config;
    config.game_dir = "Game";
    config.start_time = vette::host::RealTime{1989, 10, 23, 12, 0, 0, 0};  // deterministic
    fs::path pc98_dir, mac_rsrc, out_dir = ".", dump_dir;
    double seconds = 10;
    std::vector<double> shots;
    std::vector<KeyEvent> keys;
    std::vector<Poke> pokes;
    int out_w = 1280, out_h = 960;
    bool skip_manual_check = false, idle_skip = false, do_stills = false, trace_io = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        if (a == "--game" && has_value) {
            config.game_dir = argv[++i];
        } else if (a == "--pc98" && has_value) {
            pc98_dir = argv[++i];
        } else if (a == "--mac-rsrc" && has_value) {
            mac_rsrc = argv[++i];
        } else if (a == "--out" && has_value) {
            out_dir = argv[++i];
        } else if (a == "--dump" && has_value) {
            dump_dir = argv[++i];
        } else if (a == "--size" && has_value) {
            char* end = nullptr;
            out_w = static_cast<int>(std::strtol(argv[++i], &end, 10));
            out_h = *end == 'x' ? static_cast<int>(std::strtol(end + 1, nullptr, 10)) : 0;
            if (out_w <= 0 || out_h <= 0 || out_w > 8192 || out_h > 8192) return usage();
        } else if (a == "--seconds" && has_value) {
            seconds = std::atof(argv[++i]);
        } else if (a == "--shot" && has_value) {
            shots.push_back(std::atof(argv[++i]));
        } else if ((a == "--key" || a == "--hold") && has_value) {
            const std::string v = argv[++i];
            const size_t c1 = v.find(':');
            if (c1 == std::string::npos) return usage();
            const size_t c2 = a == "--hold" ? v.find(':', c1 + 1) : std::string::npos;
            if (a == "--hold" && c2 == std::string::npos) return usage();
            const auto from = static_cast<uint64_t>(std::atof(v.substr(0, c1).c_str()) * 1000);
            const auto to = a == "--hold" ? static_cast<uint64_t>(std::atof(v.substr(c1 + 1, c2 - c1 - 1).c_str()) * 1000)
                                          : from + 100;
            const auto sc = static_cast<uint8_t>(std::strtoul(v.substr(a == "--hold" ? c2 + 1 : c1 + 1).c_str(), nullptr, 16));
            keys.push_back({from, sc});
            keys.push_back({to, static_cast<uint8_t>(sc | 0x80)});
        } else if (a == "--poke" && has_value) {
            const std::string v = argv[++i];
            std::vector<std::string> parts;
            for (size_t at = 0;;) {
                const size_t c = v.find(':', at);
                parts.push_back(v.substr(at, c - at));
                if (c == std::string::npos) break;
                at = c + 1;
            }
            if (parts.size() != 5) return usage();
            const auto hex = [&](std::size_t k) { return std::strtoul(parts[k].c_str(), nullptr, 16); };
            pokes.push_back({static_cast<uint64_t>(std::atof(parts[0].c_str()) * 1000),
                             static_cast<uint64_t>(std::atof(parts[1].c_str()) * 1000),
                             Cpu::linear(static_cast<uint16_t>(hex(2)), static_cast<uint16_t>(hex(3))),
                             static_cast<uint8_t>(hex(4))});
        } else if (a == "--skip-manual-check") {
            skip_manual_check = true;
        } else if (a == "--idle-skip") {
            idle_skip = true;
        } else if (a == "--stills") {
            do_stills = true;
        } else if (a == "--trace-io") {
            trace_io = true;
        } else {
            return usage();
        }
    }
    vette::graphics::ArtFiles files;
    if (pc98_dir.empty() && mac_rsrc.empty()) {
        std::vector<std::string> notes;
        files = vette::graphics::ArtFiles::from_game_dir(config.game_dir, &notes);
        for (const auto& note : notes) std::printf("%s\n", note.c_str());
        // In the repository, the extracted Mac files stand in for Game/Mac/.
        const fs::path extracted =
            fs::path("Vette_Mac_EN") / "extracted" / "VETTE! Folder" / "(Folder) Color VETTE!" / "Color VETTE!.rsrc";
        std::error_code ec;
        if (files.mac_pict(24592).empty() && fs::exists(extracted, ec)) {
            files.mac_pict = vette::graphics::ArtFiles::from_folders({}, {}, extracted).mac_pict;
            std::printf("Mac: %s\n", extracted.string().c_str());
        }
    } else {
        files = vette::graphics::ArtFiles::from_folders(config.game_dir,
                                                        pc98_dir.empty() ? config.game_dir / "PC98" : pc98_dir, mac_rsrc);
    }
    if (!dump_dir.empty()) return dump(files, dump_dir);

    std::vector<std::unique_ptr<Substitution>> sets;
    for (const Art art : {Art::Pc98, Art::Mac}) {
        auto set = std::make_unique<Substitution>(art, files);
        for (const auto& w : set->warnings()) std::printf("%s: %s\n", vette::graphics::art_name(art), w.c_str());
        std::printf("%s art: %zu screens\n", vette::graphics::art_name(art), set->available().size());
        if (!set->available().empty()) sets.push_back(std::move(set));
    }
    if (do_stills) return stills(files, sets, out_dir, out_w, out_h);

    config.save_dir = out_dir / "save";
    std::sort(keys.begin(), keys.end(), [](const KeyEvent& a, const KeyEvent& b) { return a.at_ms < b.at_ms; });
    std::sort(shots.begin(), shots.end());
    fs::create_directories(out_dir);

    vette::host::Machine machine(config);
    std::string error;
    if (!machine.boot(error)) {
        std::fprintf(stderr, "boot failed: %s\n", error.c_str());
        return 1;
    }
    if (skip_manual_check) vette::game::install_skip_manual_check(machine.cpu());
    if (idle_skip) vette::game::install_idle_skip(machine);
    for (auto& set : sets) set->set_program_memory(machine.memory().ram());

    if (trace_io) {
        // File opens and reads through the INT 21h entry, and the picture unpacker 3009:8666
        // (AX = destination segment, DS:SI = packed data, DI = offset, BH = bytes per row, BP = rows).
        auto& mem = machine.memory();
        const uint16_t off21 = mem.read16(0x84), seg21 = mem.read16(0x86);
        machine.cpu().add_watch(Cpu::linear(seg21, off21), [&machine, &mem](Cpu& cpu) {
            using namespace vette::host;
            const auto& r = cpu.regs;
            const uint32_t sp = Cpu::linear(r.s[SS], r.r[SP]);
            const uint16_t ip = mem.read16(sp), cs = mem.read16(sp + 2);
            const double t = static_cast<double>(machine.emulated_ns()) / 1e9;
            if (r.hi(AX) == 0x3D) {
                std::string name;
                for (uint32_t a = Cpu::linear(r.s[DS], r.r[DX]); mem.read8(a) && name.size() < 64; ++a)
                    name += static_cast<char>(mem.read8(a));
                std::printf("t=%.3f open %s from %04X:%04X\n", t, name.c_str(), cs, ip);
            } else if (r.hi(AX) == 0x3F) {
                std::printf("t=%.3f read %u bytes to %04X:%04X from %04X:%04X\n", t, r.r[CX], r.s[DS], r.r[DX], cs, ip);
            }
        });
        machine.cpu().add_watch(Cpu::linear(0x4009, 0x8666), [&machine, &mem](Cpu& cpu) {
            using namespace vette::host;
            const auto& r = cpu.regs;
            const uint32_t sp = Cpu::linear(r.s[SS], r.r[SP]);
            std::printf("t=%.3f unpack to %04X:%04X %dx%d from %04X:%04X, called from 4009:%04X\n",
                        static_cast<double>(machine.emulated_ns()) / 1e9, r.r[AX], r.r[DI], r.hi(BX) * 8, r.r[BP],
                        r.s[DS], r.r[SI], mem.read16(sp));
        });
    }

    const auto total_ms = static_cast<uint64_t>(seconds * 1000);
    size_t next_key = 0, next_shot = 0;
    vette::host::Ega::Frame frame;
    for (uint64_t ms = 0; ms < total_ms && !machine.stopped(); ++ms) {
        while (next_key < keys.size() && keys[next_key].at_ms <= ms) machine.key(keys[next_key++].scancode);
        for (const auto& p : pokes)
            if (ms >= p.from_ms && ms <= p.to_ms) machine.memory().write8(p.linear, p.value);
        machine.run_for(1'000'000);
        while (next_shot < shots.size() && shots[next_shot] * 1000 <= static_cast<double>(ms + 1)) {
            machine.render(frame);
            const std::string stem = "shot_" + fmt_time(shots[next_shot]);
            ++next_shot;
            if (frame.width == 0) {
                std::printf("%s: text mode\n", stem.c_str());
                continue;
            }
            save(out_dir / (stem + "_dos.png"),
                 vette::graphics::render_frame(frame.pixels.data(), frame.width, frame.height, frame.palette, out_w, out_h));
            std::printf("%s (%dx%d)", stem.c_str(), frame.width, frame.height);
            const FrameView view{frame.pixels.data(), frame.width, frame.height, &frame.palette};
            for (auto& set : sets) {
                Composite c;
                std::string art = vette::graphics::art_name(set->art());
                art.erase(std::remove(art.begin(), art.end(), '-'), art.end());
                std::transform(art.begin(), art.end(), art.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                const auto t0 = std::chrono::steady_clock::now();
                const bool composed = set->compose(view, c);
                const double took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                const Image img = composed ? vette::graphics::render(c, out_w, out_h)
                                           : vette::graphics::render_frame(frame.pixels.data(), frame.width,
                                                                           frame.height, frame.palette, out_w, out_h);
                save(out_dir / (stem + "_" + art + ".png"), img);
                std::printf("  %s (%.2f ms):", vette::graphics::art_name(set->art()), took);
                if (set->found().empty()) std::printf(" -");
                for (const auto& found : set->found())
                    std::printf(" %s %.2f", vette::graphics::screen_name(found.screen), found.match);
            }
            std::printf("\n");
        }
    }
    return machine.fault().empty() ? 0 : 3;
}
