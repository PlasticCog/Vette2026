// The Mac VETTE!'s files: lists what a Mac game folder or image holds and dumps its sounds as WAV.
//
//   vette_mac [--mac PATH] [list]                  files (type/creator, fork sizes, where from) and notes
//   vette_mac [--mac PATH] resources FILE          a file's resources: type, id, size, name
//   vette_mac [--mac PATH] dump FILE TYPE ID OUT   writes one resource's bytes
//   vette_mac [--mac PATH] fork FILE data|rsrc OUT writes a fork
//   vette_mac [--mac PATH] sounds [--wav DIR] [--native]
//                                                  the sounds with their rates, loops and uses; --wav
//                                                  writes each (at the rate the game plays it, or its
//                                                  recorded rate with --native), loops in a 'smpl' chunk
//   vette_mac [--mac PATH] engine RPM SECONDS OUT  the race engine at RPM, as the Mac pitches it
//
// PATH is a folder (searched a few levels deep) or a single image/file; the default is Game/Mac in the
// current folder or up to 4 of its parents. FILE is a path or a name ("VETTE!.Data", "Color VETTE!").

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "assets/mac_files.h"
#include "assets/mac_sounds.h"

namespace fs = std::filesystem;
using namespace vette::assets;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: vette_mac [--mac PATH] [list | resources FILE | dump FILE TYPE ID OUT |\n"
                 "                  fork FILE data|rsrc OUT | sounds [--wav DIR] [--native] |\n"
                 "                  engine RPM SECONDS OUT]\n");
    return 2;
}

fs::path default_location() {
    std::error_code ec;
    fs::path dir = fs::current_path(ec);
    for (int i = 0; i < 5 && !dir.empty(); ++i) {
        if (fs::is_directory(dir / "Game" / "Mac", ec)) return dir / "Game" / "Mac";
        if (dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return fs::path("Game") / "Mac";
}

bool write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}
void put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
}
void tag(std::vector<std::uint8_t>& v, const char* t) { v.insert(v.end(), t, t + 4); }

// Mono WAV: 8-bit unsigned when the source was 8-bit (exact), else 16-bit. Loops go in a 'smpl' chunk.
std::vector<std::uint8_t> wav(const std::vector<std::int16_t>& samples, int bits, double rate, std::size_t loop_start,
                              std::size_t loop_end) {
    const std::uint32_t hz = static_cast<std::uint32_t>(std::lround(rate));
    const std::uint32_t bytes = static_cast<std::uint32_t>(samples.size() * (bits == 8 ? 1 : 2));
    std::vector<std::uint8_t> v;
    tag(v, "RIFF");
    put32(v, 0);  // patched below
    tag(v, "WAVE");
    tag(v, "fmt ");
    put32(v, 16);
    put16(v, 1);
    put16(v, 1);
    put32(v, hz);
    put32(v, hz * (bits == 8 ? 1u : 2u));
    put16(v, bits == 8 ? 1 : 2);
    put16(v, bits == 8 ? 8 : 16);
    tag(v, "data");
    put32(v, bytes);
    for (const std::int16_t s : samples) {
        if (bits == 8) {
            v.push_back(static_cast<std::uint8_t>((s >> 8) + 128));
        } else {
            put16(v, static_cast<std::uint16_t>(s));
        }
    }
    if (bytes & 1) v.push_back(0);
    if (loop_end > loop_start) {
        tag(v, "smpl");
        put32(v, 36 + 24);
        for (int i = 0; i < 2; ++i) put32(v, 0);  // manufacturer, product
        put32(v, static_cast<std::uint32_t>(1e9 / rate));
        put32(v, 60);  // MIDI unity note
        for (int i = 0; i < 3; ++i) put32(v, 0);  // pitch fraction, SMPTE format, offset
        put32(v, 1);  // loops
        put32(v, 0);  // sampler data
        put32(v, 0);  // cue id
        put32(v, 0);  // forward loop
        put32(v, static_cast<std::uint32_t>(loop_start));
        put32(v, static_cast<std::uint32_t>(loop_end - 1));  // inclusive
        put32(v, 0);
        put32(v, 0);  // play count: forever
    }
    const std::uint32_t riff = static_cast<std::uint32_t>(v.size() - 8);
    for (int i = 0; i < 4; ++i) v[4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(riff >> (8 * i));
    return v;
}

std::string safe_name(std::string s) {
    for (char& c : s) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
            c == '|' || static_cast<unsigned char>(c) < 0x20)
            c = '_';
    }
    return s;
}

