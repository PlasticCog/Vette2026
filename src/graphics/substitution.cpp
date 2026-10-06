#include "graphics/substitution.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <utility>

#include "assets/pc98_pic.h"
#include "assets/pict.h"
#include "graphics/dos_art.h"
#include "graphics/draw_tracker.h"
#include "graphics/game_state.h"
#include "graphics/screen_handler.h"

namespace vette::graphics {

namespace {

// --- The screen table ---------------------------------------------------------------------------

enum class Fit {
    Stretch,      // fill the target rectangle
    Contain,      // keep the art's aspect ratio, as large as fits, centred (letterboxed)
    Cover,        // keep the aspect ratio, cover the target, centred (overflowing it)
    BottomWidth,  // keep the aspect ratio, the target's width, standing on its bottom edge
    Native,       // sprites: at the screen's art's scale, centred on the DOS sprite (ArtSpec::bottom:
                  // standing on its bottom edge)
};

enum class Handler { None, MacDash, MacMap };

// A DOS region moved to where the replacement layout has it: `dos` in frame pixels, `art` in the
// replacement image's pixels. Opaque moves the whole rectangle; otherwise only the pixels that
// differ from the DOS picture (the dynamic content).
struct Remap {
    IRect dos;
    IRect art;
    bool opaque;
    int ignore = -1;         // a colour left behind (e.g. DOS grid lines where the art has its own grid)
    std::uint16_t only = 0;  // nonzero: only these colours (bit per palette index) move
};

// A highlight drawn on the replacement when a DOS region shows a colour: e.g. the selected car
// in the garage's menu bar (red background) frames the matching Mac button.
struct Indicator {
    IRect dos;
    int color;
    int min_pixels;
    IRect art;
    std::uint32_t rgb;
    int pict;  // nonzero: this Mac picture at art's top left instead of a frame (a check mark)
};

// One step in building a Mac art picture from several: a PICT drawn at (x, y), or a filled rectangle.
struct Paint {
    int pict;
    int x, y;
    IRect fill;
    std::uint32_t argb;
};

struct ArtSpec {
    const char* pc98 = nullptr;  // PC-98 file
    int pict = 0;                // Mac Color VETTE! PICT id
    Fit fit = Fit::Stretch;
    bool close_holes = false;    // fill pinholes in the DOS pixels kept on top (text over differing art)
    std::vector<IRect> keep;     // frame rectangles where the DOS pixels always stay on top
    std::vector<IRect> hide;     // frame rectangles where nothing of the DOS frame is drawn over the art
    std::vector<Remap> remaps;
    std::vector<Indicator> indicators;
    // Rectangles of the picture (its own coordinates) where the art has Japanese text and the DOS
    // picture English: with Options::english_text, the DOS pixels stay on top there.
    std::vector<IRect> japanese;
    // Course map: the text panel the game fills in white for course N (DS:FD10), all DOS pixels. The
    // draw tracker sees the fill too; this also holds without it.
    std::vector<std::pair<int, IRect>> course_panels;
    // A picture inside a PC-98 file (TICKET.PIC holds three): byte offset and size.
    int pc98_offset = 0, pc98_width = 0, pc98_height = 0;
    // Mac: the art built on a canvas of this size from several pictures (pict is then unused).
    int canvas_w = 0, canvas_h = 0;
    std::vector<Paint> canvas;
    // The part of the art that is fitted to the DOS picture's rectangle (empty: all of it); the rest
    // of the art reaches beyond it (a speech bubble beside the officer).
    IRect fit_part{0, 0, 0, 0};
    // The indicators mark one choice out of several (a list's highlighted line): when a frame shows
    // none, the last one stays.
    bool one_choice = false;
    // ... read from the game's memory when it runs: a word at this linear address, (value - base) /
    // step being the indicator's index (the highlight bar alone can't be trusted with Smooth on:
    // its white where the replayed 3D view is white too is the 3D view's).
    std::uint32_t choice_at = 0;
    int choice_base = 0, choice_step = 1;
    Handler handler = Handler::None;  // code that adds what depends on the game's state
    int mask_pict = 0;                // Mac sprites: the 1-bit picture of their shape (black = opaque)
    bool bottom = false;              // Fit::Native: align bottom edges
};

struct ScreenSpec {
    Screen id;
    const char* file;      // DOS picture; nullptr: packed into VETTE.EXE, at `header` in the program image
    int header;            // bytes before the packed data (or the offset in the program image)
    int width, height;     // the picture
    int frame_w, frame_h;  // the video mode it appears in
    int x, y;              // where; x < 0: wherever the game draws it (searched for)
    float threshold;       // fraction of the compared pixels that must equal the picture
    bool background;       // a full-screen picture (one per frame) rather than an inset
    ArtSpec pc98, mac;
    IRect area{0, 0, 0, 0};  // the part of the picture compared (empty: all); the rest may be covered
    int xor_match = 0;       // nonzero: a pixel XORed with this also counts as the picture's
    bool sprite = false;     // `file` is a masked sprite (decode_dos_sprite), not an RLE picture
    Screen parent = Screen::None;  // looked for only over this full-screen picture
    // No picture file: the game draws solid boxes (picture coordinates, colour); the rest of the
    // rectangle isn't compared.
    std::vector<std::pair<IRect, std::uint8_t>> boxes;
};

constexpr std::uint32_t kHighlight = 0xFFFFD020;  // indicator frames: Mac-style yellow

// Builders (whole-object initialisation keeps -Wmissing-field-initializers quiet).
ArtSpec pc98_art(const char* file) {
    ArtSpec a;
    a.pc98 = file;
    return a;
}
ArtSpec mac_art(int pict, Fit fit) {
    ArtSpec a;
    a.pict = pict;
    a.fit = fit;
    a.close_holes = true;  // the Mac art differs from the DOS picture under the DOS text
    return a;
}
Remap remap(IRect dos, IRect art, bool opaque, int ignore = -1, std::uint16_t only = 0) {
    return {dos, art, opaque, ignore, only};
}
ScreenSpec screen(Screen id, const char* file, int header, IRect pic, int frame_w, int frame_h, float threshold,
                  bool background, ArtSpec pc98, ArtSpec mac) {
    return {id, file, header, pic.w, pic.h, frame_w, frame_h, pic.x, pic.y, threshold, background, std::move(pc98),
            std::move(mac), {0, 0, 0, 0}, 0, false, Screen::None, {}};
}

std::vector<ScreenSpec> make_table() {
    std::vector<ScreenSpec> t;
    // Title: the DOS sprites (cable car, logo, the car) and the credits stay on top. The Mac title
    // background has the same composition. Its copyright line falls in the letterbox: keep DOS's.
    {
        ArtSpec mac = mac_art(24592, Fit::Contain);
        mac.keep = {{0, 190, 306, 10}};
        t.push_back(screen(Screen::Title, "TITLE.BIN", 0, {0, 0, 640, 200}, 640, 200, 0.5f, true, pc98_art("TITLE.PIC"), mac));
    }
    // The title's animation, Mac only (the PC-98 has the same sprites): "Spectrum HoloByte presents",
    // the VETTE! logo and the car coming up the road (five frames of VX.BIN, unpacked at fixed
    // places, 3009:C73A-C79E) become the Mac's pictures, at the Mac title's scale. The cable car and
    // the man in white stay the DOS game's.
    {
        const auto sprite = [&](Screen id, const char* file, int header, IRect at, bool masked, int pict, int mask,
                                bool bottom) {
            ArtSpec mac = mac_art(pict, Fit::Native);
            mac.mask_pict = mask;
            mac.bottom = bottom;
            mac.close_holes = false;
            ScreenSpec s = screen(id, file, header, at, 640, 200, 0.8f, false, ArtSpec{}, mac);
            s.sprite = masked;
            s.parent = Screen::Title;
            t.push_back(s);
        };
        sprite(Screen::TitlePresents, "SPETRUM.BIN", 0, {120, 14, 392, 12}, true, 31166, 0, false);
        sprite(Screen::TitleLogo, "BIGVET.BIN", 0, {72, 31, 496, 78}, true, 20793, 31198, false);
        sprite(Screen::TitleCar, "VX.BIN", 0x0000, {320, 137, 128, 16}, false, 198, 25396, true);
        sprite(Screen::TitleCar, "VX.BIN", 0x0333, {320, 126, 144, 26}, false, 198, 25396, true);
        sprite(Screen::TitleCar, "VX.BIN", 0x08C8, {320, 114, 176, 38}, false, 3499, 439, true);
        sprite(Screen::TitleCar, "VX.BIN", 0x12F8, {320, 97, 208, 54}, false, 3499, 439, true);
        sprite(Screen::TitleCar, "VX.BIN", 0x22E9, {344, 120, 224, 51}, false, 7083, 22525, true);
    }
    // Garage: the DOS menu bar stays (in the Mac letterbox, like a menu bar); the statistics panel
    // and the graph move into the Mac garage's display; the DOS car driving in is hidden (the Mac
    // garage has its own car). The PC-98 picture says "NORMAL" where the game highlights "STOCK".
    {
        ArtSpec pc98 = pc98_art("GARAGE.PIC");
        pc98.keep = {{0, 0, 640, 11}};
        ArtSpec mac = mac_art(17313, Fit::Contain);
        mac.keep = {{0, 0, 640, 11}};
        // Hidden: the DOS car and mechanic, and the graph's labels (the Mac grid has none).
        mac.hide = {{0, 128, 640, 72}, {436, 12, 204, 128}};
        // The graph's curve without the DOS grid lines (colour 2): the Mac display has its own grid.
        mac.remaps = {remap({168, 19, 182, 101}, {356, 45, 111, 132}, true),
                      remap({478, 19, 120, 99}, {410, 183, 58, 82}, false, 2)};
        // The selected car's menu item gets a red background (4): frame the Mac button.
        const IRect items[4] = {{0, 0, 82, 10}, {88, 0, 78, 10}, {168, 0, 158, 10}, {328, 0, 198, 10}};
        const IRect buttons[4] = {{266, 37, 47, 24}, {266, 64, 47, 24}, {266, 91, 47, 24}, {266, 117, 47, 24}};
        for (int i = 0; i < 4; ++i) mac.indicators.push_back({items[i], 4, 200, buttons[i], kHighlight, 0});
        t.push_back(screen(Screen::Garage, "GARAGE.BIN", 0, {0, 0, 640, 200}, 640, 200, 0.55f, true, pc98, mac));
    }
    // Opponent selection (320x200): the same layout on the Mac; the graph's curves, the statistics
    // and the turning 3D car move into the Mac panels.
    {
        ArtSpec mac = mac_art(12670, Fit::Contain);
        mac.remaps = {remap({35, 10, 110, 68}, {55, 15, 180, 100}, false),
                      remap({0, 101, 106, 99}, {24, 167, 130, 147}, true),
                      remap({108, 101, 212, 99}, {178, 167, 322, 147}, false)};
        // The selected opponent's name strip turns yellow (14): frame the Mac card instead.
        const IRect strips[4] = {{161, 0, 79, 10}, {241, 0, 79, 10}, {161, 50, 79, 10}, {241, 50, 79, 10}};
        const IRect cards[4] = {{252, 7, 126, 61}, {379, 7, 126, 61}, {252, 87, 126, 61}, {379, 87, 126, 61}};
        mac.hide = {{159, 0, 161, 10}, {159, 50, 161, 10}};
        for (int i = 0; i < 4; ++i) mac.indicators.push_back({strips[i], 14, 200, cards[i], kHighlight, 0});
        t.push_back(screen(Screen::Opponents, "EGAPIC.BIN", 2, {0, 0, 320, 200}, 320, 200, 0.5f, true,
                           pc98_art("EGAPIC.PIC"), mac));
    }
    // High scores: the course number, the opponent's and the player's times and the top ten list move
    // next to the Mac picture's labels (its list is spaced wider than the DOS one).
    {
        ArtSpec mac = mac_art(134, Fit::Contain);
        mac.remaps = {remap({52, 12, 26, 16}, {98, 1, 21, 31}, false),
                      remap({154, 40, 100, 15}, {126, 60, 80, 29}, false),
                      remap({154, 82, 130, 15}, {113, 136, 104, 29}, false)};
        // The top ten, one line each (DOS rows 28 + 11k, the digits' middle 4.5 rows into the
        // 11): each on its Mac rank number, whose middles are these rows of the picture.
        constexpr float kRanks[10] = {56, 74, 93, 110, 127.5f, 146, 164, 182, 200, 217.5f};
        constexpr float kScale = 173.0f / 112.0f;  // Mac rows per DOS row, as for the other fields
        for (int k = 0; k < 10; ++k)
            mac.remaps.push_back(remap({300, 28 + 11 * k, 340, 11},
                                       {252, static_cast<int>(kRanks[k] - 4.5f * kScale + 0.5f), 258,
                                        static_cast<int>(11 * kScale + 0.5f)},
                                       false));
        t.push_back(screen(Screen::HighScores, "HIGHSC.BIN", 0, {0, 0, 640, 200}, 640, 200, 0.5f, true,
                           pc98_art("HIGHSC.PIC"), mac));
    }
    t.push_back(screen(Screen::Winner, "WINNER.BIN", 0, {0, 0, 640, 200}, 640, 200, 0.5f, true,
                       pc98_art("WINNER.PIC"), mac_art(141, Fit::Contain)));
    // The race dashboards (mode 0Dh rows 120-199), packed into VETTE.EXE: ahead, looking left (F1),
    // looking right (F3). The PC-98 has its own picture of the front one only. On the Mac, the gauges,
    // lights and hands are drawn from the game's state (mac_dash.cpp); of the DOS dashboard's own
    // drawing only the clock and messages (green, colour 2) and the road signs move onto the Mac's
    // displays.
    {
        ArtSpec mac = mac_art(24055, Fit::Stretch);
        mac.handler = Handler::MacDash;
        // The clock and both message lines (rows 139-163) inside the Mac display's black (rows 55-81).
        mac.remaps = {remap({224, 138, 96, 27}, {378, 55, 124, 27}, false, -1, 1u << 2),
                      remap({240, 120, 80, 15}, {376, 28, 128, 22}, false)};
        mac.hide = {{0, 120, 320, 80}};
        t.push_back(screen(Screen::Dash, nullptr, 0x100, {0, 120, 320, 80}, 320, 200, 0.6f, false, pc98_art("DASH.PIC"), mac));
        t.push_back(screen(Screen::DashLeft, nullptr, 0xA226, {0, 120, 320, 80}, 320, 200, 0.6f, false, ArtSpec{},
                           mac_art(1091, Fit::BottomWidth)));
        t.push_back(screen(Screen::DashRight, nullptr, 0x8C95, {0, 120, 320, 80}, 320, 200, 0.6f, false, ArtSpec{},
                           mac_art(28120, Fit::BottomWidth)));
    }
    // The course map. The DOS game draws its text panels over a different part of MAPPIC.BIN for each
    // course; the overview map top right is always there, so the picture is recognised by it. The
    // Mac's map shows the course's route and box (mac_map.cpp); nothing of the DOS screen stays but
    // the instructions.
    {
        ArtSpec mac = mac_art(26478, Fit::Contain);
        mac.handler = Handler::MacMap;
        mac.hide = {{0, 0, 640, 200}};
        // The panels (3009:88AF, colour 15): course 4 has none. Where the map under a panel is white
        // too, the panel doesn't differ from the picture, so without these the PC-98 map showed
        // through the white as faint lines.
        ArtSpec pc98 = pc98_art("MAPPIC.PIC");
        pc98.course_panels = {{1, {160, 0, 320, 200}}, {2, {0, 60, 480, 140}}, {3, {0, 0, 288, 136}}};
        ScreenSpec s = screen(Screen::CourseMap, "MAPPIC.BIN", 0, {0, 0, 640, 200}, 640, 200, 0.6f, true, pc98, mac);
        // The game highlights the course's part of the overview map by XORing colour 1 into the rest.
        s.area = {480, 0, 160, 60};
        s.xor_match = 1;
        t.push_back(s);
    }
    // Pictures the race view shows over the 3D view, wherever the game draws them.
    const struct {
        Screen id;
        const char* dos;
        const char* pc98;
        int pict;
        IRect speech;  // the PC-98 loser pictures' speech bubbles are in Japanese
    } insets[] = {
        {Screen::Crash0, "CRASH0.BIN", "CRASH0.PIC", 147, {0, 0, 0, 0}},
        {Screen::Crash1, "CRASH1.BIN", "CRASH1.PIC", 140, {0, 0, 0, 0}},
        {Screen::Loser0, "LOSER0.BIN", "LOSER0.PIC", 135, {0, 0, 100, 26}},
        {Screen::Loser1, "LOSER1.BIN", "LOSER1.PIC", 136, {0, 0, 84, 30}},
        {Screen::Loser2, "LOSER2.BIN", "LOSER2.PIC", 137, {90, 0, 86, 52}},
        {Screen::Loser3, "LOSER3.BIN", "LOSER3.PIC", 138, {0, 0, 124, 24}},
    };
    // The opponents are matched by car (the Mac pairs the drivers with different cars).
    for (const auto& in : insets) {
        ArtSpec pc98 = pc98_art(in.pc98);
        if (in.speech.w > 0) pc98.japanese = {in.speech};
        t.push_back(screen(in.id, in.dos, 0, {-1, 0, 176, 128}, 320, 200, 0.6f, false, pc98, mac_art(in.pict, Fit::Cover)));
    }
    // The police stop (re/notes/10-graphics.md): the excuse list over the dashboard, then the ticket
    // or the officer over the 3D view until the player drives off. TICKET.BIN holds an 8x7 check
    // mark (packed bytes 0-1Bh), the ticket 96x121 (from 1Ch, drawn at (40,0)) and the officer
    // 96x120 (from EB7h, at (0,1)); TICKET.PIC the same at twice the width, the officer's words in
    // Japanese. The Mac's notice to appear (145) has other offences; the DOS ones are checked on
    // their nearest Mac line (re/notes/10-graphics.md).
    constexpr IRect kMacCheck[5] = {{7, 45, 12, 13},    // speeding: 106 speeding
                                    {7, 93, 12, 13},    // moving violation (hit a car): 173 hit and run
                                    {7, 57, 12, 13},    // reckless driving (hit a wall): 123 reckless driving
                                    {7, 105, 12, 13},   // vehicular manslaughter: 180
                                    {130, 167, 12, 13}};  // evading arrest: by "failure to respond"
    {
        ArtSpec pc98 = pc98_art("TICKET.PIC");
        pc98.pc98_offset = 56;
        pc98.pc98_width = 192;
        pc98.pc98_height = 121;
        ArtSpec mac = mac_art(145, Fit::Cover);
        mac.hide = {{40, 0, 96, 121}};
        // The game's check marks (3009:DC9D, table DS:5B03): blue (1) in the boxes at (41, 35 + 8i).
        for (int i = 0; i < 5; ++i) mac.indicators.push_back({{41, 35 + 8 * i, 7, 7}, 1, 4, kMacCheck[i], 0, 146});
        t.push_back(screen(Screen::Ticket, "TICKET.BIN", 0x1C, {40, 0, 96, 121}, 320, 200, 0.6f, false, pc98, mac));
    }
    {
        ArtSpec pc98 = pc98_art("TICKET.PIC");
        pc98.pc98_offset = 56 + 11616;
        pc98.pc98_width = 192;
        pc98.pc98_height = 120;
        pc98.japanese = {{0, 104, 96, 16}};
        // The Mac officer (144) with his words (142) beside him, over the 3D view.
        ArtSpec mac = mac_art(0, Fit::Cover);
        mac.canvas_w = 204;
        mac.canvas_h = 198;
        mac.canvas = {{144, 0, 0, {0, 0, 0, 0}, 0}, {142, 148, 8, {0, 0, 0, 0}, 0}};
        mac.fit_part = {0, 0, 144, 198};
        mac.hide = {{0, 1, 96, 120}};
        t.push_back(screen(Screen::Officer, "TICKET.BIN", 0xEB7, {0, 1, 96, 120}, 320, 200, 0.6f, false, pc98, mac));
    }
    {
        // 3009:DADE: a light blue box (9) with a blue one (1) inside over rows 100-199, the question
        // and eight excuses (3009:DBD0), the chosen one XOR-highlighted white (3009:DCDC, rows
        // 115 + 10i). The Mac: its "List of Excuses" dialog (139) and "What's your excuse?" (143) over
        // its dashboard (24055), at the Mac's own scale (512 pixels across: 320 frame pixels, 100
        // rows: 192 pixels).
        ArtSpec mac = mac_art(0, Fit::Stretch);
        mac.canvas_w = 512;
        mac.canvas_h = 192;
        mac.canvas = {{24055, 0, 46, {0, 0, 0, 0}, 0}, {139, 83, 15, {0, 0, 0, 0}, 0}, {143, 19, 15, {0, 0, 0, 0}, 0}};
        mac.hide = {{0, 100, 320, 100}};
        mac.one_choice = true;
        mac.choice_at = kGameDs2 + 0x600A;  // the chosen excuse's entry in the list at DS:5FF8
        mac.choice_base = 0x5FF8;
        mac.choice_step = 2;
        // The dialog's lines: text rows 33 + 11i of 139.
        for (int i = 0; i < 8; ++i)
            mac.indicators.push_back({{8, 115 + 10 * i, 8, 7}, 15, 40, {83 + 26, 15 + 31 + 11 * i, 296, 13}, kHighlight, 0});
        ScreenSpec s = screen(Screen::Excuses, nullptr, 0, {0, 100, 320, 100}, 320, 200, 0.6f, false, ArtSpec{}, mac);
        s.boxes = {{{0, 0, 320, 100}, 9}, {{8, 3, 304, 94}, 1}};
        t.push_back(s);
    }
    {
        // On the high scores (3009:D007) when the race brought tickets: PENALTY.BIN at (360,20), the
        // number of each offence's tickets at (368, 40 + 13i) and the penalty time at (464,127),
        // colour 12. The Mac: its notice to appear with the counts in the offences' boxes and the
        // DOS "PENALTY TIME" line (black and red only) on a strip added under it; 18 rows lower than
        // the DOS picture, so the Mac's "TOP TEN DRIVERS" stays readable above it.
        ArtSpec mac = mac_art(0, Fit::Contain);
        mac.canvas_w = 144;
        mac.canvas_h = 250;
        constexpr std::uint32_t kYellow = 0xFFFFC200, kBlack = 0xFF000000;
        constexpr int kTop = 18;
        mac.canvas = {{145, 0, kTop, {0, 0, 0, 0}, 0},
                      {0, 0, 0, {0, kTop + 198, 144, 34}, kYellow},
                      {0, 0, 0, {0, kTop + 198, 1, 34}, kBlack},
                      {0, 0, 0, {143, kTop + 198, 1, 34}, kBlack},
                      {0, 0, 0, {0, kTop + 231, 144, 1}, kBlack}};
        mac.hide = {{360, 20, 208, 121}};
        for (int i = 0; i < 5; ++i) {
            const IRect box = kMacCheck[i];
            mac.remaps.push_back(remap({368, 40 + 13 * i, 16, 10}, {box.x + 1, box.y + kTop + 1, box.w - 2, box.h - 1}, false));
        }
        // "PENALTY TIME" and its box (picture rows 101-120; the serial number's box right of it stays).
        mac.remaps.push_back(remap({360, 121, 184, 20}, {4, kTop + 199, 136, 32}, true, -1, (1u << 0) | (1u << 12)));
        t.push_back(screen(Screen::Penalty, "PENALTY.BIN", 0, {360, 20, 208, 121}, 640, 200, 0.6f, false,
                           pc98_art("PENALTY.PIC"), mac));
    }
    return t;
}

// The EGA's default palette (0xRRGGBB): the PC-98 set maps these colours to its own palette.
constexpr std::array<std::uint32_t, 16> kEgaDefault = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

FRect place(const IRect& target, int img_w, int img_h, Fit fit, int frame_w, int frame_h, IRect part = {0, 0, 0, 0}) {
    const FRect t{static_cast<float>(target.x), static_cast<float>(target.y), static_cast<float>(target.w),
                  static_cast<float>(target.h)};
    if (part.w > 0 && part.h > 0 && img_w > 0 && img_h > 0) {
        // Fit `part` of the art to the target; the rest of the art goes where it falls around it.
        const FRect p = place(target, part.w, part.h, fit, frame_w, frame_h);
        const float kx = p.w / static_cast<float>(part.w), ky = p.h / static_cast<float>(part.h);
        return {p.x - static_cast<float>(part.x) * kx, p.y - static_cast<float>(part.y) * ky,
                static_cast<float>(img_w) * kx, static_cast<float>(img_h) * ky};
    }
    if (fit == Fit::Stretch || img_w <= 0 || img_h <= 0) return t;
    // The frame is shown 4:3: a frame pixel is 4/frame_w by 3/frame_h display units; art pixels are square.
    const float ux = 4.0f / static_cast<float>(frame_w), uy = 3.0f / static_cast<float>(frame_h);
    if (fit == Fit::BottomWidth) {
        const float h = t.w * ux / static_cast<float>(img_w) * static_cast<float>(img_h) / uy;
        return {t.x, t.y + t.h - h, t.w, h};
    }
    const float sw = t.w * ux / static_cast<float>(img_w), sh = t.h * uy / static_cast<float>(img_h);
    const float s = fit == Fit::Contain ? std::min(sw, sh) : std::max(sw, sh);
    const float w = static_cast<float>(img_w) * s / ux, h = static_cast<float>(img_h) * s / uy;
    return {t.x + (t.w - w) / 2, t.y + (t.h - h) / 2, w, h};
}

// A rectangle of the art image's `src` part (placed at `dst`) in frame pixels.
FRect art_to_frame(const FRect& dst, const IRect& src, const IRect& a) {
    return graphics::art_to_frame(dst, src.w, src.h, {a.x - src.x, a.y - src.y, a.w, a.h});
}

IRect clip(const IRect& r, int w, int h) {
    const int x0 = std::clamp(r.x, 0, w), y0 = std::clamp(r.y, 0, h);
    const int x1 = std::clamp(r.x + r.w, 0, w), y1 = std::clamp(r.y + r.h, 0, h);
    return {x0, y0, x1 - x0, y1 - y0};
}

// Morphological closing (3x3 dilate, then erode): fills one- and two-pixel holes in the mask, so
// text drawn over a background of its own colour doesn't get holes when the art behind differs.
// Separable: a 3x3 square is a row of three, then a column of three; the edges repeat.
void close_mask(std::vector<std::uint8_t>& m, int w, int h) {
    std::vector<std::uint8_t> t(m.size());
    const auto pass = [w, h](const std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out, bool rows, bool any) {
        for (int y = 0; y < h; ++y) {
            const std::size_t row = static_cast<std::size_t>(y) * w;
            for (int x = 0; x < w; ++x) {
                const std::size_t i = row + x;
                std::size_t a = i, b = i;
                if (rows) {
                    a = x > 0 ? i - 1 : i;
                    b = x + 1 < w ? i + 1 : i;
                } else {
                    a = y > 0 ? i - static_cast<std::size_t>(w) : i;
                    b = y + 1 < h ? i + static_cast<std::size_t>(w) : i;
                }
                out[i] = any ? static_cast<std::uint8_t>(in[a] | in[i] | in[b]) : static_cast<std::uint8_t>(in[a] & in[i] & in[b]);
            }
        }
    };
    pass(m, t, true, true);    // dilate
    pass(t, m, false, true);
    pass(m, t, true, false);   // erode
    pass(t, m, false, false);
}

// Art showing through the DOS drawing by chance: where the game drew over the picture in a colour
// the picture has there too, the pixel matches and the art would show as a sliver or hairline
// inside the drawing (a dissolve's blocks still to come show the art's lines where the picture is
// black). An art pixel (0) most of whose 5x5 neighbourhood is the DOS frame's (1) is the frame's too.
void support_mask(std::vector<std::uint8_t>& m, int w, int h) {
    if (w <= 0 || h <= 0 || std::find(m.begin(), m.end(), std::uint8_t{1}) == m.end() ||
        std::find(m.begin(), m.end(), std::uint8_t{0}) == m.end())
        return;
    constexpr int kR = 2;
    const auto W = static_cast<std::size_t>(w);
    // Per column, the DOS pixels in rows y-2..y+2 as they were before this pass; rows y-3..y-1 have
    // changed by then, so their old states are kept in a ring.
    std::vector<std::uint8_t> col(W, 0), ring(W * (kR + 2), 0);
    for (int y = 0; y < std::min(h, kR); ++y)
        for (std::size_t x = 0; x < W; ++x) col[x] = static_cast<std::uint8_t>(col[x] + (m[static_cast<std::size_t>(y) * W + x] == 1));
    for (int y = 0; y < h; ++y) {
        std::uint8_t* row = m.data() + static_cast<std::size_t>(y) * W;
        if (y + kR < h) {
            const std::uint8_t* add = m.data() + static_cast<std::size_t>(y + kR) * W;
            for (std::size_t x = 0; x < W; ++x) col[x] = static_cast<std::uint8_t>(col[x] + (add[x] == 1));
        }
        if (y - kR - 1 >= 0) {
            const std::uint8_t* sub = ring.data() + static_cast<std::size_t>((y - kR - 1) % (kR + 2)) * W;
            for (std::size_t x = 0; x < W; ++x) col[x] = static_cast<std::uint8_t>(col[x] - (sub[x] == 1));
        }
        std::copy(row, row + W, ring.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y % (kR + 2)) * W));
        const int rows = std::min(h, y + kR + 1) - std::max(0, y - kR);
        int sum = 0;  // columns x-2..x+2
        for (int x = 0; x < std::min(w, kR); ++x) sum += col[static_cast<std::size_t>(x)];
        for (int x = 0; x < w; ++x) {
            if (x + kR < w) sum += col[static_cast<std::size_t>(x + kR)];
            if (x - kR - 1 >= 0) sum -= col[static_cast<std::size_t>(x - kR - 1)];
            if (row[x] != 0) continue;
            const int cols = std::min(w, x + kR + 1) - std::max(0, x - kR);
            if (2 * sum > rows * cols) row[x] = 1;
        }
    }
}

