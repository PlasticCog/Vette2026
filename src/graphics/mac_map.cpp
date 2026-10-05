// The Mac course map (PICT 26478, an aerial view of San Francisco with buttons for the four
// courses) for the DOS map screen (MAPPIC.BIN). The DOS game shows one course at a time (DS:FD10,
// changed with the arrow keys); for it the Mac draws the course's route line and its course box.
// Places from the Mac application's initialised globals (CODE 10's A5 data: the route and box
// rectangles per course, the button rectangles). The box's text is in the picture as text opcodes;
// it is drawn here with the DOS game's own font. The DOS instructions ("Arrow Key to view", "Enter
// Key selects") move to the bar under the map.

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

#include "assets/pict.h"
#include "graphics/game_state.h"
#include "graphics/screen_handler.h"

namespace vette::graphics {

namespace {

struct Course {
    int route;      // PICT: the route as a 1-bit line drawing with START/FINISH labels
    IRect route_at;
    int box;        // PICT: rounded box with "Course One:", "Start: ...", "Finish: ..."
    IRect box_at;
    IRect button;   // "COURSE n" on the map
};
constexpr std::array<Course, 4> kCourses = {{
    {4208, {0, 92, 289, 92}, 6398, {300, 10, 193, 100}, {38, 297, 64, 20}},
    {4358, {175, 92, 337, 91}, 5383, {30, 150, 193, 100}, {132, 297, 64, 20}},
    {19759, {0, 164, 512, 76}, 27402, {150, 10, 193, 100}, {226, 297, 64, 20}},
    {30266, {0, 97, 499, 143}, 15714, {300, 10, 193, 100}, {320, 297, 64, 20}},
}};
constexpr std::uint32_t kRouteColor = 0xFFE02828;
constexpr std::uint32_t kHighlight = 0xFFFFD020;
constexpr int kBoxScale = 4;  // course boxes are drawn at 4x so the DOS font comes near Chicago's sizes

// The DOS screen's instructions (red, colour 4) in the right-hand column.
constexpr IRect kDosArrowKeys{480, 180, 160, 10}, kDosEnterKey{480, 190, 160, 10};

class MacMap final : public ScreenHandler {
public:
    bool load(const ArtFiles& files, std::vector<std::string>& warnings) {
        bool ok = true;
        for (std::size_t c = 0; c < kCourses.size(); ++c) {
            routes_[c] = load_mac_picture(files, kCourses[c].route, warnings);
            for (auto& p : routes_[c].pixels)
                if (p >> 24) p = kRouteColor;  // the black line, in the Mac's colour for routes
            boxes_[c] = load_mac_picture(files, kCourses[c].box, warnings);
            const auto data = files.mac_pict ? files.mac_pict(static_cast<std::int16_t>(kCourses[c].box))
                                             : std::vector<std::uint8_t>{};
            assets::Pict pict;
            if (assets::decode_pict(data, pict)) texts_[c] = pict.texts;
            ok = ok && !routes_[c].empty() && !boxes_[c].empty();
        }
        button_frame_ = frame_image(kCourses[0].button.w + 6, kCourses[0].button.h + 6, kHighlight, 3);
        return ok;
    }

    void compose(const HandlerInput& in, Composite& out) override {
        const int course = read_map_course(in.ram);
        if (course == 0) return;
        const auto c = static_cast<std::size_t>(course - 1);
        const auto& k = kCourses[c];
        const auto place = [&](const Image& img, const IRect& at) {
            if (!img.empty())
                out.layers.push_back({&img, {0, 0, img.width, img.height}, art_to_frame(in.dst, in.art_w, in.art_h, at)});
        };
        place(routes_[c], k.route_at);
        if (!font_.valid) make_boxes(in.ram);
        place(texted_[c].empty() ? boxes_[c] : texted_[c], k.box_at);
        place(button_frame_, {k.button.x - 3, k.button.y - 3, k.button.w + 6, k.button.h + 6});

        // The instructions, side by side in the bar under the map.
        const float bar = in.dst.y + in.dst.h, mid = (bar + static_cast<float>(in.frame.height)) / 2;
        const float cx = in.dst.x + in.dst.w / 2;
        move_pixels(in.frame, kDosArrowKeys, 1u << 4, {cx - 170, mid - 5, 160, 10}, out);
        move_pixels(in.frame, kDosEnterKey, 1u << 4, {cx + 10, mid - 5, 160, 10}, out);
    }

private:
    // The course boxes with their text, once the game's font can be read.
    void make_boxes(const std::uint8_t* ram) {
        font_ = read_dos_font(ram);
        if (!font_.valid) return;
        for (std::size_t c = 0; c < boxes_.size(); ++c) {
            const Image& box = boxes_[c];
            Image big;
            big.width = box.width * kBoxScale;
            big.height = box.height * kBoxScale;
            big.pixels.resize(static_cast<std::size_t>(big.width) * big.height);
            for (int y = 0; y < big.height; ++y)
                for (int x = 0; x < big.width; ++x)
                    big.pixels[static_cast<std::size_t>(y) * big.width + x] = box.at(x / kBoxScale, y / kBoxScale);
            // QuickDraw places text by its baseline; the DOS font's baseline is under row 8. The title
            // is Chicago 12 (font 0, bold-looking: drawn twice) on the Mac, the rest Monaco 9.
            for (const auto& t : texts_[c]) {
                const bool title = t.font == 0;
                const int scale = title ? 4 : 3, x = t.x * kBoxScale, y = t.y * kBoxScale - 8 * scale;
                draw_text(big, font_, t.text, x, y, scale, 0xFF000000);
                if (title) draw_text(big, font_, t.text, x + scale / 2, y, scale, 0xFF000000);
            }
            texted_[c] = std::move(big);
        }
    }

    std::array<Image, 4> routes_, boxes_, texted_;
    std::array<std::vector<assets::PictText>, 4> texts_;
    Image button_frame_;
    DosFont font_;
};

}  // namespace

std::unique_ptr<ScreenHandler> make_mac_map(const ArtFiles& files, std::vector<std::string>& warnings) {
    auto map = std::make_unique<MacMap>();
    if (!map->load(files, warnings)) return nullptr;
    return map;
}

}  // namespace vette::graphics
