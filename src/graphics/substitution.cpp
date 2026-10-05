#include "graphics/substitution.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <utility>

#include "assets/pc98_pic.h"
#include "assets/pict.h"
#include "graphics/dos_art.h"

namespace vette::graphics {

namespace {

// --- The screen table ---------------------------------------------------------------------------

enum class Fit {
    Stretch,  // fill the target rectangle
    Contain,  // keep the art's aspect ratio, as large as fits, centred (letterboxed)
    Cover,    // keep the aspect ratio, cover the target, centred (overflowing it)
};

// A DOS region moved to where the replacement layout has it: `dos` in frame pixels, `art` in the
// replacement image's pixels. Opaque moves the whole rectangle; otherwise only the pixels that
// differ from the DOS picture (the dynamic content).
struct Remap {
    IRect dos;
    IRect art;
    bool opaque;
    int ignore = -1;  // a colour left behind (e.g. DOS grid lines where the art has its own grid)
};

// A highlight drawn on the replacement when a DOS region shows a colour: e.g. the selected car
// in the garage's menu bar (red background) frames the matching Mac button.
struct Indicator {
    IRect dos;
    int color;
    int min_pixels;
    IRect art;
    std::uint32_t rgb;
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
};

struct ScreenSpec {
    Screen id;
    const char* file;      // DOS picture; nullptr = the dashboard in VETTE.EXE
    int header;            // bytes before the packed data
    int width, height;     // the picture
    int frame_w, frame_h;  // the video mode it appears in
    int x, y;              // where; x < 0: wherever the game draws it (searched for)
    float threshold;       // fraction of the compared pixels that must equal the picture
    bool background;       // a full-screen picture (one per frame) rather than an inset
    ArtSpec pc98, mac;
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
Remap remap(IRect dos, IRect art, bool opaque, int ignore = -1) { return {dos, art, opaque, ignore}; }
ScreenSpec screen(Screen id, const char* file, int header, IRect pic, int frame_w, int frame_h, float threshold,
                  bool background, ArtSpec pc98, ArtSpec mac) {
    return {id, file, header, pic.w, pic.h, frame_w, frame_h, pic.x, pic.y, threshold, background, std::move(pc98),
            std::move(mac)};
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
        for (int i = 0; i < 4; ++i) mac.indicators.push_back({items[i], 4, 200, buttons[i], kHighlight});
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
        for (int i = 0; i < 4; ++i) mac.indicators.push_back({strips[i], 14, 200, cards[i], kHighlight});
        t.push_back(screen(Screen::Opponents, "EGAPIC.BIN", 2, {0, 0, 320, 200}, 320, 200, 0.5f, true,
                           pc98_art("EGAPIC.PIC"), mac));
    }
    t.push_back(screen(Screen::HighScores, "HIGHSC.BIN", 0, {0, 0, 640, 200}, 640, 200, 0.5f, true,
                       pc98_art("HIGHSC.PIC"), mac_art(134, Fit::Contain)));
    t.push_back(screen(Screen::Winner, "WINNER.BIN", 0, {0, 0, 640, 200}, 640, 200, 0.5f, true,
                       pc98_art("WINNER.PIC"), mac_art(141, Fit::Contain)));
    // The race dashboard (mode 0Dh rows 120-199). Hands, gauges, digits, lights and messages are
    // the DOS game's and stay on top.
    {
        ArtSpec mac = mac_art(24055, Fit::Stretch);
        // The gauges (bars and digits), the cruise/auto lights and the message display move onto
        // the Mac dashboard's own (rough placement; the gauges should be redrawn natively later).
        mac.remaps = {remap({55, 148, 50, 48}, {100, 66, 80, 80}, false),
                      remap({145, 148, 55, 48}, {268, 62, 90, 84}, false),
                      remap({110, 133, 34, 14}, {183, 64, 76, 18}, false),
                      remap({222, 135, 98, 32}, {372, 42, 132, 48}, false)};
        t.push_back(screen(Screen::Dash, nullptr, 0, {0, 120, 320, 80}, 320, 200, 0.6f, false, pc98_art("DASH.PIC"), mac));
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
    return t;
}

// The EGA's default palette (0xRRGGBB): the PC-98 set maps these colours to its own palette.
constexpr std::array<std::uint32_t, 16> kEgaDefault = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

FRect place(const IRect& target, int img_w, int img_h, Fit fit, int frame_w, int frame_h) {
    const FRect t{static_cast<float>(target.x), static_cast<float>(target.y), static_cast<float>(target.w),
                  static_cast<float>(target.h)};
    if (fit == Fit::Stretch || img_w <= 0 || img_h <= 0) return t;
    // The frame is shown 4:3: a frame pixel is 4/frame_w by 3/frame_h display units; art pixels are square.
    const float ux = 4.0f / static_cast<float>(frame_w), uy = 3.0f / static_cast<float>(frame_h);
    const float sw = t.w * ux / static_cast<float>(img_w), sh = t.h * uy / static_cast<float>(img_h);
    const float s = fit == Fit::Contain ? std::min(sw, sh) : std::max(sw, sh);
    const float w = static_cast<float>(img_w) * s / ux, h = static_cast<float>(img_h) * s / uy;
    return {t.x + (t.w - w) / 2, t.y + (t.h - h) / 2, w, h};
}

// A rectangle of the art image (placed at `dst`) in frame pixels.
FRect art_to_frame(const FRect& dst, const IRect& src, const IRect& a) {
    const float kx = dst.w / static_cast<float>(src.w), ky = dst.h / static_cast<float>(src.h);
    return {dst.x + static_cast<float>(a.x - src.x) * kx, dst.y + static_cast<float>(a.y - src.y) * ky,
            static_cast<float>(a.w) * kx, static_cast<float>(a.h) * ky};
}

IRect clip(const IRect& r, int w, int h) {
    const int x0 = std::clamp(r.x, 0, w), y0 = std::clamp(r.y, 0, h);
    const int x1 = std::clamp(r.x + r.w, 0, w), y1 = std::clamp(r.y + r.h, 0, h);
    return {x0, y0, x1 - x0, y1 - y0};
}

// Morphological closing (3x3 dilate, then erode): fills one- and two-pixel holes in the mask, so
// text drawn over a background of its own colour doesn't get holes when the art behind differs.
void close_mask(std::vector<std::uint8_t>& m, int w, int h) {
    std::vector<std::uint8_t> d(m.size());
    const auto at = [&](const std::vector<std::uint8_t>& v, int x, int y) {
        return v[static_cast<std::size_t>(std::clamp(y, 0, h - 1)) * w + std::clamp(x, 0, w - 1)];
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            std::uint8_t any = 0;
            for (int dy = -1; dy <= 1 && !any; ++dy)
                for (int dx = -1; dx <= 1 && !any; ++dx) any = at(m, x + dx, y + dy);
            d[static_cast<std::size_t>(y) * w + x] = any;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            std::uint8_t all = 1;
            for (int dy = -1; dy <= 1 && all; ++dy)
                for (int dx = -1; dx <= 1 && all; ++dx) all = at(d, x + dx, y + dy);
            m[static_cast<std::size_t>(y) * w + x] = all;
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

Image frame_image(int w, int h, std::uint32_t rgb) {
    Image img;
    img.width = w;
    img.height = h;
    img.pixels.assign(static_cast<std::size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (x < 2 || y < 2 || x >= w - 2 || y >= h - 2) img.pixels[static_cast<std::size_t>(y) * w + x] = rgb;
    return img;
}

}  // namespace

const char* art_name(Art art) {
    switch (art) {
    case Art::Dos: return "DOS";
    case Art::Pc98: return "PC-98";
    case Art::Mac: return "Mac";
    }
    return "?";
}

const char* screen_name(Screen screen) {
    static constexpr const char* kNames[] = {"none", "title", "garage", "opponents", "high scores", "winner",
                                             "dashboard", "crash 0", "crash 1", "loser 0", "loser 1",
                                             "loser 2", "loser 3"};
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
        bool active = false;       // found in the previous frame (lower threshold: hysteresis)
        bool confirmed = false;    // the dashboard's reference has been recognised once
        std::vector<std::pair<int, int>> probes;  // sample points for searching
    };

    Art art = Art::Dos;
    SubstitutionOptions options;
    std::vector<Entry> entries;
    std::vector<std::string> warnings;
    std::vector<Found> found;
    std::array<std::uint32_t, 16> pc98_palette{};
    const std::uint8_t* ram = nullptr;

    void load(const ArtFiles& files);
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
            if (data.empty()) {
                warnings.push_back(std::string(e.art->pc98) + ": not found");
            } else if (!assets::decode_pc98_pic(e.art->pc98, data, pic, &err)) {
                warnings.push_back(std::string(e.art->pc98) + ": " + err);
            } else {
                e.image = from_pc98(pic, pc98_palette);
            }
        } else if (art == Art::Mac && e.art->pict && files.mac_pict) {
            assets::Pict pict;
            const auto data = files.mac_pict(static_cast<std::int16_t>(e.art->pict));
            if (data.empty()) {
                warnings.push_back("PICT " + std::to_string(e.art->pict) + ": not found");
            } else if (!assets::decode_pict(data, pict, &err)) {
                warnings.push_back("PICT " + std::to_string(e.art->pict) + ": " + err);
            } else {
                e.image = from_pict(pict);
            }
        }
        if (e.image.empty()) continue;
        // The DOS picture it replaces (the dashboard comes from the running program later).
        if (spec.file) {
            const auto data = files.dos_file ? files.dos_file(spec.file) : std::vector<std::uint8_t>{};
            if (data.empty()) {
                warnings.push_back(std::string(spec.file) + ": not found");
                continue;
            }
            if (!decode_dos_picture(data, spec.header, spec.width, spec.height, e.ref, &err)) {
                warnings.push_back(std::string(spec.file) + ": " + err);
                continue;
            }
            e.ref_ok = true;
        }
        for (const auto& ind : e.art->indicators) e.indicator_images.push_back(frame_image(ind.art.w, ind.art.h, ind.rgb));
        for (int j = 0; j < 8; ++j)
            for (int i = 0; i < 8; ++i) e.probes.push_back({(2 * i + 1) * spec.width / 16, (2 * j + 1) * spec.height / 16});
        entries.push_back(std::move(e));
    }
    // `art` pointers must point into the entries' own copies of the spec.
    for (auto& e : entries) e.art = art == Art::Pc98 ? &e.spec.pc98 : &e.spec.mac;
}

// Fraction of the compared pixels (every `step`th, skipping transparent ones) equal to the picture
// placed at (x0, y0).
float Substitution::Impl::match_at(const FrameView& f, const Entry& e, int x0, int y0, int step) const {
    long n = 0, eq = 0, samples = 0;
    for (int y = 0; y < e.ref.height; y += step) {
        const std::uint8_t* row = f.pixels + static_cast<std::size_t>(y0 + y) * f.width + x0;
        const std::uint8_t* ref = e.ref.pixels.data() + static_cast<std::size_t>(y) * e.ref.width;
        for (int x = (y / step) % step; x < e.ref.width; x += step) {
            ++samples;
            const std::uint8_t v = row[x];
            if (v == kTransparent) continue;
            ++n;
            eq += v == ref[x];
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
    for (const auto& e : impl_->entries) out.push_back(e.spec.id);
    return out;
}

const std::vector<std::string>& Substitution::warnings() const { return impl_->warnings; }

const std::vector<Substitution::Found>& Substitution::found() const { return impl_->found; }

void Substitution::set_program_memory(const std::uint8_t* ram) { impl_->ram = ram; }

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
    for (auto& e : m.entries) {
        const auto& s = e.spec;
        if (s.frame_w != f.width || s.frame_h != f.height) {
            e.active = false;
            continue;
        }
        // The dashboard, from the program image (load segment 1000h). Until it has been recognised
        // once it is read again each time: before the EXEPACK stub has run, the bytes are still packed.
        if (!s.file && !e.confirmed && m.ram)
            e.ref_ok = decode_dos_dash(std::span<const std::uint8_t>(m.ram + 0x10000, 0x100000 - 0x10000), e.ref);
        if (!e.ref_ok) continue;
        float ratio = 0;
        if (!m.locate(f, e, ratio)) {
            e.active = false;
            continue;
        }
        if (!s.file) e.confirmed = true;
        if (s.background) {
            if (ratio > best_ratio) {
                if (best_bg) best_bg->active = false;
                best_bg = &e;
                best_ratio = ratio;
            } else {
                e.active = false;
            }
            continue;
        }
        e.active = true;
        matches.push_back({&e, e.last});
        m.found.push_back({s.id, ratio, e.last});
    }
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
        const FRect dst = place(match.rect, e.image.width, e.image.height, a.fit, w, h);
        out.layers.push_back({&e.image, src, dst});
        if (&match != &matches.front()) covers.push_back({out.pieces.size(), dst});
        // An inset's art can be larger than the DOS picture (Fit::Cover): what the frame showed
        // around the picture mustn't be drawn over it.
        if (&match != &matches.front() || !full) {
            const IRect under = clip({static_cast<int>(std::floor(dst.x)), static_cast<int>(std::floor(dst.y)),
                                      static_cast<int>(std::ceil(dst.w)) + 1, static_cast<int>(std::ceil(dst.h)) + 1},
                                     w, h);
            for (int y = under.y; y < under.y + under.h; ++y) {
                std::fill_n(out.over.begin() + static_cast<std::ptrdiff_t>(y) * w + under.x, under.w, kTransparent);
                if (!out.base.empty() && &match != &matches.front())
                    std::fill_n(out.base.begin() + static_cast<std::ptrdiff_t>(y) * w + under.x, under.w, kTransparent);
            }
        }

        // The DOS pixels on top: wherever the frame differs from the DOS picture, except where a
        // later inset (a crash picture over the dashboard) is: those pixels are the inset's.
        std::vector<std::uint8_t> mask(static_cast<std::size_t>(r.w) * r.h);
        for (int y = 0; y < r.h; ++y)
            for (int x = 0; x < r.w; ++x) {
                const std::uint8_t v = f.pixels[static_cast<std::size_t>(r.y + y) * w + r.x + x];
                const std::uint8_t ref = e.ref.pixels[static_cast<std::size_t>(r.y + y - match.rect.y) * e.ref.width +
                                                      (r.x + x - match.rect.x)];
                mask[static_cast<std::size_t>(y) * r.w + x] = v != kTransparent && v != ref;
            }
        for (const Match* later = &match + 1; later != matches.data() + matches.size(); ++later) {
            const IRect c = clip(later->rect, w, h);
            for (int y = std::max(c.y, r.y); y < std::min(c.y + c.h, r.y + r.h); ++y)
                for (int x = std::max(c.x, r.x); x < std::min(c.x + c.w, r.x + r.w); ++x)
                    mask[static_cast<std::size_t>(y - r.y) * r.w + (x - r.x)] = 0;
        }
        if (a.close_holes && r.w > 0 && r.h > 0) close_mask(mask, r.w, r.h);
        for (int y = 0; y < r.h; ++y)
            for (int x = 0; x < r.w; ++x) {
                const std::size_t p = static_cast<std::size_t>(r.y + y) * w + r.x + x;
                out.over[p] = mask[static_cast<std::size_t>(y) * r.w + x] ? f.pixels[p] : kTransparent;
            }
        std::vector<IRect> keep = a.keep;
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
                    out.moved[p] = v == rm.ignore ? kTransparent : v;
                    out.over[p] = kTransparent;
                }
            out.pieces.push_back({c, art_to_frame(dst, src, rm.art)});
        }
        for (const auto& hd : a.hide) {
            const IRect c = clip(hd, w, h);
            for (int y = c.y; y < c.y + c.h; ++y)
                std::fill_n(out.over.begin() + static_cast<std::ptrdiff_t>(y) * w + c.x, c.w, kTransparent);
        }
        for (std::size_t i = 0; i < a.indicators.size(); ++i) {
            const auto& ind = a.indicators[i];
            const IRect c = clip(ind.dos, w, h);
            int count = 0;
            for (int y = c.y; y < c.y + c.h; ++y)
                for (int x = c.x; x < c.x + c.w; ++x) count += f.pixels[static_cast<std::size_t>(y) * w + x] == ind.color;
            if (count >= ind.min_pixels) {
                const Image& img = e.indicator_images[i];
                out.layers.push_back({&img, {0, 0, img.width, img.height}, art_to_frame(dst, src, ind.art)});
            }
        }
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