// Cuts the part of a moved piece that `cover` (later art) hides, when the cover takes a whole edge
// of it; returns false when nothing (or no rectangle) of it stays visible.
bool trim_piece(Composite::Piece& p, const FRect& cover) {
    FRect& d = p.dst;
    IRect& s = p.src;
    if (!(d.x < cover.x + cover.w && cover.x < d.x + d.w && d.y < cover.y + cover.h && cover.y < d.y + d.h))
        return true;  // no overlap
    const bool spans_w = cover.x <= d.x && cover.x + cover.w >= d.x + d.w;
    const bool spans_h = cover.y <= d.y && cover.y + cover.h >= d.y + d.h;
    // Source pixels to drop from one side, rounded up; the destination shrinks by as many pixels' worth.
    const auto cut = [](float covered, float dst_len, int& src_pos, int& src_len, float& dst_pos, float& len, bool front) {
        const float per = dst_len / static_cast<float>(src_len);
        const int n = static_cast<int>(std::ceil(covered / per - 1e-4f));
        if (n >= src_len) return false;
        src_len -= n;
        len -= static_cast<float>(n) * per;
        if (front) {
            src_pos += n;
            dst_pos += static_cast<float>(n) * per;
        }
        return true;
    };
    if (spans_w && cover.y <= d.y) return cut(cover.y + cover.h - d.y, d.h, s.y, s.h, d.y, d.h, true);
    if (spans_w && cover.y + cover.h >= d.y + d.h) return cut(d.y + d.h - cover.y, d.h, s.y, s.h, d.y, d.h, false);
    if (spans_h && cover.x <= d.x) return cut(cover.x + cover.w - d.x, d.w, s.x, s.w, d.x, d.w, true);
    if (spans_h && cover.x + cover.w >= d.x + d.w) return cut(d.x + d.w - cover.x, d.w, s.x, s.w, d.x, d.w, false);
    return false;
}

}  // namespace

