#pragma once
// The PC-98 version's music cues on the running DOS game. The PC-98 VETTE.EXE calls its FM driver at
// four screens; the DOS code has the same routines at parallel addresses (the codebases are shared,
// re/notes/09-pc98.md), so watches there report "play song n" and "stop" at the moments a PC-98 did:
//
//   title sequence    DOS 3009:C560 play Title (where DOS plays its title tune), 3009:C61B stop
//   pre-race menus    DOS 3009:0071 play Menu (DOS: silent) for the garage, skill and opponent screens,
//                     3009:00B7 stop (on to the course map and race), 3009:062F stop (quit)
//   lost race screen  DOS 3009:CA10 play Loser (DOS: silent), 3009:CA27 stop (key pressed)
//   won race screen   DOS 3009:CEBD play Winner (where DOS plays its winner tune), 3009:CEC3 stop
//
// As on the PC-98, the music ignores the game's sound switch (S key); the caller may honour it. An
// observer: the game is unchanged. Each watch checks the code it sits on, so nothing fires before the
// game is unpacked or with another VETTE.EXE.

#include <cstdint>
#include <optional>
#include <vector>

#include "host/cpu.h"
#include "sound/pc98_sound.h"

namespace vette::host {
class Machine;
}

namespace vette::sound {

struct Pc98Cue {
    uint64_t t_ns;                // emulated time (Machine::emulated_ns)
    std::optional<Pc98Song> song;  // empty: stop the music
};

class Pc98MusicCues {
public:
    explicit Pc98MusicCues(host::Machine& machine);
    ~Pc98MusicCues();
    Pc98MusicCues(const Pc98MusicCues&) = delete;
    Pc98MusicCues& operator=(const Pc98MusicCues&) = delete;

    // Appends the cues since the last call, in order.
    void take(std::vector<Pc98Cue>& out);
    // Plays or stops the cued songs on `sound`.
    void apply(Pc98Sound& sound);

private:
    host::Machine& machine_;
    std::vector<host::Cpu::WatchId> watches_;
    std::vector<Pc98Cue> cues_;
};

}  // namespace vette::sound
