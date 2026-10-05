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
#include <memory>
#include <string>
#include <vector>

#include "graphics/art_files.h"
#include "graphics/composite.h"

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

    // The race dashboard is packed into VETTE.EXE, so it is read from the running program: the
    // emulator's memory (Memory::ram(), 1 MB). Read lazily when a race frame first appears (the
    // EXEPACK stub has unpacked the program by then). Without it the dashboard isn't replaced.
    void set_program_memory(const std::uint8_t* ram);

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