std::string size_text(std::uint64_t n) { return n ? std::to_string(n) : "-"; }

int cmd_list(const MacFiles& files) {
    std::printf("%-6s %-6s %10s %10s  %-60s %s\n", "type", "crea", "data", "rsrc", "path", "from");
    for (const MacFile& f : files.files()) {
        std::printf("%-6s %-6s %10s %10s  %-60s %s\n", f.type ? fourcc_string(f.type).c_str() : "",
                    f.creator ? fourcc_string(f.creator).c_str() : "", size_text(f.data_size).c_str(),
                    size_text(f.rsrc_size).c_str(), f.path.c_str(), f.source.c_str());
    }
    std::printf("%zu files\n", files.files().size());
    return 0;
}

const MacFile* need_file(const MacFiles& files, const char* name) {
    const MacFile* f = files.find(name);
    if (!f) std::fprintf(stderr, "no file '%s'\n", name);
    return f;
}

int cmd_resources(const MacFiles& files, const char* name) {
    const MacFile* f = need_file(files, name);
    if (!f) return 1;
    ResourceFork fork;
    std::string error;
    if (!fork.parse(files.resource_fork(*f), &error)) {
        std::fprintf(stderr, "%s: %s\n", f->path.c_str(), error.c_str());
        return 1;
    }
    std::printf("%s: %zu resources\n", f->path.c_str(), fork.resources().size());
    for (const Resource& r : fork.resources()) {
        std::printf("  %-4s %6d %8u  %s\n", fourcc_string(r.type).c_str(), r.id, r.size,
                    r.has_name ? ("'" + r.name + "'").c_str() : "");
    }
    if (fork.skipped()) std::printf("  (%d resources skipped: data outside the fork)\n", fork.skipped());
    return 0;
}

int cmd_dump(const MacFiles& files, const char* name, const char* type, const char* id, const char* out) {
    const MacFile* f = need_file(files, name);
    const auto code = fourcc_from_string(type);
    if (!f || !code) return 1;
    const auto fork = files.resources(*f);
    const Resource* r = fork ? fork->find(*code, static_cast<std::int16_t>(std::atoi(id))) : nullptr;
    if (!r) {
        std::fprintf(stderr, "no resource %s %s in %s\n", type, id, f->path.c_str());
        return 1;
    }
    const auto bytes = fork->data(*r);
    return write_file(out, std::vector<std::uint8_t>(bytes.begin(), bytes.end())) ? 0 : 1;
}

int cmd_fork(const MacFiles& files, const char* name, const std::string& which, const char* out) {
    const MacFile* f = need_file(files, name);
    if (!f || (which != "data" && which != "rsrc")) return 1;
    return write_file(out, which == "data" ? files.data_fork(*f) : files.resource_fork(*f)) ? 0 : 1;
}

