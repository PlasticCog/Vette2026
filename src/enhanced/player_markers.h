#pragma once
// Two-player races (VETTE! 2026's own, Tab in the race shows or hides them): the other player's name in a
// tag floating over their car, and an arrow on the ground that circles this player's car and points at
// theirs.
//
// Both are drawn as the race view's camera sees them (re/notes/03: the camera DS:2C71, the viewport, the
// rear-view mirror's camera turned round), from the memory of the frame on screen, over everything else
// in the 3D view (the tag shows through buildings: it's there to find the other car). The Enhanced view
// gets them as triangles at the display's resolution; the original's frame (Classic) has the same
// triangles drawn into its pixels. Nothing is shown on a freeway (either car's): the freeway has its own
// coordinates.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "enhanced/scene.h"

namespace vette::host {
class Machine;
}

namespace vette::enhanced {

struct MarkerOptions {
    std::string name;      // the other player's, in the tag
    bool mirror = false;   // the rear-view mirror's view instead of the main one
    // Output pixels per race-frame pixel (the tag's text is drawn in whole output pixels).
    float pixel_w = 1, pixel_h = 1;
    bool depth = false;    // SceneVertex::depth for the depth buffer (on top of everything); else 0
    // The main view's, with the rear-view mirror on: the tag keeps out from under the mirror (beside it,
    // its point slanting to the car, or below it).
    bool avoid_mirror = false;
};

// Adds the tag and the arrow to `scene` (its viewport is set to the view's), from the frame's data
// segment `ds` (DS:0000, 64 KB). False if there's nothing to show (not in the city's race view).
bool add_player_markers(const uint8_t* ds, const MarkerOptions& options, Scene& scene);

// A line of text at the top of the 3D view, centred (e.g. "Name tag and arrow: on" after Tab).
void add_banner(const uint8_t* ds, std::string_view text, const MarkerOptions& options, Scene& scene);

// Adds `from`'s triangles to `to`, cut to `from`'s viewport (e.g. the mirror's markers into the main view's
// scene, which is drawn clipped to its own viewport only).
void append_clipped(Scene& to, const Scene& from);

// Draws a scene's triangles into a frame of palette indices (Classic): each pixel whose centre a triangle
// covers takes the palette colour nearest to the triangle's (alpha under 0.5: left alone), inside the
// scene's viewport, outside `skip` (x0, y0, x1, y1; e.g. the rear-view mirror's) if it isn't empty.
void draw_scene_paletted(const Scene& scene, uint8_t* pixels, int width, int height,
                         const std::array<uint32_t, 16>& palette, std::array<int, 4> skip = {0, 0, 0, 0});

// The memory of the race frame on screen (Classic, without the Smooth frame rate): the data segment as it
// was when the game drew that frame's world (3009:036E), kept until the frame is shown (the page flip,
// 0546), and whether the rear-view mirror was drawn in it.
class ShownFrame {
public:
    explicit ShownFrame(host::Machine& machine);
    ~ShownFrame();
    ShownFrame(const ShownFrame&) = delete;
    ShownFrame& operator=(const ShownFrame&) = delete;

    // The frame's DS (64 KB), or null if no race frame (in the city) has been shown in the last half second.
    const uint8_t* ds(uint64_t now_ns, bool& mirror) const;

private:
    struct State;
    host::Machine& machine_;
    std::shared_ptr<State> state_;
    std::vector<uint64_t> watches_;
};

}  // namespace vette::enhanced
