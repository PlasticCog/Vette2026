#pragma once
// Exactly what the DOS game drew over its pictures: text and sprites, tracked from its drawing
// routines rather than inferred from colour differences (a letter the colour of the picture under
// it doesn't differ from the picture, yet it is the game's drawing and must stay on top of art that
// isn't that colour there).
//
// CPU watches on the game's routines keep a shadow of video memory (A0000h-AFFFFh, every page):
// per pixel, the colour of the text (or solid rectangle) the game drew there, or "a sprite pixel",
// or nothing. The text routines' glyphs are drawn into it from the game's own fonts; masked sprites
// mark the pixels their mask pass writes; solid rectangles (the course map's text panels) set their
// colour; unpacking a picture or erasing a run of bytes clears it; page copies copy it; XOR
// highlight bars change its colours; a mode set clears all of it. While the title's transition
// brings the new screen in, what it hasn't brought in yet counts as drawn. A shadow pixel counts
// only while the frame still shows its colour there, so text the game erased some other way never
// lingers.
// Routines and their registers: re/notes/10-graphics.md.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "host/machine.h"

namespace vette::graphics {

class DrawTracker {
public:
    static constexpr std::uint8_t kNone = 0xFF;    // nothing tracked
    static constexpr std::uint8_t kSprite = 0xFE;  // a sprite pixel: whatever colour the frame shows

    // Adds the watches; they are removed again when the tracker is destroyed (the machine must outlive it).
    explicit DrawTracker(host::Machine& machine);
    ~DrawTracker();
    DrawTracker(const DrawTracker&) = delete;
    DrawTracker& operator=(const DrawTracker&) = delete;

    // The tracked pixels of the page on display, for a frame of width x height (640 or 320 x 200):
    // `pixels` gets a colour, kSprite or kNone per frame pixel, kept only where `frame` shows that
    // colour; `cells` is nonzero where a text character's cell is (for keeping whole cells readable):
    // the pixel's row in its cell from 1, plus the cell's height - 1 times 16.
    // Returns false when nothing is tracked on that page.
    bool overlay(const std::uint8_t* frame, int width, int height, std::vector<std::uint8_t>& pixels,
                 std::vector<std::uint8_t>& cells) const;

    // Diagnostics: one line per tracked call (vette_gfx --trace-draw).
    void set_log(std::function<void(const std::string&)> log) { log_ = std::move(log); }

private:
    struct Rect {
        std::uint32_t at;  // byte address in video memory (0..FFFFh)
        int bytes, rows, stride;
    };
    void clear(const Rect& r, const char* what);
    void fill(const Rect& r, std::uint8_t colour);
    void copy(const Rect& from, const Rect& to, bool logged);
    void xor_colour(const Rect& r, std::uint8_t value);
    void glyph(std::uint32_t at, int stride, const std::uint8_t* rows, int count, std::uint8_t colour);
    void text_88eb(host::Cpu& cpu);
    void text_f3e1(host::Cpu& cpu);
    void text_5d55(host::Cpu& cpu);
    void sprite_mask(host::Cpu& cpu);
    void log(const std::string& line) const;

    host::Machine& machine_;
    std::vector<host::Cpu::WatchId> watches_;
    std::vector<std::uint8_t> shadow_;  // 64K bytes x 8 pixels
    std::vector<std::uint8_t> cells_;   // 64K bytes: in a text character's cell (row from 1 | (height-1) << 4)
    std::vector<std::uint8_t> pages_;   // anything tracked at all (a quick "nothing to do")
    // The screen transition in progress: its page (a video memory byte address; kVram: none) and,
    // per byte of it, whether the dissolve has brought it in yet.
    std::uint32_t transition_ = 0x10000;
    std::vector<std::uint8_t> revealed_;
    std::function<void(const std::string&)> log_;
};

}  // namespace vette::graphics
