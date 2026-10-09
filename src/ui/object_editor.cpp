#include "ui/object_editor.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include "core/path_utf8.h"
#include "enhanced/object_models.h"
#include "enhanced/scene.h"
#include "enhanced/scene_geometry.h"
#include "enhanced/world.h"
#include "game/city_map.h"
#include "game/model_tools.h"
#include "host/machine.h"
#include "platform/gamepad.h"
#include "platform/presenter.h"
#include "ui/canvas.h"
#include "ui/editor_icons.h"
#include "ui/shortcuts.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace vette::ui {
namespace {

namespace fs = std::filesystem;
namespace en = enhanced;
using namespace theme;
using game::ModelData;
using game::ModelPack;
using game::Shape;

constexpr const char* kExtension = ".vobj";
constexpr uint32_t kSeeThrough = 0xFF00FF;  // the overlay canvas's background: the 3D views show there
constexpr uint32_t kViewBack = 0x1A2238, kPanel = 0x161C30, kBar = 0x1D2440;
constexpr uint32_t kButton = 0x232C4A, kButtonHover = 0x34406A, kViewLabel = 0xA8AFC0;
// The layout, in canvas pixels (3ds Max's): the title, the menus and the toolbar along the top, the object
// list on the left, the command panel on the right and the status bar along the bottom; the views between.
constexpr int kTitleH = 14, kMenuH = 13, kToolH = 26, kTop = kTitleH + kMenuH + kToolH, kStatusH = 30;
constexpr int kListW = 166, kSideW = 150, kSwatch = 16, kBtn = 22, kRowH = 11;
constexpr uint64_t kMessageNs = 4'000'000'000;
constexpr int kFirst = game::kReferenceVertices;  // the vertices before this are the model's frame, not shown
// The list's items: a model's id, or kObjectItem + the routine of one of the city's code-drawn objects.
constexpr int kObjectItem = 0x10000;
constexpr int kSectionRow = -2;  // the list's row heading the city's objects
constexpr double kPi = 3.14159265358979323846;
constexpr double kOrthoBack = 200000;  // a flat view's eye: behind everything
constexpr float kBackDepth = 1e-9f;    // the grid's depth: behind everything

using Vec = std::array<double, 3>;
Vec sub(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec add(const Vec& a, const Vec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec mul(const Vec& a, double k) { return {a[0] * k, a[1] * k, a[2] * k}; }
double dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec cross(const Vec& a, const Vec& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
Vec unit(const Vec& a) {
    const double l = std::sqrt(dot(a, a));
    return l > 1e-9 ? mul(a, 1 / l) : Vec{0, 0, 0};
}
Vec as_vec(const ModelData::Vertex& v) { return {double(v[0]), double(v[1]), double(v[2])}; }
ModelData::Vertex to_vertex(const Vec& p) {
    ModelData::Vertex v{};
    for (size_t c = 0; c < 3; ++c) v[c] = static_cast<int16_t>(std::clamp(std::lround(p[c]), -32000L, 32000L));
    return v;
}
Vec axis_vec(int c) {
    Vec a{0, 0, 0};
    a[static_cast<size_t>(c)] = 1;
    return a;
}
// Turns `d` round unit axis `a` by `phi` radians (Rodrigues).
Vec turn_round(const Vec& d, const Vec& a, double phi) {
    return add(add(mul(d, std::cos(phi)), mul(cross(a, d), std::sin(phi))), mul(a, dot(a, d) * (1 - std::cos(phi))));
}

bool name_char(char c) {
    const auto u = static_cast<unsigned char>(c);
    return u < 0x80 && (std::isalnum(u) || c == ' ' || c == '-' || c == '_');
}
std::string clean_name(const std::string& name) {
    std::string out;
    for (const char c : name) {
        if (name_char(c) && out.size() < 40) out += c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out;
}
fs::path pack_path(const fs::path& dir, const std::string& name) { return dir / path_from_utf8(name + kExtension); }

bool is_object(int item) { return item >= kObjectItem; }
uint16_t routine_of(int item) { return static_cast<uint16_t>(item - kObjectItem); }
// An item as the set has it, changed (nullptr: the original's).
const ModelData* edited(const ModelPack& pack, int item) {
    if (is_object(item)) {
        const auto it = pack.objects.find(routine_of(item));
        return it == pack.objects.end() ? nullptr : &it->second;
    }
    const auto it = pack.models.find(item);
    return it == pack.models.end() ? nullptr : &it->second;
}
ModelData& edit_slot(ModelPack& pack, int item) { return is_object(item) ? pack.objects[routine_of(item)] : pack.models[item]; }
void unedit(ModelPack& pack, int item) {
    if (is_object(item)) pack.objects.erase(routine_of(item));
    else pack.models.erase(item);
}
std::string hex4(uint16_t v) {
    char s[8];
    std::snprintf(s, sizeof s, "%04X", v);
    return s;
}

bool save_pack(const fs::path& dir, const std::string& name, const ModelPack& pack, std::string& error) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream out(pack_path(dir, name), std::ios::binary | std::ios::trunc);
    const std::string text = pack.serialize();
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        error = "couldn't write " + path_to_utf8(pack_path(dir, name));
        return false;
    }
    return true;
}

// --- Drawing on the canvas ------------------------------------------------------------------------------

void box(Canvas& c, int x, int y, int w, int h, uint32_t colour) {
    c.fill_rect(x, y, w, 1, colour);
    c.fill_rect(x, y + h - 1, w, 1, colour);
    c.fill_rect(x, y, 1, h, colour);
    c.fill_rect(x + w - 1, y, 1, h, colour);
}

uint32_t mix(uint32_t a, uint32_t b, double t) {
    const auto ch = [&](int s) {
        const double x = ((a >> s) & 255) * (1 - t) + ((b >> s) & 255) * t;
        return static_cast<uint32_t>(std::lround(x)) << s;
    };
    return ch(16) | ch(8) | ch(0);
}

uint32_t icon_colour(char ch) {
    switch (ch) {
    case 'w': return 0xE6EAF2;
    case 'g': return 0xA0A8BC;
    case 'd': return 0x5C6680;
    case 'y': return 0xF0C040;
    case 'r': return 0xE05A5A;
    case 'c': return 0x40C8D0;
    default: return 0xFFFFFF;
    }
}

void draw_icon(Canvas& c, int x, int y, const icons::Icon& icon, bool dim, bool flip = false) {
    for (int r = 0; r < 16; ++r) {
        for (int k = 0; k < 16; ++k) {
            const char ch = icon[r][flip ? 15 - k : k];
            if (ch == '.') continue;
            c.fill_rect(x + k, y + r, 1, 1, dim ? mix(icon_colour(ch), kPanel, 0.65) : icon_colour(ch));
        }
    }
}

// --- The views ------------------------------------------------------------------------------------------

enum class ViewKind { Perspective, User, Top, Bottom, Front, Back, Left, Right, Iso };

struct ViewInfo {
    ViewKind kind;
    const char* name;
    const char* key;
    double yaw, pitch;
    bool flat;
};
// The original's models face south (-z: the Corvette's cabin is north of its middle), so the front view
// looks north at them from the south, and their left side is to the east.
constexpr ViewInfo kViewInfo[] = {
    {ViewKind::Perspective, "Perspective", "Num 0", 215, 22, false},
    {ViewKind::Iso, "Isometric", "I", 225, 35.264, true},
    {ViewKind::Top, "Top", "Num 7", 180, 90, true},
    {ViewKind::Bottom, "Bottom", "Ctrl+Num 7", 180, -90, true},
    {ViewKind::Front, "Front", "Num 1", 180, 0, true},
    {ViewKind::Back, "Back", "Ctrl+Num 1", 0, 0, true},
    {ViewKind::Left, "Left", "Ctrl+Num 3", 90, 0, true},
    {ViewKind::Right, "Right", "Num 3", 270, 0, true},
};
const ViewInfo* view_info(ViewKind k) {
    for (const ViewInfo& v : kViewInfo) {
        if (v.kind == k) return &v;
    }
    return nullptr;
}

// A view of the model: an orbit round `target` (the eye `dist` away; a flat view at that zoom).
struct Viewport {
    ViewKind kind = ViewKind::Perspective;
    bool flat = false;
    double yaw = 215, pitch = 22, dist = 600;
    Vec target{0, 0, 0};
    void set(ViewKind k) {
        kind = k;
        if (const ViewInfo* v = view_info(k)) {
            yaw = v->yaw;
            pitch = v->pitch;
            flat = v->flat;
        }
    }
    const char* name() const {
        const ViewInfo* v = view_info(kind);
        return v ? v->name : "User";
    }
};

struct View {
    Vec cam{}, r{}, u{}, f{};
    double focal = 1, cx = 0, cy = 0, scale = 1;  // scale: a flat view's pixels per unit
    bool flat = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // its rectangle, output pixels
    // Model point -> output pixel and camera depth; false if behind the near plane.
    bool project(const Vec& p, float& sx, float& sy, double& z) const {
        const Vec d = sub(p, cam);
        z = dot(d, f);
        if (flat) {
            sx = static_cast<float>(cx + scale * dot(d, r));
            sy = static_cast<float>(cy - scale * dot(d, u));
            return true;
        }
        if (z < 1) return false;
        sx = static_cast<float>(cx + focal * dot(d, r) / z);
        sy = static_cast<float>(cy - focal * dot(d, u) / z);
        return true;
    }
    double units_per_pixel(double depth) const { return flat ? 1 / scale : depth / focal; }
    bool contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};

View make_view(const Viewport& vp, float x0, float y0, float x1, float y1) {
    View v;
    const double yaw = vp.yaw * kPi / 180, pitch = vp.pitch * kPi / 180;
    const Vec dir{std::cos(pitch) * std::sin(yaw), -std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
    v.f = mul(dir, -1);
    v.r = {-std::cos(yaw), 0, std::sin(yaw)};  // level, whatever the pitch (straight down too)
    v.u = cross(v.r, v.f);
    v.flat = vp.flat;
    v.cam = add(vp.target, mul(dir, vp.flat ? kOrthoBack : vp.dist));
    v.cx = (x0 + x1) / 2.0;
    v.cy = (y0 + y1) / 2.0;
    v.focal = (y1 - y0) * 1.25;
    v.scale = v.focal / vp.dist;
    v.x0 = x0;
    v.y0 = y0;
    v.x1 = x1;
    v.y1 = y1;
    return v;
}

struct ScreenTri {
    float x[3], y[3];
    double z;  // the nearest of its corners
    int face;
};

// Builds the scene's triangles, each cut to its view's rectangle: the grids first, then the model in
// painter's order (far first) for drawing without a depth buffer, depth 1/z for drawing with one, then the
// markers (vertices, the selection, guides), always in front.
class SceneMaker {
public:
    enum class Layer { Back, Model, Front };
    explicit SceneMaker(en::Scene& s) : scene_(s) {
        scene_.vertices.clear();
        scene_.indices.clear();
    }
    void clip(const View& v) {
        x0_ = v.x0;
        y0_ = v.y0;
        x1_ = v.x1;
        y1_ = v.y1;
    }
    void tri(const float* x, const float* y, const double* z, const en::SceneColour& c, float a, double key) {
        Item it;
        it.key = key;
        for (int k = 0; k < 3; ++k) it.v[static_cast<size_t>(k)] = vert(x[k], y[k], c, a, static_cast<float>(1.0 / z[k]));
        put(it, Layer::Model);
    }
    // A segment `width` pixels wide.
    void line(float ax, float ay, double az, float bx, float by, double bz, const en::SceneColour& c, float width,
              Layer layer, double key = 0) {
        const float dx = bx - ax, dy = by - ay, l = std::sqrt(dx * dx + dy * dy);
        if (l < 0.01f) return;
        const float nx = -dy / l * width / 2, ny = dx / l * width / 2;
        const auto depth = [&](double z) {
            return layer == Layer::Front ? 1.0f : layer == Layer::Back ? kBackDepth : static_cast<float>(1.0 / z * (1 + 3 * en::kDepthStep));
        };
        const float da = depth(az), db = depth(bz);
        Item a, b;
        a.key = b.key = key;
        a.v = {vert(ax + nx, ay + ny, c, 1, da), vert(ax - nx, ay - ny, c, 1, da), vert(bx + nx, by + ny, c, 1, db)};
        b.v = {vert(ax - nx, ay - ny, c, 1, da), vert(bx - nx, by - ny, c, 1, db), vert(bx + nx, by + ny, c, 1, db)};
        put(a, layer);
        put(b, layer);
    }
    void square(float x, float y, float half, const en::SceneColour& c) {
        Item a, b;
        a.v = {vert(x - half, y - half, c, 1, 1), vert(x + half, y - half, c, 1, 1), vert(x + half, y + half, c, 1, 1)};
        b.v = {vert(x - half, y - half, c, 1, 1), vert(x + half, y + half, c, 1, 1), vert(x - half, y + half, c, 1, 1)};
        put(a, Layer::Front);
        put(b, Layer::Front);
    }
    void finish() {
        std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) { return a.key > b.key; });
        for (const auto* list : {&back_, &items_, &markers_}) {
            for (const Item& it : *list) {
                for (const auto& v : it.v) {
                    scene_.indices.push_back(static_cast<int32_t>(scene_.vertices.size()));
                    scene_.vertices.push_back(v);
                }
            }
        }
        scene_.stats.triangles = static_cast<int>(scene_.indices.size() / 3);
    }

private:
    struct Item {
        double key = 0;
        std::array<en::SceneVertex, 3> v;
    };
    static en::SceneVertex vert(float x, float y, const en::SceneColour& c, float a, float depth) {
        en::SceneVertex v;
        v.x = x;
        v.y = y;
        v.r = c.r;
        v.g = c.g;
        v.b = c.b;
        v.a = a;
        v.depth = depth;
        return v;
    }
    static en::SceneVertex lerp(const en::SceneVertex& a, const en::SceneVertex& b, float t) {
        en::SceneVertex v = a;
        v.x = a.x + (b.x - a.x) * t;
        v.y = a.y + (b.y - a.y) * t;
        v.r = a.r + (b.r - a.r) * t;
        v.g = a.g + (b.g - a.g) * t;
        v.b = a.b + (b.b - a.b) * t;
        v.a = a.a + (b.a - a.a) * t;
        v.depth = a.depth + (b.depth - a.depth) * t;  // 1/z is affine on screen
        return v;
    }
    // Cut to one side of a line: x (axis 0) or y (1) at `bound`, keeping the greater side or the lesser.
    static void cut(std::vector<en::SceneVertex>& poly, int axis, float bound, bool greater) {
        std::vector<en::SceneVertex> out;
        const auto inside = [&](const en::SceneVertex& v) {
            const float c = axis ? v.y : v.x;
            return greater ? c >= bound : c <= bound;
        };
        for (size_t i = 0; i < poly.size(); ++i) {
            const en::SceneVertex& a = poly[i];
            const en::SceneVertex& b = poly[(i + 1) % poly.size()];
            const bool ia = inside(a), ib = inside(b);
            if (ia) out.push_back(a);
            if (ia != ib) {
                const float ca = axis ? a.y : a.x, cb = axis ? b.y : b.x;
                out.push_back(lerp(a, b, (bound - ca) / (cb - ca)));
            }
        }
        poly = std::move(out);
    }
    void put(const Item& it, Layer layer) {
        auto& list = layer == Layer::Back ? back_ : layer == Layer::Front ? markers_ : items_;
        float lx = it.v[0].x, hx = lx, ly = it.v[0].y, hy = ly;
        for (const auto& v : it.v) {
            lx = std::min(lx, v.x);
            hx = std::max(hx, v.x);
            ly = std::min(ly, v.y);
            hy = std::max(hy, v.y);
        }
        if (hx < x0_ || lx > x1_ || hy < y0_ || ly > y1_) return;
        if (lx >= x0_ && hx <= x1_ && ly >= y0_ && hy <= y1_) {
            list.push_back(it);
            return;
        }
        std::vector<en::SceneVertex> poly(it.v.begin(), it.v.end());
        cut(poly, 0, x0_, true);
        cut(poly, 0, x1_, false);
        cut(poly, 1, y0_, true);
        cut(poly, 1, y1_, false);
        for (size_t i = 1; i + 1 < poly.size(); ++i) {
            Item t;
            t.key = it.key;
            t.v = {poly[0], poly[i], poly[i + 1]};
            list.push_back(t);
        }
    }
    en::Scene& scene_;
    std::vector<Item> back_, items_, markers_;
    float x0_ = 0, y0_ = 0, x1_ = 1e9f, y1_ = 1e9f;
};

en::SceneColour face_colour(const ModelData::Face& f) {
    const en::SceneColour a = en::ega_colour(f.colour & 15);
    if ((f.colour >> 4) == 0) return a;
    const en::SceneColour b = en::ega_colour(f.colour >> 4);
    return {(a.r + b.r) / 2, (a.g + b.g) / 2, (a.b + b.b) / 2};
}
en::SceneColour rgb(uint32_t c) {
    return {static_cast<float>((c >> 16) & 255) / 255.0f, static_cast<float>((c >> 8) & 255) / 255.0f,
            static_cast<float>(c & 255) / 255.0f};
}
uint32_t argb_of(const en::SceneColour& c) {
    return static_cast<uint32_t>(c.r * 255) << 16 | static_cast<uint32_t>(c.g * 255) << 8 | static_cast<uint32_t>(c.b * 255);
}
constexpr uint32_t kAxisColour[3] = {0xE04848, 0x48C058, 0x5080FF};  // x east, y down, z north

// --- Commands: the menus, the toolbar, the panels and the keys all run these --------------------------------

enum class Cmd {
    None,
    Save, SaveAs, Revert, Restart, Close,
    Undo, Redo, PickAll, PickNone, Delete, Vertices, Faces,
    Move, Turn, Scale, Snap, Symmetry, Mirror, Extrude, NewVertex, NewFace, Flip, TypePlace, Lock, GridStep,
    AddShape, Smaller, Bigger,
    FourViews, SetView, Flat, Frame, Shading, Edges, ZoomIn, ZoomOut, Pan, Orbit,
    FlagBack, FlagOutline, FlagHidden, SetColour,
    Help, PickItem, Tab, OpenMenu, ViewMenu,
};

struct MenuItem {
    const char* label = nullptr;  // nullptr: a rule between groups
    const char* key = "";
    Cmd cmd = Cmd::None;
    int arg = 0;
};
struct Menu {
    const char* title;
    std::vector<MenuItem> items;
};

constexpr Shape kPanelShapes[] = {Shape::Cube, Shape::Cone, Shape::Sphere, Shape::Cylinder,
                                  Shape::Pyramid, Shape::Plane, Shape::Wedge, Shape::Roof};

const std::vector<Menu>& menus() {
    static const std::vector<Menu> m = [] {
        std::vector<Menu> out;
        out.push_back({"File",
                       {{"Save", "Ctrl+S", Cmd::Save},
                        {"Save as...", "Ctrl+Shift+S", Cmd::SaveAs},
                        {},
                        {"This object as the original's", "Ctrl+R", Cmd::Revert},
                        {"Every object as the original's", "Ctrl+N", Cmd::Restart},
                        {},
                        {"Close the editor", "Esc", Cmd::Close}}});
        out.push_back({"Edit",
                       {{"Undo", "Ctrl+Z", Cmd::Undo},
                        {"Redo", "Ctrl+Y", Cmd::Redo},
                        {},
                        {"Pick all", "Ctrl+A", Cmd::PickAll},
                        {"Pick nothing", "Esc", Cmd::PickNone},
                        {"Delete what's picked", "Delete", Cmd::Delete},
                        {},
                        {"Pick vertices", "Tab", Cmd::Vertices},
                        {"Pick faces", "Tab", Cmd::Faces}}});
        out.push_back({"Tools",
                       {{"Move", "V", Cmd::Move},
                        {"Turn", "T", Cmd::Turn},
                        {"Size", "S", Cmd::Scale},
                        {},
                        {"Snap to vertices", "Shift+Tab", Cmd::Snap},
                        {"Symmetry (east-west)", "Shift+M", Cmd::Symmetry},
                        {"Along x only", "X", Cmd::Lock, 0},
                        {"Along y only", "Y", Cmd::Lock, 1},
                        {"Along z only", "Z", Cmd::Lock, 2},
                        {"Grid steps", "G", Cmd::GridStep},
                        {},
                        {"New vertex", "A", Cmd::NewVertex},
                        {"New face", "F", Cmd::NewFace},
                        {"Extrude faces", "E", Cmd::Extrude},
                        {"Turn faces round", "R", Cmd::Flip},
                        {"Mirror copy", "M", Cmd::Mirror},
                        {"Type a vertex's place", "Enter", Cmd::TypePlace}}});
        Menu create{"Create", {}};
        for (const Shape s : kPanelShapes) create.items.push_back({game::shape_name(s), "", Cmd::AddShape, static_cast<int>(s)});
        create.items.push_back({});
        create.items.push_back({"Bigger shapes", "", Cmd::Bigger});
        create.items.push_back({"Smaller shapes", "", Cmd::Smaller});
        out.push_back(create);
        Menu views{"Views", {{"Four views", "Q", Cmd::FourViews}, {}}};
        for (const ViewInfo& v : kViewInfo) views.items.push_back({v.name, v.key, Cmd::SetView, static_cast<int>(v.kind)});
        views.items.push_back({});
        views.items.push_back({"Flat (no perspective)", "P", Cmd::Flat});
        views.items.push_back({"The whole object", "Home", Cmd::Frame});
        views.items.push_back({"Zoom in", "+", Cmd::ZoomIn});
        views.items.push_back({"Zoom out", "-", Cmd::ZoomOut});
        views.items.push_back({});
        views.items.push_back({"Shading", "L", Cmd::Shading});
        views.items.push_back({"Edges", "W", Cmd::Edges});
        out.push_back(views);
        out.push_back({"Help", {{"Keys", "F1", Cmd::Help}}});
        return out;
    }();
    return m;
}

// A view's own menu (its name, clicked).
const std::vector<MenuItem>& view_menu_items() {
    static const std::vector<MenuItem> items = [] {
        std::vector<MenuItem> out;
        for (const ViewInfo& v : kViewInfo) out.push_back({v.name, v.key, Cmd::SetView, static_cast<int>(v.kind)});
        out.push_back({});
        out.push_back({"Flat (no perspective)", "P", Cmd::Flat});
        out.push_back({"The whole object", "Home", Cmd::Frame});
        out.push_back({});
        out.push_back({"Four views", "Q", Cmd::FourViews});
        out.push_back({"Shading", "L", Cmd::Shading});
        out.push_back({"Edges", "W", Cmd::Edges});
        return out;
    }();
    return items;
}

struct ToolButton {
    Cmd cmd;
    int arg;
    const icons::Icon* icon;
    bool flip;
    const char* tip;
};
// The toolbar, groups split by a null icon.
const std::vector<ToolButton>& toolbar() {
    static const std::vector<ToolButton> b = {
        {Cmd::Undo, 0, &icons::kUndo, false, "Undo (Ctrl+Z)"},
        {Cmd::Redo, 0, &icons::kUndo, true, "Redo (Ctrl+Y)"},
        {Cmd::None, 0, nullptr, false, ""},
        {Cmd::Move, 0, &icons::kMove, false, "Move (V)\nDrag what's picked; drag empty space to pick with a box"},
        {Cmd::Turn, 0, &icons::kTurn, false, "Turn (T)\nDrag round what's picked (Ctrl: 15 degree steps)"},
        {Cmd::Scale, 0, &icons::kScale, false, "Size (S)\nDrag out from what's picked (Ctrl: quarter steps)"},
        {Cmd::None, 0, nullptr, false, ""},
        {Cmd::Snap, 0, &icons::kSnap, false, "Snap to vertices (Shift+Tab)\nA dragged point lands on vertices and lines up with them"},
        {Cmd::Symmetry, 0, &icons::kSymmetry, false, "Symmetry (Shift+M)\nWhat's done on one side is done on the other"},
        {Cmd::None, 0, nullptr, false, ""},
        {Cmd::Vertices, 0, &icons::kVertices, false, "Pick vertices (Tab)"},
        {Cmd::Faces, 0, &icons::kFaces, false, "Pick faces (Tab)"},
        {Cmd::None, 0, nullptr, false, ""},
        {Cmd::NewVertex, 0, &icons::kNewVertex, false, "New vertex (A)\nIn the middle of what's picked"},
        {Cmd::NewFace, 0, &icons::kNewFace, false, "New face (F)\nThrough the picked vertices, in the order picked"},
        {Cmd::Extrude, 0, &icons::kExtrude, false, "Extrude (E)\nPull the picked faces out, with sides"},
        {Cmd::Flip, 0, &icons::kFlip, false, "Turn round (R)\nThe picked faces show their other side"},
        {Cmd::Mirror, 0, &icons::kMirror, false, "Mirror copy (M)\nA copy of what's picked, across east-west"},
        {Cmd::Delete, 0, &icons::kDelete, false, "Delete (Delete)"},
        {Cmd::None, 0, nullptr, false, ""},
        {Cmd::FourViews, 0, &icons::kQuad, false, "Four views (Q)\nTop, front, left and perspective at once"},
    };
    return b;
}
const std::vector<ToolButton>& view_controls() {
    static const std::vector<ToolButton> b = {
        {Cmd::Frame, 0, &icons::kFrame, false, "The whole object (Home)"},
        {Cmd::ZoomIn, 0, &icons::kZoomIn, false, "Zoom in (+ or the wheel)"},
        {Cmd::ZoomOut, 0, &icons::kZoomOut, false, "Zoom out (- or the wheel)"},
        {Cmd::Pan, 0, &icons::kPan, false, "Slide the view\nLeft-drag slides (or middle-drag any time)"},
        {Cmd::Orbit, 0, &icons::kOrbit, false, "Turn the view\nLeft-drag turns it (or right-drag any time)"},
        {Cmd::FourViews, 0, &icons::kQuad, false, "Four views, or one (Q)"},
    };
    return b;
}

enum class Prompt { None, SaveAs, Leave, Revert, Restart, Coords };
enum class Mode { Vertices, Faces };
enum class Tool { Move, Turn, Scale };
enum class Nav { None, Pan, Orbit };

// Somewhere on the canvas that does something when clicked.
struct Hit {
    int x = 0, y = 0, w = 0, h = 0;
    Cmd cmd = Cmd::None;
    int arg = 0;
    std::string tip;  // a line, and more after '\n'
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

class Editor {
public:
    Editor(Presenter& presenter, Gamepad& gamepad, const GameDir& game, const fs::path& dir, const std::string& name,
           const EditorScript* script)
        : presenter_(presenter), gamepad_(gamepad), game_(game), dir_(dir), name_(name), script_(script) {}

    std::string run() {
        if (!load()) return name_;
        opened_ = SDL_GetTicksNS();
        density_ = std::max(0.5f, SDL_GetWindowPixelDensity(presenter_.window()));
        for (;;) {
            layout();
            hover();
            if (script_) {
                if (const auto done = play_script()) return *done;
            }
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (const auto done = handle(e)) return *done;
            }
            const ModelData& md = shown();
            std::erase_if(sel_verts_, [&](uint16_t v) { return v < kFirst || v >= md.verts.size(); });
            std::erase_if(sel_faces_, [&](uint16_t f) { return f >= md.faces.size(); });
            hits_.clear();
            build_scene();
            draw_ui();
            if (script_) {
                const uint64_t ms = (SDL_GetTicksNS() - opened_) / 1'000'000;
                if (script_shot_ < script_->shots.size() && script_->shots[script_shot_].first <= ms) {
                    presenter_.request_screenshot(script_->shots[script_shot_].second);
                    ++script_shot_;
                }
            }
            presenter_.show_overlay(canvas_, false);
            presenter_.present_view(scene_, kViewBack);
            if (!presenter_.visible()) SDL_Delay(10);
        }
    }

private:
    // --- Loading ---
    void layout_canvas() {
        presenter_.output_size(out_w_, out_h_);
        scale_ = std::max(1, std::min(out_w_ / 640, out_h_ / 400));
        canvas_.reset(std::max(out_w_ / scale_, 1), std::max(out_h_ / scale_, 1), scale_, kBackground);
    }
    void notice(const std::vector<std::string>& lines, bool wait) {
        for (;;) {
            layout_canvas();
            canvas_.text(16, 12, "VETTE!", kGold, 3, true);
            canvas_.text(16 + text_width("VETTE!", 3) + 32, 14, "Object editor", kSubtitle);
            int y = 64;
            for (const std::string& l : lines) {
                for (const std::string& w : wrap(l, static_cast<size_t>((canvas_.width - 32) / kGlyph))) {
                    canvas_.text(16, y, w, kHelp);
                    y += 12;
                }
            }
            if (wait) canvas_.text(16, canvas_.height - 14, "Press a key", kHint);
            presenter_.hide_overlay();
            presenter_.present(canvas_);
            if (!wait) return;
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) {
                    SDL_PushEvent(&e);
                    return;
                }
                if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
                    return;
            }
            SDL_Delay(10);
        }
    }

