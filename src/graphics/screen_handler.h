#pragma once
// Screens whose replacement depends on the game's state: the Mac dashboard (gauges, lights, hands
// and gear shifter drawn from the game's values) and the Mac course map (the route and course box of
// the course on show). They run after the table-driven substitution of their screen and add layers
// and moved pieces to its composite. Internal to the graphics library.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "graphics/art_files.h"
#include "graphics/composite.h"
#include "graphics/dos_art.h"
#include "graphics/substitution.h"

namespace vette::graphics {

struct HandlerInput {
    const FrameView& frame;
    const std::uint8_t* ram;  // the emulator's memory, or nullptr (then only what needs no state)
    const DosPicture& ref;    // the DOS picture that was recognised
    IRect rect;               // where it is, in frame pixels
    FRect dst;                // where its art went, in frame pixels
    int art_w, art_h;         // the art's size in its own pixels
};

class ScreenHandler {
public:
    virtual ~ScreenHandler() = default;
    virtual void compose(const HandlerInput& in, Composite& out) = 0;
};

// nullptr, with a warning, when a picture they need is missing.
std::unique_ptr<ScreenHandler> make_mac_dash(const ArtFiles& files, std::vector<std::string>& warnings);
std::unique_ptr<ScreenHandler> make_mac_map(const ArtFiles& files, std::vector<std::string>& warnings);

// --- Shared helpers ----------------------------------------------------------------------------------

// A rectangle of an art image placed at `dst` (art_w x art_h) in frame pixels.
FRect art_to_frame(const FRect& dst, int art_w, int art_h, const IRect& a);
// A Mac picture as an image (undrawn pixels transparent), or empty with a warning.
Image load_mac_picture(const ArtFiles& files, int id, std::vector<std::string>& warnings);
// A frame for highlighting a rectangle: `thickness` pixels of `argb`, transparent inside.
Image frame_image(int w, int h, std::uint32_t argb, int thickness = 2);
// Moves DOS frame pixels into the composite: the pixels of `dos` (frame rect) whose colour is in
// `colors` (bit per palette index) are copied to out.moved and drawn at `dst` (frame pixels).
void move_pixels(const FrameView& frame, const IRect& dos, std::uint16_t colors, const FRect& dst, Composite& out);

}  // namespace vette::graphics
