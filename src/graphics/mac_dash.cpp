// The Mac dashboard drawn from the DOS game's state: the speedometer and tachometer arcs and their
// digits, the cruise/automatic lights, the steering arrows, the shift light, the left hand on the
// wheel and the right hand on the gear shifter with the gear gate. The Mac Color VETTE! keeps these
// as separate pictures (re/notes/10-graphics.md); their places on the dashboard picture (24055,
// 512x146) were found by matching the "off" pictures against it, or chosen where the Mac's layout
// has room for them.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <deque>
#include <limits>

#include "graphics/game_state.h"
#include "graphics/screen_handler.h"

namespace vette::graphics {

namespace {

constexpr int kSteps = 64;  // gauge fill resolution

// Places on the Mac dashboard picture.
constexpr IRect kSpeedGauge{120, 75, 56, 61};
constexpr IRect kRpmGauge{272, 75, 72, 61};
constexpr int kDigitW = 8, kDigitH = 13;  // PICT 18439: "0123456789" in 80x13
constexpr int kSpeedDigitsRight = 176, kRpmDigitsRight = 340, kDigitsTop = 108;
constexpr IRect kCruise{184, 70, 32, 7}, kAuto{232, 70, 24, 7};
constexpr IRect kLeftArrow{192, 91, 16, 12}, kRightArrow{232, 91, 24, 12};
constexpr IRect kShiftLight{212, 40, 16, 18};
constexpr IRect kHands{0, 36, 120, 110};
constexpr IRect kShifter{390, 83, 120, 63};
constexpr int kGateX = 31, kGateY = 13;  // the gear gate on the shifter knob (PICT 9999 minus its border)

// What the DOS dashboard shows, as rectangles of the frame (mode 0Dh, the dashboard at rows 120-199):
// a light is on when the game drew over the dashboard picture there.
constexpr IRect kDosCruise{112, 136, 16, 7}, kDosAuto{128, 136, 16, 7};
constexpr IRect kDosLeftArrow{112, 170, 16, 10}, kDosRightArrow{128, 170, 16, 10};
constexpr IRect kDosShiftLight{144, 144, 16, 10};
constexpr IRect kDosShifter{232, 170, 88, 30};

// Where the gauges' bands reach at their scale marks (fraction of the band, from the steps below):
// measured at the Mac pictures' label dots (speed) and numbers (revs).
struct Mark {
    double value, t;
};
constexpr Mark kSpeedMarks[] = {{0, 0}, {5, 0.01}, {15, 0.145}, {25, 0.28}, {35, 0.41}, {45, 0.555},
                                {55, 0.69}, {65, 0.81}, {75, 0.90}, {85, 0.96}, {95, 1.0}};
constexpr Mark kRpmMarks[] = {{0, 0.08}, {10, 0.227}, {20, 0.36}, {30, 0.493}, {40, 0.64},
                              {50, 0.72}, {60, 0.84}, {70, 1.0}};

template <std::size_t N>
double band_position(const Mark (&marks)[N], double v) {
    if (v <= marks[0].value) return v < marks[0].value ? 0.0 : marks[0].t;
    for (std::size_t i = 1; i < N; ++i)
        if (v <= marks[i].value)
            return marks[i - 1].t + (v - marks[i - 1].value) / (marks[i].value - marks[i - 1].value) * (marks[i].t - marks[i - 1].t);
    return 1.0;
}

// The lit band of a gauge (where `full` differs from `empty`, the dashboard under it), revealed from
// its start (the bottom row) along the band: kSteps + 1 images, step k showing the first k/kSteps.
std::vector<Image> gauge_steps(const Image& full, const Image& empty) {
    const int w = full.width, h = full.height;
    const auto n = static_cast<std::size_t>(w) * h;
    std::vector<std::uint8_t> band(n, 0);
    int bottom = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            const std::uint32_t e = x < empty.width && y < empty.height ? empty.at(x, y) : 0;
            if ((full.pixels[i] >> 24) != 0 && full.pixels[i] != e) {
                band[i] = 1;
                bottom = y;
            }
        }
    // Distance along the band (8-connected steps) from its bottom rows.
    constexpr int kFar = std::numeric_limits<int>::max();
    std::vector<int> dist(n, kFar);
    std::deque<std::size_t> queue;
    for (int y = std::max(bottom - 1, 0); y <= bottom && bottom >= 0; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            if (band[i]) {
                dist[i] = 0;
                queue.push_back(i);
            }
        }
    while (!queue.empty()) {
        const std::size_t i = queue.front();
        queue.pop_front();
        const int x = static_cast<int>(i % static_cast<std::size_t>(w)), y = static_cast<int>(i / static_cast<std::size_t>(w));
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx, ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                const std::size_t j = static_cast<std::size_t>(ny) * w + nx;
                if (band[j] && dist[j] == kFar) {
                    dist[j] = dist[i] + 1;
                    queue.push_back(j);
                }
            }
    }
    // Parts of the band not connected to its start (the tachometer's red marks) continue from the
    // nearest connected pixel.
    std::vector<int> final_dist = dist;
    for (std::size_t i = 0; i < n; ++i) {
        if (!band[i] || dist[i] != kFar) continue;
        const int x = static_cast<int>(i % static_cast<std::size_t>(w)), y = static_cast<int>(i / static_cast<std::size_t>(w));
        double best = 1e18;
        for (std::size_t j = 0; j < n; ++j) {
            if (dist[j] == kFar) continue;
            const double dx = static_cast<double>(x - static_cast<int>(j % static_cast<std::size_t>(w)));
            const double dy = static_cast<double>(y - static_cast<int>(j / static_cast<std::size_t>(w)));
            best = std::min(best, dist[j] + std::sqrt(dx * dx + dy * dy));
        }
        final_dist[i] = best < 1e17 ? static_cast<int>(best) : 0;
    }
    int longest = 1;
    for (std::size_t i = 0; i < n; ++i)
        if (band[i]) longest = std::max(longest, final_dist[i]);
    std::vector<Image> steps(kSteps + 1);
    for (int k = 0; k <= kSteps; ++k) {
        Image& s = steps[static_cast<std::size_t>(k)];
        s.width = w;
        s.height = h;
        s.pixels.assign(n, 0);
        for (std::size_t i = 0; i < n; ++i)
            if (band[i] && static_cast<long long>(final_dist[i]) * kSteps <= static_cast<long long>(longest) * k)
                s.pixels[i] = full.pixels[i];
    }
    return steps;
}

