// The PC-98 release's files and FM music, from the player's copy (Game/PC98/: a disk image or the
// files themselves).
//
//   vette_pc98 [--game DIR] list               the image's files, then the game folder's
//   vette_pc98 [--game DIR] extract OUTDIR     copies the game folder's files out of the image
//   vette_pc98 [--game DIR] wav [OUTDIR]       renders each FM song to OUTDIR/pc98_<n>_<name>.wav
//
// wav options: --rate N (48000), --loops N (looping songs: passes before the fade-out, default 1),
// --max-seconds N (300). It prints each song's length, peak and RMS level.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "assets/pc98_disk.h"
#include "sound/pc98_sound.h"

namespace {

namespace fs = std::filesystem;
using vette::assets::Pc98Files;
using vette::sound::Pc98Song;
using vette::sound::Pc98Sound;

void write_u16(std::ofstream& f, uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); }
void write_u32(std::ofstream& f, uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); }

bool save_wav(const fs::path& path, const std::vector<float>& samples, int rate) {
    std::ofstream f(path, std::ios::binary);
    const auto bytes = static_cast<uint32_t>(samples.size() * 2);
    f.write("RIFF", 4);
    write_u32(f, 36 + bytes);
    f.write("WAVEfmt ", 8);
    write_u32(f, 16);
    write_u16(f, 1);  // PCM
    write_u16(f, 1);  // mono
    write_u32(f, static_cast<uint32_t>(rate));
    write_u32(f, static_cast<uint32_t>(rate) * 2);
    write_u16(f, 2);
    write_u16(f, 16);
    f.write("data", 4);
    write_u32(f, bytes);
    for (const float s : samples) {
        const auto v = static_cast<int16_t>(std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f));
        write_u16(f, static_cast<uint16_t>(v));
    }
    return static_cast<bool>(f);
}

int usage() {
    std::fprintf(stderr,
                 "usage: vette_pc98 [--game DIR] list | extract OUTDIR | wav [OUTDIR] [--rate N] [--loops N] "
                 "[--max-seconds N]\n");
    return 2;
}

int list(const Pc98Files& files) {
    std::printf("%s\n", files.description().c_str());
    if (!files.image_listing().empty()) {
        std::printf("\nimage:\n");
        for (const auto& e : files.image_listing()) {
            if (e.directory) {
                std::printf("  [%d] %-40s <dir>\n", e.volume, e.path.c_str());
            } else {
                std::printf("  [%d] %-40s %9u\n", e.volume, e.path.c_str(), e.size);
            }
        }
    }
    std::printf("\ngame folder (%zu files):\n", files.names().size());
    for (const std::string& name : files.names()) {
        const auto data = files.read(name);
        std::printf("  %-14s %9zu%s\n", name.c_str(), data ? data->size() : size_t{0}, data ? "" : "  (unreadable)");
    }
    return 0;
}

int extract(const Pc98Files& files, const fs::path& out) {
    std::error_code ec;
    fs::create_directories(out, ec);
    int written = 0;
    for (const std::string& name : files.names()) {
        const auto data = files.read(name);
        std::ofstream f(out / name, std::ios::binary);
        if (data && f.write(reinterpret_cast<const char*>(data->data()), static_cast<std::streamsize>(data->size()))) {
            ++written;
        } else {
            std::fprintf(stderr, "can't write %s\n", (out / name).string().c_str());
        }
    }
    std::printf("%d file(s) written to %s\n", written, out.string().c_str());
    return written == static_cast<int>(files.names().size()) ? 0 : 1;
}

int wav(const Pc98Files& files, const fs::path& out, int rate, int loops, int max_seconds) {
    std::string error;
    std::error_code ec;
    fs::create_directories(out, ec);
    for (int n = 1; n <= vette::sound::kPc98SongCount; ++n) {
        const auto song = static_cast<Pc98Song>(n);
        auto sound = Pc98Sound::create(files, rate, error);
        if (!sound) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        sound->play(song);
        // Songs that end: until they do, plus the release. Looping songs: `loops` passes, then the
        // driver's fade-out.
        std::vector<float> samples;
        const int block = rate / 100;
        const size_t limit = static_cast<size_t>(max_seconds) * static_cast<size_t>(rate);
        int tail = -1;
        double loop_seconds = 0;
        bool fading = false;
        while (samples.size() < limit && tail != 0) {
            const size_t at = samples.size();
            samples.resize(at + static_cast<size_t>(block));
            sound->render(samples.data() + at, block);
            if (!fading && sound->loops() >= loops && loops > 0) {
                loop_seconds = static_cast<double>(samples.size()) / rate;
                sound->fade_out(2.0);
                fading = true;
            }
            if (tail < 0 && !sound->playing()) {
                tail = 150;  // 1.5 s of release
            } else if (tail > 0) {
                --tail;
            }
        }
        float peak = 0;
        double sum = 0;
        for (const float s : samples) {
            peak = std::max(peak, std::fabs(s));
            sum += double{s} * s;
        }
        const fs::path path = out / ("pc98_" + std::to_string(n) + "_" + std::string(pc98_song_name(song)) + ".wav");
        if (!save_wav(path, samples, rate)) {
            std::fprintf(stderr, "can't write %s\n", path.string().c_str());
            return 1;
        }
        std::printf("song %d %-7s %6.1f s%s  peak %.3f  rms %.3f  -> %s\n", n, std::string(pc98_song_name(song)).c_str(),
                    static_cast<double>(samples.size()) / rate,
                    loop_seconds > 0 ? (" (loop " + std::to_string(loop_seconds).substr(0, 5) + " s)").c_str() : "",
                    static_cast<double>(peak), std::sqrt(sum / std::max<size_t>(samples.size(), 1)), path.string().c_str());
        if (sound->failed()) {
            std::fprintf(stderr, "  %s\n", sound->failure().c_str());
            return 1;
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    fs::path game = "Game/PC98";
    std::vector<std::string> args;
    int rate = 48000, loops = 1, max_seconds = 300;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        if (a == "--game" && has_value) {
            game = argv[++i];
        } else if (a == "--rate" && has_value) {
            rate = std::atoi(argv[++i]);
        } else if (a == "--loops" && has_value) {
            loops = std::atoi(argv[++i]);
        } else if (a == "--max-seconds" && has_value) {
            max_seconds = std::max(1, std::atoi(argv[++i]));
        } else {
            args.push_back(a);
        }
    }
    if (args.empty()) {
        return usage();
    }
    std::string error;
    std::optional<Pc98Files> files = fs::is_directory(game) ? Pc98Files::open(game, error) : Pc98Files::open_image(game, error);
    if (!files) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    if (args[0] == "list") {
        return list(*files);
    }
    if (args[0] == "extract" && args.size() == 2) {
        return extract(*files, args[1]);
    }
    if (args[0] == "wav") {
        return wav(*files, args.size() > 1 ? fs::path(args[1]) : fs::path("."), rate, loops, max_seconds);
    }
    return usage();
}