int cmd_sounds(const MacFiles& files, const fs::path& wav_dir, bool native) {
    const auto fork = find_vette_data(files);
    if (!fork) {
        std::fprintf(stderr, "no file with INST sounds (VETTE!.Data) found\n");
        return 1;
    }
    std::vector<std::string> problems;
    const auto sounds = decode_mac_sounds(*fork, &problems);
    std::printf("%-4s %6s %-15s %7s %8s %8s %4s %-13s %s\n", "type", "id", "name", "samples", "rate", "native",
                "note", "loop", "seconds");
    for (const MacSound& s : sounds) {
        const std::string loop = s.loops() ? std::to_string(s.loop_start) + "-" + std::to_string(s.loop_end)
                                           : (s.header_valid ? "" : "(no header)");
        std::printf("%-4s %6d %-15s %7zu %8.0f %8.0f %4d %-13s %.2f\n", fourcc_string(s.type).c_str(), s.id,
                    s.name.c_str(), s.samples.size(), s.rate, s.native_rate, s.base_note, loop.c_str(), s.seconds());
    }
    for (const std::string& p : problems) std::printf("problem: %s\n", p.c_str());
    std::printf("\nuses (channel, rate, the game's time limit, DOS sound):\n");
    for (const MacSoundUse& u : mac_sound_uses()) {
        std::printf("  %-15s ch%d %6.0f Hz %6s  %-15s %s\n", u.sound, u.channel, u.rate,
                    u.max_seconds > 0 ? (std::to_string(u.max_seconds).substr(0, 5) + "s").c_str() : "held",
                    u.dos_sfx ? u.dos_sfx : "-", u.when);
    }
    if (wav_dir.empty()) return 0;
    std::error_code ec;
    fs::create_directories(wav_dir, ec);
    for (const MacSound& s : sounds) {
        const fs::path out = wav_dir / (safe_name(s.name.empty() ? std::to_string(s.id) : s.name) + ".wav");
        if (!write_file(out, wav(s.samples, s.bits, native ? s.native_rate : s.rate, s.loop_start, s.loop_end))) {
            std::fprintf(stderr, "can't write %s\n", out.string().c_str());
            return 1;
        }
    }
    std::printf("\nwrote %zu WAV files to %s\n", sounds.size(), wav_dir.string().c_str());
    return 0;
}

// The engine loop as the driver plays it: nearest-sample stepping at the pitched rate, mixed at 11127 Hz.
int cmd_engine(const MacFiles& files, double rpm, double seconds, const char* out) {
    const auto fork = find_vette_data(files);
    const Resource* r = fork ? fork->find(fourcc("INST"), "engine") : nullptr;
    const auto s = r ? decode_inst(fork->data(*r), r->id, r->name) : std::nullopt;
    if (!s || !s->loops()) {
        std::fprintf(stderr, "no engine sound found\n");
        return 1;
    }
    const double step = mac_engine_rate(rpm) / kMacMixHz;
    std::vector<std::int16_t> mix;
    double pos = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(seconds * kMacMixHz); ++i) {
        mix.push_back(s->samples[static_cast<std::size_t>(pos)]);
        pos += step;
        while (pos >= static_cast<double>(s->loop_end)) pos -= static_cast<double>(s->loop_end - s->loop_start);
    }
    std::printf("engine at %.0f rpm: sample rate %.0f Hz (step %.3f)\n", rpm, mac_engine_rate(rpm), step);
    return write_file(out, wav(mix, 8, kMacMixHz, 0, 0)) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    fs::path location;
    fs::path wav_dir;
    bool native = false;
    std::vector<std::string> rest;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--mac" && i + 1 < args.size()) {
            location = args[++i];
        } else if (args[i] == "--wav" && i + 1 < args.size()) {
            wav_dir = args[++i];
        } else if (args[i] == "--native") {
            native = true;
        } else if (args[i] == "-h" || args[i] == "--help") {
            return usage();
        } else {
            rest.push_back(args[i]);
        }
    }
    if (location.empty()) location = default_location();
    const MacFiles files = MacFiles::open(location);
    for (const std::string& n : files.notes()) std::printf("note: %s\n", n.c_str());
    if (files.empty()) {
        std::fprintf(stderr, "no Mac files found in %s\n", location.string().c_str());
        return 1;
    }
    const std::string cmd = rest.empty() ? "list" : rest[0];
    if (cmd == "list" && rest.size() <= 1) return cmd_list(files);
    if (cmd == "resources" && rest.size() == 2) return cmd_resources(files, rest[1].c_str());
    if (cmd == "dump" && rest.size() == 5)
        return cmd_dump(files, rest[1].c_str(), rest[2].c_str(), rest[3].c_str(), rest[4].c_str());
    if (cmd == "fork" && rest.size() == 4) return cmd_fork(files, rest[1].c_str(), rest[2], rest[3].c_str());
    if (cmd == "sounds" && rest.size() == 1) return cmd_sounds(files, wav_dir, native);
    if (cmd == "engine" && rest.size() == 4)
        return cmd_engine(files, std::atof(rest[1].c_str()), std::atof(rest[2].c_str()), rest[3].c_str());
    return usage();
}