class MacDash final : public ScreenHandler {
public:
    bool load(const ArtFiles& files, std::vector<std::string>& warnings) {
        const Image dash = load_mac_picture(files, 24055, warnings);
        const Image speed_full = load_mac_picture(files, 16972, warnings);
        const Image rpm_full = load_mac_picture(files, 4612, warnings);
        digits_ = load_mac_picture(files, 18439, warnings);
        cruise_ = load_mac_picture(files, 9582, warnings);
        auto_ = load_mac_picture(files, 7166, warnings);
        left_ = load_mac_picture(files, 5080, warnings);
        right_ = load_mac_picture(files, 23259, warnings);
        shift_on_ = load_mac_picture(files, 10353, warnings);
        shift_off_ = load_mac_picture(files, 21439, warnings);
        shifter_ = crop(load_mac_picture(files, 9999, warnings), 1, 1, kShifter.w, kShifter.h);
        static constexpr int kHandIds[5] = {16269, 3738, 16018, 27381, 30004};
        for (std::size_t i = 0; i < hands_.size(); ++i) hands_[i] = load_mac_picture(files, kHandIds[i], warnings);
        for (std::size_t i = 0; i < gates_.size(); ++i)
            gates_[i] = load_mac_picture(files, 10000 + static_cast<int>(i), warnings);
        if (dash.empty() || speed_full.empty() || rpm_full.empty() || digits_.empty()) return false;
        speed_ = gauge_steps(speed_full, crop(dash, kSpeedGauge.x, kSpeedGauge.y, kSpeedGauge.w, kSpeedGauge.h));
        rpm_ = gauge_steps(rpm_full, crop(dash, kRpmGauge.x, kRpmGauge.y, kRpmGauge.w, kRpmGauge.h));
        return true;
    }

