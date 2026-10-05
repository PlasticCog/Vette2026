// The PC-98 FM music: the driver hosted on a scratch 286 with an emulated YM2203. A stand-in driver
// (hand-assembled, with the real one's entry-point signatures) checks the hosting: extraction from the
// EXE, the port routing, the timer IRQ running the handler at the chip's rate, a runaway driver.
// With the player's PC-98 files (Game/PC98/, or VETTE_PC98_DIR): the four songs' lengths, loops,
// stop and fade.

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "assets/pc98_disk.h"
#include "game/sound_events.h"
#include "host/machine.h"
#include "sound/pc98_backend.h"
#include "sound/pc98_sound.h"
#include "test.h"

using vette::assets::Pc98Files;
using vette::sound::Pc98Song;
using vette::sound::Pc98Sound;

namespace {

constexpr int kRate = 48000;
constexpr size_t kDriver = 0x0FFD * 16;  // the module's place in the 1.02J load image

struct Asm {
    std::vector<uint8_t>& image;
    size_t pc;  // offset within the driver module
    void db(std::initializer_list<int> bytes) {
        for (const int b : bytes) {
            image[kDriver + pc++] = static_cast<uint8_t>(b);
        }
    }
    void call(size_t target) {
        const auto rel = static_cast<uint16_t>(target - (pc + 3));
        db({0xE8, rel & 0xFF, rel >> 8});
    }
};

// An EXE with a stand-in driver: its init marks the board present; function 1 starts an SSG tone and
// timer B (C8h: 16.15 ms) and sets the "music active" count to 62; each timer B interrupt counts it
// down and stops the timer at 0, so the "song" lasts 62 ticks = 1.0015 s. Function 3 (stop) never
// returns.
std::vector<uint8_t> stand_in_exe() {
    std::vector<uint8_t> exe(0x20 + kDriver + 0x1CA5, 0);
    exe[0] = 'M';
    exe[1] = 'Z';
    exe[8] = 2;  // header paragraphs
    std::vector<uint8_t> image(exe.size() - 0x20, 0);

    // Song list: four entries whose lengths reach the code at 11B1h.
    size_t pos = 0x2B4;
    for (const int len : {0x20, 0x20, 0x20, 0x11B1 - 0x2B4 - 3 * 0x20}) {
        image[kDriver + pos] = static_cast<uint8_t>(len);
        image[kDriver + pos + 1] = static_cast<uint8_t>(len >> 8);
        image[kDriver + pos + 2] = image[kDriver + pos + 4] = image[kDriver + pos + 6] = 8;
        pos += static_cast<size_t>(len);
    }

    Asm a{image, 0x1C00};  // write register AL = AH
    a.db({0x52, 0xBA, 0x88, 0x01, 0xEE, 0x42, 0x42, 0x86, 0xC4, 0xEE, 0x86, 0xC4, 0x5A, 0xC3});
    a.pc = 0x1C9D;  // busy wait (signature)
    a.db({0x50, 0xEC, 0xD0, 0xE0, 0x72, 0xFB, 0x58, 0xC3});

    a.pc = 0x11B1;  // init: signature, then [2B1] = 1
    a.db({0x1E, 0x0E, 0x1F, 0xBA, 0x88, 0x01, 0xEC, 0xFE, 0xC0});
    a.db({0xC6, 0x06, 0xB1, 0x02, 0x01, 0x1F, 0xCB});

    a.pc = 0x1292;  // entry: signature, then functions 3 and 1
    a.db({0x1E, 0x06, 0x60, 0x8C, 0xCB, 0x8E, 0xDB, 0x8E, 0xC3, 0x80, 0x3E, 0xB1, 0x02, 0x00});
    a.db({0x80, 0xFC, 0x03, 0x75, 0x02, 0xEB, 0xFE});  // cmp ah,3 / jne / jmp $
    a.db({0x80, 0xFC, 0x01, 0x75, 0x29});              // cmp ah,1 / jne done
    a.db({0xC6, 0x06, 0xAF, 0x02, 62});                // [2AF] = 62
    for (const int reg_value : {0xFE00, 0x0001, 0x3E07, 0x0F08, 0xC826, 0x2A27}) {
        a.db({0xB8, reg_value & 0xFF, reg_value >> 8});  // tone A, timer B = C8h, load + enable B
        a.call(0x1C00);
    }
    a.db({0x61, 0x07, 0x1F, 0xCB});  // done: popa, pop es, pop ds, retf

    a.pc = 0x1671;  // timer interrupt: signature (reads status, tests timer B), count down
    a.db({0xFB, 0x60, 0x1E, 0x06, 0xFC, 0x0E, 0x1F, 0x0E, 0x07, 0xBA, 0x88, 0x01, 0xEC, 0xA8, 0x02});
    a.db({0x74, 0x14, 0xFE, 0x0E, 0xAF, 0x02, 0x75, 0x08});  // jz out / dec [2AF] / jnz more
    a.db({0xB8, 0x27, 0x30});                                // timers off, flags reset
    a.call(0x1C00);
    a.db({0xEB, 0x06});
    a.db({0xB8, 0x27, 0x2A});  // more: reset timer B's flag
    a.call(0x1C00);
    a.db({0x07, 0x1F, 0x61, 0xCF});  // out: pop es, pop ds, popa, iret

    a.pc = 0x1890;  // the track-end instruction the host watches (signature only)
    a.db({0x8B, 0x75, 0x0C, 0x23, 0xF6, 0x75, 0xF4, 0xBE, 0x67, 0x00});

    std::copy(image.begin(), image.end(), exe.begin() + 0x20);
    return exe;
}

float peak(const std::vector<float>& v) {
    float p = 0;
    for (const float s : v) {
        p = std::max(p, std::fabs(s));
    }
    return p;
}

// Renders until the song stops (or `max_s`); returns the seconds taken, at 1 ms resolution.
double seconds_until_stopped(Pc98Sound& s, double max_s, std::vector<float>* keep = nullptr) {
    std::vector<float> block(kRate / 1000);
    int ms = 0;
    while (s.playing() && ms < max_s * 1000) {
        s.render(block.data(), static_cast<int>(block.size()));
        if (keep) keep->insert(keep->end(), block.begin(), block.end());
        ++ms;
    }
    return ms / 1000.0;
}

std::string env(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&value, &len, name) == 0 && value) out = value;
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}

