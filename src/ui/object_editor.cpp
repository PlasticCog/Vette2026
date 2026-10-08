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
#include <set>
#include <sstream>

#include "core/path_utf8.h"
#include "enhanced/scene.h"
#include "enhanced/scene_geometry.h"
#include "game/city_map.h"
#include "host/machine.h"
#include "platform/gamepad.h"
#include "platform/presenter.h"
#include "ui/canvas.h"
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

constexpr const char* kExtension = ".vobj";
constexpr uint32_t kSeeThrough = 0xFF00FF;  // the overlay canvas's background: the 3D view shows there
constexpr uint32_t kViewBack = 0x1A2238, kPanel = 0x161C30;
constexpr int kTopBar = 14, kBottomBar = 28, kListW = 150, kSideW = 128, kSwatch = 22;
constexpr uint64_t kMessageNs = 4'000'000'000;
constexpr int kFirst = game::kReferenceVertices;  // the vertices before this are the model's frame, not shown

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

void box(Canvas& c, int x, int y, int w, int h, uint32_t colour) {
    c.fill_rect(x, y, w, 1, colour);
    c.fill_rect(x, y + h - 1, w, 1, colour);
    c.fill_rect(x, y, 1, h, colour);
    c.fill_rect(x + w - 1, y, 1, h, colour);
}

// --- Editing a model ---------------------------------------------------------------------------------------

// The face set changed: the view orders are made again from the faces when the model is written.
void faces_changed(ModelData& m) {
    for (auto& o : m.order) o.clear();
}

// Removes vertices (from kFirst on): faces lose them, and those left with too few points go.
void delete_vertices(ModelData& m, const std::set<uint16_t>& gone) {
    std::vector<int> remap(m.verts.size(), -1);
    std::vector<ModelData::Vertex> kept;
    for (size_t i = 0; i < m.verts.size(); ++i) {
        if (i >= kFirst && gone.count(static_cast<uint16_t>(i))) continue;
        remap[i] = static_cast<int>(kept.size());
        kept.push_back(m.verts[i]);
    }
    m.verts = std::move(kept);
    std::vector<ModelData::Face> faces;
    for (ModelData::Face f : m.faces) {
        std::vector<std::vector<uint16_t>> prims;
        for (const auto& prim : f.prims) {
            std::vector<uint16_t> p;
            for (const uint16_t v : prim) {
                if (v < remap.size() && remap[v] >= 0) p.push_back(static_cast<uint16_t>(remap[v]));
            }
            if (p.size() >= (f.lines() ? 2u : 3u)) prims.push_back(std::move(p));
        }
        if (prims.empty()) continue;
        f.prims = std::move(prims);
        faces.push_back(std::move(f));
    }
    m.faces = std::move(faces);
    faces_changed(m);
}

void delete_faces(ModelData& m, const std::set<uint16_t>& gone) {
    std::vector<ModelData::Face> faces;
    for (size_t i = 0; i < m.faces.size(); ++i) {
        if (!gone.count(static_cast<uint16_t>(i))) faces.push_back(m.faces[i]);
    }
    m.faces = std::move(faces);
    faces_changed(m);
}

