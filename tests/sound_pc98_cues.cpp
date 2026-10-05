// The PC-98 music cues on the hosted DOS game (needs Game/VETTE.EXE; skipped without it): the title
// song starts with the title sequence, gives way to the menu song, and the menu song stops when the
// race begins, as on a PC-98. With the PC-98 files too, the cued songs are heard.

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "assets/pc98_disk.h"
#include "game/options.h"
#include "host/machine.h"
#include "sound/pc98_cues.h"
#include "test.h"

using vette::sound::Pc98Cue;
using vette::sound::Pc98MusicCues;
using vette::sound::Pc98Song;
using vette::sound::Pc98Sound;

namespace {

std::filesystem::path repo() { return std::filesystem::path(__FILE__).parent_path().parent_path(); }

std::string show(const std::vector<Pc98Cue>& cues) {
    std::string s;
    for (const Pc98Cue& c : cues) {
        char buf[64];
        std::snprintf(buf, sizeof buf, " %.2fs:%s", static_cast<double>(c.t_ns) / 1e9,
                      c.song ? std::string(vette::sound::pc98_song_name(*c.song)).c_str() : "stop");
        s += buf;
    }
    return s;
}

}  // namespace

TEST(pc98_cues_on_the_dos_game) {
    const auto game = repo() / "Game";
    if (!std::filesystem::exists(game / "VETTE.EXE")) {
        std::printf("  (skipped: no DOS game in %s)\n", game.string().c_str());
        return;
    }
    vette::host::MachineConfig config;
    config.game_dir = game;
    config.start_time = vette::host::RealTime{};
    vette::host::Machine machine(config);
    std::string error;
    CHECK(machine.boot(error));
    if (!error.empty()) return;
    vette::game::install_skip_manual_check(machine.cpu());
    vette::game::install_idle_skip(machine);
    Pc98MusicCues cues(machine);

    std::string pc98_error;
    const auto files = vette::assets::Pc98Files::open(game / "PC98", pc98_error);
    auto sound = files ? Pc98Sound::create(*files, 48000, pc98_error) : nullptr;

    // The README's key script up to the race: Space at 13 s, Enter at 17, 21, 25 and 30 s.
    struct Key {
        int at_ds;
        uint8_t scancode;
    };
    const Key keys[] = {{130, 0x39}, {170, 0x1C}, {210, 0x1C}, {250, 0x1C}, {300, 0x1C}};
    std::vector<Pc98Cue> all;
    std::vector<float> audio(4800);
    float title_peak = 0;
    for (int ds = 0; ds < 450 && !machine.stopped(); ++ds) {
        for (const Key& k : keys) {
            if (k.at_ds == ds) {
                machine.key(k.scancode);
            } else if (k.at_ds + 1 == ds) {
                machine.key(static_cast<uint8_t>(k.scancode | 0x80));  // released 100 ms later
            }
        }
        machine.run_for(100'000'000);
        std::vector<Pc98Cue> got;
        cues.take(got);
        all.insert(all.end(), got.begin(), got.end());
        if (sound) {
            for (const Pc98Cue& c : got) {
                c.song ? sound->play(*c.song) : sound->stop();
            }
            sound->render(audio.data(), static_cast<int>(audio.size()));
            if (!all.empty() && all.back().song == Pc98Song::Title) {
                for (const float v : audio) title_peak = std::max(title_peak, std::abs(v));
            }
        }
    }
    std::printf("  cues:%s\n", show(all).c_str());
    // Title, then (the sequence over) stop and the menu, then stop for the race.
    CHECK(all.size() >= 4);
    if (all.size() >= 4) {
        CHECK(all[0].song == Pc98Song::Title);
        CHECK(!all[1].song.has_value());
        CHECK(all[2].song == Pc98Song::Menu);
        CHECK(!all.back().song.has_value());
    }
    if (sound) {
        CHECK(title_peak > 0.05f);
    }
}