FRect art_to_frame(const FRect& dst, int art_w, int art_h, const IRect& a) {
    const float kx = dst.w / static_cast<float>(art_w), ky = dst.h / static_cast<float>(art_h);
    return {dst.x + static_cast<float>(a.x) * kx, dst.y + static_cast<float>(a.y) * ky, static_cast<float>(a.w) * kx,
            static_cast<float>(a.h) * ky};
}

Image load_mac_picture(const ArtFiles& files, int id, std::vector<std::string>& warnings, std::uint32_t background) {
    const auto data = files.mac_pict ? files.mac_pict(static_cast<std::int16_t>(id)) : std::vector<std::uint8_t>{};
    assets::Pict pict;
    std::string err;
    if (data.empty()) {
        warnings.push_back("PICT " + std::to_string(id) + ": not found");
        return {};
    }
    if (!assets::decode_pict(data, pict, &err)) {
        warnings.push_back("PICT " + std::to_string(id) + ": " + err);
        return {};
    }
    return from_pict(pict, background);
}

// Draws `src` onto `dst` at (x, y), over it where src is opaque (straight alpha, 0 or 255 here).
void paint(Image& dst, const Image& src, int x, int y) {
    for (int sy = 0; sy < src.height; ++sy)
        for (int sx = 0; sx < src.width; ++sx) {
            const int dx = x + sx, dy = y + sy;
            const std::uint32_t v = src.at(sx, sy);
            if (dx < 0 || dy < 0 || dx >= dst.width || dy >= dst.height || (v >> 24) == 0) continue;
            dst.pixels[static_cast<std::size_t>(dy) * dst.width + dx] = v;
        }
}