    bool load() {
        // The models as the game has them: VETTE.EXE started far enough to have unpacked itself, and the
        // city's code-drawn objects made into models.
        notice({"Loading the objects..."}, false);
        std::string error;
        {
            host::MachineConfig config;
            config.game_dir = game_.root();
            config.save_dir = fs::temp_directory_path() / "vette2026_object_editor";
            config.cpu_hz = 140'000'000;
            host::Machine machine(config);
            bool ready = false;
            if (machine.boot(error)) {
                ready = game::run_until_started(machine);
                if (!ready) error = "it didn't start";
                for (int id = 0; ready && id < game::kModelCount; ++id) original_[static_cast<size_t>(id)] = game::read_model(machine.memory(), id);
                std::string why;
                en::World world;
                if (ready && en::extract_world(machine, world, why)) {
                    for (en::CityObject& o : en::city_objects(world)) {
                        if (auto m = en::routine_model(world, o.routine, why)) {
                            original_objects_[o.routine] = std::move(*m);
                            city_.push_back(std::move(o));
                        }
                    }
                }
            }
            if (!ready || !original_[1]) {
                notice({"The object editor couldn't read the objects from VETTE.EXE: " + error}, true);
                return false;
            }
        }
        for (int id = 0; id < game::kModelCount; ++id) {
            if (original_[static_cast<size_t>(id)]) ids_.push_back(id);
        }
        rows_ = ids_;
        if (!city_.empty()) rows_.push_back(kSectionRow);
        for (const en::CityObject& o : city_) {
            ids_.push_back(kObjectItem + o.routine);
            rows_.push_back(kObjectItem + o.routine);
        }
        if (!name_.empty()) {
            if (auto loaded = load_object_pack(dir_, name_, error)) {
                pack_ = std::move(*loaded);
                pack_name_ = name_;
            } else {
                say("Couldn't open \"" + name_ + "\": " + error + ". These are the original's.");
            }
        }
        views_vp_[0].set(ViewKind::Top);
        views_vp_[1].set(ViewKind::Front);
        views_vp_[2].set(ViewKind::Left);
        views_vp_[3].set(ViewKind::Perspective);
        frame_model();
        return true;
    }

    // --- The items and the set ---
    const ModelData& original_of(int item) const {
        return is_object(item) ? original_objects_.at(routine_of(item)) : *original_[static_cast<size_t>(item)];
    }
    const en::CityObject& city_object(int item) const {
        return *std::find_if(city_.begin(), city_.end(), [&](const en::CityObject& o) { return o.routine == routine_of(item); });
    }
    std::string item_name(int item) const {
        return is_object(item) ? city_object(item).name + " " + hex4(routine_of(item)) : game::model_name(item);
    }
    const ModelData& shown() const {
        const ModelData* m = edited(pack_, current_);
        return m ? *m : original_of(current_);
    }
    void say(std::string text) {
        message_ = std::move(text);
        message_at_ = SDL_GetTicksNS();
    }

