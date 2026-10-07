#pragma once
// Enhanced renderer: the race's 3D view drawn from the extracted World at any resolution and draw
// distance (the Extended and Maximum draw distance settings).
//
// The CPU builds the view as 2D triangles in back-to-front order: the original's painter's algorithm
// (re/notes/03-renderer-and-visibility.md), walked over a larger cell window, with floating-point
// transform and projection. The GPU only rasterizes them, at the display's resolution (Presenter,
// SDL_RenderGeometryRaw, no depth test). Vertices are in the original's screen coordinates, so the
// view lines up with everything the game draws around and over it (dash, mirror, messages), which the
// Presenter composites from the game's own frame.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "enhanced/world.h"

namespace vette::enhanced {

// A vertex in race-frame coordinates: the original's 320x200 screen, x right, y down, in floats, so
// the view can be drawn at any resolution (scale by output pixels per race-frame pixel).
struct SceneVertex {
    float x = 0, y = 0;
    float r = 0, g = 0, b = 0, a = 1;  // the layout of SDL_FColor
    // For the GPU's depth test: larger is nearer, 0 never hides anything. Test: greater or equal, against a
    // buffer cleared to 0 (written by translucent faces too). depth = kNear / z * (1 + s * kDepthStep): z the
    // vertex's camera depth (kNear = 1, the near plane, where the primitive was clipped), interpolated linearly
    // on screen (1/z is affine there); s the primitive's layer, 0..kMaxDepthLayer: one above the highest
    // earlier primitive of its group that it lies on (coplanar and overlapping on screen: a marking on a road,
    // a window on a wall, an outline on a face), else 0. The backdrop (ground, water, sky) is 0. At most
    // 1 + kMaxDepthLayer * kDepthStep.
    float depth = 0;
};
inline constexpr float kDepthStep = 1.0f / 1536;
inline constexpr int kMaxDepthLayer = 7;

struct Scene {
    std::vector<SceneVertex> vertices;
    std::vector<int32_t> indices;  // triangles, 3 indices each, drawn in this order with no depth test
    // The 3D viewport in race-frame pixels, [x0, x1) x [y0, y1). Triangles can extend past it: the
    // Presenter scissors them to it.
    int view_x0 = 0, view_y0 = 0, view_x1 = 320, view_y1 = 120;

    struct Stats {
        int cells = 0;
        int objects = 0;
        int vehicles = 0;
        int triangles = 0;
        double milliseconds = 0;  // build()
        // Added by the SceneBuilder implementation:
        int cells_culled = 0;  // within the radius but outside the view frustum
        int lines = 0;         // line segments (2 triangles each, included in `triangles`)
        int lines_dropped = 0; // shorter than SceneOptions::min_line_length
        int models = 0;        // segment-245A model instances (static and vehicles)
        int slices = 0;        // freeway road slices drawn (highway mode)
        bool city = false;     // the city was drawn (not a freeway alone)
        int max_layer = 0;     // the highest depth layer used (SceneVertex::depth)
        int replicas = 0;      // copies of traffic and pedestrians considered (SceneOptions::replicas)
        int replicas_dropped = 0;  // ... left out: not on the road their moves were made for (SceneOptions::replicas)
        std::array<int, kMaxDepthLayer + 1> layers{};  // primitives per depth layer
    } stats;

    void clear() {
        vertices.clear();
        indices.clear();
        stats = {};
    }
};

class SceneHook;
class SceneObserver;

struct SceneOptions {
    // Draw distance: cells around the camera's cell; kMapCells or more draws the whole map.
    int radius = kMapCells;
    // Output pixels per race-frame pixel, horizontally and vertically (for line widths).
    float pixel_w = 1, pixel_h = 1;
    // Lines (outlines, barrier edges, posts, cables, model polylines) are drawn on screen, line_world_width
    // world units wide in perspective, at most line_max race-frame pixels (thin at high resolution: the
    // original's whole-pixel weight looks heavy there) and at least line_width output pixels or a quarter
    // of a race-frame pixel, whichever is more (so they stay visible). At the original's resolution, both
    // limits come to one pixel.
    float line_width = 1.5f;
    float line_world_width = 2.5f;
    float line_max = 0.35f;
    // Markings painted on the ground (the horizontal lines at the ground level of the ground layer's
    // objects, of the bridges' pieces and of the freeway's slices: lane dashes, centre and kerb lines,
    // crossings) are flat stripes this wide in world units (1 unit is about 3 inches), lying on the road
    // and foreshortened with it. Where a stripe projects thinner than an output pixel it is drawn as a
    // line as well.
    float marking_width = 1.25f;