Image frame_image(int w, int h, std::uint32_t argb, int thickness) {
    Image img;
    img.width = std::max(w, 0);
    img.height = std::max(h, 0);
    img.pixels.assign(static_cast<std::size_t>(img.width) * img.height, 0);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x)
            if (x < thickness || y < thickness || x >= w - thickness || y >= h - thickness)
                img.pixels[static_cast<std::size_t>(y) * img.width + x] = argb;
    return img;
}

void move_pixels(const FrameView& frame, const IRect& dos, std::uint16_t colors, const FRect& dst, Composite& out) {
    const IRect c = clip(dos, frame.width, frame.height);
    if (c.w <= 0 || c.h <= 0) return;
    const auto n = static_cast<std::size_t>(frame.width) * frame.height;
    if (out.moved.size() != n) out.moved.assign(n, kTransparent);
    for (int y = c.y; y < c.y + c.h; ++y)
        for (int x = c.x; x < c.x + c.w; ++x) {
            const std::size_t p = static_cast<std::size_t>(y) * frame.width + x;
            const std::uint8_t v = frame.pixels[p];
            out.moved[p] = v != kTransparent && ((colors >> (v & 15)) & 1) ? v : kTransparent;
        }
    out.pieces.push_back({c, dst});
}

const char* art_name(Art art) {
    switch (art) {
    case Art::Dos: return "DOS";
    case Art::Pc98: return "PC-98";
    case Art::Mac: return "Mac";
    }
    return "?";
}