    void compose(const HandlerInput& in, Composite& out) override {
        const DashState s = read_dash_state(in.ram);
        const auto place = [&](const Image& img, int x, int y) {
            if (!img.empty())
                out.layers.push_back({&img, {0, 0, img.width, img.height},
                                      art_to_frame(in.dst, in.art_w, in.art_h, {x, y, img.width, img.height})});
        };
        // Changed DOS pixels in a rectangle of the frame: the DOS game lit something there.
        const auto dos_lit = [&](const IRect& r, int min_pixels) {
            int changed = 0;
            for (int y = r.y; y < r.y + r.h; ++y)
                for (int x = r.x; x < r.x + r.w; ++x) {
                    const int ry = y - in.rect.y, rx = x - in.rect.x;
                    if (rx < 0 || ry < 0 || rx >= in.ref.width || ry >= in.ref.height || x >= in.frame.width ||
                        y >= in.frame.height)
                        continue;
                    const std::uint8_t v = in.frame.pixels[static_cast<std::size_t>(y) * in.frame.width + x];
                    changed += v != kTransparent && v != in.ref.pixels[static_cast<std::size_t>(ry) * in.ref.width + rx];
                }
            return changed >= min_pixels;
        };

        // The left hand on the wheel, as the DOS game picks its glove picture (3009:5F39).
        const std::size_t hand = s.steering < 0 ? 4 : static_cast<std::size_t>(std::min(s.steering >> 1, 3));
        place(hands_[hand], kHands.x, kHands.y);

        // Speedometer and tachometer arcs, and the numbers (two digits at least, like the DOS game's).
        const auto step = [](double t) { return static_cast<std::size_t>(std::lround(std::clamp(t, 0.0, 1.0) * kSteps)); };
        if (in.ram) {
            if (s.speed_mph > 0) place(speed_[step(band_position(kSpeedMarks, s.speed_mph))], kSpeedGauge.x, kSpeedGauge.y);
            if (s.rpm100 > 0) place(rpm_[step(band_position(kRpmMarks, s.rpm100))], kRpmGauge.x, kRpmGauge.y);
            digits(out, in, std::clamp(s.speed_mph, 0, 999), kSpeedDigitsRight);
            digits(out, in, std::clamp(s.rpm100, 0, 99), kRpmDigitsRight);
        }

        // Lights, as the DOS dashboard shows them.
        if (dos_lit(kDosCruise, 6)) place(cruise_, kCruise.x, kCruise.y);
        if (dos_lit(kDosAuto, 6)) place(auto_, kAuto.x, kAuto.y);
        if (dos_lit(kDosLeftArrow, 6)) place(left_, kLeftArrow.x, kLeftArrow.y);
        if (dos_lit(kDosRightArrow, 6)) place(right_, kRightArrow.x, kRightArrow.y);
        place(dos_lit(kDosShiftLight, 6) ? shift_on_ : shift_off_, kShiftLight.x, kShiftLight.y);

        // The right hand on the shifter while the DOS game shows it (after a shift, in neutral), with
        // the gear gate of the car's gearbox (4, 5 or 6 speeds) and the gear lit.
        if (dos_lit(kDosShifter, 150)) {
            place(shifter_, kShifter.x, kShifter.y);
            if (in.ram) {
                const int max = std::clamp(s.max_gear, 4, 6);
                const int base = max == 4 ? 0 : max == 5 ? 6 : 13;
                const int gear = std::clamp(s.gear, 0, max + 1);
                place(gates_[static_cast<std::size_t>(base + gear)], kShifter.x + kGateX, kShifter.y + kGateY);
            }
        }
    }

private:
    // A number right-aligned at `right` in the Mac's LCD digits.
    void digits(Composite& out, const HandlerInput& in, int value, int right) const {
        int x = right;
        for (int n = 0; n < 2 || value > 0; ++n) {
            x -= kDigitW;
            const int d = value % 10;
            out.layers.push_back({&digits_, {d * kDigitW, 0, kDigitW, kDigitH},
                                  art_to_frame(in.dst, in.art_w, in.art_h, {x, kDigitsTop, kDigitW, kDigitH})});
            value /= 10;
        }
    }

    std::vector<Image> speed_, rpm_;
    Image digits_, cruise_, auto_, left_, right_, shift_on_, shift_off_, shifter_;
    std::array<Image, 5> hands_;
    std::array<Image, 21> gates_;
};

}  // namespace

std::unique_ptr<ScreenHandler> make_mac_dash(const ArtFiles& files, std::vector<std::string>& warnings) {
    auto dash = std::make_unique<MacDash>();
    if (!dash->load(files, warnings)) return nullptr;
    return dash;
}

}  // namespace vette::graphics
