#pragma once
// Objects of one's own (ui/object_editor.h): the original's data-driven 3D models (segment 245A: the
// cars, the pedestrians and the landmark buildings; notes 05 section 6) reshaped vertex by vertex and
// face by face, kept as a pack in a text file, and put into the game as it starts in place of the
// original's. Both the original's renderer and the Enhanced view draw them (the latter reads the
// models from the game's memory).
//
// A model: vertices in the model's axes (x east, y down, z north; 0-3 the frame the original works
// out the view's octant from, never changed), faces (a fill or polyline kind, a colour, an outline
// colour, and one or more polygons or lines of vertex indices), and for each of the 8 view octants the
// faces to draw, back to front (the original has no depth buffer: it paints them in that order).
// Edited models are written to segment 8000h, which nothing of the game's uses, and the model's
// 8-byte header in segment 245A is pointed at them (the original reads every other address from it).
//
// Code-drawn objects (the city's ordinary buildings, its street lamps: routines in segment 3009 that draw
// with the renderer's own primitives; enhanced/object_models.h makes models of them) can be played as
// models too: a set's `objects`, by routine. Each gets a model number after the original's (kModelCount
// on, in the routines' order), in table slots where the Chinatown gate's model data sat (that moves to
// segment 8000h with the edited ones), and the game draws it in place of the routine: at the routine's
// entry, the model number in AX and on to 3009:B9E6, as the original's own model objects go (B92A).

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vette::host {
class Cpu;
class Machine;
class Memory;
}

namespace vette::game {

struct ModelData {
    using Vertex = std::array<int16_t, 3>;
    struct Face {
        // Bit 0: culled when its winding shows its back; bits 1-3: kind (0 convex fill, 2 concave fill,
        // 4 and 6 polylines); bit 13: outlined in `outline`; bit 14: never drawn; bit 15: screen door
        // (every other pixel).
        uint16_t flags = 0;
        uint8_t colour = 0;   // EGA colour 0-15 (a high nibble: dithered with that colour)
        uint8_t outline = 0;  // EGA colour of the outline
        std::vector<std::vector<uint16_t>> prims;  // vertex indices: polygons (no closing repeat) or polylines
        bool lines() const { return (flags & 0x0E) >= 4; }
        bool operator==(const Face&) const = default;
    };
    std::vector<Vertex> verts;
    std::vector<Face> faces;
    // Per view octant (bit 0: the camera on the west (-x), bit 1: north (+z), bit 2: below (+y)), the
    // faces to draw, back to front. Empty: made from the faces (make_orders) when written.
    std::array<std::vector<uint16_t>, 8> order;
    bool operator==(const ModelData&) const = default;
};

constexpr int kModelCount = 59;  // the model table's entries (245A:6FF8, 8 bytes each)
constexpr int kMaxObjects = 41;  // code-drawn objects as models: table slots in the room freed for them
constexpr int kReferenceVertices = 4;
constexpr int kMaxVertices = 128;  // the original's projected-vertex buffer (DS:1A86)
constexpr int kMaxPoints = 16;     // per polygon, as the original's own models

// The model's name (as identified from the Mac version's names, notes 05), or "object N".
std::string model_name(int id);

// Model `id` as the game has it (its nearest detail), from memory once VETTE.EXE has unpacked itself.
std::optional<ModelData> read_model(host::Memory& memory, int id);

// Every octant's order from the faces: the drawn ones (not bit 14), farthest first from a viewpoint
// out in that octant, and a face painted on another (a window on its wall: in its plane) after it.
void make_orders(ModelData& model);

// What's wrong with a model for the game (too many vertices, a face with too many points or an index
// out of range, the reference frame moved), or empty if nothing.
std::string check_model(const ModelData& model);

struct ModelPack {
    std::map<int, ModelData> models;  // by model id; the ones not here stay the original's
    std::map<uint16_t, ModelData> objects;  // code-drawn objects drawn as these models, by routine (3009)
    std::string serialize() const;
    static std::optional<ModelPack> parse(const std::string& text, std::string& error);
};

// The model number each of the pack's objects is drawn with: {routine, model}, in routine order.
std::vector<std::pair<uint16_t, int>> object_models(const ModelPack& pack);

// The drawing of the pack's objects as models (their routines' entries sent to 3009:B9E6), on a CPU that
// runs the game's drawing: install_model_pack does the game's; game::SmoothRenderer's replays need it too.
void install_object_draws(host::Cpu& cpu, const ModelPack& pack);

// Writes the pack's models into memory: their data in segment 8000h, their headers in 245A pointed at
// it. False (with the reason) if a model is wrong (check_model) or they don't fit in 64 KB.
bool write_models(host::Memory& memory, const ModelPack& pack, std::string& error);

// Plays with the pack's models: written as the game starts (3009:0025, after the image unpacks).
void install_model_pack(host::Machine& machine, ModelPack pack);

// Runs a machine that has just booted until VETTE.EXE has unpacked itself and started (3009:0025), its
// models then in place; false if it hasn't within `max_ns` of emulated time.
bool run_until_started(host::Machine& machine, uint64_t max_ns = 10'000'000'000);

}  // namespace vette::game