const char* screen_name(Screen screen) {
    static constexpr const char* kNames[] = {"none",       "title",          "garage",         "opponents",
                                             "high scores", "winner",        "dashboard",      "crash 0",
                                             "crash 1",    "loser 0",        "loser 1",        "loser 2",
                                             "loser 3",    "course map",     "dashboard left", "dashboard right",
                                             "title logo", "title presents", "title car",      "ticket",
                                             "officer",    "excuses",        "penalty"};
    const auto i = static_cast<std::size_t>(screen);
    return i < std::size(kNames) ? kNames[i] : "?";
}

struct Substitution::Impl {
    struct Entry {
        ScreenSpec spec;
        const ArtSpec* art = nullptr;
        DosPicture ref;
        bool ref_ok = false;
        Image image;
        std::vector<Image> indicator_images;
        IRect last{-1, -1, 0, 0};  // where it was last found
        int choice = -1;           // ArtSpec::one_choice: the indicator shown last
        bool active = false;       // found in the previous frame (lower threshold: hysteresis)
        std::vector<std::uint8_t> packed;  // pictures in VETTE.EXE: the bytes ref was decoded from
        std::vector<std::pair<int, int>> probes;  // sample points for searching
        std::unique_ptr<ScreenHandler> handler;
    };

    Art art = Art::Dos;
    SubstitutionOptions options;
    std::vector<Entry> entries;
    std::vector<std::string> warnings;
    std::vector<Found> found;
    std::array<std::uint32_t, 16> pc98_palette{};
    const std::uint8_t* ram = nullptr;
    std::unique_ptr<DrawTracker> tracker;
    std::vector<std::uint8_t> tracked, tracked_cells;  // DrawTracker::overlay() of this frame

    void load(const ArtFiles& files);
    void refresh_program_picture(Entry& e);
    float match_at(const FrameView& f, const Entry& e, int x0, int y0, int step) const;
    bool locate(const FrameView& f, Entry& e, float& ratio);
};

