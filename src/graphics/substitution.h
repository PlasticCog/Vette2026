#pragma once
// Graphics option: the PC-98 or Mac version's art in place of the DOS screens.
//
// The DOS game keeps running unchanged; this layer only looks at the frame it displays. Each frame
// is compared with the DOS pictures the game draws its screens from (decoded from the player's DOS
// files): a full-screen picture (title, garage, high scores, ...) or an inset (the race dashboard,
// the crash and loser pictures, found wherever the game put them). Where a picture is recognised,
// the replacement art goes in its place at the output's resolution, and every pixel where the
// frame differs from the DOS picture (text, highlight bars, sprites, gauges, the mouse pointer) is
// kept on top. Where the replacement's layout differs from the DOS one (the Mac screens), DOS
// regions can be moved into the replacement's matching panels. See re/notes/10-graphics.md.
//
// compose() is cheap (a sampled comparison plus one pass over the frame) and has no effect on the
// game, so Classic mode's 1:1 behaviour is untouched.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "graphics/art_files.h"
#include "graphics/composite.h"

namespace vette::host {
class Machine;
}

namespace vette::graphics {

enum class Art { Dos, Pc98, Mac };
const char* art_name(Art art);

// The DOS pictures this layer recognises.
enum class Screen {
    None,
    Title,       // TITLE.BIN
    Garage,      // GARAGE.BIN
    Opponents,   // EGAPIC.BIN (opponent selection, 320x200)
    HighScores,  // HIGHSC.BIN
    Winner,      // WINNER.BIN
    Dash,        // the race dashboard (in VETTE.EXE)
    Crash0,      // CRASH0.BIN (insets in the race view, found wherever they are drawn)
    Crash1,      // CRASH1.BIN
    Loser0,      // LOSER0-3.BIN
    Loser1,
    Loser2,
    Loser3,
    CourseMap,   // MAPPIC.BIN (the course selection map)
    DashLeft,    // the race dashboard looking left (F1) and right (F3), also in VETTE.EXE
    DashRight,
    TitleLogo,      // the title's sprites: BIGVET.BIN (the VETTE! logo), SPETRUM.BIN ("presents"),
    TitlePresents,  // VX.BIN (the car coming up the road, five frames)
    TitleCar,
    Ticket,   // the police stop (TICKET.BIN): the ticket with the offences checked,
    Officer,  // the officer letting you off ("OK, just don't let it happen again"),
    Excuses,  // the excuse list the game draws over the dashboard (no picture: two boxes),
    Penalty,  // PENALTY.BIN: the tickets' penalty time, over the high scores
    Count
};
const char* screen_name(Screen screen);

// A displayed frame: palette indices (kTransparent allowed) and its 0xRRGGBB palette.
struct FrameView {
    const std::uint8_t* pixels = nullptr;
    int width = 0, height = 0;  // 640x200 or 320x200
    const std::array<std::uint32_t, 16>* palette = nullptr;
};

struct SubstitutionOptions {
    // The PC-98 pictures with Japanese text baked in (the loser pictures' speech bubbles) show the DOS
    // picture's English there instead.
    bool english_text = true;
};

class Substitution {
public:
    // Loads the art set (decoding everything it needs up front). Art::Dos loads nothing and compose()
    // always returns false. Missing files only disable the screens that need them (see warnings()).
    Substitution(Art art, const ArtFiles& files, SubstitutionOptions options = {});
    ~Substitution();
    Substitution(const Substitution&) = delete;
    Substitution& operator=(const Substitution&) = delete;

    Art art() const;
    // Screens with replacement art in this set.
    std::vector<Screen> available() const;
    const std::vector<std::string>& warnings() const;

    // The running game's memory (Memory::ram(), 1 MB): the race dashboards are packed into VETTE.EXE
    // and read from there (lazily, once the EXEPACK stub has unpacked the program), and the Mac
    // dashboard and course map draw the game's values (speed, revs, gear, the course on show) from
    // it every frame. Without it those screens aren't replaced.
    void set_program_memory(const std::uint8_t* ram);

    // Attaches to the running game: set_program_memory() plus watches on the game's text and sprite
    // drawing (draw_tracker.h), so what it draws over a replaced picture stays exactly on top, letters
    // of the picture's own colour included. Without it, that is inferred from colour differences.
    // The machine must outlive this object (the watches are removed in its destructor).
    void attach(host::Machine& machine);
    // Diagnostics: what the tracker sees (vette_gfx --trace-draw). After attach().
    void set_draw_log(std::function<void(const std::string&)> log);

    // Builds the composite for a displayed frame. Returns false, leaving `out` alone, when the frame
    // should be shown as it is. The composite's images stay valid while this object lives.
    bool compose(const FrameView& frame, Composite& out);

    // What the last compose() recognised, for diagnostics: the screens found and their match ratios.
    struct Found {
        Screen screen;
        float match;
        IRect rect;  // where the DOS picture is, in frame pixels
    };
    const std::vector<Found>& found() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vette::graphics