    // Edits, as before-and-after copies of a model (they're small), to undo and redo.
    ModelData& begin_edit() {
        const ModelData* now = edited(pack_, current_);
        edit_before_ = now ? std::optional<ModelData>(*now) : std::nullopt;
        if (!now) edit_slot(pack_, current_) = original_of(current_);
        return edit_slot(pack_, current_);
    }
    void end_edit() {
        const ModelData& now = edit_slot(pack_, current_);
        const ModelData& was = edit_before_ ? *edit_before_ : original_of(current_);
        if (now == was) {
            if (!edit_before_) unedit(pack_, current_);
            return;
        }
        undo_.push_back({current_, edit_before_, now});
        redo_.clear();
        dirty_ = true;
    }
    void put(int item, const std::optional<ModelData>& m) {
        if (m) edit_slot(pack_, item) = *m;
        else unedit(pack_, item);
    }

    bool save(const std::string& as) {
        std::string error;
        for (const auto& [id, m] : pack_.models) {
            if (const std::string why = game::check_model(m); !why.empty()) {
                say("Not saved: " + game::model_name(id) + ": " + why + ".");
                return false;
            }
        }
        for (const auto& [routine, m] : pack_.objects) {
            if (const std::string why = game::check_model(m); !why.empty()) {
                say("Not saved: " + item_name(kObjectItem + routine) + ": " + why + ".");
                return false;
            }
        }
        if (pack_.objects.size() > static_cast<size_t>(game::kMaxObjects)) {
            say("Not saved: the game takes " + std::to_string(game::kMaxObjects) + " changed city objects at most.");
            return false;
        }
        if (save_pack(dir_, as, pack_, error)) {
            pack_name_ = as;
            dirty_ = false;
            say("Saved as \"" + as + "\". Choose it as the launch menu's Objects to play with it.");
            return true;
        }
        say("Not saved: " + error);
        return false;
    }
    std::string leave() {
        presenter_.hide_overlay();
        SDL_StopTextInput(presenter_.window());
        if (quit_after_) {
            SDL_Event q{};
            q.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&q);
        }
        return pack_name_.empty() ? name_ : pack_name_;
    }
    void start_save_as(bool leaving) {
        prompt_ = Prompt::SaveAs;
        field_ = pack_name_.empty() ? "My objects" : pack_name_;
        leave_after_save_ = leaving;
        SDL_StartTextInput(presenter_.window());
    }

    // --- Selection ---
    void clear_selection() {
        sel_verts_.clear();
        sel_faces_.clear();
    }
    bool has_selection() const { return !sel_verts_.empty() || !sel_faces_.empty(); }
    std::set<uint16_t> selected_vertices() const {
        const ModelData& m = shown();
        std::set<uint16_t> vs(sel_verts_.begin(), sel_verts_.end());
        for (const uint16_t f : sel_faces_) {
            if (f >= m.faces.size()) continue;
            for (const auto& prim : m.faces[f].prims) {
                for (const uint16_t v : prim) {
                    if (v >= kFirst) vs.insert(v);
                }
            }
        }
        return vs;
    }
    Vec selection_centre() const {
        const auto vs = selected_vertices();
        if (vs.empty()) return views_vp_[static_cast<size_t>(key_view())].target;
        Vec s{0, 0, 0};
        for (const uint16_t v : vs) s = add(s, as_vec(shown().verts[v]));
        return mul(s, 1.0 / static_cast<double>(vs.size()));
    }
    // With symmetry on, the faces and their mirror images.
    std::vector<uint16_t> with_mirrors(const ModelData& m, std::vector<uint16_t> faces) const {
        if (!symmetry_) return faces;
        const size_t n = faces.size();
        for (size_t i = 0; i < n; ++i) {
            const int g = game::mirror_face(m, faces[i]);
            if (g >= 0 && std::find(faces.begin(), faces.end(), static_cast<uint16_t>(g)) == faces.end()) faces.push_back(static_cast<uint16_t>(g));
        }
        return faces;
    }
    void set_mode(Mode m) {
        if (m == mode_) return;
        const ModelData& md = shown();
        mode_ = m;
        // Faces whose points are all picked become the selection, and back.
        if (mode_ == Mode::Faces) {
            const std::set<uint16_t> vs(sel_verts_.begin(), sel_verts_.end());
            sel_faces_.clear();
            for (size_t f = 0; f < md.faces.size() && !vs.empty(); ++f) {
                bool all = true;
                for (const auto& prim : md.faces[f].prims) {
                    for (const uint16_t v : prim) all = all && vs.count(v);
                }
                if (all) sel_faces_.push_back(static_cast<uint16_t>(f));
            }
            sel_verts_.clear();
        } else {
            const auto vs = selected_vertices();
            sel_faces_.clear();
            sel_verts_.assign(vs.begin(), vs.end());
        }
    }

    // --- Views ---
    void frame_model() {
        const ModelData& m = shown();
        Vec lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
        for (size_t i = kFirst; i < m.verts.size(); ++i) {
            for (size_t c = 0; c < 3; ++c) {
                lo[c] = std::min(lo[c], double(m.verts[i][c]));
                hi[c] = std::max(hi[c], double(m.verts[i][c]));
            }
        }
        if (lo[0] > hi[0]) lo = hi = Vec{0, 0, 0};
        const double radius = std::max(40.0, 0.5 * std::sqrt(dot(sub(hi, lo), sub(hi, lo))));
        for (Viewport& vp : views_vp_) {
            vp.target = mul(add(lo, hi), 0.5);
            vp.dist = radius * 2.6;
        }
        radius_ = radius;
        // Steps in proportion: a car is some 25 x 55, a building some thousands.
        step_unit_ = 1;
        while (step_unit_ * 2 <= radius / 24) step_unit_ *= 2;
        snap_ = step_unit_;
        shape_size_ = std::max(step_unit_ * 2, static_cast<int>(std::lround(radius * 0.5 / step_unit_)) * step_unit_);
    }
    int key_view() const { return hover_view_ >= 0 ? hover_view_ : active_; }
    int target_view() const { return menu_view_ >= 0 ? menu_view_ : key_view(); }
    void switch_model(int item) {
        current_ = item;
        clear_selection();
        frame_model();
        reveal_ = true;
    }
    void step_model(int dir) {
        auto it = std::find(ids_.begin(), ids_.end(), current_);
        const int i = it == ids_.end() ? 0 : static_cast<int>(it - ids_.begin());
        const int n = static_cast<int>(ids_.size());
        switch_model(ids_[static_cast<size_t>((i + dir + n) % n)]);
    }

    // Each frame: the canvas, the views' rectangles.
    void layout() {
        layout_canvas();
        canvas_.reset(canvas_.width, canvas_.height, scale_, kSeeThrough);
        W_ = canvas_.width;
        H_ = canvas_.height;
        ox_ = static_cast<float>((out_w_ - W_ * scale_) / 2);
        oy_ = static_cast<float>((out_h_ - H_ * scale_) / 2);
        const int ax0 = kListW, ay0 = kTop, ax1 = W_ - kSideW, ay1 = H_ - kStatusH;
        const int mx = (ax0 + ax1) / 2, my = (ay0 + ay1) / 2;
        for (int i = 0; i < 4; ++i) {
            std::array<int, 4> r{ax0, ay0, ax1, ay1};
            if (quad_) {
                r = i == 0 ? std::array<int, 4>{ax0, ay0, mx - 1, my - 1}
                    : i == 1 ? std::array<int, 4>{mx + 1, ay0, ax1, my - 1}
                    : i == 2 ? std::array<int, 4>{ax0, my + 1, mx - 1, ay1}
                             : std::array<int, 4>{mx + 1, my + 1, ax1, ay1};
            }
            rect_[static_cast<size_t>(i)] = r;
            visible_[static_cast<size_t>(i)] = quad_ || i == active_;
            views_[static_cast<size_t>(i)] =
                make_view(views_vp_[static_cast<size_t>(i)], out_x(r[0]), out_y(r[1]), out_x(r[2]), out_y(r[3]));
        }
    }
    float out_x(int cx) const { return ox_ + static_cast<float>(cx * scale_); }
    float out_y(int cy) const { return oy_ + static_cast<float>(cy * scale_); }
    int canvas_x(float sx) const { return static_cast<int>(std::floor((sx - ox_) / static_cast<float>(scale_))); }
    int canvas_y(float sy) const { return static_cast<int>(std::floor((sy - oy_) / static_cast<float>(scale_))); }
    int view_at(float sx, float sy) const {
        for (int i = 0; i < 4; ++i) {
            if (visible_[static_cast<size_t>(i)] && views_[static_cast<size_t>(i)].contains(sx, sy)) return i;
        }
        return -1;
    }
    int hit_at(float sx, float sy) const {
        const int x = canvas_x(sx), y = canvas_y(sy);
        for (size_t i = hits_.size(); i-- > 0;) {
            if (hits_[i].contains(x, y)) return static_cast<int>(i);
        }
        return -1;
    }
    bool menu_open() const { return open_menu_ >= 0 || menu_view_ >= 0; }

    // What's under the pointer (from the last frame's picture).
    void hover() {
        hover_hit_ = hit_at(mouse_x_, mouse_y_);
        hover_view_ = hover_hit_ < 0 && !menu_open() ? view_at(mouse_x_, mouse_y_) : -1;
        // An open menu follows the pointer along the menu bar.
        if (open_menu_ >= 0 && hover_hit_ >= 0 && hits_[static_cast<size_t>(hover_hit_)].cmd == Cmd::OpenMenu)
            open_menu_ = hits_[static_cast<size_t>(hover_hit_)].arg;
        hover_vertex_ = hover_face_ = -1;
        const int v = hover_view_;
        if (v < 0 || menu_open() || drag_.moving || box_.on) return;
        const Shown& s = shown_[static_cast<size_t>(v)];
        if (mode_ == Mode::Vertices) {
            float best = static_cast<float>(8 * scale_);
            for (size_t i = kFirst; i < s.vscreen.size() && i < shown().verts.size(); ++i) {
                if (s.vscreen[i][2] < 0) continue;
                const float d = std::hypot(s.vscreen[i][0] - mouse_x_, s.vscreen[i][1] - mouse_y_);
                if (d < best) {
                    best = d;
                    hover_vertex_ = static_cast<int>(i);
                }
            }
        } else {
            double nearest = 1e18;
            for (const ScreenTri& t : s.tris) {
                const float d = (t.y[1] - t.y[2]) * (t.x[0] - t.x[2]) + (t.x[2] - t.x[1]) * (t.y[0] - t.y[2]);
                if (std::abs(d) < 1e-6f) continue;
                const float a = ((t.y[1] - t.y[2]) * (mouse_x_ - t.x[2]) + (t.x[2] - t.x[1]) * (mouse_y_ - t.y[2])) / d;
                const float b = ((t.y[2] - t.y[0]) * (mouse_x_ - t.x[2]) + (t.x[0] - t.x[2]) * (mouse_y_ - t.y[2])) / d;
                if (a < 0 || b < 0 || a + b > 1) continue;
                if (t.z < nearest) {
                    nearest = t.z;
                    hover_face_ = t.face;
                }
            }
        }
    }

    // --- Commands ---
    bool enabled(Cmd c) const {
        switch (c) {
        case Cmd::Undo: return !undo_.empty();
        case Cmd::Redo: return !redo_.empty();
        case Cmd::Revert: return edited(pack_, current_) != nullptr;
        case Cmd::Delete:
        case Cmd::Mirror: return has_selection();
        case Cmd::NewFace: return mode_ == Mode::Vertices && sel_verts_.size() >= 2;
        case Cmd::Extrude:
        case Cmd::Flip:
        case Cmd::FlagBack:
        case Cmd::FlagOutline:
        case Cmd::FlagHidden: return !sel_faces_.empty();
        case Cmd::TypePlace: return mode_ == Mode::Vertices && sel_verts_.size() == 1;
        default: return true;
        }
    }
    bool checked(Cmd c, int arg) const {
        const Viewport& vp = views_vp_[static_cast<size_t>(target_view())];
        const auto flag = [&](uint16_t bit) {
            return !sel_faces_.empty() && sel_faces_[0] < shown().faces.size() && (shown().faces[sel_faces_[0]].flags & bit) != 0;
        };
        switch (c) {
        case Cmd::Vertices: return mode_ == Mode::Vertices;
        case Cmd::Faces: return mode_ == Mode::Faces;
        case Cmd::Move: return nav_ == Nav::None && tool_ == Tool::Move;
        case Cmd::Turn: return nav_ == Nav::None && tool_ == Tool::Turn;
        case Cmd::Scale: return nav_ == Nav::None && tool_ == Tool::Scale;
        case Cmd::Snap: return snap_on_;
        case Cmd::Symmetry: return symmetry_;
        case Cmd::Lock: return lock_ == 'x' + arg;
        case Cmd::FourViews: return quad_;
        case Cmd::SetView: return static_cast<int>(vp.kind) == arg;
        case Cmd::Flat: return vp.flat;
        case Cmd::Shading: return lighting_;
        case Cmd::Edges: return wire_;
        case Cmd::Pan: return nav_ == Nav::Pan;
        case Cmd::Orbit: return nav_ == Nav::Orbit;
        case Cmd::Tab: return panel_tab_ == arg;
        case Cmd::FlagBack: return flag(0x0001);
        case Cmd::FlagOutline: return flag(0x2000);
        case Cmd::FlagHidden: return flag(0x4000);
        default: return false;
        }
    }

    void run_cmd(Cmd c, int arg) {
        const int tv = target_view();
        Viewport& vp = views_vp_[static_cast<size_t>(tv)];
        open_menu_ = -1;
        menu_view_ = -1;
        if (!enabled(c)) return;
        switch (c) {
        case Cmd::None: break;
        case Cmd::Save:
            if (pack_name_.empty()) start_save_as(false);
            else save(pack_name_);
            break;
        case Cmd::SaveAs: start_save_as(false); break;
        case Cmd::Revert: prompt_ = Prompt::Revert; break;
        case Cmd::Restart: prompt_ = Prompt::Restart; break;
        case Cmd::Close:
            if (!dirty_) closing_ = true;
            else prompt_ = Prompt::Leave;
            break;
        case Cmd::Undo:
            put(undo_.back().id, undo_.back().before);
            if (current_ != undo_.back().id) switch_model(undo_.back().id);
            redo_.push_back(std::move(undo_.back()));
            undo_.pop_back();
            clear_selection();
            dirty_ = true;
            break;
        case Cmd::Redo:
            put(redo_.back().id, redo_.back().after);
            if (current_ != redo_.back().id) switch_model(redo_.back().id);
            undo_.push_back(std::move(redo_.back()));
            redo_.pop_back();
            clear_selection();
            dirty_ = true;
            break;
        case Cmd::PickAll: {
            const ModelData& m = shown();
            clear_selection();
            if (mode_ == Mode::Vertices) {
                for (size_t i = kFirst; i < m.verts.size(); ++i) sel_verts_.push_back(static_cast<uint16_t>(i));
            } else {
                for (size_t i = 0; i < m.faces.size(); ++i) sel_faces_.push_back(static_cast<uint16_t>(i));
            }
            break;
        }
        case Cmd::PickNone: clear_selection(); break;
        case Cmd::Delete: delete_picked(); break;
        case Cmd::Vertices: set_mode(Mode::Vertices); break;
        case Cmd::Faces: set_mode(Mode::Faces); break;
        case Cmd::Move:
        case Cmd::Turn:
        case Cmd::Scale:
            tool_ = c == Cmd::Move ? Tool::Move : c == Cmd::Turn ? Tool::Turn : Tool::Scale;
            nav_ = Nav::None;
            break;
        case Cmd::Snap:
            snap_on_ = !snap_on_;
            say(snap_on_ ? "Snap: a dragged point lands on vertices and lines up with them (hold Alt for a free move)."
                         : "Snap off: points move freely (in the grid's steps).");
            break;
        case Cmd::Symmetry:
            symmetry_ = !symmetry_;
            say(symmetry_ ? "Symmetry: what's done on one side of east-west is done on the other."
                          : "Symmetry off.");
            break;
        case Cmd::Mirror: mirror_copy(); break;
        case Cmd::Extrude: extrude_picked(); break;
        case Cmd::NewVertex: new_vertex(); break;
        case Cmd::NewFace: new_face(); break;
        case Cmd::Flip: flip_picked(); break;
        case Cmd::TypePlace: {
            const auto& v = shown().verts[sel_verts_[0]];
            field_ = std::to_string(v[0]) + " " + std::to_string(v[1]) + " " + std::to_string(v[2]);
            prompt_ = Prompt::Coords;
            SDL_StartTextInput(presenter_.window());
            break;
        }
        case Cmd::Lock: {
            const char a = static_cast<char>('x' + arg);
            lock_ = lock_ == a ? 0 : a;
            say(lock_ ? std::string("Along ") + (a == 'x' ? "x (east-west)" : a == 'y' ? "y (up-down)" : "z (north-south)") + " only."
                      : "Free in the view.");
            break;
        }
        case Cmd::GridStep:
            snap_ = snap_ == 1 ? step_unit_ * 4 : snap_ > step_unit_ ? step_unit_ : 1;
            say("Moves in steps of " + std::to_string(snap_) + ".");
            break;
        case Cmd::AddShape: add_shape(static_cast<Shape>(arg)); break;
        case Cmd::Smaller:
        case Cmd::Bigger:
            shape_size_ = std::clamp(c == Cmd::Bigger ? shape_size_ * 2 : shape_size_ / 2, 2, 16000);
            say("New shapes " + std::to_string(shape_size_) + " across.");
            break;
        case Cmd::FourViews:
            quad_ = !quad_;
            if (!quad_ && hover_view_ >= 0) active_ = hover_view_;
            break;
        case Cmd::SetView: {
            vp.set(static_cast<ViewKind>(arg));
            active_ = tv;
            break;
        }
        case Cmd::Flat:
            vp.flat = !vp.flat;
            vp.kind = vp.flat ? ViewKind::User : ViewKind::Perspective;
            break;
        case Cmd::Frame: frame_model(); break;
        case Cmd::Shading: lighting_ = !lighting_; break;
        case Cmd::Edges: wire_ = !wire_; break;
        case Cmd::ZoomIn: vp.dist = std::max(30.0, vp.dist / 1.25); break;
        case Cmd::ZoomOut: vp.dist = std::min(80000.0, vp.dist * 1.25); break;
        case Cmd::Pan: nav_ = nav_ == Nav::Pan ? Nav::None : Nav::Pan; break;
        case Cmd::Orbit: nav_ = nav_ == Nav::Orbit ? Nav::None : Nav::Orbit; break;
        case Cmd::FlagBack: toggle_flag(0x0001); break;
        case Cmd::FlagOutline: toggle_flag(0x2000); break;
        case Cmd::FlagHidden: toggle_flag(0x4000); break;
        case Cmd::SetColour: set_colour(static_cast<uint8_t>(arg)); break;
        case Cmd::Help: help_ = true; break;
        case Cmd::PickItem: switch_model(arg); break;
        case Cmd::Tab: panel_tab_ = arg; break;
        case Cmd::OpenMenu: break;
        case Cmd::ViewMenu: break;
        }
    }

    // --- Edits ---
    // Moves the picked vertices to where `to` puts them, as one edit; with symmetry, their twins follow.
    template <typename F>
    void transform_picked(F&& to) {
        const auto vs = selected_vertices();
        if (vs.empty()) return;
        ModelData& ed = begin_edit();
        const auto partners = symmetry_ ? game::mirror_partners(ed, vs) : std::vector<std::pair<uint16_t, uint16_t>>{};
        for (const uint16_t v : vs) ed.verts[v] = to_vertex(to(as_vec(ed.verts[v])));
        game::keep_mirrored(ed, partners);
        end_edit();
    }
    void nudge(const Vec& d) {
        transform_picked([&](const Vec& p) { return add(p, d); });
    }
    void turn_by(const Vec& axis, double degrees) {
        const Vec c = selection_centre();
        transform_picked([&](const Vec& p) { return add(c, turn_round(sub(p, c), axis, degrees * kPi / 180)); });
    }
    void size_by(double k) {
        const Vec c = selection_centre();
        transform_picked([&](const Vec& p) {
            Vec d = sub(p, c);
            if (lock_) d[static_cast<size_t>(lock_ - 'x')] *= k;
            else d = mul(d, k);
            return add(c, d);
        });
    }
    void delete_picked() {
        const ModelData& m = shown();
        if (mode_ == Mode::Vertices && !sel_verts_.empty()) {
            std::set<uint16_t> gone(sel_verts_.begin(), sel_verts_.end());
            if (symmetry_) {
                for (const uint16_t v : sel_verts_) {
                    if (const int t = game::mirror_twin(m, v); t >= 0) gone.insert(static_cast<uint16_t>(t));
                }
            }
            ModelData& ed = begin_edit();
            game::delete_vertices(ed, gone);
            end_edit();
        } else if (mode_ == Mode::Faces && !sel_faces_.empty()) {
            const auto faces = with_mirrors(m, sel_faces_);
            ModelData& ed = begin_edit();
            game::delete_faces(ed, std::set<uint16_t>(faces.begin(), faces.end()));
            end_edit();
        }
        clear_selection();
    }
    void new_vertex() {
        const ModelData& m = shown();
        if (m.verts.size() >= static_cast<size_t>(game::kMaxVertices)) {
            say("The game takes " + std::to_string(game::kMaxVertices - kFirst) + " vertices at most.");
            return;
        }
        const Vec c = selection_centre();
        const bool beside = has_selection();
        ModelData& ed = begin_edit();
        ed.verts.push_back(to_vertex({c[0] + (beside ? snap_ * 2 : 0), c[1], c[2]}));
        const auto made = static_cast<uint16_t>(ed.verts.size() - 1);
        if (symmetry_ && ed.verts[made][0] != 0 && ed.verts.size() < static_cast<size_t>(game::kMaxVertices))
            game::vertex_at(ed, game::mirrored(ed.verts[made]));
        end_edit();
        mode_ = Mode::Vertices;
        clear_selection();
        sel_verts_.push_back(made);
    }
    void new_face() {
        if (sel_verts_.size() > static_cast<size_t>(game::kMaxPoints)) {
            say("A face has " + std::to_string(game::kMaxPoints) + " corners at most.");
            return;
        }
        ModelData& ed = begin_edit();
        ModelData::Face f;
        game::usual_style(ed, f.flags, f.outline);
        f.colour = colour_;
        f.prims.push_back(sel_verts_);
        if (sel_verts_.size() == 2) f.flags = static_cast<uint16_t>((f.flags & ~0x0E) | 4);
        else game::set_fill_kind(ed, f);
        ed.faces.push_back(std::move(f));
        const auto made = static_cast<uint16_t>(ed.faces.size() - 1);
        if (symmetry_) game::add_mirror_face(ed, made);
        game::faces_changed(ed);
        end_edit();
        mode_ = Mode::Faces;
        clear_selection();
        sel_faces_.push_back(made);
        say("A new face. R turns it round if it shows its back.");
    }
    void flip_picked() {
        const auto faces = with_mirrors(shown(), sel_faces_);
        ModelData& ed = begin_edit();
        for (const uint16_t f : faces) {
            for (auto& prim : ed.faces[f].prims) std::reverse(prim.begin(), prim.end());
        }
        end_edit();
    }
    void toggle_flag(uint16_t bit) {
        const auto faces = with_mirrors(shown(), sel_faces_);
        ModelData& ed = begin_edit();
        const bool on = !(ed.faces[sel_faces_[0]].flags & bit);
        for (const uint16_t f : faces) ed.faces[f].flags = static_cast<uint16_t>(on ? ed.faces[f].flags | bit : ed.faces[f].flags & ~bit);
        if (bit == 0x4000) game::faces_changed(ed);
        end_edit();
    }
    void set_colour(uint8_t c) {
        colour_ = static_cast<uint8_t>(c & 15);
        if (sel_faces_.empty()) return;
        const auto faces = with_mirrors(shown(), sel_faces_);
        ModelData& ed = begin_edit();
        for (const uint16_t f : faces) ed.faces[f].colour = colour_;
        end_edit();
    }
    void mirror_copy() {
        ModelData& ed = begin_edit();
        std::vector<uint16_t> made = mode_ == Mode::Vertices ? game::mirror_copy_vertices(ed, sel_verts_)
                                                             : game::mirror_copy_faces(ed, sel_faces_);
        end_edit();
        (mode_ == Mode::Vertices ? sel_verts_ : sel_faces_) = made;
        say("Mirrored across east-west.");
    }
    void extrude_picked() {
        const auto faces = with_mirrors(shown(), sel_faces_);
        const std::vector<uint16_t> mine = sel_faces_;
        const double distance = std::max<double>(step_unit_, std::lround(radius_ * 0.15 / step_unit_) * step_unit_);
        std::string why;
        ModelData& ed = begin_edit();
        const auto moved = game::extrude(ed, faces, distance, why);
        end_edit();
        if (moved.empty()) {
            say("Not extruded: " + why + ".");
            return;
        }
        sel_faces_ = mine;  // (with symmetry, their images follow them)
        say("Extruded. Drag the picked faces to pull them further.");
    }
    void add_shape(Shape s) {
        const Viewport& vp = views_vp_[static_cast<size_t>(key_view())];
        const auto grid = [&](double v) { return std::round(v / snap_) * snap_; };
        // In the middle of the view, standing on the ground (y 0; the plane lies on it).
        const Vec at{grid(vp.target[0]), s == Shape::Plane ? 0.0 : -shape_size_ / 2.0, grid(vp.target[2])};
        std::string why;
        ModelData& ed = begin_edit();
        const auto faces = game::add_shape(ed, s, at, shape_size_, colour_, why);
        if (!faces.empty() && symmetry_ && std::lround(at[0]) != 0) game::mirror_copy_faces(ed, faces);
        end_edit();
        if (faces.empty()) {
            say(std::string("No ") + game::shape_name(s) + ": " + why + ".");
            return;
        }
        mode_ = Mode::Faces;
        clear_selection();
        sel_faces_ = faces;
        say(std::string(game::shape_name(s)) + " added, picked: move, turn or size it.");
    }

    // --- Dragging ---
    struct Drag {
        bool armed = false;   // the left button went down on something to change
        bool moving = false;  // and the pointer has gone far enough to start
        int view = 0;
        int lead = -1;  // the vertex the pointer took (snaps and lines up)
        Vec centre{};
        double depth = 1;
        bool clear_on_click = false;  // a click on empty space (no drag) picks nothing
        int only = -1;                // a click (no drag) on one of several picked: just that one
        std::vector<std::pair<uint16_t, ModelData::Vertex>> start;
        std::vector<std::pair<uint16_t, uint16_t>> partners;
    };
    struct Guide {
        int vertex;
        int axis;  // -1: on the vertex
    };
    struct Box {
        bool on = false;
        int view = 0;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        bool add = false;
    };

    void arm_drag(int view, int lead) {
        const auto vs = selected_vertices();
        if (vs.empty()) return;
        ModelData& ed = begin_edit();
        drag_ = Drag{};
        drag_.armed = true;
        drag_.view = view;
        drag_.lead = lead;
        for (const uint16_t v : vs) drag_.start.emplace_back(v, ed.verts[v]);
        drag_.partners = symmetry_ ? game::mirror_partners(ed, vs) : std::vector<std::pair<uint16_t, uint16_t>>{};
        drag_.centre = selection_centre();
        float px = 0, py = 0;
        if (!views_[static_cast<size_t>(view)].project(drag_.centre, px, py, drag_.depth)) drag_.depth = views_vp_[static_cast<size_t>(view)].dist;
    }
    void end_drag(bool keep) {
        if (!drag_.armed) return;
        if (!keep) {
            ModelData& ed = edit_slot(pack_, current_);
            for (const auto& [v, s] : drag_.start) ed.verts[v] = s;
            game::keep_mirrored(ed, drag_.partners);
        }
        end_edit();  // (nothing recorded if nothing changed)
        drag_ = Drag{};
        guides_.clear();
        drag_note_.clear();
    }
    void update_drag(float nx, float ny) {
        const View& v = views_[static_cast<size_t>(drag_.view)];
        ModelData& ed = edit_slot(pack_, current_);
        const bool ctrl = (SDL_GetModState() & SDL_KMOD_CTRL) != 0, alt = (SDL_GetModState() & SDL_KMOD_ALT) != 0;
        guides_.clear();
        drag_note_.clear();
        std::vector<ModelData::Vertex> moved;
        if (tool_ == Tool::Move) {
            const double k = v.units_per_pixel(drag_.depth);
            Vec d = add(mul(v.r, (nx - press_x_) * k), mul(v.u, -(ny - press_y_) * k));
            if (lock_) {
                // Along the axis only: as far as the pointer went along the axis's own line on screen.
                const auto a = static_cast<size_t>(lock_ - 'x');
                Vec step{0, 0, 0};
                step[a] = 100;
                float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                double z0 = 0, z1 = 0;
                d = Vec{0, 0, 0};
                if (v.project(drag_.centre, x0, y0, z0) && v.project(add(drag_.centre, step), x1, y1, z1)) {
                    const double ax = x1 - x0, ay = y1 - y0, l2 = ax * ax + ay * ay;
                    if (l2 > 1) d[a] = ((nx - press_x_) * ax + (ny - press_y_) * ay) / l2 * 100;
                }
            }
            Vec lead0 = drag_.centre;
            for (const auto& [id, s] : drag_.start) {
                if (id == drag_.lead) lead0 = as_vec(s);
            }
            Vec p = add(lead0, d);
            for (size_t c = 0; c < 3; ++c) {
                if (lock_ && c != static_cast<size_t>(lock_ - 'x')) continue;
                if (drag_.lead >= 0) p[c] = std::round(p[c] / snap_) * snap_;
                else p[c] = lead0[c] + std::round(d[c] / snap_) * snap_;
            }
            if (snap_on_ && !alt && drag_.lead >= 0) snap_point(v, ed, p);
            const Vec shift = sub(p, lead0);
            for (const auto& [id, s] : drag_.start) ed.verts[id] = to_vertex(add(as_vec(s), shift));
        } else {
            float cx = press_x_, cy = press_y_;
            double cz = 0;
            if (!v.project(drag_.centre, cx, cy, cz)) {
                cx = press_x_;
                cy = press_y_;
            }
            if (tool_ == Tool::Turn) {
                double a = std::atan2(ny - cy, nx - cx) - std::atan2(press_y_ - cy, press_x_ - cx);
                while (a > kPi) a -= 2 * kPi;
                while (a <= -kPi) a += 2 * kPi;
                const double step = ctrl ? 15 : 1;
                double deg = std::round(a * 180 / kPi / step) * step;
                // The way that follows the pointer round on screen.
                const double k = dot(cross(v.f, v.r), mul(v.u, -1));
                Vec axis = v.f;
                double sense = k;
                if (lock_) {
                    axis = axis_vec(lock_ - 'x');
                    const double s = dot(axis, v.f);
                    if (std::abs(s) < 0.2) {
                        deg = std::round((nx - press_x_) * 0.5 / step) * step;  // edge-on: sideways turns it
                        sense = 1;
                    } else {
                        sense = k * (s > 0 ? 1 : -1);
                    }
                }
                const double phi = deg * kPi / 180 * sense;
                for (const auto& [id, s] : drag_.start)
                    ed.verts[id] = to_vertex(add(drag_.centre, turn_round(sub(as_vec(s), drag_.centre), axis, phi)));
                drag_note_ = "Turned " + std::to_string(static_cast<int>(std::lround(std::abs(deg)))) + " degrees";
            } else {
                const double d0 = std::max(4.0, static_cast<double>(std::hypot(press_x_ - cx, press_y_ - cy)));
                const double d1 = std::hypot(nx - cx, ny - cy);
                const double step = ctrl ? 0.25 : 0.05;
                const double k = std::max(step, std::round(d1 / d0 / step) * step);
                for (const auto& [id, s] : drag_.start) {
                    Vec d = sub(as_vec(s), drag_.centre);
                    if (lock_) d[static_cast<size_t>(lock_ - 'x')] *= k;
                    else d = mul(d, k);
                    ed.verts[id] = to_vertex(add(drag_.centre, d));
                }
                char text[32];
                std::snprintf(text, sizeof text, "Sized x%.2f", k);
                drag_note_ = text;
            }
        }
        game::keep_mirrored(ed, drag_.partners);
    }
    // A dragged point onto a vertex near it on screen, or else lined up with vertices along x, y or z:
    // the guides show which.
    void snap_point(const View& v, const ModelData& ed, Vec& p) {
        std::set<uint16_t> moving;
        for (const auto& [id, s] : drag_.start) moving.insert(id);
        for (const auto& [a, b] : drag_.partners) moving.insert(b);
        float px = 0, py = 0;
        double pz = 0;
        if (!v.project(p, px, py, pz)) return;
        float best = static_cast<float>(9 * scale_);
        int onto = -1;
        for (size_t i = kFirst; i < ed.verts.size(); ++i) {
            if (moving.count(static_cast<uint16_t>(i))) continue;
            float sx = 0, sy = 0;
            double sz = 0;
            if (!v.project(as_vec(ed.verts[i]), sx, sy, sz)) continue;
            const float d = std::hypot(sx - px, sy - py);
            if (d < best) {
                best = d;
                onto = static_cast<int>(i);
            }
        }
        if (onto >= 0) {
            p = as_vec(ed.verts[static_cast<size_t>(onto)]);
            guides_.push_back({onto, -1});
            drag_note_ = "On vertex " + std::to_string(onto);
            return;
        }
        int lined[3] = {-1, -1, -1};
        for (int c = 0; c < 3; ++c) {
            if (lock_ && c != lock_ - 'x') continue;
            if (!lock_ && std::abs(dot(axis_vec(c), v.f)) > 0.7) continue;  // along the line of sight
            float closest = static_cast<float>(6 * scale_);
            for (size_t i = kFirst; i < ed.verts.size(); ++i) {
                if (moving.count(static_cast<uint16_t>(i))) continue;
                Vec q = p;
                q[static_cast<size_t>(c)] = ed.verts[i][static_cast<size_t>(c)];
                float sx = 0, sy = 0;
                double sz = 0;
                if (!v.project(q, sx, sy, sz)) continue;
                const float d = std::hypot(sx - px, sy - py);
                if (d < closest) {
                    closest = d;
                    lined[c] = static_cast<int>(i);
                }
            }
        }
        std::string axes;
        for (int c = 0; c < 3; ++c) {
            if (lined[c] < 0) continue;
            p[static_cast<size_t>(c)] = ed.verts[static_cast<size_t>(lined[c])][static_cast<size_t>(c)];
            guides_.push_back({lined[c], c});
            axes += axes.empty() ? "" : ", ";
            axes += static_cast<char>('x' + c);
        }
        if (!axes.empty()) drag_note_ = "In line with vertices on " + axes;
    }

    // --- Input ---
    std::optional<std::string> play_script() {
        const uint64_t ms = (SDL_GetTicksNS() - opened_) / 1'000'000;
        for (; script_key_ < script_->keys.size() && script_->keys[script_key_].at_ms <= ms; ++script_key_) {
            SDL_Event k{};
            k.type = SDL_EVENT_KEY_DOWN;
            k.key.key = script_->keys[script_key_].key;
            k.key.mod = script_->keys[script_key_].mod;
            k.key.scancode = SDL_GetScancodeFromKey(k.key.key, nullptr);
            k.key.down = true;
            SDL_PushEvent(&k);
        }
        for (; script_mouse_ < script_->mouse.size() && script_->mouse[script_mouse_].at_ms <= ms; ++script_mouse_) {
            const EditorScript::Mouse& sm = script_->mouse[script_mouse_];
            SDL_Event ev{};
            if (sm.kind == EditorScript::Mouse::Kind::Move) {
                ev.type = SDL_EVENT_MOUSE_MOTION;
                ev.motion.x = sm.x;
                ev.motion.y = sm.y;
            } else {
                ev.type = sm.kind == EditorScript::Mouse::Kind::Down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
                ev.button.button = sm.button;
                ev.button.down = sm.kind == EditorScript::Mouse::Kind::Down;
                ev.button.x = sm.x;
                ev.button.y = sm.y;
            }
            SDL_PushEvent(&ev);
        }
        if (script_->quit_ms && ms >= script_->quit_ms) {
            dirty_ = false;
            return leave();
        }
        return std::nullopt;
    }

    std::optional<std::string> handle(const SDL_Event& e) {
        if (quit_shortcut(e)) return std::nullopt;
        pad_keys_.clear();
        gamepad_.handle_event(e, pad_keys_);
        switch (e.type) {
        case SDL_EVENT_QUIT:
            if (!dirty_) {
                SDL_Event again = e;
                SDL_PushEvent(&again);
                return leave();
            }
            quit_after_ = true;
            prompt_ = Prompt::Leave;
            break;
        case SDL_EVENT_TEXT_INPUT:
            if (prompt_ == Prompt::SaveAs) {
                for (const char* p = e.text.text; *p; ++p) {
                    if (name_char(*p) && field_.size() < 40) field_ += *p;
                }
            } else if (prompt_ == Prompt::Coords) {
                for (const char* p = e.text.text; *p; ++p) {
                    if ((std::isdigit(static_cast<unsigned char>(*p)) || *p == '-' || *p == ' ' || *p == ',') && field_.size() < 30)
                        field_ += *p == ',' ? ' ' : *p;
                }
            }
            break;
        case SDL_EVENT_KEY_DOWN:
            if (auto done = on_key(e)) return done;
            break;
        case SDL_EVENT_MOUSE_MOTION: on_motion(e.motion.x * density_, e.motion.y * density_); break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: on_press(e.button.button, e.button.x * density_, e.button.y * density_); break;
        case SDL_EVENT_MOUSE_BUTTON_UP: on_release(e.button.button); break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (canvas_x(mouse_x_) < kListW && canvas_y(mouse_y_) >= kTop) {
                list_scroll_ -= static_cast<int>(e.wheel.y * kRowH * 3);
            } else if (const int v = view_at(mouse_x_, mouse_y_); v >= 0) {
                Viewport& vp = views_vp_[static_cast<size_t>(v)];
                vp.dist = std::clamp(vp.dist * std::pow(0.87, static_cast<double>(e.wheel.y)), 30.0, 80000.0);
            }
            break;
        default: break;
        }
        if (closing_) return leave();
        return std::nullopt;
    }

    std::optional<std::string> on_key(const SDL_Event& e) {
        const SDL_Keycode k = e.key.key;
        const bool ctrl = (e.key.mod & SDL_KMOD_CTRL) != 0, shift = (e.key.mod & SDL_KMOD_SHIFT) != 0;
        if (k == SDLK_F11 || ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && (e.key.mod & SDL_KMOD_ALT))) {
            if (!e.key.repeat) presenter_.toggle_fullscreen();
            return std::nullopt;
        }
        if (prompt_ == Prompt::SaveAs || prompt_ == Prompt::Coords) {
            if (k == SDLK_ESCAPE) {
                prompt_ = Prompt::None;
                quit_after_ = leave_after_save_ = false;
                SDL_StopTextInput(presenter_.window());
            } else if (k == SDLK_BACKSPACE && !field_.empty()) {
                field_.pop_back();
            } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                if (prompt_ == Prompt::SaveAs && !clean_name(field_).empty()) {
                    const bool leaving = leave_after_save_;
                    leave_after_save_ = false;
                    SDL_StopTextInput(presenter_.window());
                    prompt_ = Prompt::None;
                    if (save(clean_name(field_)) && leaving) return leave();
                } else if (prompt_ == Prompt::Coords) {
                    std::istringstream in(field_);
                    long x = 0, y = 0, z = 0;
                    if ((in >> x >> y >> z) && sel_verts_.size() == 1 && std::abs(x) <= 32000 && std::abs(y) <= 32000 &&
                        std::abs(z) <= 32000) {
                        const Vec to{double(x), double(y), double(z)};
                        transform_picked([&](const Vec&) { return to; });
                        prompt_ = Prompt::None;
                        SDL_StopTextInput(presenter_.window());
                    } else {
                        say("Three whole numbers: x (east), y (down), z (north).");
                    }
                }
            }
            return std::nullopt;
        }
        if (prompt_ == Prompt::Leave) {
            if (k == SDLK_Y) {
                if (pack_name_.empty()) start_save_as(true);
                else if (save(pack_name_)) return leave();
            } else if (k == SDLK_N) {
                return leave();
            } else if (k == SDLK_ESCAPE) {
                prompt_ = Prompt::None;
                quit_after_ = false;
            }
            return std::nullopt;
        }
        if (prompt_ == Prompt::Revert || prompt_ == Prompt::Restart) {
            if (k == SDLK_Y) {
                if (prompt_ == Prompt::Revert && edited(pack_, current_)) {
                    begin_edit();
                    edit_slot(pack_, current_) = original_of(current_);
                    end_edit();
                    unedit(pack_, current_);
                    say(item_name(current_) + " is the original's again.");
                } else if (prompt_ == Prompt::Restart) {
                    pack_.models.clear();
                    pack_.objects.clear();
                    undo_.clear();
                    redo_.clear();
                    dirty_ = true;
                    say("Every object is the original's again.");
                }
                clear_selection();
            }
            if (k == SDLK_Y || k == SDLK_N || k == SDLK_ESCAPE) prompt_ = Prompt::None;
            return std::nullopt;
        }
        if (help_) {
            help_ = false;
            return std::nullopt;
        }
        if (menu_open()) {
            if (k == SDLK_ESCAPE) open_menu_ = menu_view_ = -1;
            return std::nullopt;
        }
        const auto cmd = [&](Cmd c, int arg = 0) { run_cmd(c, arg); };
        const auto view_key = [&](ViewKind normal, ViewKind other) { cmd(Cmd::SetView, static_cast<int>(ctrl ? other : normal)); };
        const View& view = views_[static_cast<size_t>(key_view())];
        const auto axis_like = [](const Vec& d) {
            int best = 0;
            for (int c = 1; c < 3; ++c) {
                if (std::abs(d[static_cast<size_t>(c)]) > std::abs(d[static_cast<size_t>(best)])) best = c;
            }
            Vec a{0, 0, 0};
            a[static_cast<size_t>(best)] = d[static_cast<size_t>(best)] > 0 ? 1 : -1;
            return a;
        };
        const double step = ctrl && !shift ? 1 : shift ? step_unit_ * 8 : step_unit_;
        if (k == SDLK_ESCAPE) {
            if (drag_.armed) end_drag(false);
            else if (nav_ != Nav::None) nav_ = Nav::None;
            else if (has_selection()) clear_selection();
            else cmd(Cmd::Close);
        } else if (k == SDLK_F1) {
            cmd(Cmd::Help);
        } else if (ctrl && k == SDLK_Z && !shift) {
            cmd(Cmd::Undo);
        } else if (ctrl && (k == SDLK_Y || (k == SDLK_Z && shift))) {
            cmd(Cmd::Redo);
        } else if (ctrl && k == SDLK_S) {
            cmd(shift ? Cmd::SaveAs : Cmd::Save);
        } else if (ctrl && k == SDLK_R) {
            cmd(Cmd::Revert);
        } else if (ctrl && k == SDLK_N) {
            cmd(Cmd::Restart);
        } else if (ctrl && k == SDLK_A) {
            cmd(Cmd::PickAll);
        } else if (k == SDLK_TAB) {
            if (shift) cmd(Cmd::Snap);
            else cmd(mode_ == Mode::Vertices ? Cmd::Faces : Cmd::Vertices);
        } else if (k == SDLK_LEFTBRACKET || k == SDLK_RIGHTBRACKET) {
            step_model(k == SDLK_RIGHTBRACKET ? 1 : -1);
        } else if (k == SDLK_HOME || k == SDLK_KP_PERIOD) {
            cmd(Cmd::Frame);
        } else if (k == SDLK_KP_7) {
            view_key(ViewKind::Top, ViewKind::Bottom);
        } else if (k == SDLK_KP_1) {
            view_key(ViewKind::Front, ViewKind::Back);
        } else if (k == SDLK_KP_3) {
            view_key(ViewKind::Right, ViewKind::Left);
        } else if (k == SDLK_KP_0) {
            cmd(Cmd::SetView, static_cast<int>(ViewKind::Perspective));
        } else if (k == SDLK_I) {
            cmd(Cmd::SetView, static_cast<int>(ViewKind::Iso));
        } else if (k == SDLK_P || k == SDLK_KP_5) {
            cmd(Cmd::Flat);
        } else if (k == SDLK_Q) {
            cmd(Cmd::FourViews);
        } else if (k == SDLK_V) {
            cmd(Cmd::Move);
        } else if (k == SDLK_T) {
            cmd(Cmd::Turn);
        } else if (k == SDLK_S) {
            cmd(Cmd::Scale);
        } else if (k == SDLK_E) {
            if (sel_faces_.empty()) say("Pick faces (Tab) to extrude.");
            cmd(Cmd::Extrude);
        } else if (k == SDLK_M) {
            cmd(shift ? Cmd::Symmetry : Cmd::Mirror);
        } else if (k == SDLK_LEFT || k == SDLK_RIGHT || k == SDLK_UP || k == SDLK_DOWN) {
            const bool horizontal = k == SDLK_LEFT || k == SDLK_RIGHT;
            const double sign = k == SDLK_RIGHT || k == SDLK_UP ? 1 : -1;
            if (tool_ == Tool::Move) {
                nudge(mul(axis_like(horizontal ? view.r : view.u), sign * step));
            } else if (tool_ == Tool::Turn) {
                const Vec axis = lock_ ? axis_vec(lock_ - 'x') : horizontal ? Vec{0, -1, 0} : axis_like(view.r);
                turn_by(axis, sign * (shift ? 90 : ctrl ? 1 : 15));
            } else {
                const double k2 = shift ? 2 : ctrl ? 1.05 : 1.25;
                size_by(sign > 0 ? k2 : 1 / k2);
            }
        } else if (k == SDLK_PAGEUP || k == SDLK_PAGEDOWN) {
            nudge(mul(axis_like(view.f), k == SDLK_PAGEUP ? step : -step));
        } else if (k == SDLK_X || k == SDLK_Y || k == SDLK_Z) {
            cmd(Cmd::Lock, k == SDLK_X ? 0 : k == SDLK_Y ? 1 : 2);
        } else if (k == SDLK_G) {
            cmd(Cmd::GridStep);
        } else if (k == SDLK_L) {
            cmd(Cmd::Shading);
        } else if (k == SDLK_W) {
            cmd(Cmd::Edges);
        } else if (k == SDLK_INSERT || k == SDLK_A) {
            cmd(Cmd::NewVertex);
        } else if (k == SDLK_DELETE || k == SDLK_BACKSPACE) {
            cmd(Cmd::Delete);
        } else if (k == SDLK_F && mode_ == Mode::Vertices) {
            if (sel_verts_.size() < 2) say("Pick the new face's corners in order (2 for a line), then F.");
            cmd(Cmd::NewFace);
        } else if (k == SDLK_R) {
            cmd(Cmd::Flip);
        } else if (k == SDLK_C && !sel_faces_.empty()) {
            const uint8_t c = shown().faces[sel_faces_[0]].colour;
            cmd(Cmd::SetColour, ((c & 15) + (shift ? 15 : 1)) % 16);
        } else if (k == SDLK_B || k == SDLK_H || k == SDLK_O) {
            cmd(k == SDLK_B ? Cmd::FlagBack : k == SDLK_H ? Cmd::FlagHidden : Cmd::FlagOutline);
        } else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER)) {
            cmd(Cmd::TypePlace);
        } else if (k == SDLK_EQUALS || k == SDLK_PLUS || k == SDLK_KP_PLUS) {
            cmd(Cmd::ZoomIn);
        } else if (k == SDLK_MINUS || k == SDLK_KP_MINUS) {
            cmd(Cmd::ZoomOut);
        } else if (k == SDLK_COMMA || k == SDLK_PERIOD) {
            orbit_by(key_view(), k == SDLK_PERIOD ? 15 : -15, 0);
        } else if (k == SDLK_SEMICOLON || k == SDLK_APOSTROPHE) {
            orbit_by(key_view(), 0, k == SDLK_SEMICOLON ? 10 : -10);
        } else if (k >= SDLK_0 && k <= SDLK_9 && !ctrl) {
            cmd(Cmd::SetColour, static_cast<int>(k - SDLK_0) + (shift ? 10 : 0));
        }
        if (closing_) return leave();
        return std::nullopt;
    }

    // Turning a view round: a set view becomes the user's own.
    void orbit_by(int view, double yaw, double pitch) {
        Viewport& vp = views_vp_[static_cast<size_t>(view)];
        vp.yaw += yaw;
        vp.pitch = std::clamp(vp.pitch + pitch, -90.0, 90.0);
        if (vp.kind != ViewKind::Perspective) vp.kind = ViewKind::User;
    }

    void on_press(uint8_t button, float x, float y) {
        mouse_x_ = x;
        mouse_y_ = y;
        if (prompt_ != Prompt::None || help_) {
            help_ = false;
            return;
        }
        const int hit = hit_at(x, y);
        if (button == SDL_BUTTON_LEFT && hit >= 0) {
            const Hit h = hits_[static_cast<size_t>(hit)];
            if (h.cmd == Cmd::OpenMenu) {
                open_menu_ = open_menu_ == h.arg ? -1 : h.arg;
                menu_view_ = -1;
            } else if (h.cmd == Cmd::ViewMenu) {
                menu_view_ = menu_view_ == h.arg ? -1 : h.arg;
                open_menu_ = -1;
                active_ = h.arg;
            } else {
                run_cmd(h.cmd, h.arg);
            }
            return;
        }
        if (menu_open()) {  // a click away from an open menu closes it
            open_menu_ = menu_view_ = -1;
            return;
        }
        const int v = view_at(x, y);
        if (v < 0) return;
        const bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
        if (button == SDL_BUTTON_RIGHT) {
            if (drag_.moving) {
                end_drag(false);  // a right-click while dragging puts it back
                return;
            }
            if (nav_ != Nav::None) {
                nav_ = Nav::None;
                return;
            }
            (shift ? pan_ : orbit_) = true;
            nav_view_ = v;
            return;
        }
        if (button == SDL_BUTTON_MIDDLE) {
            pan_ = true;
            nav_view_ = v;
            return;
        }
        if (button != SDL_BUTTON_LEFT) return;
        active_ = v;
        press_x_ = x;
        press_y_ = y;
        if (nav_ != Nav::None) {
            (nav_ == Nav::Pan ? pan_ : orbit_) = true;
            nav_view_ = v;
            return;
        }
        auto& sel = mode_ == Mode::Vertices ? sel_verts_ : sel_faces_;
        const int under = mode_ == Mode::Vertices ? hover_vertex_ : hover_face_;
        if (under < 0) {
            if (tool_ != Tool::Move && has_selection()) {
                arm_drag(v, -1);  // turning and sizing work from anywhere
                drag_.clear_on_click = !shift;
                return;
            }
            box_ = Box{true, v, x, y, x, y, shift};
            return;
        }
        const auto at = std::find(sel.begin(), sel.end(), static_cast<uint16_t>(under));
        if (shift && at != sel.end()) {
            sel.erase(at);
            return;
        }
        const bool among = at != sel.end() && sel.size() > 1;
        if (at == sel.end()) {
            if (!shift) sel.clear();
            sel.push_back(static_cast<uint16_t>(under));
        }
        // Ready to move what's picked: the point taken is the vertex, or the face's corner nearest the pointer.
        int lead = mode_ == Mode::Vertices ? under : -1;
        if (mode_ == Mode::Faces) {
            const Shown& s = shown_[static_cast<size_t>(v)];
            float best = 1e9f;
            for (const auto& prim : shown().faces[static_cast<size_t>(under)].prims) {
                for (const uint16_t p : prim) {
                    if (p >= s.vscreen.size() || s.vscreen[p][2] < 0) continue;
                    const float d = std::hypot(s.vscreen[p][0] - x, s.vscreen[p][1] - y);
                    if (d < best) {
                        best = d;
                        lead = p;
                    }
                }
            }
        }
        arm_drag(v, lead);
        if (among && !shift) drag_.only = under;
    }

    void on_motion(float nx, float ny) {
        if (orbit_) {
            orbit_by(nav_view_, (nx - mouse_x_) * 0.4, (ny - mouse_y_) * 0.4);
        } else if (pan_) {
            Viewport& vp = views_vp_[static_cast<size_t>(nav_view_)];
            const View& v = views_[static_cast<size_t>(nav_view_)];
            const double k = vp.dist / v.focal;
            vp.target = add(vp.target, add(mul(v.r, -(nx - mouse_x_) * k), mul(v.u, (ny - mouse_y_) * k)));
        } else if (drag_.armed && (drag_.moving || std::abs(nx - press_x_) + std::abs(ny - press_y_) > 4)) {
            drag_.moving = true;
            update_drag(nx, ny);
        } else if (box_.on) {
            box_.x1 = nx;
            box_.y1 = ny;
        }
        mouse_x_ = nx;
        mouse_y_ = ny;
    }

    void on_release(uint8_t button) {
        if (button != SDL_BUTTON_LEFT) {
            orbit_ = pan_ = false;
            return;
        }
        if (orbit_ || pan_) {
            orbit_ = pan_ = false;
            return;
        }
        if (drag_.armed) {
            const bool clicked = !drag_.moving && drag_.clear_on_click;
            const int only = drag_.moving ? -1 : drag_.only;
            end_drag(true);
            if (clicked) clear_selection();
            if (only >= 0) (mode_ == Mode::Vertices ? sel_verts_ : sel_faces_).assign(1, static_cast<uint16_t>(only));
            return;
        }
        if (box_.on) {
            box_.on = false;
            if (std::abs(box_.x1 - box_.x0) + std::abs(box_.y1 - box_.y0) <= 4) {
                if (!box_.add) clear_selection();
                return;
            }
            const float lx = std::min(box_.x0, box_.x1), hx = std::max(box_.x0, box_.x1);
            const float ly = std::min(box_.y0, box_.y1), hy = std::max(box_.y0, box_.y1);
            const Shown& s = shown_[static_cast<size_t>(box_.view)];
            const auto inside = [&](uint16_t v) {
                return v < s.vscreen.size() && s.vscreen[v][2] >= 0 && s.vscreen[v][0] >= lx && s.vscreen[v][0] <= hx &&
                       s.vscreen[v][1] >= ly && s.vscreen[v][1] <= hy;
            };
            const ModelData& m = shown();
            if (!box_.add) clear_selection();
            if (mode_ == Mode::Vertices) {
                for (size_t i = kFirst; i < m.verts.size(); ++i) {
                    const auto v = static_cast<uint16_t>(i);
                    if (inside(v) && std::find(sel_verts_.begin(), sel_verts_.end(), v) == sel_verts_.end()) sel_verts_.push_back(v);
                }
            } else {
                for (size_t f = 0; f < m.faces.size(); ++f) {
                    bool all = true;
                    for (const auto& prim : m.faces[f].prims) {
                        for (const uint16_t v : prim) all = all && inside(v);
                    }
                    const auto fi = static_cast<uint16_t>(f);
                    if (all && std::find(sel_faces_.begin(), sel_faces_.end(), fi) == sel_faces_.end()) sel_faces_.push_back(fi);
                }
            }
        }
    }

    // --- The 3D views ---
    struct Shown {
        std::vector<std::array<float, 3>> vscreen;  // per vertex: x, y, depth (depth < 0: not on screen)
        std::vector<ScreenTri> tris;
    };

    // A segment in model space, cut at the near plane, drawn.
    static void segment(SceneMaker& sm, const View& v, Vec a, Vec b, const en::SceneColour& c, float width,
                        SceneMaker::Layer layer) {
        if (!v.flat) {
            const double za = dot(sub(a, v.cam), v.f), zb = dot(sub(b, v.cam), v.f);
            constexpr double kNear = 2;
            if (za < kNear && zb < kNear) return;
            if (za < kNear) a = add(a, mul(sub(b, a), (kNear - za) / (zb - za)));
            if (zb < kNear) b = add(b, mul(sub(a, b), (kNear - zb) / (za - zb)));
        }
        float ax = 0, ay = 0, bx = 0, by = 0;
        double az = 0, bz = 0;
        if (!v.project(a, ax, ay, az) || !v.project(b, bx, by, bz)) return;
        sm.line(ax, ay, az, bx, by, bz, c, width, layer, (az + bz) / 2);
    }

    // The grid: in a set flat view across it through the origin, else on the ground (y 0).
    void draw_grid(SceneMaker& sm, const View& v, const Viewport& vp) {
        const double ppu = v.flat ? v.scale : v.focal / vp.dist;  // pixels a unit, at the target
        double g = std::max(1, step_unit_ * 4);
        while (g * ppu < 10 * scale_) g *= 2;
        while (g * ppu > 48 * scale_ && g >= 2) g /= 2;
        int a = 0, b = 2;  // the plane's axes
        switch (vp.kind) {
        case ViewKind::Front:
        case ViewKind::Back: b = 1; break;
        case ViewKind::Left:
        case ViewKind::Right: a = 2, b = 1; break;
        default: break;
        }
        const bool across = vp.kind != ViewKind::Perspective && vp.kind != ViewKind::User && vp.kind != ViewKind::Iso;
        double lo[2], hi[2];
        if (across) {
            const double half = std::hypot(v.x1 - v.x0, v.y1 - v.y0) / 2 / v.scale;
            lo[0] = vp.target[static_cast<size_t>(a)] - half;
            hi[0] = vp.target[static_cast<size_t>(a)] + half;
            lo[1] = vp.target[static_cast<size_t>(b)] - half;
            hi[1] = vp.target[static_cast<size_t>(b)] + half;
        } else {
            const double r = std::max(radius_ * 1.6, g * 8);
            lo[0] = lo[1] = -r;
            hi[0] = hi[1] = r;
        }
        while ((hi[0] - lo[0]) / g + (hi[1] - lo[1]) / g > 160) g *= 2;
        for (int side = 0; side < 2; ++side) {
            const int along = side ? b : a, at = side ? a : b;  // lines along one axis, at steps of the other
            const double from = std::floor(lo[side ? 0 : 1] / g) * g, to = std::ceil(hi[side ? 0 : 1] / g) * g;
            for (double t = from; t <= to + 0.5; t += g) {
                Vec p{0, 0, 0}, q{0, 0, 0};
                p[static_cast<size_t>(at)] = q[static_cast<size_t>(at)] = t;
                p[static_cast<size_t>(along)] = lo[side ? 1 : 0];
                q[static_cast<size_t>(along)] = hi[side ? 1 : 0];
                const long n = std::lround(t / g);
                const uint32_t colour = n == 0 ? mix(kAxisColour[along], kViewBack, 0.45) : n % 4 == 0 ? 0x34406A : 0x28304E;
                segment(sm, v, p, q, rgb(colour), static_cast<float>(scale_), SceneMaker::Layer::Back);
            }
        }
    }

    void build_scene() {
        const ModelData& md = shown();
        SceneMaker sm(scene_);
        scene_.view_x0 = static_cast<int>(out_x(kListW));
        scene_.view_y0 = static_cast<int>(out_y(kTop));
        scene_.view_x1 = static_cast<int>(out_x(W_ - kSideW));
        scene_.view_y1 = static_cast<int>(out_y(H_ - kStatusH));
        const Vec light = unit(Vec{0.4, -1.0, -0.6});
        const std::set<uint16_t> fsel(sel_faces_.begin(), sel_faces_.end()), vsel(sel_verts_.begin(), sel_verts_.end());
        const std::set<uint16_t> picked = selected_vertices();
        Vec lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
        for (size_t i = kFirst; i < md.verts.size(); ++i) {
            for (size_t c = 0; c < 3; ++c) {
                lo[c] = std::min(lo[c], double(md.verts[i][c]));
                hi[c] = std::max(hi[c], double(md.verts[i][c]));
            }
        }
        for (size_t vi = 0; vi < 4; ++vi) {
            Shown& s = shown_[vi];
            s.tris.clear();
            s.vscreen.assign(md.verts.size(), {0, 0, -1});
            if (!visible_[vi]) continue;
            const View& v = views_[vi];
            const Viewport& vp = views_vp_[vi];
            sm.clip(v);
            draw_grid(sm, v, vp);
            std::vector<Vec> mv(md.verts.size());
            for (size_t i = 0; i < md.verts.size(); ++i) {
                mv[i] = as_vec(md.verts[i]);
                float sx = 0, sy = 0;
                double z = 0;
                if (v.project(mv[i], sx, sy, z)) s.vscreen[i] = {sx, sy, static_cast<float>(z)};
            }
            const bool hovered = static_cast<int>(vi) == hover_view_;
            for (size_t fi = 0; fi < md.faces.size(); ++fi) {
                const ModelData::Face& f = md.faces[fi];
                en::SceneColour c = face_colour(f);
                const bool hidden = (f.flags & 0x4000) != 0;
                const float alpha = hidden ? 0.3f : (f.flags & 0x8000) ? 0.6f : 1.0f;
                const bool selected = fsel.count(static_cast<uint16_t>(fi)) != 0;
                if (selected) c = {c.r * 0.5f + 0.5f, c.g * 0.5f + 0.38f, c.b * 0.5f + 0.12f};
                else if (hovered && static_cast<int>(fi) == hover_face_) c = {c.r * 0.75f + 0.25f, c.g * 0.75f + 0.25f, c.b * 0.75f + 0.25f};
                for (const auto& prim : f.prims) {
                    if (f.lines()) {
                        for (size_t k = 0; k + 1 < prim.size(); ++k)
                            segment(sm, v, mv[prim[k]], mv[prim[k + 1]], c, static_cast<float>(1.5 * scale_), SceneMaker::Layer::Model);
                        continue;
                    }
                    std::vector<en::geometry::P3> pts;
                    for (const uint16_t p : prim) pts.push_back({float(mv[p][0]), float(mv[p][1]), float(mv[p][2])});
                    std::vector<uint16_t> idx;
                    en::geometry::triangulate(pts.data(), static_cast<int>(pts.size()), idx);
                    en::SceneColour lit = c;
                    if (lighting_) {
                        const double k = 0.6 + 0.4 * std::abs(dot(game::polygon_normal(md, prim), light));
                        lit = {float(c.r * k), float(c.g * k), float(c.b * k)};
                    }
                    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                        float x[3], y[3];
                        double z[3];
                        bool ok = true;
                        for (size_t q = 0; q < 3; ++q) {
                            const auto& p = s.vscreen[prim[idx[t + q]]];
                            ok = ok && p[2] >= 0;
                            x[q] = p[0];
                            y[q] = p[1];
                            z[q] = p[2];
                        }
                        if (!ok) continue;
                        sm.tri(x, y, z, lit, alpha, (z[0] + z[1] + z[2]) / 3);
                        s.tris.push_back({{x[0], x[1], x[2]}, {y[0], y[1], y[2]}, std::min({z[0], z[1], z[2]}), static_cast<int>(fi)});
                    }
                    // Edges: the picked faces' always, every face's with the edges on.
                    if (wire_ || selected) {
                        for (size_t k = 0; k < prim.size(); ++k) {
                            segment(sm, v, mv[prim[k]], mv[prim[(k + 1) % prim.size()]], selected ? rgb(kGold) : rgb(0x101010),
                                    static_cast<float>((selected ? 1.5 : 1.0) * scale_),
                                    selected ? SceneMaker::Layer::Front : SceneMaker::Layer::Model);
                        }
                    }
                }
            }
            // The mirror's plane (x 0), with symmetry on.
            if (symmetry_ && lo[0] <= hi[0]) {
                const double m = std::max(radius_ * 0.15, 4.0);
                const Vec corners[4] = {{0, lo[1] - m, lo[2] - m}, {0, hi[1] + m, lo[2] - m}, {0, hi[1] + m, hi[2] + m}, {0, lo[1] - m, hi[2] + m}};
                for (int k = 0; k < 4; ++k)
                    segment(sm, v, corners[k], corners[(k + 1) % 4], rgb(0x2FA8B8), static_cast<float>(scale_), SceneMaker::Layer::Model);
            }
            // Vertices (picking vertices), the picked in gold.
            if (mode_ == Mode::Vertices) {
                for (size_t i = kFirst; i < md.verts.size(); ++i) {
                    if (s.vscreen[i][2] < 0) continue;
                    const bool sel = vsel.count(static_cast<uint16_t>(i)) != 0, hov = hovered && static_cast<int>(i) == hover_vertex_;
                    sm.square(s.vscreen[i][0], s.vscreen[i][1], static_cast<float>((sel || hov ? 3.0 : 2.0) * scale_),
                              rgb(sel ? kGold : hov ? 0xFFFFFF : 0xB0B8C8));
                }
            }
            // The pivot that turning and sizing work round.
            if (tool_ != Tool::Move && !picked.empty()) {
                const Vec c = drag_.armed ? drag_.centre : selection_centre();
                float px = 0, py = 0;
                double pz = 0;
                if (v.project(c, px, py, pz)) {
                    const float r = static_cast<float>(6 * scale_);
                    sm.line(px - r, py, 1, px + r, py, 1, rgb(kGold), static_cast<float>(scale_), SceneMaker::Layer::Front);
                    sm.line(px, py - r, 1, px, py + r, 1, rgb(kGold), static_cast<float>(scale_), SceneMaker::Layer::Front);
                }
            }
            // Snapping's guides: dashes from the vertices lined up with, a ring round the one landed on.
            if (drag_.moving && static_cast<int>(vi) == drag_.view && drag_.lead >= 0 && drag_.lead < static_cast<int>(md.verts.size())) {
                const Vec to = mv[static_cast<size_t>(drag_.lead)];
                for (const Guide& g : guides_) {
                    const Vec from = mv[static_cast<size_t>(g.vertex)];
                    if (g.axis < 0) {
                        if (s.vscreen[static_cast<size_t>(g.vertex)][2] < 0) continue;
                        const float x = s.vscreen[static_cast<size_t>(g.vertex)][0], y = s.vscreen[static_cast<size_t>(g.vertex)][1];
                        const float r = static_cast<float>(7 * scale_);
                        const en::SceneColour c = rgb(0xFFFFFF);
                        sm.line(x - r, y - r, 1, x + r, y - r, 1, c, static_cast<float>(scale_), SceneMaker::Layer::Front);
                        sm.line(x + r, y - r, 1, x + r, y + r, 1, c, static_cast<float>(scale_), SceneMaker::Layer::Front);
                        sm.line(x + r, y + r, 1, x - r, y + r, 1, c, static_cast<float>(scale_), SceneMaker::Layer::Front);
                        sm.line(x - r, y + r, 1, x - r, y - r, 1, c, static_cast<float>(scale_), SceneMaker::Layer::Front);
                        continue;
                    }
                    constexpr int kDashes = 12;
                    for (int d = 0; d < kDashes; d += 2) {
                        const Vec a = add(from, mul(sub(to, from), static_cast<double>(d) / kDashes));
                        const Vec b = add(from, mul(sub(to, from), static_cast<double>(d + 1) / kDashes));
                        segment(sm, v, a, b, rgb(kAxisColour[g.axis]), static_cast<float>(1.5 * scale_), SceneMaker::Layer::Front);
                    }
                }
            }
            // The axes' tripod in the corner.
            const float tx = v.x0 + static_cast<float>(20 * scale_), ty = v.y1 - static_cast<float>(20 * scale_);
            for (int c = 0; c < 3; ++c) {
                const Vec e = axis_vec(c);
                const float dx = static_cast<float>(dot(e, v.r)), dy = static_cast<float>(-dot(e, v.u));
                const float l = static_cast<float>(13 * scale_);
                sm.line(tx, ty, 1, tx + dx * l, ty + dy * l, 1, rgb(kAxisColour[c]), static_cast<float>(1.5 * scale_), SceneMaker::Layer::Front);
            }
        }
        sm.finish();
    }

    // --- The panels ---
    void add_hit(int x, int y, int w, int h, Cmd c, int arg = 0, std::string tip = {}) { hits_.push_back({x, y, w, h, c, arg, std::move(tip)}); }
    bool hovering(int x, int y, int w, int h) const {
        const int mx = canvas_x(mouse_x_), my = canvas_y(mouse_y_);
        return mx >= x && mx < x + w && my >= y && my < y + h;
    }

    void icon_button(int x, int y, const ToolButton& b) {
        const bool on = checked(b.cmd, b.arg), ok = enabled(b.cmd), hov = ok && hovering(x, y, kBtn, kBtn);
        if (on || hov) canvas_.fill_rect(x, y, kBtn, kBtn, on ? kSelection : kButtonHover);
        if (on) box(canvas_, x, y, kBtn, kBtn, kGold);
        draw_icon(canvas_, x + 3, y + 3, *b.icon, !ok, b.flip);
        add_hit(x, y, kBtn, kBtn, b.cmd, b.arg, b.tip);
    }
    // A text button `w` wide, lit when its command is on.
    void text_button(int x, int y, int w, const std::string& label, Cmd c, int arg = 0, std::string tip = {}) {
        const bool on = checked(c, arg), ok = enabled(c), hov = ok && hovering(x, y, w, 14);
        canvas_.fill_rect(x, y, w, 14, on ? kSelection : hov ? kButtonHover : kButton);
        box(canvas_, x, y, w, 14, on ? kGold : kRule);
        const std::string t = label.substr(0, static_cast<size_t>((w - 2) / kGlyph));
        canvas_.text(x + (w - text_width(t)) / 2, y + 3, t, !ok ? kDim : on ? kGold : kValue);
        add_hit(x, y, w, 14, c, arg, std::move(tip));
    }
    void check_box(int x, int y, int w, const std::string& label, Cmd c) {
        const bool on = checked(c, 0), ok = enabled(c);
        box(canvas_, x, y + 1, 8, 8, ok ? kLabel : kDim);
        if (on) canvas_.fill_rect(x + 2, y + 3, 4, 4, kGold);
        canvas_.text(x + 12, y, label.substr(0, static_cast<size_t>((w - 12) / kGlyph)), ok ? kHelp : kDim);
        add_hit(x, y, w, 10, c);
    }
    int group(int x, int y, int w, const char* title) {
        canvas_.text(x, y, title, kLabel);
        const int tw = text_width(title) + 4;
        canvas_.fill_rect(x + tw, y + 4, w - tw, 1, kRule);
        return y + 12;
    }

    void draw_ui() {
        const int W = W_;
        // The title.
        canvas_.fill_rect(0, 0, W, kTitleH, kPanel);
        const std::string title = "Object editor: " + (pack_name_.empty() ? std::string("a new set") : pack_name_) +
                                  (dirty_ ? " (changed)" : "") + "  -  " + item_name(current_);
        canvas_.text(6, 3, fit_left(title, static_cast<size_t>((W - 12) / kGlyph)), kGold);
        // The menus.
        canvas_.fill_rect(0, kTitleH, W, kMenuH, kBar);
        int mx = 4;
        for (size_t i = 0; i < menus().size(); ++i) {
            const int w = text_width(menus()[i].title) + 12;
            const bool open = open_menu_ == static_cast<int>(i);
            if (open || hovering(mx, kTitleH, w, kMenuH)) canvas_.fill_rect(mx, kTitleH, w, kMenuH, open ? kSelection : kButtonHover);
            canvas_.text(mx + 6, kTitleH + 3, menus()[i].title, open ? kGold : kHelp);
            add_hit(mx, kTitleH, w, kMenuH, Cmd::OpenMenu, static_cast<int>(i));
            menu_x_[i] = mx;
            mx += w;
        }
        // The toolbar.
        const int ty = kTitleH + kMenuH;
        canvas_.fill_rect(0, ty, W, kToolH, kPanel);
        canvas_.fill_rect(0, ty + kToolH - 1, W, 1, kRule);
        int tx = 4;
        for (const ToolButton& b : toolbar()) {
            if (!b.icon) {
                canvas_.fill_rect(tx + 2, ty + 4, 1, kToolH - 8, kRule);
                tx += 6;
                continue;
            }
            icon_button(tx, ty + 2, b);
            tx += kBtn + 1;
        }
        // What the tool does now, beside the toolbar.
        const std::string tool = nav_ == Nav::Pan ? "Sliding the view" : nav_ == Nav::Orbit ? "Turning the view"
                                 : std::string(tool_ == Tool::Move ? "Move" : tool_ == Tool::Turn ? "Turn" : "Size") +
                                       (mode_ == Mode::Vertices ? " vertices" : " faces");
        if (tx + text_width(tool) + 12 < W) canvas_.text(tx + 8, ty + 9, tool, kHint);

        draw_list();
        draw_views();
        draw_panel();
        draw_status();
        draw_menus();
        draw_tooltip();
        draw_prompts();
    }

    void draw_list() {
        const int top = kTop, bottom = H_ - kStatusH;
        canvas_.fill_rect(0, top, kListW, bottom - top, kPanel);
        canvas_.fill_rect(kListW - 1, top, 1, bottom - top, kRule);
        canvas_.text(6, top + 4, "Models", kLabel);
        const int list_top = top + 16;
        if (reveal_) {
            const int ry = static_cast<int>(std::find(rows_.begin(), rows_.end(), current_) - rows_.begin()) * kRowH;
            list_scroll_ = std::max(std::min(list_scroll_, ry), ry + kRowH - (bottom - list_top));
            reveal_ = false;
        }
        list_scroll_ = std::clamp(list_scroll_, 0, std::max(0, static_cast<int>(rows_.size()) * kRowH - (bottom - list_top)));
        for (size_t i = 0; i < rows_.size(); ++i) {
            const int y = list_top + static_cast<int>(i) * kRowH - list_scroll_;
            if (y < list_top || y + kRowH > bottom) continue;
            const int id = rows_[i];
            if (id == kSectionRow) {
                canvas_.fill_rect(4, y + 1, kListW - 10, 1, kRule);
                canvas_.text(6, y + 3, "City objects", kLabel);
                continue;
            }
            const bool hov = hovering(1, y - 1, kListW - 3, kRowH);
            if (id == current_) canvas_.fill_rect(1, y - 1, kListW - 3, kRowH, kSelection);
            const std::string label = (edited(pack_, id) ? "*" : " ") +
                                      (is_object(id) ? hex4(routine_of(id)) + " " + city_object(id).name
                                                     : std::to_string(id) + " " + game::model_name(id));
            canvas_.text(4, y, label.substr(0, static_cast<size_t>((kListW - 8) / kGlyph)), id == current_ ? kGold : hov ? kValue : kHelp);
            add_hit(1, y - 1, kListW - 3, kRowH, Cmd::PickItem, id);
        }
    }

    void draw_views() {
        const int ax0 = kListW, ay0 = kTop, ax1 = W_ - kSideW, ay1 = H_ - kStatusH;
        if (quad_) {
            const int mx = (ax0 + ax1) / 2, my = (ay0 + ay1) / 2;
            canvas_.fill_rect(mx - 1, ay0, 2, ay1 - ay0, kPanel);
            canvas_.fill_rect(ax0, my - 1, ax1 - ax0, 2, kPanel);
        }
        for (size_t i = 0; i < 4; ++i) {
            if (!visible_[i]) continue;
            const auto& r = rect_[i];
            if (quad_ && static_cast<int>(i) == active_) box(canvas_, r[0], r[1], r[2] - r[0], r[3] - r[1], 0xE0B030);
            // Its name, a menu of views when clicked.
            const std::string label = views_vp_[i].name();
            const int lw = text_width(label) + 8;
            const bool hov = hovering(r[0] + 3, r[1] + 3, lw, 11) || menu_view_ == static_cast<int>(i);
            canvas_.text(r[0] + 7, r[1] + 5, label, hov ? kValue : kViewLabel);
            if (hov) box(canvas_, r[0] + 3, r[1] + 3, lw, 12, kRule);
            add_hit(r[0] + 3, r[1] + 3, lw, 12, Cmd::ViewMenu, static_cast<int>(i), "Click for the views");
            // The tripod's letters.
            const View& v = views_[i];
            const int tx = r[0] + 20, ty = r[3] - 20;
            for (int c = 0; c < 3; ++c) {
                const Vec e = axis_vec(c);
                const double dx = dot(e, v.r), dy = -dot(e, v.u);
                if (std::hypot(dx, dy) < 0.3) continue;
                canvas_.text(tx + static_cast<int>(std::lround(dx * 17)) - 3, ty + static_cast<int>(std::lround(dy * 17)) - 4,
                             std::string(1, static_cast<char>('x' + c)), kAxisColour[c]);
            }
        }
        // Picking with a box.
        if (box_.on) {
            const int x0 = canvas_x(std::min(box_.x0, box_.x1)), y0 = canvas_y(std::min(box_.y0, box_.y1));
            const int x1 = canvas_x(std::max(box_.x0, box_.x1)), y1 = canvas_y(std::max(box_.y0, box_.y1));
            box(canvas_, x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0), kGold);
        }
    }

    void draw_panel() {
        const int x0 = W_ - kSideW, top = kTop, bottom = H_ - kStatusH;
        canvas_.fill_rect(x0, top, kSideW, bottom - top, kPanel);
        canvas_.fill_rect(x0, top, 1, bottom - top, kRule);
        const int x = x0 + 6, w = kSideW - 12, half = (w - 4) / 2;
        // The tabs.
        for (int t = 0; t < 2; ++t) {
            const int bx = x0 + 3 + t * ((kSideW - 6) / 2), bw = (kSideW - 6) / 2 - 1;
            const bool on = panel_tab_ == t;
            canvas_.fill_rect(bx, top + 3, bw, 14, on ? kSelection : hovering(bx, top + 3, bw, 14) ? kButtonHover : kBar);
            if (on) canvas_.fill_rect(bx, top + 16, bw, 1, kGold);
            const char* name = t ? "Modify" : "Create";
            canvas_.text(bx + (bw - text_width(name)) / 2, top + 6, name, on ? kGold : kHelp);
            add_hit(bx, top + 3, bw, 14, Cmd::Tab, t);
        }
        int y = top + 22;
        if (panel_tab_ == 0) {
            y = group(x, y, w, "Shapes");
            for (size_t i = 0; i < std::size(kPanelShapes); ++i) {
                const Shape s = kPanelShapes[i];
                text_button(x + static_cast<int>(i % 2) * (half + 4), y + static_cast<int>(i / 2) * 16, half, game::shape_name(s),
                            Cmd::AddShape, static_cast<int>(s),
                            std::string("Add a ") + game::shape_name(s) + "\nIn the middle of the view, on the ground");
            }
            y += 4 * 16 + 2;
            canvas_.text(x, y + 3, "Size " + std::to_string(shape_size_), kHelp);
            text_button(x + w - 34, y, 16, "-", Cmd::Smaller, 0, "Smaller shapes");
            text_button(x + w - 16, y, 16, "+", Cmd::Bigger, 0, "Bigger shapes");
            y += 20;
        } else {
            y = group(x, y, w, "Pick");
            text_button(x, y, half, "Vertices", Cmd::Vertices, 0, "Pick vertices (Tab)");
            text_button(x + half + 4, y, half, "Faces", Cmd::Faces, 0, "Pick faces (Tab)");
            y += 16;
            const size_t n = mode_ == Mode::Vertices ? sel_verts_.size() : sel_faces_.size();
            canvas_.text(x, y + 1, n ? std::to_string(n) + " picked" : "Nothing picked", n ? kValue : kHint);
            y += 13;
            y = group(x, y, w, "Add");
            text_button(x, y, half, "Vertex", Cmd::NewVertex, 0, "New vertex (A)\nIn the middle of what's picked");
            text_button(x + half + 4, y, half, "Face", Cmd::NewFace, 0, "New face (F)\nThrough the picked vertices, in order");
            y += 18;
            y = group(x, y, w, "Change");
            text_button(x, y, half, "Extrude", Cmd::Extrude, 0, "Extrude (E)\nPull the picked faces out, with sides");
            text_button(x + half + 4, y, half, "Flip", Cmd::Flip, 0, "Turn round (R)\nThe picked faces show their other side");
            text_button(x, y + 16, half, "Mirror", Cmd::Mirror, 0, "Mirror copy (M)\nA copy across east-west");
            text_button(x + half + 4, y + 16, half, "Delete", Cmd::Delete, 0, "Delete (Delete)");
            y += 34;
            if (mode_ == Mode::Faces) {
                y = group(x, y, w, "Faces");
                check_box(x, y, w, "Hides its back (B)", Cmd::FlagBack);
                check_box(x, y + 11, w, "Outlined (O)", Cmd::FlagOutline);
                check_box(x, y + 22, w, "Hidden (H)", Cmd::FlagHidden);
                y += 35;
            }
        }
        // The colour: for new faces and shapes, and the picked faces'.
        y = group(x, y, w, ("Colour " + std::to_string(colour_)).c_str());
        for (int c = 0; c < 16; ++c) {
            const int sx = x + (c % 8) * (kSwatch + 1), sy = y + (c / 8) * (kSwatch + 1);
            canvas_.fill_rect(sx, sy, kSwatch, kSwatch, argb_of(en::ega_colour(c)));
            if (c == colour_) box(canvas_, sx - 1, sy - 1, kSwatch + 2, kSwatch + 2, kGold);
            add_hit(sx, sy, kSwatch, kSwatch, Cmd::SetColour, c);
        }
        y += 2 * (kSwatch + 1) + 6;
        const auto info = [&](const std::string& s, uint32_t col) {
            if (y + 10 > bottom) return;
            canvas_.text(x, y, s.substr(0, static_cast<size_t>(w / kGlyph)), col);
            y += 11;
        };
        const ModelData& md = shown();
        info(std::to_string(md.verts.size() - kFirst) + " vertices", kValue);
        info(std::to_string(md.faces.size()) + " faces", kValue);
        if (const std::string why = game::check_model(md); !why.empty()) {
            for (const std::string& l : wrap("Not for the game yet: " + why, static_cast<size_t>(w / kGlyph))) info(l, kBad);
        } else if (panel_tab_ == 0) {
            for (const std::string& l : wrap("A shape goes in the middle of the view, picked, to move, turn or size.",
                                             static_cast<size_t>(w / kGlyph)))
                info(l, kHint);
        }
    }

    void draw_status() {
        const int y0 = H_ - kStatusH;
        canvas_.fill_rect(0, y0, W_, kStatusH, kPanel);
        canvas_.fill_rect(0, y0, W_, 1, kRule);
        // The picked vertex's place (or the middle of what's picked), click to type it.
        const auto vs = selected_vertices();
        Vec p{0, 0, 0};
        if (!vs.empty()) p = selection_centre();
        const bool one = mode_ == Mode::Vertices && sel_verts_.size() == 1;
        int x = 6;
        for (int c = 0; c < 3; ++c) {
            canvas_.text(x, y0 + 5, std::string(1, static_cast<char>('X' + c)), kAxisColour[c]);
            const int fx = x + 10, fw = 52;
            canvas_.fill_rect(fx, y0 + 3, fw, 11, kBar);
            box(canvas_, fx, y0 + 3, fw, 11, kRule);
            const std::string v = vs.empty() ? "" : std::to_string(std::lround(p[static_cast<size_t>(c)]));
            canvas_.text(fx + fw - 3 - text_width(v), y0 + 5, v, one ? kValue : kHelp);
            if (one) add_hit(fx, y0 + 3, fw, 11, Cmd::TypePlace, 0, "Type the vertex's place (Enter)");
            x = fx + fw + 6;
        }
        std::string flags = "Grid " + std::to_string(snap_);
        if (snap_on_) flags += "  Snap";
        if (symmetry_) flags += "  Symmetry";
        if (lock_) flags += std::string("  Along ") + lock_;
        if (hover_vertex_ >= 0) flags += "  Vertex " + std::to_string(hover_vertex_);
        if (hover_face_ >= 0) flags += "  Face " + std::to_string(hover_face_);
        const int nav_w = static_cast<int>(view_controls().size()) * (kBtn + 1) + 6;
        canvas_.text(x + 4, y0 + 5, flags.substr(0, static_cast<size_t>(std::max(0, W_ - nav_w - x - 8) / kGlyph)), kHint);
        // The prompt line.
        const bool fresh = !message_.empty() && SDL_GetTicksNS() - message_at_ < kMessageNs;
        std::string line = fresh ? message_ : drag_note_;
        if (line.empty()) {
            if (nav_ == Nav::Pan) line = "Drag: slide the view (right-click or Esc: done)";
            else if (nav_ == Nav::Orbit) line = "Drag: turn the view (right-click or Esc: done)";
            else if (tool_ == Tool::Turn) line = "Drag round what's picked: turn it (Ctrl: 15 degrees)";
            else if (tool_ == Tool::Scale) line = "Drag out from what's picked: size it (Ctrl: quarters)";
            else line = "Click: pick  Drag: move  Empty space: a box  F1: keys";
        }
        canvas_.text(6, y0 + 17, line.substr(0, static_cast<size_t>(std::max(0, W_ - nav_w - 12) / kGlyph)), fresh ? kGood : kHint);
        // The view controls.
        int bx = W_ - nav_w + 2;
        for (const ToolButton& b : view_controls()) {
            icon_button(bx, y0 + 4, b);
            bx += kBtn + 1;
        }
    }

    void draw_dropdown(int x, int y, const std::vector<MenuItem>& items) {
        size_t longest = 0, keys = 0;
        for (const MenuItem& it : items) {
            if (!it.label) continue;
            longest = std::max(longest, std::string(it.label).size());
            keys = std::max(keys, std::string(it.key).size());
        }
        const int w = static_cast<int>(longest + keys) * kGlyph + 40;
        int h = 4;
        for (const MenuItem& it : items) h += it.label ? 12 : 5;
        x = std::min(x, W_ - w - 2);
        add_hit(x, y, w, h, Cmd::None);  // the menu's own space: a click there doesn't reach what's under it
        canvas_.fill_rect(x, y, w, h, kPanel);
        box(canvas_, x, y, w, h, kRule);
        int iy = y + 2;
        for (const MenuItem& it : items) {
            if (!it.label) {
                canvas_.fill_rect(x + 4, iy + 2, w - 8, 1, kRule);
                iy += 5;
                continue;
            }
            const bool ok = enabled(it.cmd), on = checked(it.cmd, it.arg), hov = ok && hovering(x + 1, iy, w - 2, 12);
            if (hov) canvas_.fill_rect(x + 1, iy, w - 2, 12, kButtonHover);
            if (on) canvas_.fill_rect(x + 6, iy + 4, 4, 4, kGold);
            canvas_.text(x + 16, iy + 2, it.label, !ok ? kDim : on ? kGold : kHelp);
            canvas_.text(x + w - 6 - text_width(it.key), iy + 2, it.key, kHint);
            if (ok) add_hit(x + 1, iy, w - 2, 12, it.cmd, it.arg);
            iy += 12;
        }
    }

    void draw_menus() {
        if (open_menu_ >= 0 && open_menu_ < static_cast<int>(menus().size()))
            draw_dropdown(menu_x_[static_cast<size_t>(open_menu_)], kTitleH + kMenuH, menus()[static_cast<size_t>(open_menu_)].items);
        if (menu_view_ >= 0) {
            const auto& r = rect_[static_cast<size_t>(menu_view_)];
            draw_dropdown(r[0] + 3, r[1] + 16, view_menu_items());
        }
    }

    void draw_tooltip() {
        const int under = hit_at(mouse_x_, mouse_y_);
        if (under < 0 || menu_open()) return;
        const Hit& h = hits_[static_cast<size_t>(under)];
        if (h.tip.empty()) return;
        std::vector<std::string> lines;
        std::string rest = h.tip;
        for (size_t nl; (nl = rest.find('\n')) != std::string::npos;) {
            lines.push_back(rest.substr(0, nl));
            rest = rest.substr(nl + 1);
        }
        lines.push_back(rest);
        size_t longest = 0;
        for (const auto& l : lines) longest = std::max(longest, l.size());
        const int w = static_cast<int>(longest) * kGlyph + 10, hgt = static_cast<int>(lines.size()) * 11 + 6;
        int x = canvas_x(mouse_x_) + 12, y = canvas_y(mouse_y_) + 16;
        x = std::clamp(x, 2, std::max(2, W_ - w - 2));
        if (y + hgt > H_ - 2) y = canvas_y(mouse_y_) - hgt - 6;
        canvas_.fill_rect(x, y, w, hgt, 0x0C1020);
        box(canvas_, x, y, w, hgt, kRule);
        for (size_t i = 0; i < lines.size(); ++i)
            canvas_.text(x + 5, y + 4 + static_cast<int>(i) * 11, lines[i], i == 0 ? kValue : kHint);
    }

    void draw_prompts() {
        const auto panel_box = [&](const std::vector<std::pair<std::string, uint32_t>>& lines) {
            size_t longest = 0;
            for (const auto& l : lines) longest = std::max(longest, l.first.size());
            const int bw = std::min(W_ - 16, static_cast<int>(longest) * kGlyph + 24);
            const int bh = static_cast<int>(lines.size()) * 12 + 18;
            const int bx = (W_ - bw) / 2, by = (H_ - bh) / 2;
            canvas_.fill_rect(bx, by, bw, bh, kPanel);
            box(canvas_, bx, by, bw, bh, kRule);
            for (size_t i = 0; i < lines.size(); ++i)
                canvas_.text(bx + 12, by + 10 + static_cast<int>(i) * 12, fit_left(lines[i].first, static_cast<size_t>((bw - 24) / kGlyph)), lines[i].second);
        };
        if (help_) {
            panel_box({{"Object editor keys", kGold},
                       {"Left: pick (Shift: add or take away); drag: the tool; empty space: a box", kHelp},
                       {"V: move   T: turn   S: size   X, Y, Z: along or round one axis only", kHelp},
                       {"Shift+Tab: snap to vertices (hold Alt while dragging: not now)", kHelp},
                       {"Shift+M: symmetry across east-west   M: a mirror copy", kHelp},
                       {"Right-drag: turn the view   Shift+right or middle drag: slide", kHelp},
                       {"Wheel, + and -: zoom   Home: the whole object   Q: four views", kHelp},
                       {"Num 7, 1, 3 (Ctrl: the other side), I, Num 0: views   P: flat", kHelp},
                       {"[ and ]: the previous, next object (or pick it from the list)", kHelp},
                       {"Arrows, Page Up/Down: move, turn or size what's picked", kHelp},
                       {"G: grid steps   Enter: type the picked vertex's x, y, z", kHelp},
                       {"Tab: vertices or faces   Ctrl+A: all   Esc: pick nothing", kHelp},
                       {"A or Insert: a new vertex   F: a face through the picked ones", kHelp},
                       {"E: extrude the picked faces   Delete: what's picked", kHelp},
                       {"Faces: 0-9, Shift+0-5 or the palette: colour   C: next", kHelp},
                       {"R: turn round   B: hides its back   O: outlined   H: hidden", kHelp},
                       {"L: shading   W: edges   Ctrl+Z, Ctrl+Y: undo, redo", kHelp},
                       {"Ctrl+S: save   Ctrl+Shift+S: save as   Esc: back", kHelp},
                       {"Ctrl+R: this object as the original's   Ctrl+N: all of them", kHelp},
                       {"The game takes 124 vertices, 16 corners a face, at most.", kHint}});
        } else if (prompt_ == Prompt::SaveAs) {
            panel_box({{"Save the objects as:", kLabel}, {field_ + "_", kValue}, {"Enter: save   Esc: cancel", kHint}});
        } else if (prompt_ == Prompt::Coords) {
            panel_box({{"Vertex " + (sel_verts_.empty() ? std::string() : std::to_string(sel_verts_[0])) + ": x (east) y (down) z (north)", kLabel},
                       {field_ + "_", kValue},
                       {"Enter: move it there   Esc: cancel", kHint}});
        } else if (prompt_ == Prompt::Leave) {
            panel_box({{"Save your changes" + (pack_name_.empty() ? std::string() : " to \"" + pack_name_ + "\"") + "?", kLabel},
                       {"Y: save   N: don't save   Esc: keep editing", kHint}});
        } else if (prompt_ == Prompt::Revert) {
            panel_box({{item_name(current_) + " back to the original's?", kLabel}, {"Y: yes   Esc: keep it", kHint}});
        } else if (prompt_ == Prompt::Restart) {
            panel_box({{"Every object back to the original's? Your unsaved changes go.", kLabel},
                       {"Y: start again   Esc: keep editing", kHint}});
        }
    }

    // --- State ---
    Presenter& presenter_;
    Gamepad& gamepad_;
    const GameDir& game_;
    fs::path dir_;
    std::string name_;
    const EditorScript* script_;

    Canvas canvas_;
    int out_w_ = 0, out_h_ = 0, scale_ = 1, W_ = 640, H_ = 400;
    float ox_ = 0, oy_ = 0;
    std::vector<std::uint8_t> pad_keys_;
    float density_ = 1;
    uint64_t opened_ = 0;
    size_t script_key_ = 0, script_shot_ = 0, script_mouse_ = 0;

    std::array<std::optional<ModelData>, game::kModelCount> original_;
    std::map<uint16_t, ModelData> original_objects_;  // the city's code-drawn objects as models
    std::vector<en::CityObject> city_;                 // the ones there are, most used first
    std::vector<int> ids_;   // the items there are: the models, then the city's objects
    std::vector<int> rows_;  // the list's rows: the same, with a heading row before the city's objects

    ModelPack pack_;  // the set being edited: the items changed, the rest the original's
    std::string pack_name_;
    std::string message_;
    uint64_t message_at_ = 0;
    int current_ = 1;  // the Corvette
    struct Edit {
        int id = 0;  // the item
        std::optional<ModelData> before, after;  // nullopt: the original's
    };
    std::vector<Edit> undo_, redo_;
    bool dirty_ = false, closing_ = false;
    std::optional<ModelData> edit_before_;

    Mode mode_ = Mode::Vertices;
    Tool tool_ = Tool::Move;
    Nav nav_ = Nav::None;
    std::vector<uint16_t> sel_verts_, sel_faces_;  // in the order picked (a new face's points follow it)
    bool symmetry_ = false, snap_on_ = true, quad_ = true, lighting_ = true, wire_ = false, help_ = false;
    char lock_ = 0;  // 'x', 'y', 'z': along (or round) that axis only
    int step_unit_ = 1, snap_ = 1, shape_size_ = 16;
    double radius_ = 40;
    uint8_t colour_ = 15;
    int panel_tab_ = 0;  // 0 create, 1 modify

    std::array<Viewport, 4> views_vp_{};  // top, front, left, perspective
    std::array<View, 4> views_{};
    std::array<std::array<int, 4>, 4> rect_{};  // canvas pixels
    std::array<bool, 4> visible_{};
    std::array<Shown, 4> shown_{};
    int active_ = 3;  // the view keys act on (the one under the pointer first)

    Prompt prompt_ = Prompt::None;
    std::string field_;
    bool quit_after_ = false, leave_after_save_ = false;
    int list_scroll_ = 0;
    bool reveal_ = false;
    int open_menu_ = -1, menu_view_ = -1;
    std::array<int, 8> menu_x_{};

    float mouse_x_ = -1, mouse_y_ = -1, press_x_ = 0, press_y_ = 0;
    bool orbit_ = false, pan_ = false;
    int nav_view_ = 0;
    Drag drag_;
    Box box_;
    std::vector<Guide> guides_;
    std::string drag_note_;
    int hover_view_ = -1, hover_vertex_ = -1, hover_face_ = -1, hover_hit_ = -1;
    std::vector<Hit> hits_;
    en::Scene scene_;
};

}  // namespace

std::vector<std::string> list_object_packs(const fs::path& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == kExtension) names.push_back(path_to_utf8(entry.path().stem()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::optional<ModelPack> load_object_pack(const fs::path& dir, const std::string& name, std::string& error) {
    std::ifstream in(pack_path(dir, name), std::ios::binary);
    if (!in) {
        error = "there's no set of objects called \"" + name + "\"";
        return std::nullopt;
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return ModelPack::parse(text, error);
}

std::string run_object_editor(Presenter& presenter, Gamepad& gamepad, const GameDir& game, const fs::path& dir,
                              const std::string& name, const EditorScript* script) {
    Editor editor(presenter, gamepad, game, dir, name, script);
    return editor.run();
}

}  // namespace vette::ui