void Substitution::Impl::load(const ArtFiles& files) {
    if (art == Art::Pc98) {
        assets::Pc98Palette pal = assets::kPc98Palette;
        if (files.pc98_file) {
            const auto exe = files.pc98_file("VETTE.EXE");
            if (!exe.empty() && !assets::read_pc98_palette(exe, pal))
                warnings.push_back("PC-98 VETTE.EXE: palette not found, using the 1.02J one");
        }
        for (std::size_t i = 0; i < 16; ++i) pc98_palette[i] = assets::pc98_rgb(pal[i]);
    }
    for (const auto& spec : make_table()) {
        Entry e;
        e.spec = spec;
        e.art = art == Art::Pc98 ? &e.spec.pc98 : &e.spec.mac;
        // The art.
        std::string err;
        if (art == Art::Pc98 && e.art->pc98 && files.pc98_file) {
            assets::Pc98Pic pic;
            const auto data = files.pc98_file(e.art->pc98);
            const auto offset = static_cast<std::size_t>(e.art->pc98_offset);
            const bool part = e.art->pc98_width > 0;
            if (data.empty()) {
                warnings.push_back(std::string(e.art->pc98) + ": not found");
            } else if (part ? offset > data.size() ||
                                  !assets::decode_pc98_pic(std::span<const std::uint8_t>(data).subspan(offset),
                                                           e.art->pc98_width, e.art->pc98_height, pic, &err)
                            : !assets::decode_pc98_pic(e.art->pc98, data, pic, &err)) {
                warnings.push_back(std::string(e.art->pc98) + ": " + (err.empty() ? "too short" : err));
            } else {
                e.image = from_pc98(pic, pc98_palette);
            }
        } else if (art == Art::Mac && e.art->canvas_w > 0 && files.mac_pict) {
            Image canvas;
            canvas.width = e.art->canvas_w;
            canvas.height = e.art->canvas_h;
            canvas.pixels.assign(static_cast<std::size_t>(canvas.width) * canvas.height, 0);
            bool ok = true;
            for (const Paint& step : e.art->canvas) {
                if (step.pict) {
                    const Image pic = load_mac_picture(files, step.pict, warnings, 0xFFFFFFFF);
                    ok = ok && !pic.empty();
                    paint(canvas, pic, step.x, step.y);
                } else {
                    const IRect c = clip(step.fill, canvas.width, canvas.height);
                    for (int y = c.y; y < c.y + c.h; ++y)
                        std::fill_n(canvas.pixels.begin() + static_cast<std::ptrdiff_t>(y) * canvas.width + c.x, c.w, step.argb);
                }
            }
            if (ok) e.image = std::move(canvas);
        } else if (art == Art::Mac && e.art->pict && files.mac_pict) {
            assets::Pict pict;
            const auto data = files.mac_pict(static_cast<std::int16_t>(e.art->pict));
            if (data.empty()) {
                warnings.push_back("PICT " + std::to_string(e.art->pict) + ": not found");
            } else if (!assets::decode_pict(data, pict, &err)) {
                warnings.push_back("PICT " + std::to_string(e.art->pict) + ": " + err);
            } else {
                e.image = from_pict(pict, e.art->fit == Fit::Native ? 0u : 0xFFFFFFFFu);
                if (e.art->mask_pict) {
                    const Image mask = load_mac_picture(files, e.art->mask_pict, warnings, 0);
                    e.image = mask.empty() ? Image{} : apply_mask(e.image, mask);
                }
            }
        }
        if (e.image.empty()) continue;
        if (e.art->handler == Handler::MacDash) e.handler = make_mac_dash(files, warnings);
        if (e.art->handler == Handler::MacMap) e.handler = make_mac_map(files, warnings);
        // The DOS picture it replaces (the dashboards come from the running program later).
        if (spec.file) {
            const auto data = files.dos_file ? files.dos_file(spec.file) : std::vector<std::uint8_t>{};
            if (data.empty()) {
                warnings.push_back(std::string(spec.file) + ": not found");
                continue;
            }
            if (spec.sprite ? !decode_dos_sprite(data, e.ref, &err)
                            : !decode_dos_picture(data, spec.header, spec.width, spec.height, e.ref, &err)) {
                warnings.push_back(std::string(spec.file) + ": " + err);
                continue;
            }
            e.ref_ok = true;
        } else if (!spec.boxes.empty()) {
            e.ref.width = spec.width;
            e.ref.height = spec.height;
            e.ref.pixels.assign(static_cast<std::size_t>(spec.width) * spec.height, 0);
            e.ref.opaque.assign(e.ref.pixels.size(), 0);
            for (const auto& [box, colour] : spec.boxes) {
                const IRect c = clip(box, spec.width, spec.height);
                for (int y = c.y; y < c.y + c.h; ++y)
                    for (int x = c.x; x < c.x + c.w; ++x) {
                        const std::size_t i = static_cast<std::size_t>(y) * spec.width + x;
                        e.ref.pixels[i] = colour;
                        e.ref.opaque[i] = 1;
                    }
            }
            e.ref_ok = true;
        }
        // The PC-98 art's Japanese words become the DOS picture's English ones, in the art itself
        // (scaled to the art: the race pictures are twice as wide), so they don't depend on the frame
        // showing the DOS pixels there (the Enhanced view's layer can leave some out).
        if (art == Art::Pc98 && options.english_text && e.ref_ok && e.ref.width > 0 && e.ref.height > 0)
            for (const IRect& j : e.art->japanese) {
                const int sx = e.image.width / e.ref.width, sy = e.image.height / e.ref.height;
                if (sx < 1 || sy < 1) continue;
                const IRect c = clip(j, e.ref.width, e.ref.height);
                for (int y = c.y * sy; y < (c.y + c.h) * sy; ++y)
                    for (int x = c.x * sx; x < (c.x + c.w) * sx; ++x) {
                        const std::uint8_t v = e.ref.pixels[static_cast<std::size_t>(y / sy) * e.ref.width + x / sx];
                        e.image.pixels[static_cast<std::size_t>(y) * e.image.width + x] = 0xFF000000u | pc98_palette[v & 15];
                    }
            }
        for (const auto& ind : e.art->indicators)
            e.indicator_images.push_back(ind.pict ? load_mac_picture(files, ind.pict, warnings, 0)
                                                  : frame_image(ind.art.w, ind.art.h, ind.rgb));
        for (int j = 0; j < 8; ++j)
            for (int i = 0; i < 8; ++i) e.probes.push_back({(2 * i + 1) * spec.width / 16, (2 * j + 1) * spec.height / 16});
        entries.push_back(std::move(e));
    }
    // `art` pointers must point into the entries' own copies of the spec.
    for (auto& e : entries) e.art = art == Art::Pc98 ? &e.spec.pc98 : &e.spec.mac;
}

// A picture packed into VETTE.EXE, read from the running program: decoded again whenever its packed
// bytes change (before the EXEPACK stub has run they are still compressed).
void Substitution::Impl::refresh_program_picture(Entry& e) {
    const auto& s = e.spec;
    const std::size_t at = kProgramImage + static_cast<std::size_t>(s.header);
    constexpr std::size_t kCompared = 256, kRange = 0x100000;
    if (!ram || at + kCompared > kRange) return;
    if (e.packed.size() == kCompared && std::equal(e.packed.begin(), e.packed.end(), ram + at)) return;
    e.packed.assign(ram + at, ram + at + kCompared);
    e.ref_ok = decode_dos_picture(std::span<const std::uint8_t>(ram + at, kRange - at), 0, s.width, s.height, e.ref);
}

// Fraction of the compared pixels (every `step`th, skipping transparent ones) equal to the picture
// placed at (x0, y0), within the spec's comparison area.
float Substitution::Impl::match_at(const FrameView& f, const Entry& e, int x0, int y0, int step) const {
    const IRect a = e.spec.area.w > 0 ? e.spec.area : IRect{0, 0, e.ref.width, e.ref.height};
    long n = 0, eq = 0, samples = 0;
    for (int y = a.y; y < a.y + a.h; y += step) {
        const std::uint8_t* row = f.pixels + static_cast<std::size_t>(y0 + y) * f.width + x0;
        const std::uint8_t* ref = e.ref.pixels.data() + static_cast<std::size_t>(y) * e.ref.width;
        for (int x = a.x + (y / step) % step; x < a.x + a.w; x += step) {
            ++samples;
            const std::uint8_t v = row[x];
            if (v == kTransparent || !e.ref.covers(static_cast<std::size_t>(y) * e.ref.width + x)) continue;
            ++n;
            eq += v == ref[x] || (e.spec.xor_match && (v ^ e.spec.xor_match) == ref[x]);
        }
    }
    if (n == 0 || n * 10 < samples) return 0;  // mostly transparent: nothing to judge by
    return static_cast<float>(eq) / static_cast<float>(n);
}

// Finds the picture in the frame: at its fixed place, or (x < 0 in the table) wherever it is, on
// byte-aligned columns like the game's blits.
bool Substitution::Impl::locate(const FrameView& f, Entry& e, float& ratio) {
    const auto& s = e.spec;
    const float threshold = e.active ? s.threshold * 0.75f : s.threshold;
    ratio = 0;
    if (s.x >= 0) {
        ratio = match_at(f, e, s.x, s.y, 2);
        e.last = {s.x, s.y, s.width, s.height};
        return ratio >= threshold;
    }
    if (e.last.x >= 0 && e.last.x + s.width <= f.width && e.last.y + s.height <= f.height) {
        ratio = match_at(f, e, e.last.x, e.last.y, 2);
        if (ratio >= threshold) return true;
    }
    const int need = static_cast<int>(e.probes.size() * 3 / 4);
    float best = 0;
    IRect best_at{-1, -1, 0, 0};
    for (int y = 0; y + s.height <= f.height; ++y) {
        for (int x = 0; x + s.width <= f.width; x += 8) {
            int hits = 0, left = static_cast<int>(e.probes.size());
            for (const auto& [px, py] : e.probes) {
                hits += f.pixels[static_cast<std::size_t>(y + py) * f.width + x + px] ==
                        e.ref.pixels[static_cast<std::size_t>(py) * s.width + px];
                if (hits + --left < need) break;
            }
            if (hits < need) continue;
            const float r = match_at(f, e, x, y, 3);
            if (r > best) {
                best = r;
                best_at = {x, y, s.width, s.height};
            }
        }
    }
    if (best >= threshold) {
        e.last = best_at;
        ratio = match_at(f, e, best_at.x, best_at.y, 2);
        return true;
    }
    return false;
}

Substitution::Substitution(Art art, const ArtFiles& files, SubstitutionOptions options)
    : impl_(std::make_unique<Impl>()) {
    impl_->art = art;
    impl_->options = options;
    if (art != Art::Dos) impl_->load(files);
}

Substitution::~Substitution() = default;

Art Substitution::art() const { return impl_->art; }

std::vector<Screen> Substitution::available() const {
    std::vector<Screen> out;
    for (const auto& e : impl_->entries)
        if (std::find(out.begin(), out.end(), e.spec.id) == out.end()) out.push_back(e.spec.id);
    return out;
}

const std::vector<std::string>& Substitution::warnings() const { return impl_->warnings; }