std::optional<Pc98Files> player_files() {
    std::filesystem::path folder = env("VETTE_PC98_DIR");
    if (folder.empty()) folder = std::filesystem::path(__FILE__).parent_path().parent_path() / "Game" / "PC98";
    std::string error;
    auto files = Pc98Files::open(folder, error);
    if (!files) {
        std::printf("  (skipped: no PC-98 files in %s)\n", folder.string().c_str());
    }
    return files;
}

}  // namespace

TEST(pc98_sound_stand_in_driver) {
    std::string error;
    auto s = Pc98Sound::create(stand_in_exe(), kRate, error);
    CHECK(s != nullptr);
    if (!s) {
        std::fprintf(stderr, "  (%s)\n", error.c_str());
        return;
    }
    std::vector<float> out(kRate / 10);
    s->render(out.data(), static_cast<int>(out.size()));
    CHECK(peak(out) < 1e-4f);  // nothing until asked
    CHECK(!s->playing());

    s->play(Pc98Song::Title);
    CHECK(s->playing());
    std::vector<float> played;
    const double t = seconds_until_stopped(*s, 3, &played);
    CHECK(std::fabs(t - 62 * 0.0161538) < 0.003);  // 62 timer B interrupts, each (256 - C8h) * 1152 clocks
    CHECK(peak(played) > 0.02f);                    // the tone, through ports 188h/18Ah
    CHECK(!s->failed());
}

TEST(pc98_sound_runaway_driver) {
    std::string error;
    auto s = Pc98Sound::create(stand_in_exe(), kRate, error);
    CHECK(s != nullptr);
    if (!s) return;
    s->play(static_cast<Pc98Song>(7));  // out of range: ignored
    CHECK(!s->playing());
    s->play(Pc98Song::Menu);
    std::vector<float> out(kRate / 10);
    s->render(out.data(), static_cast<int>(out.size()));
    CHECK(peak(out) > 0.02f);
    // The stand-in's stop loops forever: the host gives up, mutes the chip, and nothing hangs.
    s->stop();
    CHECK(s->failed() && !s->failure().empty());
    CHECK(!s->playing());
    s->play(Pc98Song::Menu);  // ignored from now on
    std::vector<float> after(kRate / 2);
    s->render(after.data(), static_cast<int>(after.size()));
    CHECK(peak(std::vector<float>(after.end() - kRate / 10, after.end())) < 1e-3f);
}