// A polygon's normal (Newell), in model axes.
Vec normal_of(const ModelData& m, const std::vector<uint16_t>& prim) {
    Vec n{0, 0, 0};
    for (size_t i = 0; i < prim.size(); ++i) {
        const Vec a = as_vec(m.verts[prim[i]]), b = as_vec(m.verts[prim[(i + 1) % prim.size()]]);
        n[0] += (a[1] - b[1]) * (a[2] + b[2]);
        n[1] += (a[2] - b[2]) * (a[0] + b[0]);
        n[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
    return unit(n);
}

// Whether a polygon is convex in its plane (the original's plain filler takes only convex ones).
bool convex(const ModelData& m, const std::vector<uint16_t>& prim) {
    if (prim.size() <= 3) return true;
    const Vec n = normal_of(m, prim);
    int sign = 0;
    for (size_t i = 0; i < prim.size(); ++i) {
        const Vec a = as_vec(m.verts[prim[i]]), b = as_vec(m.verts[prim[(i + 1) % prim.size()]]);
        const Vec c = as_vec(m.verts[prim[(i + 2) % prim.size()]]);
        const double s = dot(cross(sub(b, a), sub(c, b)), n);
        if (std::abs(s) < 1e-6) continue;
        const int sg = s > 0 ? 1 : -1;
        if (sign && sg != sign) return false;
        sign = sg;
    }
    return true;
}

// Fill kind by shape: the convex filler where it will do, else the concave one.
void set_fill_kind(ModelData& m, ModelData::Face& f) {
    if (f.lines()) return;
    bool all_convex = true;
    for (const auto& prim : f.prims) all_convex = all_convex && convex(m, prim);
    f.flags = static_cast<uint16_t>((f.flags & ~0x0E) | (all_convex ? 0 : 2));
}

// The most usual flags and outline of the model's fills, for a new face to match.
void usual_style(const ModelData& m, uint16_t& flags, uint8_t& outline) {
    std::map<std::pair<uint16_t, uint8_t>, int> count;
    for (const auto& f : m.faces) {
        if (!f.lines()) ++count[{static_cast<uint16_t>(f.flags & 0xA001), f.outline}];
    }
    flags = 0x2001;
    outline = 0;
    int best = 0;
    for (const auto& [k, n] : count) {
        if (n > best) {
            best = n;
            flags = k.first;
            outline = k.second;
        }
    }
}

// The vertex at `p`, or a new one there.
uint16_t vertex_at(ModelData& m, const ModelData::Vertex& p) {
    for (size_t i = kFirst; i < m.verts.size(); ++i) {
        if (m.verts[i] == p) return static_cast<uint16_t>(i);
    }
    m.verts.push_back(p);
    return static_cast<uint16_t>(m.verts.size() - 1);
}

ModelData::Vertex mirrored(const ModelData::Vertex& v) { return {static_cast<int16_t>(-v[0]), v[1], v[2]}; }

// --- The view --------------------------------------------------------------------------------------------

// An orbit round `target`; model axes x east, y down, z north (up is -y).
struct Camera {
    double yaw = 215, pitch = 22, dist = 600;
    Vec target{0, 0, 0};
};

struct View {
    Vec cam{}, r{}, u{}, f{};
    double focal = 1, cx = 0, cy = 0;
    // Model point -> output pixel and camera depth; false if behind the near plane.
    bool project(const Vec& p, float& sx, float& sy, double& z) const {
        const Vec d = sub(p, cam);
        z = dot(d, f);
        if (z < 1) return false;
        sx = static_cast<float>(cx + focal * dot(d, r) / z);
        sy = static_cast<float>(cy - focal * dot(d, u) / z);
        return true;
    }
};

View make_view(const Camera& c, double x0, double y0, double x1, double y1) {
    View v;
    constexpr double kDegree = 3.14159265358979323846 / 180;
    const double yaw = c.yaw * kDegree, pitch = c.pitch * kDegree;
    const Vec dir{std::cos(pitch) * std::sin(yaw), -std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
    v.cam = add(c.target, mul(dir, c.dist));
    v.f = mul(dir, -1);
    v.r = unit(cross(v.f, Vec{0, -1, 0}));
    v.u = cross(v.r, v.f);
    v.cx = (x0 + x1) / 2;
    v.cy = (y0 + y1) / 2;
    v.focal = (y1 - y0) * 1.25;
    return v;
}

struct ScreenTri {
    float x[3], y[3];
    double z;  // the nearest of its corners
    int face;
};

// Builds the scene's triangles; painter's order (far first) for drawing without a depth buffer, depth
// 1/z for drawing with one. Markers (vertices, the selection, the axes) go last, always in front.
class SceneMaker {
public:
    explicit SceneMaker(en::Scene& s) : scene_(s) {
        scene_.vertices.clear();
        scene_.indices.clear();
    }
    void tri(const float* x, const float* y, const double* z, const en::SceneColour& c, float a, double key) {
        Item it;
        it.key = key;
        for (int k = 0; k < 3; ++k) it.v[k] = vert(x[k], y[k], c, a, static_cast<float>(1.0 / z[k]));
        items_.push_back(it);
    }
    // A segment `width` pixels wide; `front` draws it over everything.
    void line(float ax, float ay, double az, float bx, float by, double bz, const en::SceneColour& c, float width,
              bool front, double key) {
        const float dx = bx - ax, dy = by - ay, l = std::sqrt(dx * dx + dy * dy);
        if (l < 0.01f) return;
        const float nx = -dy / l * width / 2, ny = dx / l * width / 2;
        const float da = front ? 1.0f : static_cast<float>(1.0 / az * (1 + 3 * en::kDepthStep));
        const float db = front ? 1.0f : static_cast<float>(1.0 / bz * (1 + 3 * en::kDepthStep));
        Item a, b;
        a.key = b.key = front ? -1 : key;
        a.v = {vert(ax + nx, ay + ny, c, 1, da), vert(ax - nx, ay - ny, c, 1, da), vert(bx + nx, by + ny, c, 1, db)};
        b.v = {vert(ax - nx, ay - ny, c, 1, da), vert(bx - nx, by - ny, c, 1, db), vert(bx + nx, by + ny, c, 1, db)};
        (front ? markers_ : items_).push_back(a);
        (front ? markers_ : items_).push_back(b);
    }
    void square(float x, float y, float half, const en::SceneColour& c) {
        Item a, b;
        a.key = b.key = -1;
        a.v = {vert(x - half, y - half, c, 1, 1), vert(x + half, y - half, c, 1, 1), vert(x + half, y + half, c, 1, 1)};
        b.v = {vert(x - half, y - half, c, 1, 1), vert(x + half, y + half, c, 1, 1), vert(x - half, y + half, c, 1, 1)};
        markers_.push_back(a);
        markers_.push_back(b);
    }
    void finish() {
        std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) { return a.key > b.key; });
        for (const auto* list : {&items_, &markers_}) {
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
    en::Scene& scene_;
    std::vector<Item> items_, markers_;
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

enum class Prompt { None, SaveAs, Leave, Revert, Restart, Coords };
enum class Mode { Vertices, Faces };

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
    Canvas canvas;
    std::vector<std::uint8_t> pad_keys;
    int out_w = 0, out_h = 0, scale = 1;
    const auto layout_canvas = [&] {
        presenter.output_size(out_w, out_h);
        scale = std::max(1, std::min(out_w / 640, out_h / 400));
        canvas.reset(std::max(out_w / scale, 1), std::max(out_h / scale, 1), scale, kBackground);
    };
    const auto notice = [&](const std::vector<std::string>& lines, bool wait) {
        for (;;) {
            layout_canvas();
            canvas.text(16, 12, "VETTE!", kGold, 3, true);
            canvas.text(16 + text_width("VETTE!", 3) + 32, 14, "Object editor", kSubtitle);
            int y = 64;
            for (const std::string& l : lines) {
                for (const std::string& w : wrap(l, static_cast<size_t>((canvas.width - 32) / kGlyph))) {
                    canvas.text(16, y, w, kHelp);
                    y += 12;
                }
            }
            if (wait) canvas.text(16, canvas.height - 14, "Press a key", kHint);
            presenter.hide_overlay();
            presenter.present(canvas);
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
    };

    // The models as the game has them: VETTE.EXE started far enough to have unpacked itself.
    notice({"Loading the objects..."}, false);
    std::array<std::optional<ModelData>, game::kModelCount> original;
    std::string error;
    {
        host::MachineConfig config;
        config.game_dir = game.root();
        config.save_dir = fs::temp_directory_path() / "vette2026_object_editor";
        config.cpu_hz = 140'000'000;
        host::Machine machine(config);
        bool ready = false;
        if (machine.boot(error)) {
            ready = game::run_until_started(machine);
            if (!ready) error = "it didn't start";
            for (int id = 0; ready && id < game::kModelCount; ++id) original[static_cast<size_t>(id)] = game::read_model(machine.memory(), id);
        }
        if (!ready || !original[1]) {
            notice({"The object editor couldn't read the objects from VETTE.EXE: " + error}, true);
            return name;
        }
    }
    std::vector<int> ids;  // the models there are
    for (int id = 0; id < game::kModelCount; ++id) {
        if (original[static_cast<size_t>(id)]) ids.push_back(id);
    }

    // The set being edited: the models changed, the rest the original's.
    ModelPack pack;
    std::string pack_name = name;
    std::string message;
    std::uint64_t message_at = 0;
    const auto say = [&](std::string text) {
        message = std::move(text);
        message_at = SDL_GetTicksNS();
    };
    if (!name.empty()) {
        if (auto loaded = load_object_pack(dir, name, error)) {
            pack = std::move(*loaded);
        } else {
            say("Couldn't open \"" + name + "\": " + error + ". These are the original's.");
            pack_name.clear();
        }
    }
    int current = 1;  // the Corvette
    const auto shown = [&]() -> const ModelData& {
        const auto it = pack.models.find(current);
        return it != pack.models.end() ? it->second : *original[static_cast<size_t>(current)];
    };

    // Edits, as before-and-after copies of a model (they're small), to undo and redo.
    struct Edit {
        int id = 0;
        std::optional<ModelData> before, after;  // nullopt: the original's
    };
    std::vector<Edit> undo, redo;
    bool dirty = false;
    std::optional<ModelData> edit_before;
    const auto begin_edit = [&]() -> ModelData& {
        const auto it = pack.models.find(current);
        edit_before = it != pack.models.end() ? std::optional<ModelData>(it->second) : std::nullopt;
        if (it == pack.models.end()) pack.models[current] = *original[static_cast<size_t>(current)];
        return pack.models[current];
    };
    const auto end_edit = [&] {
        const ModelData& now = pack.models[current];
        const ModelData& was = edit_before ? *edit_before : *original[static_cast<size_t>(current)];
        if (now == was) {
            if (!edit_before) pack.models.erase(current);
            return;
        }
        undo.push_back({current, edit_before, now});
        redo.clear();
        dirty = true;
    };
    const auto put = [&](int id, const std::optional<ModelData>& m) {
        if (m) pack.models[id] = *m;
        else pack.models.erase(id);
    };

    // Selection and view.
    Mode mode = Mode::Vertices;
    std::vector<uint16_t> sel_verts, sel_faces;  // in the order picked (a new face's points follow it)
    const auto clear_selection = [&] {
        sel_verts.clear();
        sel_faces.clear();
    };
    Camera cam;
    double axis_length = 32;
    int step_unit = 1, snap = 1;  // the object's step (frame_model), the mouse's
    const auto frame_model = [&] {
        const ModelData& m = shown();
        Vec lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
        for (size_t i = kFirst; i < m.verts.size(); ++i) {
            for (int c = 0; c < 3; ++c) {
                lo[c] = std::min(lo[c], double(m.verts[i][c]));
                hi[c] = std::max(hi[c], double(m.verts[i][c]));
            }
        }
        if (lo[0] > hi[0]) lo = hi = Vec{0, 0, 0};
        cam.target = mul(add(lo, hi), 0.5);
        const double radius = std::max(40.0, 0.5 * std::sqrt(dot(sub(hi, lo), sub(hi, lo))));
        cam.dist = radius * 2.6;
        axis_length = std::max(8.0, radius * 0.35);
        // Steps in proportion: a car is some 25 x 55, a building some thousands.
        step_unit = 1;
        while (step_unit * 2 <= radius / 24) step_unit *= 2;
        snap = step_unit;
    };
    frame_model();
    uint8_t colour = 15;
    char lock = 0;  // 'x', 'y', 'z': mouse moves along that axis only
    bool lighting = true, wire = false, help = false;
    Prompt prompt = Prompt::None;
    std::string field;
    bool quit_after = false, leave_after_save = false;
    int list_scroll = 0;
    // The mouse: orbiting (right), panning (middle or shift+right), moving the selection (left on it).
    float mouse_x = -1, mouse_y = -1, press_x = 0, press_y = 0;
    bool orbit = false, pan = false, dragging = false, left_down = false;
    std::vector<std::pair<uint16_t, ModelData::Vertex>> drag_start;
    double drag_depth = 1;
    Vec drag_centre{0, 0, 0};
    const float density = std::max(0.5f, SDL_GetWindowPixelDensity(presenter.window()));
    const std::uint64_t opened = SDL_GetTicksNS();
    size_t script_key = 0, script_shot = 0, script_mouse = 0;
    en::Scene scene;
    std::vector<ScreenTri> tris;
    std::vector<std::array<float, 3>> vscreen;  // per vertex: x, y, depth (depth < 0: not on screen)

    const auto save = [&](const std::string& as) {
        for (const auto& [id, m] : pack.models) {
            if (const std::string why = game::check_model(m); !why.empty()) {
                say("Not saved: " + game::model_name(id) + ": " + why + ".");
                return false;
            }
        }
        if (save_pack(dir, as, pack, error)) {
            pack_name = as;
            dirty = false;
            say("Saved as \"" + as + "\". Choose it as the launch menu's Objects to play with it.");
            return true;
        }
        say("Not saved: " + error);
        return false;
    };
    const auto leave = [&] {
        presenter.hide_overlay();
        SDL_StopTextInput(presenter.window());
        if (quit_after) {
            SDL_Event q{};
            q.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&q);
        }
        return pack_name.empty() ? name : pack_name;
    };
    const auto switch_model = [&](int id) {
        current = id;
        clear_selection();
        frame_model();
    };
    const auto step_model = [&](int dir_) {
        auto it = std::find(ids.begin(), ids.end(), current);
        const int i = it == ids.end() ? 0 : static_cast<int>(it - ids.begin());
        const int n = static_cast<int>(ids.size());
        switch_model(ids[static_cast<size_t>((i + dir_ + n) % n)]);
    };

    for (;;) {
        layout_canvas();
        canvas.reset(canvas.width, canvas.height, scale, kSeeThrough);
        const int W = canvas.width, H = canvas.height;
        const float ox = static_cast<float>((out_w - W * scale) / 2), oy = static_cast<float>((out_h - H * scale) / 2);
        const float vx0 = ox + static_cast<float>(kListW * scale), vy0 = oy + static_cast<float>(kTopBar * scale);
        const float vx1 = ox + static_cast<float>((W - kSideW) * scale), vy1 = oy + static_cast<float>((H - kBottomBar) * scale);
        const View view = make_view(cam, vx0, vy0, vx1, vy1);
        const bool in_view = mouse_x >= vx0 && mouse_x < vx1 && mouse_y >= vy0 && mouse_y < vy1;
        const auto canvas_x = [&](float sx) { return static_cast<int>((sx - ox) / static_cast<float>(scale)); };
        const auto canvas_y = [&](float sy) { return static_cast<int>((sy - oy) / static_cast<float>(scale)); };

        // What's under the pointer (from the last frame's picture).
        int hover_vertex = -1, hover_face = -1;
        if (in_view && mode == Mode::Vertices) {
            float best = static_cast<float>(8 * scale);
            for (size_t i = kFirst; i < vscreen.size() && i < shown().verts.size(); ++i) {
                if (vscreen[i][2] < 0) continue;
                const float d = std::hypot(vscreen[i][0] - mouse_x, vscreen[i][1] - mouse_y);
                if (d < best) {
                    best = d;
                    hover_vertex = static_cast<int>(i);
                }
            }
        }
        if (in_view && mode == Mode::Faces) {
            double nearest = 1e18;
            for (const ScreenTri& t : tris) {
                const float d = (t.y[1] - t.y[2]) * (t.x[0] - t.x[2]) + (t.x[2] - t.x[1]) * (t.y[0] - t.y[2]);
                if (std::abs(d) < 1e-6f) continue;
                const float a = ((t.y[1] - t.y[2]) * (mouse_x - t.x[2]) + (t.x[2] - t.x[1]) * (mouse_y - t.y[2])) / d;
                const float b = ((t.y[2] - t.y[0]) * (mouse_x - t.x[2]) + (t.x[0] - t.x[2]) * (mouse_y - t.y[2])) / d;
                if (a < 0 || b < 0 || a + b > 1) continue;
                if (t.z < nearest) {
                    nearest = t.z;
                    hover_face = t.face;
                }
            }
        }
        // The model list (canvas pixels).
        const int list_top = kTopBar + 16, list_bottom = H - kBottomBar, row_h = 11;
        list_scroll = std::clamp(list_scroll, 0, std::max(0, static_cast<int>(ids.size()) * row_h - (list_bottom - list_top)));
        const auto list_at = [&](float sx, float sy) {
            const int x = canvas_x(sx), y = canvas_y(sy);
            if (x < 0 || x >= kListW || y < list_top || y >= list_bottom) return -1;
            const int i = (y - list_top + list_scroll) / row_h;
            return i < static_cast<int>(ids.size()) ? ids[static_cast<size_t>(i)] : -1;
        };
        // The palette (canvas pixels).
        const int pal_x = W - kSideW + 8, pal_y = kTopBar + 40;
        const auto palette_at = [&](float sx, float sy) {
            const int x = canvas_x(sx) - pal_x, y = canvas_y(sy) - pal_y;
            if (x < 0 || y < 0 || x >= 4 * (kSwatch + 2) || y >= 4 * (kSwatch + 2)) return -1;
            return (y / (kSwatch + 2)) * 4 + x / (kSwatch + 2);
        };

        // Selection's centre, for new vertices and the coordinates shown.
        const auto selection_centre = [&]() -> Vec {
            const ModelData& m = shown();
            std::set<uint16_t> vs(sel_verts.begin(), sel_verts.end());
            for (const uint16_t f : sel_faces) {
                if (f >= m.faces.size()) continue;
                for (const auto& prim : m.faces[f].prims) vs.insert(prim.begin(), prim.end());
            }
            if (vs.empty()) return cam.target;
            Vec s{0, 0, 0};
            for (const uint16_t v : vs) s = add(s, as_vec(m.verts[v]));
            return mul(s, 1.0 / static_cast<double>(vs.size()));
        };
        const auto selected_vertices = [&]() {
            const ModelData& m = shown();
            std::set<uint16_t> vs(sel_verts.begin(), sel_verts.end());
            for (const uint16_t f : sel_faces) {
                if (f >= m.faces.size()) continue;
                for (const auto& prim : m.faces[f].prims) {
                    for (const uint16_t v : prim) {
                        if (v >= kFirst) vs.insert(v);
                    }
                }
            }
            return vs;
        };
        // Moves the selection by d (model units, rounded to the snap), as one edit.
        const auto nudge = [&](const Vec& d) {
            const auto vs = selected_vertices();
            if (vs.empty()) return;
            ModelData& e = begin_edit();
            for (const uint16_t v : vs) {
                for (int c = 0; c < 3; ++c) {
                    e.verts[v][static_cast<size_t>(c)] = static_cast<int16_t>(std::clamp(
                        std::lround(e.verts[v][static_cast<size_t>(c)] + d[static_cast<size_t>(c)]), -32000L, 32000L));
                }
            }
            end_edit();
        };
        // The model axis nearest a direction in the view, signed.
        const auto axis_like = [](const Vec& d) {
            int best = 0;
            for (int c = 1; c < 3; ++c) {
                if (std::abs(d[static_cast<size_t>(c)]) > std::abs(d[static_cast<size_t>(best)])) best = c;
            }
            Vec a{0, 0, 0};
            a[static_cast<size_t>(best)] = d[static_cast<size_t>(best)] > 0 ? 1 : -1;
            return a;
        };

        // --- Input ---
        if (script) {
            const uint64_t ms = (SDL_GetTicksNS() - opened) / 1'000'000;
            for (; script_key < script->keys.size() && script->keys[script_key].at_ms <= ms; ++script_key) {
                SDL_Event k{};
                k.type = SDL_EVENT_KEY_DOWN;
                k.key.key = script->keys[script_key].key;
                k.key.mod = script->keys[script_key].mod;
                k.key.scancode = SDL_GetScancodeFromKey(k.key.key, nullptr);
                k.key.down = true;
                SDL_PushEvent(&k);
            }
            for (; script_mouse < script->mouse.size() && script->mouse[script_mouse].at_ms <= ms; ++script_mouse) {
                const EditorScript::Mouse& sm = script->mouse[script_mouse];
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
            if (script->quit_ms && ms >= script->quit_ms) {
                dirty = false;
                return leave();
            }
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (quit_shortcut(e)) continue;
            pad_keys.clear();
            gamepad.handle_event(e, pad_keys);
            switch (e.type) {
            case SDL_EVENT_QUIT:
                if (!dirty) {
                    SDL_PushEvent(&e);
                    return leave();
                }
                quit_after = true;
                prompt = Prompt::Leave;
                break;
            case SDL_EVENT_TEXT_INPUT:
                if (prompt == Prompt::SaveAs) {
                    for (const char* p = e.text.text; *p; ++p) {
                        if (name_char(*p) && field.size() < 40) field += *p;
                    }
                } else if (prompt == Prompt::Coords) {
                    for (const char* p = e.text.text; *p; ++p) {
                        if ((std::isdigit(static_cast<unsigned char>(*p)) || *p == '-' || *p == ' ' || *p == ',') && field.size() < 30)
                            field += *p == ',' ? ' ' : *p;
                    }
                }
                break;
            case SDL_EVENT_KEY_DOWN: {
                const SDL_Keycode k = e.key.key;
                const bool ctrl = (e.key.mod & SDL_KMOD_CTRL) != 0, shift = (e.key.mod & SDL_KMOD_SHIFT) != 0;
                if (k == SDLK_F11 || ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && (e.key.mod & SDL_KMOD_ALT))) {
                    if (!e.key.repeat) presenter.toggle_fullscreen();
                    break;
                }
                if (prompt == Prompt::SaveAs || prompt == Prompt::Coords) {
                    if (k == SDLK_ESCAPE) {
                        prompt = Prompt::None;
                        quit_after = leave_after_save = false;
                        SDL_StopTextInput(presenter.window());
                    } else if (k == SDLK_BACKSPACE && !field.empty()) {
                        field.pop_back();
                    } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                        if (prompt == Prompt::SaveAs && !clean_name(field).empty()) {
                            const bool leaving = leave_after_save;
                            leave_after_save = false;
                            SDL_StopTextInput(presenter.window());
                            prompt = Prompt::None;
                            if (save(clean_name(field)) && leaving) return leave();
                        } else if (prompt == Prompt::Coords) {
                            std::istringstream in(field);
                            long x = 0, y = 0, z = 0;
                            if ((in >> x >> y >> z) && sel_verts.size() == 1 && std::abs(x) <= 32000 && std::abs(y) <= 32000 &&
                                std::abs(z) <= 32000) {
                                ModelData& ed = begin_edit();
                                ed.verts[sel_verts[0]] = {static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(z)};
                                end_edit();
                                prompt = Prompt::None;
                                SDL_StopTextInput(presenter.window());
                            } else {
                                say("Three whole numbers: x (east), y (down), z (north).");
                            }
                        }
                    }
                    break;
                }
                if (prompt == Prompt::Leave) {
                    if (k == SDLK_Y) {
                        if (pack_name.empty()) {
                            prompt = Prompt::SaveAs;
                            field = "My objects";
                            leave_after_save = true;
                            SDL_StartTextInput(presenter.window());
                        } else if (save(pack_name)) {
                            return leave();
                        }
                    } else if (k == SDLK_N) {
                        return leave();
                    } else if (k == SDLK_ESCAPE) {
                        prompt = Prompt::None;
                        quit_after = false;
                    }
                    break;
                }
                if (prompt == Prompt::Revert || prompt == Prompt::Restart) {
                    if (k == SDLK_Y) {
                        if (prompt == Prompt::Revert && pack.models.count(current)) {
                            begin_edit();
                            pack.models[current] = *original[static_cast<size_t>(current)];
                            end_edit();
                            pack.models.erase(current);
                            say(game::model_name(current) + " is the original's again.");
                        } else if (prompt == Prompt::Restart) {
                            pack.models.clear();
                            undo.clear();
                            redo.clear();
                            dirty = true;
                            say("Every object is the original's again.");
                        }
                        clear_selection();
                    }
                    if (k == SDLK_Y || k == SDLK_N || k == SDLK_ESCAPE) prompt = Prompt::None;
                    break;
                }
                if (help) {
                    help = false;
                    break;
                }
                const double step = ctrl && !shift ? 1 : shift ? step_unit * 8 : step_unit;
                if (k == SDLK_ESCAPE) {
                    if (!sel_verts.empty() || !sel_faces.empty()) {
                        clear_selection();
                    } else if (!dirty) {
                        return leave();
                    } else {
                        prompt = Prompt::Leave;
                    }
                } else if (k == SDLK_F1) {
                    help = true;
                } else if (ctrl && k == SDLK_Z && !shift) {
                    if (!undo.empty()) {
                        put(undo.back().id, undo.back().before);
                        if (current != undo.back().id) switch_model(undo.back().id);
                        redo.push_back(std::move(undo.back()));
                        undo.pop_back();
                        clear_selection();
                        dirty = true;
                    }
                } else if (ctrl && (k == SDLK_Y || (k == SDLK_Z && shift))) {
                    if (!redo.empty()) {
                        put(redo.back().id, redo.back().after);
                        if (current != redo.back().id) switch_model(redo.back().id);
                        undo.push_back(std::move(redo.back()));
                        redo.pop_back();
                        clear_selection();
                        dirty = true;
                    }
                } else if (ctrl && k == SDLK_S) {
                    if (pack_name.empty() || shift) {
                        prompt = Prompt::SaveAs;
                        field = pack_name.empty() ? "My objects" : pack_name;
                        SDL_StartTextInput(presenter.window());
                    } else {
                        save(pack_name);
                    }
                } else if (ctrl && k == SDLK_R) {
                    if (pack.models.count(current)) prompt = Prompt::Revert;
                } else if (ctrl && k == SDLK_N) {
                    prompt = Prompt::Restart;
                } else if (ctrl && k == SDLK_A) {
                    const ModelData& m = shown();
                    clear_selection();
                    if (mode == Mode::Vertices) {
                        for (size_t i = kFirst; i < m.verts.size(); ++i) sel_verts.push_back(static_cast<uint16_t>(i));
                    } else {
                        for (size_t i = 0; i < m.faces.size(); ++i) sel_faces.push_back(static_cast<uint16_t>(i));
                    }
                } else if (k == SDLK_TAB) {
                    const ModelData& m = shown();
                    mode = mode == Mode::Vertices ? Mode::Faces : Mode::Vertices;
                    // Faces whose points are all selected become the selection, and back.
                    if (mode == Mode::Faces) {
                        const std::set<uint16_t> vs(sel_verts.begin(), sel_verts.end());
                        sel_faces.clear();
                        for (size_t f = 0; f < m.faces.size() && !vs.empty(); ++f) {
                            bool all = true;
                            for (const auto& prim : m.faces[f].prims) {
                                for (const uint16_t v : prim) all = all && vs.count(v);
                            }
                            if (all) sel_faces.push_back(static_cast<uint16_t>(f));
                        }
                        sel_verts.clear();
                    } else {
                        const auto vs = selected_vertices();
                        sel_faces.clear();
                        sel_verts.assign(vs.begin(), vs.end());
                    }
                } else if (k == SDLK_LEFTBRACKET || k == SDLK_RIGHTBRACKET) {
                    step_model(k == SDLK_RIGHTBRACKET ? 1 : -1);
                } else if (k == SDLK_HOME) {
                    frame_model();
                    cam.yaw = 215;
                    cam.pitch = 22;
                } else if (k == SDLK_LEFT || k == SDLK_RIGHT) {
                    nudge(mul(axis_like(view.r), k == SDLK_RIGHT ? step : -step));
                } else if (k == SDLK_UP || k == SDLK_DOWN) {
                    nudge(mul(axis_like(view.u), k == SDLK_UP ? step : -step));
                } else if (k == SDLK_PAGEUP || k == SDLK_PAGEDOWN) {
                    nudge(mul(axis_like(view.f), k == SDLK_PAGEUP ? step : -step));
                } else if (k == SDLK_X || k == SDLK_Y || k == SDLK_Z) {
                    const char a = k == SDLK_X ? 'x' : k == SDLK_Y ? 'y' : 'z';
                    lock = lock == a ? 0 : a;
                    say(lock ? std::string("Mouse moves along ") + (a == 'x' ? "x (east-west)" : a == 'y' ? "y (up-down)" : "z (north-south)") + " only."
                             : "Mouse moves freely in the view.");
                } else if (k == SDLK_G) {
                    snap = snap == 1 ? step_unit * 4 : snap > step_unit ? step_unit : 1;
                    say("Moves in steps of " + std::to_string(snap) + ".");
                } else if (k == SDLK_L) {
                    lighting = !lighting;
                } else if (k == SDLK_W) {
                    wire = !wire;
                } else if (k == SDLK_INSERT || k == SDLK_A) {
                    if (shown().verts.size() >= static_cast<size_t>(game::kMaxVertices)) {
                        say("The game takes " + std::to_string(game::kMaxVertices - kFirst) + " vertices at most.");
                    } else {
                        const Vec c = selection_centre();
                        ModelData& ed = begin_edit();
                        ed.verts.push_back({static_cast<int16_t>(std::lround(c[0] + (sel_verts.empty() && sel_faces.empty() ? 0 : snap * 2))),
                                            static_cast<int16_t>(std::lround(c[1])), static_cast<int16_t>(std::lround(c[2]))});
                        end_edit();
                        mode = Mode::Vertices;
                        clear_selection();
                        sel_verts.push_back(static_cast<uint16_t>(pack.models[current].verts.size() - 1));
                    }
                } else if (k == SDLK_DELETE || k == SDLK_BACKSPACE) {
                    if (mode == Mode::Vertices && !sel_verts.empty()) {
                        ModelData& ed = begin_edit();
                        delete_vertices(ed, std::set<uint16_t>(sel_verts.begin(), sel_verts.end()));
                        end_edit();
                    } else if (mode == Mode::Faces && !sel_faces.empty()) {
                        ModelData& ed = begin_edit();
                        delete_faces(ed, std::set<uint16_t>(sel_faces.begin(), sel_faces.end()));
                        end_edit();
                    }
                    clear_selection();
                } else if (k == SDLK_F && mode == Mode::Vertices) {
                    if (sel_verts.size() < 2) {
                        say("Pick the new face's corners in order (2 for a line), then F.");
                    } else if (sel_verts.size() > static_cast<size_t>(game::kMaxPoints)) {
                        say("A face has " + std::to_string(game::kMaxPoints) + " corners at most.");
                    } else {
                        ModelData& ed = begin_edit();
                        ModelData::Face f;
                        usual_style(ed, f.flags, f.outline);
                        f.colour = colour;
                        f.prims.push_back(sel_verts);
                        if (sel_verts.size() == 2) f.flags = static_cast<uint16_t>((f.flags & ~0x0E) | 4);
                        else set_fill_kind(ed, f);
                        ed.faces.push_back(std::move(f));
                        faces_changed(ed);
                        end_edit();
                        mode = Mode::Faces;
                        clear_selection();
                        sel_faces.push_back(static_cast<uint16_t>(pack.models[current].faces.size() - 1));
                        say("A new face. R turns it round if it shows its back.");
                    }
                } else if (k == SDLK_R && !sel_faces.empty()) {
                    ModelData& ed = begin_edit();
                    for (const uint16_t f : sel_faces) {
                        for (auto& prim : ed.faces[f].prims) std::reverse(prim.begin(), prim.end());
                    }
                    end_edit();
                } else if (k == SDLK_C && !sel_faces.empty()) {
                    ModelData& ed = begin_edit();
                    for (const uint16_t f : sel_faces) {
                        auto& c = ed.faces[f].colour;
                        c = static_cast<uint8_t>(((c & 15) + (shift ? 15 : 1)) % 16);
                        colour = c;
                    }
                    end_edit();
                } else if ((k == SDLK_B || k == SDLK_H || k == SDLK_O) && !sel_faces.empty()) {
                    // The original's face flags: B culled when facing away, H never drawn, O outlined.
                    const uint16_t bit = k == SDLK_B ? 0x0001 : k == SDLK_H ? 0x4000 : 0x2000;
                    ModelData& ed = begin_edit();
                    const bool on = !(ed.faces[sel_faces[0]].flags & bit);
                    for (const uint16_t f : sel_faces) ed.faces[f].flags = static_cast<uint16_t>(on ? ed.faces[f].flags | bit : ed.faces[f].flags & ~bit);
                    if (bit == 0x4000) faces_changed(ed);
                    end_edit();
                } else if (k == SDLK_M) {
                    // A mirror image across x = 0 (east-west), as the original's models are made.
                    ModelData& ed = begin_edit();
                    std::vector<uint16_t> made;
                    if (mode == Mode::Vertices) {
                        for (const uint16_t v : sel_verts) {
                            if (ed.verts.size() >= static_cast<size_t>(game::kMaxVertices)) break;
                            made.push_back(vertex_at(ed, mirrored(ed.verts[v])));
                        }
                    } else {
                        for (const uint16_t f : sel_faces) {
                            ModelData::Face copy = ed.faces[f];
                            bool room = true;
                            for (auto& prim : copy.prims) {
                                for (uint16_t& v : prim) {
                                    if (ed.verts.size() >= static_cast<size_t>(game::kMaxVertices)) room = false;
                                    if (room) v = vertex_at(ed, mirrored(ed.verts[v]));
                                }
                                std::reverse(prim.begin(), prim.end());
                            }
                            if (!room) break;
                            ed.faces.push_back(std::move(copy));
                            made.push_back(static_cast<uint16_t>(ed.faces.size() - 1));
                        }
                        faces_changed(ed);
                    }
                    end_edit();
                    (mode == Mode::Vertices ? sel_verts : sel_faces) = made;
                    say("Mirrored across east-west.");
                } else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && mode == Mode::Vertices && sel_verts.size() == 1) {
                    const auto& v = shown().verts[sel_verts[0]];
                    field = std::to_string(v[0]) + " " + std::to_string(v[1]) + " " + std::to_string(v[2]);
                    prompt = Prompt::Coords;
                    SDL_StartTextInput(presenter.window());
                } else if (k == SDLK_EQUALS || k == SDLK_PLUS || k == SDLK_KP_PLUS) {
                    cam.dist = std::max(30.0, cam.dist / 1.2);
                } else if (k == SDLK_MINUS || k == SDLK_KP_MINUS) {
                    cam.dist = std::min(60000.0, cam.dist * 1.2);
                } else if (k == SDLK_COMMA || k == SDLK_PERIOD) {
                    cam.yaw += k == SDLK_PERIOD ? 15 : -15;
                } else if (k == SDLK_SEMICOLON || k == SDLK_APOSTROPHE) {
                    cam.pitch = std::clamp(cam.pitch + (k == SDLK_SEMICOLON ? 10.0 : -10.0), -89.0, 89.0);
                } else if (k >= SDLK_0 && k <= SDLK_9 && !ctrl) {
                    colour = static_cast<uint8_t>(k - SDLK_0 + (shift ? 10 : 0)) & 15;
                    if (!sel_faces.empty()) {
                        ModelData& ed = begin_edit();
                        for (const uint16_t f : sel_faces) ed.faces[f].colour = colour;
                        end_edit();
                    }
                }
                break;
            }
            case SDL_EVENT_MOUSE_MOTION: {
                const float nx = e.motion.x * density, ny = e.motion.y * density;
                if (orbit) {
                    cam.yaw += (nx - mouse_x) * 0.4;
                    cam.pitch = std::clamp(cam.pitch + (ny - mouse_y) * 0.4, -89.0, 89.0);
                } else if (pan) {
                    const double k = cam.dist / view.focal;
                    cam.target = add(cam.target, add(mul(view.r, -(nx - mouse_x) * k), mul(view.u, (ny - mouse_y) * k)));
                } else if (left_down && !drag_start.empty() &&
                           (dragging || std::abs(nx - press_x) + std::abs(ny - press_y) > 4)) {
                    // Moving the selection in the view's plane (along one axis if locked), in snap steps.
                    dragging = true;
                    const double k = drag_depth / view.focal;
                    Vec d = add(mul(view.r, (nx - press_x) * k), mul(view.u, -(ny - press_y) * k));
                    if (lock) {
                        // Along the axis only: as far as the pointer went along the axis's own line on screen.
                        const auto a = static_cast<size_t>(lock - 'x');
                        Vec step{0, 0, 0};
                        step[a] = 100;
                        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                        double z0 = 0, z1 = 0;
                        d = Vec{0, 0, 0};
                        if (view.project(drag_centre, x0, y0, z0) && view.project(add(drag_centre, step), x1, y1, z1)) {
                            const double ax = x1 - x0, ay = y1 - y0, l2 = ax * ax + ay * ay;
                            if (l2 > 1) d[a] = ((nx - press_x) * ax + (ny - press_y) * ay) / l2 * 100;
                        }
                    }
                    ModelData& ed = pack.models[current];
                    for (const auto& [v, start] : drag_start) {
                        for (int c = 0; c < 3; ++c) {
                            const double moved = start[static_cast<size_t>(c)] + d[static_cast<size_t>(c)];
                            ed.verts[v][static_cast<size_t>(c)] =
                                static_cast<int16_t>(std::clamp(std::lround(moved / snap) * snap, -32000L, 32000L));
                        }
                    }
                }
                mouse_x = nx;
                mouse_y = ny;
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                mouse_x = e.button.x * density;
                mouse_y = e.button.y * density;
                if (prompt != Prompt::None || help) {
                    help = false;
                    break;
                }
                const bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
                if (e.button.button == SDL_BUTTON_LEFT) {
                    if (const int id = list_at(mouse_x, mouse_y); id >= 0) {
                        switch_model(id);
                    } else if (const int c = palette_at(mouse_x, mouse_y); c >= 0) {
                        colour = static_cast<uint8_t>(c);
                        if (!sel_faces.empty()) {
                            ModelData& ed = begin_edit();
                            for (const uint16_t f : sel_faces) ed.faces[f].colour = colour;
                            end_edit();
                        }
                    } else if (in_view) {
                        left_down = true;
                        press_x = mouse_x;
                        press_y = mouse_y;
                        auto& sel = mode == Mode::Vertices ? sel_verts : sel_faces;
                        const int hit = mode == Mode::Vertices ? hover_vertex : hover_face;
                        if (hit < 0) {
                            if (!shift) clear_selection();
                        } else {
                            const auto at = std::find(sel.begin(), sel.end(), static_cast<uint16_t>(hit));
                            if (shift && at != sel.end()) {
                                sel.erase(at);
                            } else if (at == sel.end()) {
                                if (!shift) sel.clear();
                                sel.push_back(static_cast<uint16_t>(hit));
                            }
                            // Ready to move what's selected with the mouse.
                            drag_start.clear();
                            ModelData& ed = begin_edit();
                            for (const uint16_t v : selected_vertices()) drag_start.emplace_back(v, ed.verts[v]);
                            drag_centre = selection_centre();
                            float px = 0, py = 0;
                            if (!view.project(drag_centre, px, py, drag_depth)) drag_depth = cam.dist;
                        }
                    }
                } else if (e.button.button == SDL_BUTTON_RIGHT) {
                    (shift ? pan : orbit) = in_view;
                } else if (e.button.button == SDL_BUTTON_MIDDLE) {
                    pan = in_view;
                }
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    if (!drag_start.empty()) end_edit();  // (nothing recorded if it didn't move)
                    left_down = dragging = false;
                    drag_start.clear();
                } else {
                    orbit = pan = false;
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (canvas_x(mouse_x) < kListW) {
                    list_scroll -= static_cast<int>(e.wheel.y * row_h * 3);
                } else if (in_view) {
                    cam.dist = std::clamp(cam.dist * std::pow(0.87, static_cast<double>(e.wheel.y)), 30.0, 60000.0);
                }
                break;
            default:
                break;
            }
        }
        // Selections stay within the model (after deletions, undo, another model).
        const ModelData& md = shown();
        std::erase_if(sel_verts, [&](uint16_t v) { return v < kFirst || v >= md.verts.size(); });
        std::erase_if(sel_faces, [&](uint16_t f) { return f >= md.faces.size(); });

        // --- The 3D view ---
        tris.clear();
        vscreen.assign(md.verts.size(), {0, 0, -1});
        {
            SceneMaker sm(scene);
            scene.view_x0 = static_cast<int>(vx0);
            scene.view_y0 = static_cast<int>(vy0);
            scene.view_x1 = static_cast<int>(vx1);
            scene.view_y1 = static_cast<int>(vy1);
            std::vector<Vec> mv(md.verts.size());
            for (size_t i = 0; i < md.verts.size(); ++i) {
                mv[i] = as_vec(md.verts[i]);
                float sx = 0, sy = 0;
                double z = 0;
                if (view.project(mv[i], sx, sy, z)) vscreen[i] = {sx, sy, static_cast<float>(z)};
            }
            const Vec light = unit(Vec{0.4, -1.0, -0.6});
            const std::set<uint16_t> fsel(sel_faces.begin(), sel_faces.end());
            for (size_t fi = 0; fi < md.faces.size(); ++fi) {
                const ModelData::Face& f = md.faces[fi];
                en::SceneColour c = face_colour(f);
                const bool hidden = (f.flags & 0x4000) != 0;
                float alpha = hidden ? 0.3f : (f.flags & 0x8000) ? 0.6f : 1.0f;
                const bool selected = fsel.count(static_cast<uint16_t>(fi)) != 0;
                if (selected) c = {c.r * 0.5f + 0.5f, c.g * 0.5f + 0.38f, c.b * 0.5f + 0.12f};
                else if (static_cast<int>(fi) == hover_face) c = {c.r * 0.75f + 0.25f, c.g * 0.75f + 0.25f, c.b * 0.75f + 0.25f};
                for (const auto& prim : f.prims) {
                    if (f.lines()) {
                        for (size_t k = 0; k + 1 < prim.size(); ++k) {
                            const auto& a = vscreen[prim[k]];
                            const auto& b = vscreen[prim[k + 1]];
                            if (a[2] < 0 || b[2] < 0) continue;
                            sm.line(a[0], a[1], a[2], b[0], b[1], b[2], c, static_cast<float>(1.5 * scale), false, (a[2] + b[2]) / 2);
                        }
                        continue;
                    }
                    std::vector<en::geometry::P3> pts;
                    for (const uint16_t v : prim) pts.push_back({float(mv[v][0]), float(mv[v][1]), float(mv[v][2])});
                    std::vector<uint16_t> idx;
                    en::geometry::triangulate(pts.data(), static_cast<int>(pts.size()), idx);
                    en::SceneColour lit = c;
                    if (lighting) {
                        const double s = 0.6 + 0.4 * std::abs(dot(normal_of(md, prim), light));
                        lit = {float(c.r * s), float(c.g * s), float(c.b * s)};
                    }
                    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                        float x[3], y[3];
                        double z[3];
                        bool ok = true;
                        for (int q = 0; q < 3; ++q) {
                            const auto& s = vscreen[prim[idx[t + static_cast<size_t>(q)]]];
                            ok = ok && s[2] >= 0;
                            x[q] = s[0];
                            y[q] = s[1];
                            z[q] = s[2];
                        }
                        if (!ok) continue;
                        const double key = (z[0] + z[1] + z[2]) / 3;
                        sm.tri(x, y, z, lit, alpha, key);
                        tris.push_back({{x[0], x[1], x[2]}, {y[0], y[1], y[2]}, std::min({z[0], z[1], z[2]}), static_cast<int>(fi)});
                    }
                    // Edges: the selected faces' always, every face's with the wireframe on.
                    if (wire || selected) {
                        for (size_t k = 0; k < prim.size(); ++k) {
                            const auto& a = vscreen[prim[k]];
                            const auto& b = vscreen[prim[(k + 1) % prim.size()]];
                            if (a[2] < 0 || b[2] < 0) continue;
                            sm.line(a[0], a[1], a[2], b[0], b[1], b[2], selected ? rgb(kGold) : rgb(0x101010),
                                    static_cast<float>((selected ? 1.5 : 1.0) * scale), selected, (a[2] + b[2]) / 2);
                        }
                    }
                }
            }
            // The axes at the model's origin: x (east) red, y (down) green, z (north) blue.
            float ox0 = 0, oy0 = 0;
            double oz0 = 0;
            if (view.project(Vec{0, 0, 0}, ox0, oy0, oz0)) {
                const double l = axis_length;
                const std::pair<Vec, uint32_t> axes[3] = {{{l, 0, 0}, 0xE04040}, {{0, l, 0}, 0x40C040}, {{0, 0, l}, 0x5080FF}};
                for (const auto& [a, col] : axes) {
                    float ax = 0, ay = 0;
                    double az = 0;
                    if (view.project(a, ax, ay, az)) sm.line(ox0, oy0, oz0, ax, ay, az, rgb(col), static_cast<float>(scale), true, 0);
                }
            }
            // Vertices (in vertex mode), the selected in gold, numbered by the order picked.
            if (mode == Mode::Vertices) {
                const std::set<uint16_t> vsel(sel_verts.begin(), sel_verts.end());
                for (size_t i = kFirst; i < md.verts.size(); ++i) {
                    if (vscreen[i][2] < 0) continue;
                    const bool sel = vsel.count(static_cast<uint16_t>(i)) != 0, hov = static_cast<int>(i) == hover_vertex;
                    sm.square(vscreen[i][0], vscreen[i][1], static_cast<float>((sel || hov ? 3.0 : 2.0) * scale),
                              rgb(sel ? kGold : hov ? 0xFFFFFF : 0xB0B8C8));
                }
            }
            sm.finish();
        }

        // --- Panels ---
        canvas.fill_rect(0, 0, W, kTopBar, kPanel);
        const std::string title = "Object editor: " + (pack_name.empty() ? std::string("a new set") : pack_name) + (dirty ? " (changed)" : "");
        canvas.text(6, 3, fit_left(title, static_cast<size_t>((W - 12) / kGlyph)), kGold);
        // The list.
        canvas.fill_rect(0, kTopBar, kListW, H - kTopBar - kBottomBar, kPanel);
        canvas.fill_rect(kListW - 1, kTopBar, 1, H - kTopBar - kBottomBar, kRule);
        canvas.text(6, kTopBar + 4, "Objects", kLabel);
        const int hover_id = list_at(mouse_x, mouse_y);
        for (size_t i = 0; i < ids.size(); ++i) {
            const int y = list_top + static_cast<int>(i) * row_h - list_scroll;
            if (y < list_top || y + row_h > list_bottom) continue;
            const int id = ids[i];
            if (id == current) canvas.fill_rect(1, y - 1, kListW - 3, row_h, kSelection);
            const std::string label = (pack.models.count(id) ? "*" : " ") + std::to_string(id) + " " + game::model_name(id);
            canvas.text(4, y, label.substr(0, static_cast<size_t>((kListW - 8) / kGlyph)),
                        id == current ? kGold : id == hover_id ? kValue : kHelp);
        }
        // The side: mode, colours, the selection.
        const int sx = W - kSideW;
        canvas.fill_rect(sx, kTopBar, kSideW, H - kTopBar - kBottomBar, kPanel);
        canvas.fill_rect(sx, kTopBar, 1, H - kTopBar - kBottomBar, kRule);
        canvas.text(sx + 8, kTopBar + 4, mode == Mode::Vertices ? "Vertices" : "Faces", kGold);
        canvas.text(sx + 8, kTopBar + 16, "(Tab: " + std::string(mode == Mode::Vertices ? "faces)" : "vertices)"), kHint);
        canvas.text(sx + 8, kTopBar + 28, "Colour " + std::to_string(colour), kLabel);
        for (int c = 0; c < 16; ++c) {
            const int x = pal_x + (c % 4) * (kSwatch + 2), y = pal_y + (c / 4) * (kSwatch + 2);
            const en::SceneColour sc = en::ega_colour(c);
            const uint32_t argb = static_cast<uint32_t>(sc.r * 255) << 16 | static_cast<uint32_t>(sc.g * 255) << 8 | static_cast<uint32_t>(sc.b * 255);
            canvas.fill_rect(x, y, kSwatch, kSwatch, argb);
            if (c == colour) box(canvas, x - 1, y - 1, kSwatch + 2, kSwatch + 2, kGold);
        }
        int iy = pal_y + 4 * (kSwatch + 2) + 6;
        const auto info = [&](const std::string& s, uint32_t col) {
            canvas.text(sx + 8, iy, s.substr(0, static_cast<size_t>((kSideW - 12) / kGlyph)), col);
            iy += 11;
        };
        info(std::to_string(md.verts.size() - kFirst) + " vertices", kValue);
        info(std::to_string(md.faces.size()) + " faces", kValue);
        info("Steps of " + std::to_string(snap), kHelp);
        if (lock) info(std::string("Along ") + lock + " only", kGold);
        iy += 6;
        if (mode == Mode::Vertices && sel_verts.size() == 1) {
            const auto& v = md.verts[sel_verts[0]];
            info("Vertex " + std::to_string(sel_verts[0]), kLabel);
            info("x " + std::to_string(v[0]) + " east", kValue);
            info("y " + std::to_string(v[1]) + " down", kValue);
            info("z " + std::to_string(v[2]) + " north", kValue);
        } else if (mode == Mode::Vertices && !sel_verts.empty()) {
            info(std::to_string(sel_verts.size()) + " picked", kLabel);
        } else if (mode == Mode::Faces && sel_faces.size() == 1) {
            const auto& f = md.faces[sel_faces[0]];
            info("Face " + std::to_string(sel_faces[0]), kLabel);
            info("Colour " + std::to_string(f.colour & 15) + ((f.colour >> 4) ? "+" + std::to_string(f.colour >> 4) : ""), kValue);
            info(f.lines() ? "Lines" : (f.flags & 0x0E) == 2 ? "Fill (concave)" : "Fill", kValue);
            if (f.flags & 0x0001) info("Hides its back", kHelp);
            if (f.flags & 0x2000) info("Outlined " + std::to_string(f.outline & 15), kHelp);
            if (f.flags & 0x4000) info("Never drawn", kBad);
            if (f.flags & 0x8000) info("See-through", kHelp);
        } else if (mode == Mode::Faces && !sel_faces.empty()) {
            info(std::to_string(sel_faces.size()) + " picked", kLabel);
        }
        if (const std::string why = game::check_model(md); !why.empty()) {
            iy += 4;
            for (const std::string& l : wrap("Not for the game yet: " + why, static_cast<size_t>((kSideW - 12) / kGlyph))) info(l, kBad);
        }
        // Status and hints.
        canvas.fill_rect(0, H - kBottomBar, W, kBottomBar, kPanel);
        canvas.fill_rect(0, H - kBottomBar, W, 1, kRule);
        const size_t chars = static_cast<size_t>((W - 12) / kGlyph);
        const std::string status = game::model_name(current) + " (model " + std::to_string(current) + ")" +
                                   (pack.models.count(current) ? ", changed" : ", the original's") +
                                   (hover_vertex >= 0 ? ".  Vertex " + std::to_string(hover_vertex) : "") +
                                   (hover_face >= 0 ? ".  Face " + std::to_string(hover_face) : "");
        canvas.text(6, H - kBottomBar + 4, fit_left(status, chars), kValue);
        const bool fresh = !message.empty() && SDL_GetTicksNS() - message_at < kMessageNs;
        canvas.text(6, H - kBottomBar + 16,
                    fit_left(fresh ? message
                                   : "Left: pick, drag   Right-drag: turn   Wheel: zoom   F1: keys   Esc: back",
                             chars),
                    fresh ? kGood : kHint);

        const auto panel_box = [&](const std::vector<std::pair<std::string, uint32_t>>& lines) {
            size_t longest = 0;
            for (const auto& l : lines) longest = std::max(longest, l.first.size());
            const int bw = std::min(W - 16, static_cast<int>(longest) * kGlyph + 24);
            const int bh = static_cast<int>(lines.size()) * 12 + 18;
            const int bx = (W - bw) / 2, by = (H - bh) / 2;
            canvas.fill_rect(bx, by, bw, bh, kPanel);
            box(canvas, bx, by, bw, bh, kRule);
            for (size_t i = 0; i < lines.size(); ++i)
                canvas.text(bx + 12, by + 10 + static_cast<int>(i) * 12, fit_left(lines[i].first, static_cast<size_t>((bw - 24) / kGlyph)), lines[i].second);
        };
        if (help) {
            panel_box({{"Object editor keys", kGold},
                       {"Left: pick a vertex or face (Shift: add or take away)", kHelp},
                       {"Drag what's picked: move it   X, Y, Z: along that axis only", kHelp},
                       {"Right-drag: turn round   Shift+right or middle drag: slide", kHelp},
                       {"Wheel, + and -: zoom   , . ; ': turn   Home: the whole object", kHelp},
                       {"[ and ]: the previous, next object (or pick it from the list)", kHelp},
                       {"Arrows, Page Up/Down: move what's picked (Shift: 8 times, Ctrl: 1)", kHelp},
                       {"G: the mouse's steps: the object's, 4 times them, or 1", kHelp},
                       {"Enter: type the picked vertex's x (east), y (down), z (north)", kHelp},
                       {"Tab: vertices or faces   Ctrl+A: all   Esc: pick nothing", kHelp},
                       {"A or Insert: a new vertex   Delete: what's picked", kHelp},
                       {"F: a face through the picked vertices, in order (2: a line)", kHelp},
                       {"M: a mirror image of what's picked, across east-west", kHelp},
                       {"Faces: 0-9, Shift+0-5 or the palette: colour   C: next", kHelp},
                       {"R: turn round   B: hides its back   O: outlined   H: hidden", kHelp},
                       {"L: shading   W: edges   Ctrl+Z, Ctrl+Y: undo, redo", kHelp},
                       {"Ctrl+S: save   Ctrl+Shift+S: save as   Esc: back", kHelp},
                       {"Ctrl+R: this object as the original's   Ctrl+N: all of them", kHelp},
                       {"The game takes 124 vertices, 16 corners a face, at most.", kHint}});
        } else if (prompt == Prompt::SaveAs) {
            panel_box({{"Save the objects as:", kLabel}, {field + "_", kValue}, {"Enter: save   Esc: cancel", kHint}});
        } else if (prompt == Prompt::Coords) {
            panel_box({{"Vertex " + (sel_verts.empty() ? std::string() : std::to_string(sel_verts[0])) + ": x (east) y (down) z (north)", kLabel},
                       {field + "_", kValue},
                       {"Enter: move it there   Esc: cancel", kHint}});
        } else if (prompt == Prompt::Leave) {
            panel_box({{"Save your changes" + (pack_name.empty() ? std::string() : " to \"" + pack_name + "\"") + "?", kLabel},
                       {"Y: save   N: don't save   Esc: keep editing", kHint}});
        } else if (prompt == Prompt::Revert) {
            panel_box({{game::model_name(current) + " back to the original's?", kLabel}, {"Y: yes   Esc: keep it", kHint}});
        } else if (prompt == Prompt::Restart) {
            panel_box({{"Every object back to the original's? Your unsaved changes go.", kLabel},
                       {"Y: start again   Esc: keep editing", kHint}});
        }

        if (script) {
            const uint64_t ms = (SDL_GetTicksNS() - opened) / 1'000'000;
            if (script_shot < script->shots.size() && script->shots[script_shot].first <= ms) {
                presenter.request_screenshot(script->shots[script_shot].second);
                ++script_shot;
            }
        }
        presenter.show_overlay(canvas, false);
        presenter.present_view(scene, kViewBack);
        if (!presenter.visible()) SDL_Delay(10);
    }
}

}  // namespace vette::ui