const std::vector<Substitution::Found>& Substitution::found() const { return impl_->found; }

void Substitution::set_program_memory(const std::uint8_t* ram) { impl_->ram = ram; }

void Substitution::attach(host::Machine& machine) {
    impl_->ram = machine.memory().ram();
    impl_->tracker.reset();
    if (impl_->art != Art::Dos) impl_->tracker = std::make_unique<DrawTracker>(machine);
}

void Substitution::set_draw_log(std::function<void(const std::string&)> log) {
    if (impl_->tracker) impl_->tracker->set_log(std::move(log));
}

bool Substitution::compose(const FrameView& f, Composite& out) {
    auto& m = *impl_;
    m.found.clear();
    if (m.art == Art::Dos || !f.pixels || !f.palette || f.width <= 0 || f.height <= 0) return false;

    // The palette: the PC-98's in place of the EGA's colours.
    std::array<std::uint32_t, 16> palette = *f.palette;
    if (m.art == Art::Pc98)
        for (auto& c : palette)
            for (std::size_t k = 0; k < 16; ++k)
                if ((c & 0xFFFFFF) == kEgaDefault[k]) {
                    c = m.pc98_palette[k];
                    break;
                }

    // Recognise: at most one full-screen picture, any number of insets (in table order).
    struct Match {
        Impl::Entry* e;
        IRect rect;
    };
    std::vector<Match> matches;
    Impl::Entry* best_bg = nullptr;
    float best_ratio = 0;
    // Sprites (entries with a parent) are looked for in a second pass, over their parent only.
    const auto recognise = [&](Impl::Entry& e, bool second_pass) {
        const auto& s = e.spec;
        if ((s.parent != Screen::None) != second_pass) return;
        if (s.frame_w != f.width || s.frame_h != f.height ||
            (second_pass && (!best_bg || best_bg->spec.id != s.parent))) {
            e.active = false;
            return;
        }
        if (!s.file && s.boxes.empty()) m.refresh_program_picture(e);
        if (!e.ref_ok) return;
        float ratio = 0;
        if (!m.locate(f, e, ratio)) {
            e.active = false;
            e.choice = -1;
            return;
        }
        if (s.background) {
            if (ratio > best_ratio) {
                if (best_bg) best_bg->active = false;
                best_bg = &e;
                best_ratio = ratio;
            } else {
                e.active = false;
            }
            return;
        }
        e.active = true;
        matches.push_back({&e, e.last});
        m.found.push_back({s.id, ratio, e.last});
    };
    for (auto& e : m.entries) recognise(e, false);
    for (auto& e : m.entries) recognise(e, true);
    if (best_bg) {
        best_bg->active = true;
        matches.insert(matches.begin(), {best_bg, best_bg->last});
        m.found.insert(m.found.begin(), {best_bg->spec.id, best_ratio, best_bg->last});
    }
    if (matches.empty()) {
        if (m.art != Art::Pc98 || palette == *f.palette) return false;
        out = Composite{};  // PC-98: the frame as it is, in the PC-98's colours
        out.frame_w = f.width;
        out.frame_h = f.height;
        out.palette = palette;
        out.base.assign(f.pixels, f.pixels + static_cast<std::size_t>(f.width) * f.height);
        return true;
    }

    const int w = f.width, h = f.height;
    const auto n = static_cast<std::size_t>(w) * h;
    const bool tracked_ok = m.tracker && m.tracker->overlay(f.pixels, w, h, m.tracked, m.tracked_cells);
    out = Composite{};
    out.frame_w = w;
    out.frame_h = h;
    out.palette = palette;
    const auto& first = matches.front();
    const bool full = first.rect.x == 0 && first.rect.y == 0 && first.rect.w == w && first.rect.h == h;
    if (!full) out.base.assign(f.pixels, f.pixels + n);
    out.over.assign(n, kTransparent);

    // Pieces moved by a picture that a later inset covers are dropped (they'd be drawn over it).
    std::vector<std::pair<std::size_t, FRect>> covers;  // pieces before this index, the inset's art
    for (const auto& match : matches) {
        auto& e = *match.e;
        const auto& a = *e.art;
        const IRect r = clip(match.rect, w, h);
        if (!out.base.empty())
            for (int y = r.y; y < r.y + r.h; ++y)
                std::fill_n(out.base.begin() + static_cast<std::ptrdiff_t>(y) * w + r.x, r.w, kTransparent);
        const IRect src{0, 0, e.image.width, e.image.height};
        FRect dst = place(match.rect, e.image.width, e.image.height, a.fit == Fit::Native ? Fit::Contain : a.fit, w, h,
                          a.fit_part);
        if (a.fit == Fit::Native && &match != &matches.front() && !matches.front().e->image.empty()) {
            // The parent's art scale (frame pixels per art pixel), centred on the DOS sprite.
            const FRect& bg = out.layers.front().dst;
            const float kx = bg.w / static_cast<float>(matches.front().e->image.width);
            const float ky = bg.h / static_cast<float>(matches.front().e->image.height);
            const float dw = static_cast<float>(e.image.width) * kx, dh = static_cast<float>(e.image.height) * ky;
            const float cx = static_cast<float>(match.rect.x) + static_cast<float>(match.rect.w) / 2;
            const float y = a.bottom ? static_cast<float>(match.rect.y + match.rect.h) - dh
                                     : static_cast<float>(match.rect.y) + (static_cast<float>(match.rect.h) - dh) / 2;
            dst = {cx - dw / 2, y, dw, dh};
        }
        out.layers.push_back({&e.image, src, dst});
        // The part of the art fitted to the picture (all of it, or ArtSpec::fit_part: what reaches
        // beyond it, a speech bubble, is drawn over the frame as it is).
        const FRect body = a.fit_part.w > 0 ? art_to_frame(dst, src, a.fit_part) : dst;
        if (&match != &matches.front()) covers.push_back({out.pieces.size(), body});
        // An inset's art can be larger than the DOS picture (Fit::Cover): what the frame showed
        // around the picture mustn't be drawn over it.
        if (&match != &matches.front() || !full) {
            const int ux = static_cast<int>(std::floor(body.x)), uy = static_cast<int>(std::floor(body.y));
            const IRect under = clip({ux, uy, static_cast<int>(std::ceil(body.x + body.w)) - ux,
                                      static_cast<int>(std::ceil(body.y + body.h)) - uy},
                                     w, h);
            for (int y = under.y; y < under.y + under.h; ++y) {
                std::fill_n(out.over.begin() + static_cast<std::ptrdiff_t>(y) * w + under.x, under.w, kTransparent);
                if (!out.base.empty() && &match != &matches.front())
                    std::fill_n(out.base.begin() + static_cast<std::ptrdiff_t>(y) * w + under.x, under.w, kTransparent);
            }
        }

        // A sprite's parent picture: where the frame still shows it, the sprite isn't drawn (yet).
        const Impl::Entry* parent = nullptr;
        IRect parent_rect{};
        if (e.spec.parent != Screen::None && matches.front().e->spec.id == e.spec.parent &&
            matches.front().e->ref.width == w && matches.front().e->ref.height == h) {
            parent = matches.front().e;
            parent_rect = matches.front().rect;
        }
        // The DOS pixels on top: wherever the frame differs from the DOS picture, except where a
        // later inset (a crash picture over the dashboard) is: those pixels are the inset's. Not
        // needed when all of it is hidden and nothing is moved from it (the Mac course map).
        bool all_hidden = false;
        for (const auto& hd : a.hide)
            all_hidden = all_hidden || (a.remaps.empty() && hd.x <= r.x && hd.y <= r.y && hd.x + hd.w >= r.x + r.w &&
                                        hd.y + hd.h >= r.y + r.h);
        std::vector<std::uint8_t> mask(all_hidden ? 0 : static_cast<std::size_t>(r.w) * r.h);
        for (int y = 0; y < r.h && !all_hidden; ++y)
            for (int x = 0; x < r.w; ++x) {
                const std::uint8_t v = f.pixels[static_cast<std::size_t>(r.y + y) * w + r.x + x];
                const std::uint8_t ref = e.ref.pixels[static_cast<std::size_t>(r.y + y - match.rect.y) * e.ref.width +
                                                      (r.x + x - match.rect.x)];
                mask[static_cast<std::size_t>(y) * r.w + x] = v != kTransparent && v != ref;
                if (!e.ref.covers(static_cast<std::size_t>(r.y + y - match.rect.y) * e.ref.width + (r.x + x - match.rect.x)))
                    mask[static_cast<std::size_t>(y) * r.w + x] = 2;  // not the sprite's: leave as it is
                else if (parent && v == parent->ref.pixels[static_cast<std::size_t>(r.y + y - parent_rect.y) * parent->ref.width +
                                                           (r.x + x - parent_rect.x)])
                    mask[static_cast<std::size_t>(y) * r.w + x] = 0;  // not drawn yet (a sprite wiping in)
            }
        const auto later_insets = [&] {
            for (const Match* later = &match + 1; later != matches.data() + matches.size() && !all_hidden; ++later) {
                const IRect c = clip(later->rect, w, h);
                for (int y = std::max(c.y, r.y); y < std::min(c.y + c.h, r.y + r.h); ++y)
                    for (int x = std::max(c.x, r.x); x < std::min(c.x + c.w, r.x + r.w); ++x)
                        mask[static_cast<std::size_t>(y - r.y) * r.w + (x - r.x)] = 0;
            }
        };
        later_insets();
        const std::vector<std::uint8_t> raw = tracked_ok ? mask : std::vector<std::uint8_t>{};
        if (!all_hidden) support_mask(mask, r.w, r.h);
        if (a.close_holes && r.w > 0 && r.h > 0 && !all_hidden) close_mask(mask, r.w, r.h);
        // What the game drew, exactly (draw_tracker.h): its text and sprite pixels stay on top whatever
        // colour is under them, inside text cells nothing is guessed (no hole filling), and on the
        // letterbox bars, outside the art, text keeps its DOS background so it stays readable. A
        // sprite whose own art replaces it keeps its pixels hidden (text drawn over it stays).
        if (tracked_ok && !all_hidden) {
            const bool replaced_sprite = e.spec.parent != Screen::None;
            const bool background = &match == &matches.front();
            for (int y = 0; y < r.h; ++y)
                for (int x = 0; x < r.w; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * r.w + x;
                    if (mask[i] == 2) continue;
                    const std::size_t p = static_cast<std::size_t>(r.y + y) * w + r.x + x;
                    if (m.tracked_cells[p]) mask[i] = raw[i];
                    if (const std::uint8_t t = m.tracked[p]; t != DrawTracker::kNone && !(replaced_sprite && t == DrawTracker::kSprite))
                        mask[i] = 1;
                    // A whole character cell outside or across the art's edge keeps its DOS background.
                    if (const std::uint8_t cell = m.tracked_cells[p]; background && cell) {
                        const float x0 = static_cast<float>((r.x + x) & ~7), x1 = x0 + 8;
                        const float y0 = static_cast<float>(r.y + y - ((cell & 15) - 1)), y1 = y0 + static_cast<float>((cell >> 4) + 1);
                        if (x0 < dst.x || y0 < dst.y || x1 > dst.x + dst.w || y1 > dst.y + dst.h) mask[i] = 1;
                    }
                }
            later_insets();
        }
        for (int y = 0; y < r.h && !all_hidden; ++y)
            for (int x = 0; x < r.w; ++x) {
                const std::size_t p = static_cast<std::size_t>(r.y + y) * w + r.x + x;
                const std::uint8_t k = mask[static_cast<std::size_t>(y) * r.w + x];
                if (k != 2) out.over[p] = k ? f.pixels[p] : kTransparent;
            }
        std::vector<IRect> keep = a.keep;
        if (m.ram && !a.course_panels.empty())
            for (const auto& [course, panel] : a.course_panels)
                if (course == read_map_course(m.ram)) keep.push_back(panel);
        if (m.options.english_text)
            for (const auto& j : a.japanese) keep.push_back({match.rect.x + j.x, match.rect.y + j.y, j.w, j.h});
        for (const auto& k : keep) {
            const IRect c = clip(k, w, h);
            for (int y = c.y; y < c.y + c.h; ++y)
                for (int x = c.x; x < c.x + c.w; ++x) out.over[static_cast<std::size_t>(y) * w + x] = f.pixels[static_cast<std::size_t>(y) * w + x];
        }
        const auto covered = [&](int x, int y) {  // by a later inset's DOS picture
            for (const Match* later = &match + 1; later != matches.data() + matches.size(); ++later)
                if (x >= later->rect.x && x < later->rect.x + later->rect.w && y >= later->rect.y &&
                    y < later->rect.y + later->rect.h)
                    return true;
            return false;
        };
        for (const auto& rm : a.remaps) {
            const IRect c = clip(rm.dos, w, h);
            if (c.w <= 0 || c.h <= 0) continue;
            if (out.moved.empty()) out.moved.assign(n, kTransparent);
            for (int y = c.y; y < c.y + c.h; ++y)
                for (int x = c.x; x < c.x + c.w; ++x) {
                    const std::size_t p = static_cast<std::size_t>(y) * w + x;
                    const std::uint8_t v = rm.opaque && !covered(x, y) ? f.pixels[p] : out.over[p];
                    const bool wanted = v != kTransparent && v != rm.ignore && (!rm.only || ((rm.only >> (v & 15)) & 1));
                    out.moved[p] = wanted ? v : kTransparent;
                    out.over[p] = kTransparent;
                }
            out.pieces.push_back({c, art_to_frame(dst, src, rm.art)});
        }
        for (const auto& hd : a.hide) {
            const IRect c = clip(hd, w, h);
            for (int y = c.y; y < c.y + c.h; ++y)
                std::fill_n(out.over.begin() + static_cast<std::ptrdiff_t>(y) * w + c.x, c.w, kTransparent);
        }
        int chosen = -1;  // ArtSpec::one_choice: the indicator shown
        for (std::size_t i = 0; i < a.indicators.size(); ++i) {
            const auto& ind = a.indicators[i];
            const IRect c = clip(ind.dos, w, h);
            int count = 0;
            for (int y = c.y; y < c.y + c.h; ++y)
                for (int x = c.x; x < c.x + c.w; ++x) count += f.pixels[static_cast<std::size_t>(y) * w + x] == ind.color;
            const Image& img = e.indicator_images[i];
            if (count < ind.min_pixels || img.empty()) continue;
            if (a.one_choice) {
                if (chosen < 0) chosen = static_cast<int>(i);
                continue;
            }
            const IRect at = ind.pict ? IRect{ind.art.x, ind.art.y, img.width, img.height} : ind.art;
            out.layers.push_back({&img, {0, 0, img.width, img.height}, art_to_frame(dst, src, at)});
        }
        if (a.one_choice) {
            if (a.choice_at && m.ram && a.choice_step > 0) {
                const int v = (m.ram[a.choice_at] | m.ram[a.choice_at + 1] << 8) - a.choice_base;
                if (v >= 0 && v % a.choice_step == 0 && v / a.choice_step < static_cast<int>(a.indicators.size()))
                    chosen = v / a.choice_step;
            }
            // A frame caught while the game redraws its list has no highlight: keep the last one.
            if (chosen < 0) chosen = e.choice;
            e.choice = chosen;
            if (chosen >= 0) {
                const Image& img = e.indicator_images[static_cast<std::size_t>(chosen)];
                out.layers.push_back({&img, {0, 0, img.width, img.height},
                                      art_to_frame(dst, src, a.indicators[static_cast<std::size_t>(chosen)].art)});
            }
        }
        if (e.handler) e.handler->compose({f, m.ram, e.ref, match.rect, dst, e.image.width, e.image.height}, out);
    }
    if (!covers.empty()) {
        std::size_t kept = 0;
        for (std::size_t i = 0; i < out.pieces.size(); ++i) {
            bool visible = true;
            for (const auto& [before, art] : covers)
                if (i < before) visible = visible && trim_piece(out.pieces[i], art);
            if (visible) out.pieces[kept++] = out.pieces[i];
        }
        out.pieces.resize(kept);
    }
    return true;
}

}  // namespace vette::graphics