    // Added by the SceneBuilder implementation:
    // Line segments that project shorter than this (output pixels) are left out: at long range the
    // lane-marking dashes would only add speckle (each would be a line_width square).
    float min_line_length = 0.5f;
    // Traffic and pedestrians are a pattern that repeats every 4 cells (notes 03, "Traffic"). Off: each
    // entity once, where the original binds it, so a far car jumps when its nearest copy changes, and
    // pedestrians show only in the original's window. On: also every other cell of its pattern within the
    // radius where the road along the entity's way (a car's loop, a pedestrian's square) is the one its
    // moves were made for (the commonest of the pattern's cells with the most of the way on road), the
    // window's cells as the original binds them: the city populated, without cars on the water, off their
    // road or against their lane. The choice doesn't depend on the camera, so copies don't pop as it moves.
    bool replicas = false;
    // Off: traffic and pedestrians only in the original's window, where the original draws them (they
    // appear as the camera nears). Without a depth buffer, far ones could show through the scenery.
    bool far_vehicles = true;
    // SceneVertex::depth for a depth buffer. Off: every depth is 0 (drawn in painter's order; it saves the
    // layer bookkeeping).
    bool depth = true;
    // The ground quad of every big tile in its ground colour (DS:8556), drawn first, over water that
    // extends beyond the map's edge to the horizon.
    bool ground = true;
    // Validation: walk only the original's own cell window (<= 6 cells, its order, its depth and
    // lateral culls, compounds drawn whole from the first cell that lists them, models as the far box
    // beyond sort key 800h, no ground quads, its integer rounding of camera space and projection), and
    // let `hook` pick each object's variant the way the original's routine does.
    bool original_window = false;
    SceneHook* hook = nullptr;
    // Validation: told about every copy of traffic and pedestrians considered (SceneObserver).
    SceneObserver* observer = nullptr;

    // The rear-view mirror (draw_mirror_view 3009:0666) instead of the main view, from the same frame:
    // the camera turned round (yaw +180, or +-95 while looking sideways, by DS:2B87), pitch negated,
    // the picture mirrored left to right, in the mirror's own viewport (DS:35A3 / 3587 / 3595), over a
    // sky of its own (the original fills it with sky and ground, without the horizon panorama).
    bool mirror = false;

    // Freeway traffic, smooth: the original puts a new highway car on the road in plain view of the long
    // draw distance and takes one off as it gets more than 22 slices ahead (or 7 behind). Each then fades
    // in where it comes, and one taken off goes on along the road at its speed while it fades out. The
    // fades run by `time_s`, the frame's emulated time (so the mirror's build agrees with the main view's).
    bool smooth_traffic = false;
    double time_s = 0;
};

// Validation only: chooses each static object's variant as the original would (vette_world runs the
// original routine on a copy of the frame). Called in draw order.
class SceneHook {
public:
    struct Object {
        uint16_t routine = 0;
        std::array<int16_t, 3> position{};  // the original's 16-bit position (camera big-tile frame)
        uint16_t sort_key = 0;              // cs:259E, for sortables
        bool sortable = false;              // list2 entry (else list1 or a compound piece)
        uint8_t facing = 0;                 // DS:35C3
        bool own_cell = false;              // DS:35C4
    };
    virtual ~SceneHook() = default;
    // The variant the original draws (nullptr: nothing), and the yaw, pitch and roll of each of its plain
    // lists, in order (billboards and animations).
    virtual const Variant* choose(const Object& object, std::vector<std::array<int16_t, 3>>& plain_angles) = 0;
};

// Validation: the traffic and pedestrians the original's window draws, and every copy of them that
// SceneOptions::replicas considers, kept or left out by the layout rule.
class SceneObserver {
public:
    struct Copy {
        uint16_t entity = 0;
        int cell = 0;                  // gx * kMapCells + gy
        int32_t x = 0, y = 0;          // world position
        float view_x = 0, view_y = 0, view_z = 0;  // camera space: right, down, ahead (its base)
        bool kept = false;
        bool window = false;           // the window's own (always kept)
    };
    virtual ~SceneObserver() = default;
    virtual void copy(const Copy& c) = 0;
};

// The colour a Colour byte becomes: the default EGA palette, a dithered pair as the average of its two
// colours (0..1 floats). Screen-door model faces use the base colour at alpha 0.5.
struct SceneColour {
    float r = 0, g = 0, b = 0;
};
SceneColour scene_colour(Colour c);
SceneColour ega_colour(int index);  // 0..15

class SceneBuilder {
public:
    explicit SceneBuilder(const World& world);
    ~SceneBuilder();
    SceneBuilder(const SceneBuilder&) = delete;
    SceneBuilder& operator=(const SceneBuilder&) = delete;

    // Builds the 3D view of one frame into `out` (cleared first). `ram`: the machine's 1 MB memory at
    // draw_world_cells' entry (3009:30C6) of that frame. The camera, angles, game flags and the live
    // vehicles and pedestrians are read from it; nothing is written.
    //
    // In highway mode (DS:2AD4 = FF, the named freeways) `ram` is the memory at the frame loop's highway
    // branch (3009:0342), before highway_frame (775E) runs. The view is then the freeway: its road
    // slices built from the route's segment list (the original's ring of 32 slices, continued to the
    // draw distance in both directions), the highway cars (DS:82F4) and, in external views, the
    // player's car. The city is drawn as well once the end of the road is in sight (DS:8411).
    void build(const uint8_t* ram, const SceneOptions& options, Scene& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vette::enhanced