TEST(pc98_sound_rejects_other_exes) {
    std::string error;
    CHECK(Pc98Sound::create(std::vector<uint8_t>(100, 0), kRate, error) == nullptr);
    std::vector<uint8_t> exe = stand_in_exe();
    CHECK(Pc98Sound::create(std::span(exe).first(0x20 + kDriver + 0x1000), kRate, error) == nullptr);  // cut
    exe[0x20 + kDriver + 0x1671] = 0x90;  // not the interrupt handler
    CHECK(Pc98Sound::create(exe, kRate, error) == nullptr);
    CHECK(error.find("1.02J") != std::string::npos);
    exe = stand_in_exe();
    exe[0x20 + kDriver + 0x2B4] = 0x10;  // song list broken
    CHECK(Pc98Sound::create(exe, kRate, error) == nullptr);
    CHECK(Pc98Sound::create(stand_in_exe(), 100, error) == nullptr);  // output rate
    // The DOS EXE has no FM driver.
    const auto dos = std::filesystem::path(__FILE__).parent_path().parent_path() / "Game" / "VETTE.EXE";
    if (std::filesystem::exists(dos)) {
        std::ifstream f(dos, std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(Pc98Sound::create(bytes, kRate, error) == nullptr);
    }
}

TEST(pc98_sound_dos_mapping) {
    using vette::game::Sfx;
    CHECK(Pc98Sound::song_for(Sfx::TitleTune) == Pc98Song::Title);
    CHECK(Pc98Sound::song_for(Sfx::WinTune) == Pc98Song::Winner);
    for (const Sfx beeper : {Sfx::Engine, Sfx::Skid, Sfx::Siren, Sfx::Crash, Sfx::GearGrind}) {
        CHECK(!Pc98Sound::song_for(beeper).has_value());  // in-game sounds were the PC-98's beeper
    }
    CHECK(vette::sound::pc98_song_name(Pc98Song::Menu) == "menu");
    std::string error;
    auto s = Pc98Sound::create(stand_in_exe(), kRate, error);
    CHECK(s != nullptr);
    if (!s) return;
    CHECK(!s->start(Sfx::Skid) && !s->playing());
    CHECK(s->start(Sfx::WinTune) && s->playing());
    s->stop(Sfx::TitleTune);  // not the one playing
    CHECK(s->playing());
}

TEST(pc98_sound_songs) {
    const auto files = player_files();
    if (!files) return;
    std::string error;
    auto s = Pc98Sound::create(*files, kRate, error);
    CHECK(s != nullptr);
    if (!s) {
        std::fprintf(stderr, "  (%s)\n", error.c_str());
        return;
    }
    // The songs that end: lengths from the data (ticks of 16.15 ms at tempo C8h).
    s->play(Pc98Song::Winner);
    std::vector<float> winner;
    CHECK(std::fabs(seconds_until_stopped(*s, 20, &winner) - 384 * 0.0161538) < 0.03);
    CHECK(peak(winner) > 0.05f && peak(winner) < 1.0f);
    CHECK_EQ(s->loops(), 0);
    s->play(Pc98Song::Loser);
    CHECK(std::fabs(seconds_until_stopped(*s, 20) - 288 * 0.0161538) < 0.03);

    // The menu loops after 1656 ticks of 19.04 ms (tempo BEh), then every 1536.
    s->play(Pc98Song::Menu);
    std::vector<float> block(kRate / 100);
    int cs = 0;
    while (s->loops() == 0 && cs < 4000) {
        s->render(block.data(), static_cast<int>(block.size()));
        ++cs;
    }
    CHECK(std::abs(cs - 3153) <= 3);  // 31.53 s
    CHECK(s->playing());

    // Stop: silent after the release.
    s->stop();
    CHECK(!s->playing());
    std::vector<float> tail(kRate * 2);
    s->render(tail.data(), static_cast<int>(tail.size()));
    CHECK(peak(std::vector<float>(tail.end() - kRate / 10, tail.end())) < 0.002f);

    // A fade-out ends the song and stays quiet; the next song starts at full level.
    s->play(Pc98Song::Title);
    std::vector<float> title(kRate);
    s->render(title.data(), static_cast<int>(title.size()));
    CHECK(peak(title) > 0.05f);
    s->fade_out(0.5);
    CHECK(std::fabs(seconds_until_stopped(*s, 10) - 0.5) < 0.002);
    s->render(title.data(), static_cast<int>(title.size()));
    CHECK(peak(title) == 0.0f);
    s->play(Pc98Song::Title);
    s->render(title.data(), static_cast<int>(title.size()));
    CHECK(peak(title) > 0.05f);
    CHECK(!s->failed());

    // Deterministic: a second instance renders the same samples.
    auto s2 = Pc98Sound::create(*files, kRate, error);
    auto s3 = Pc98Sound::create(*files, kRate, error);
    CHECK(s2 && s3);
    if (s2 && s3) {
        s2->play(Pc98Song::Menu);
        s3->play(Pc98Song::Menu);
        std::vector<float> a(kRate), b(kRate);
        s2->render(a.data(), kRate);
        s3->render(b.data(), kRate);
        CHECK(a == b);
    }
}

TEST(pc98_sound_backend) {
    // As GameAudio's backend: it covers the two tunes with FM versions and adds its output.
    vette::host::MachineConfig config;
    config.game_dir = "no-game-here";
    vette::host::Machine machine(config);  // not booted: the cues never fire
    std::string error;
    auto sound = Pc98Sound::create(stand_in_exe(), kRate, error);
    CHECK(sound != nullptr);
    if (!sound) return;
    Pc98Sound* s = sound.get();
    vette::sound::Pc98Backend backend(machine, std::move(sound));
    CHECK(backend.covers("title_tune") && backend.covers("win_tune"));
    CHECK(!backend.covers("skid") && !backend.covers("engine") && !backend.covers("nonsense"));
    CHECK(!backend.playing("win_tune"));
    s->play(Pc98Song::Winner);
    CHECK(backend.playing("win_tune") && !backend.playing("title_tune"));
    std::vector<float> out(kRate / 10, 0.25f), alone(kRate / 10);
    auto twin = Pc98Sound::create(stand_in_exe(), kRate, error);
    twin->play(Pc98Song::Winner);
    backend.render(out.data(), static_cast<int>(out.size()));
    twin->render(alone.data(), static_cast<int>(alone.size()));
    bool added = true;
    for (size_t i = 0; i < out.size(); ++i) {
        added = added && std::fabs(out[i] - (0.25f + alone[i])) < 1e-6f;
    }
    CHECK(added);
}
