#pragma once
// The PC-98 version's FM music (VETTE! 1.02J, "Sound by H.Nagata"), played by its own YM2203 driver:
// the driver module is taken from the player's VETTE.EXE and run on a scratch 286 whose ports
// 188h/18Ah reach an emulated YM2203 (FmChip). The chip's timer IRQ runs the driver's interrupt
// handler, as on a PC-9801-26K board, so tempo and envelopes are the original's.
//
// The driver holds four songs and nothing else: the PC-98's in-game sounds (engine, siren, beeps,
// crash) are beeper versions of the DOS PC-speaker ones, not FM. Two songs replace DOS speaker tunes
// (title, winner), two play where DOS is silent (menu, loser). See re/notes/09-pc98.md.
// Not thread-safe: call everything from one thread.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace vette::assets {
class Pc98Files;
}
namespace vette::game {
enum class Sfx : uint8_t;  // game/sound_events.h
}

namespace vette::sound {

enum class Pc98Song : uint8_t {
    Title = 1,   // the opening (TITLE.PIC, VX.PIC, the rider animation); loops
    Winner = 2,  // WINNER.PIC after a won race; ends
    Loser = 3,   // LOSER0-3 / CRASH0-1 / TICKET.PIC after a lost race; ends
    Menu = 4,    // the pre-race menus: garage (car), skill level, opponent; loops
};
constexpr int kPc98SongCount = 4;

std::string_view pc98_song_name(Pc98Song song);  // "title", "winner", "loser", "menu"

class Pc98Sound {
public:
    // Reads VETTE.EXE from the PC-98 files. Null with `error` set if it isn't the 1.02J driver.
    static std::unique_ptr<Pc98Sound> create(const assets::Pc98Files& files, int output_rate, std::string& error);
    static std::unique_ptr<Pc98Sound> create(std::span<const uint8_t> vette_exe, int output_rate, std::string& error);
    ~Pc98Sound();
    Pc98Sound(const Pc98Sound&) = delete;
    Pc98Sound& operator=(const Pc98Sound&) = delete;

    // Starts a song from the beginning, replacing the current one (as the game does).
    void play(Pc98Song song);
    void stop();

    // The DOS sounds with an FM version on the PC-98: TitleTune (the Title song) and WinTune (Winner).
    // Every other sound was the beeper there, so the DOS sound is the one to play. Note the songs run
    // longer than the tunes they replace: the DOS title tune ends by itself after about 9 s, the PC-98
    // song plays until the title sequence ends. Pc98MusicCues (sound/pc98_cues.h) follows the PC-98's
    // own cues, including the menu and loser songs where DOS is silent.
    static std::optional<Pc98Song> song_for(game::Sfx sfx);
    bool start(game::Sfx sfx);  // plays its song; false if it has none
    void stop(game::Sfx sfx);   // stops its song if that's the one playing
    // Fades the music out over `seconds`, then stops it. (The driver has a fade of its own, which the
    // game never used; it miscounts the tracks still playing once one has faded, so it isn't used.)
    void fade_out(double seconds = 2.0);

    // A song is playing: false after stop() and once a song without a loop has ended.
    bool playing() const;
    std::optional<Pc98Song> current() const { return current_; }  // the last one started, until stop()
    // How many times the current song has gone round its loop (0 for songs that end).
    int loops() const { return loops_; }

    // Mono samples at the output rate, nominally -1..1.
    void render(float* out, int frames);

    // The driver stopped responding (it never returned to the host); it stays silent after that.
    bool failed() const { return failed_; }
    const std::string& failure() const { return failure_; }

private:
    struct Machine;
    explicit Pc98Sound(std::unique_ptr<Machine> m);
    bool call(uint16_t ax, uint16_t dx = 0);  // the driver's far entry point (function AH, argument AL)
    bool run(uint16_t entry, bool interrupt);
    void track_end();

    std::unique_ptr<Machine> m_;
    std::optional<Pc98Song> current_;
    int loops_ = 0;
    std::array<int, 6> track_loops_{};
    float fade_gain_ = 1, fade_step_ = 0;  // fade_out(): stays silent at 0 until the next play()
    bool failed_ = false;
    std::string failure_;
};

}  // namespace vette::sound
