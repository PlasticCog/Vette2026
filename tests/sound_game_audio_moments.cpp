// GameAudio on the real game (needs Game/VETTE.EXE; skipped without it): the moments the DOS game passes
// in silence reach the replacement (the countdown, the horn, the helicopter view, the title's cues),
// the helicopter's rotor replaces the engine, and with the PC speaker alone they change nothing.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

#include "game/options.h"
#include "game/sound_events.h"
#include "host/machine.h"
#include "sound/game_audio.h"
#include "test.h"

namespace fs = std::filesystem;
using vette::host::Machine;
using vette::sound::GameAudio;
using vette::sound::SfxBackend;

namespace {

// A replacement that only takes notes.
class Recorder final : public SfxBackend {
public:
    explicit Recorder(const Machine& m) : machine_(m) {}
    void start(std::string_view id, const vette::sound::SpeakerProgram*) override {
        starts[std::string(id)].push_back(now());
        playing_[std::string(id)] = true;
    }
    void stop(std::string_view id) override {
        stops[std::string(id)].push_back(now());
        playing_[std::string(id)] = false;
    }
    bool playing(std::string_view id) const override {
        const auto it = playing_.find(std::string(id));
        return it != playing_.end() && it->second;
    }
    bool covers(std::string_view) const override { return true; }
    void engine(const Engine& e) override { engine_on.push_back({now(), e.on}); }
    void render(float*, int) override {}

    std::map<std::string, std::vector<double>> starts, stops;
    std::vector<std::pair<double, bool>> engine_on;  // each call: when, on

private:
    double now() const { return static_cast<double>(machine_.emulated_ns()) / 1e9; }
    const Machine& machine_;
    std::map<std::string, bool> playing_;
};

std::unique_ptr<Machine> boot(const fs::path& dir) {
    vette::host::MachineConfig config;
    config.game_dir = dir;
    config.save_dir = fs::temp_directory_path() / "vette2026_sound_test";
    config.start_time = vette::host::RealTime{1989, 10, 23, 12, 0, 0, 0};
    auto m = std::make_unique<Machine>(config);
    std::string error;
    if (!m->boot(error)) {
        return nullptr;
    }
    vette::game::install_skip_manual_check(m->cpu());
    return m;
}

// The README's race, then the helicopter view (F4) at 40 s, ahead (F2) at 42 s, the horn at 43-44 s.
void run(Machine& m, GameAudio& audio, double seconds, std::vector<int16_t>* speaker_all = nullptr,
         std::vector<float>* out_all = nullptr) {
    struct Key {
        double at;
        uint8_t sc;
    };
    std::vector<Key> keys;
    const auto tap = [&](double t, uint8_t sc) {
        keys.push_back({t, sc});
        keys.push_back({t + 0.1, static_cast<uint8_t>(sc | 0x80)});
    };
    tap(13, 0x39);
    for (const double t : {17.0, 21.0, 25.0, 30.0}) tap(t, 0x1C);
    tap(37, 0x1E);
    tap(37.3, 0x02);
    keys.push_back({37.5, 0x48});
    keys.push_back({38.5, 0x4D});
    keys.push_back({39.3, 0xCD});
    tap(40, 0x3E);
    tap(42, 0x3C);
    keys.push_back({43, vette::game::kHornScancode});
    keys.push_back({44, static_cast<uint8_t>(vette::game::kHornScancode | 0x80)});
    keys.push_back({45.5, 0xC8});
    std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.at < b.at; });
    size_t next = 0;
    std::vector<int16_t> speaker;
    std::vector<float> out;
    for (int ms = 0; ms < static_cast<int>(seconds * 1000) && !m.stopped(); ++ms) {
        while (next < keys.size() && keys[next].at * 1000 <= ms) m.key(keys[next++].sc);
        const uint64_t t0 = m.emulated_ns();
        m.run_for(1'000'000);
        speaker.clear();
        m.take_audio(speaker);
        audio.render(t0, speaker, out);
        if (speaker_all) speaker_all->insert(speaker_all->end(), speaker.begin(), speaker.end());
        if (out_all) out_all->insert(out_all->end(), out.begin(), out.end());
    }
}

fs::path game_dir() { return fs::path(__FILE__).parent_path().parent_path() / "Game"; }

}  // namespace

TEST(sound_game_audio_silent_moments) {
    if (!fs::exists(game_dir() / "VETTE.EXE")) {
        std::printf("  SKIPPED: no VETTE.EXE in %s\n", game_dir().string().c_str());
        return;
    }
    auto m = boot(game_dir());
    CHECK(m != nullptr);
    if (!m) return;
    Recorder rec(*m);
    GameAudio::Sources src;
    src.effects = &rec;
    GameAudio audio(*m, 48000, src);
    run(*m, audio, 45);

    const auto count = [&](const char* id) { return rec.starts[id].size(); };
    CHECK_EQ(count("intro_cable_car"), size_t{1});
    CHECK_EQ(count("intro_car"), size_t{1});
    CHECK_EQ(count("intro_logo"), size_t{1});
    CHECK_EQ(count("countdown_beep"), size_t{2});
    CHECK_EQ(count("countdown_go"), size_t{1});
    // Stopped once in the race (GameAudio also stops everything once at boot, while the game is silent).
    const auto race_stops = [&](const char* id) {
        return std::count_if(rec.stops[id].begin(), rec.stops[id].end(), [](double t) { return t > 30; });
    };
    CHECK_EQ(count("helicopter"), size_t{1});
    CHECK_EQ(race_stops("helicopter"), 1);
    CHECK_EQ(count("horn"), size_t{1});
    CHECK_EQ(race_stops("horn"), 1);
    if (count("horn") == 1 && count("helicopter") == 1 && !rec.stops["horn"].empty() &&
        !rec.stops["helicopter"].empty()) {
        const double horn_on = rec.starts["horn"][0], horn_off = rec.stops["horn"].back();
        CHECK(horn_on > 42.9 && horn_on < 43.1 && horn_off > 43.9 && horn_off < 44.1);
        // The rotor replaces the engine while the view lasts.
        const double heli_on = rec.starts["helicopter"][0], heli_off = rec.stops["helicopter"].back();
        CHECK(heli_on > 40 && heli_on < 40.4 && heli_off > 42 && heli_off < 42.4);
        bool before = false, during = false, after = false;
        for (const auto& [t, on] : rec.engine_on) {
            if (t > 38 && t < heli_on - 0.01) before = before || on;
            if (t > heli_on + 0.01 && t < heli_off - 0.01) during = during || on;
            if (t > heli_off + 0.2 && t < 43) after = after || on;
        }
        CHECK(before && !during && after);
    }
}

TEST(sound_game_audio_speaker_unchanged_by_moments) {
    if (!fs::exists(game_dir() / "VETTE.EXE")) {
        std::printf("  SKIPPED: no VETTE.EXE in %s\n", game_dir().string().c_str());
        return;
    }
    auto m = boot(game_dir());
    CHECK(m != nullptr);
    if (!m) return;
    GameAudio audio(*m, 48000, GameAudio::Sources{});  // the PC speaker only
    std::vector<int16_t> speaker;
    std::vector<float> out;
    run(*m, audio, 45, &speaker, &out);
    CHECK_EQ(speaker.size(), out.size());
    size_t differ = 0;
    for (size_t i = 0; i < std::min(speaker.size(), out.size()); ++i) {
        differ += out[i] != speaker[i] / 32768.0f;
    }
    CHECK_EQ(differ, size_t{0});
}
