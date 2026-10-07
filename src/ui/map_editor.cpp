#include "ui/map_editor.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>

#include "core/path_utf8.h"
#include "enhanced/topdown.h"
#include "enhanced/world.h"
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
using game::CityMap;

constexpr int kPx = 32;                         // map picture pixels per cell
constexpr int kThumb = 28;                      // palette thumbnails (canvas pixels)
constexpr int kThumbPitch = kThumb + 4;
constexpr int kPaletteCols = 4;
constexpr int kPanelW = 12 + kPaletteCols * kThumbPitch;  // the palette panel (canvas pixels)
constexpr int kTopBar = 14, kBottomBar = 28, kPaletteTop = kTopBar + 42;
constexpr uint32_t kSeeThrough = 0xFF00FF;      // the overlay canvas's background: the map shows there
constexpr uint32_t kOutside = 0x10131C;         // around the map
constexpr uint32_t kPanel = 0x161C30;
constexpr uint32_t kThumbGround = 0x2E3A5C;     // under the thumbnails (not the land's grey, which roads share)
constexpr uint32_t kGrid = 0x3A4256, kShared = 0x8FA0C0, kHover = 0xFFFFFF;
constexpr const char* kExtension = ".vmap";
constexpr uint64_t kMessageNs = 4'000'000'000;
constexpr int kCellsX = CityMap::kCellsX, kCellsY = CityMap::kCellsY, kTileCells = CityMap::kTileCells;

// A file name from a map's name: letters, digits, spaces, '-' and '_'.
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
fs::path map_path(const fs::path& dir, const std::string& name) { return dir / path_from_utf8(name + kExtension); }

bool save_map(const fs::path& dir, const std::string& name, const CityMap& map, std::string& error) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream out(map_path(dir, name), std::ios::binary | std::ios::trunc);
    const std::string text = map.serialize();
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        error = "couldn't write " + path_to_utf8(map_path(dir, name));
        return false;
    }
    return true;
}

std::string hex2(int v) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02X", v & 0xFF);
    return buf;
}

// One edit, to undo and redo.
struct Change {
    enum class Kind { Cell, Layout, Ground } kind = Kind::Cell;
    int where = 0;  // Cell: design * 256 + index; Layout, Ground: the big tile
    CityMap::Cell before{}, after{};
    uint8_t before_value = 0, after_value = 0;
};
using Stroke = std::vector<Change>;

enum class Prompt { None, SaveAs, Leave, Restart };

void box(Canvas& c, int x, int y, int w, int h, uint32_t colour) {
    c.fill_rect(x, y, w, 1, colour);
    c.fill_rect(x, y + h - 1, w, 1, colour);
    c.fill_rect(x, y, 1, h, colour);
    c.fill_rect(x + w - 1, y, 1, h, colour);
}

}  // namespace

std::vector<std::string> list_maps(const fs::path& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == kExtension)
            names.push_back(path_to_utf8(entry.path().stem()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::optional<CityMap> load_map(const fs::path& dir, const std::string& name, std::string& error) {
    std::ifstream in(map_path(dir, name), std::ios::binary);
    if (!in) {
        error = "there's no map called \"" + name + "\"";
        return std::nullopt;
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    auto map = CityMap::parse(text, error);
    if (map && map->name.empty()) map->name = name;
    return map;
}

std::string run_map_editor(Presenter& presenter, Gamepad& gamepad, const GameDir& game, const fs::path& maps_dir,
                           const std::string& name) {
    Canvas canvas;
    std::vector<std::uint8_t> pad_keys;
    int out_w = 0, out_h = 0, scale = 1;
    const auto layout_canvas = [&] {
        presenter.output_size(out_w, out_h);
        scale = std::max(1, std::min(out_w / 640, out_h / 400));
        canvas.reset(std::max(out_w / scale, 1), std::max(out_h / scale, 1), scale, kBackground);
    };
    // A screen with a few lines, until a key (or at once, `wait` false).
    const auto notice = [&](const std::vector<std::string>& lines, bool wait) {
        for (;;) {
            layout_canvas();
            canvas.text(16, 12, "VETTE!", kGold, 3, true);
            canvas.text(16 + text_width("VETTE!", 3) + 32, 14, "Map editor", kSubtitle);
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
                if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                    e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
                    return;
            }
            SDL_Delay(10);
        }
    };

    // The city as the game has it: VETTE.EXE started far enough to have unpacked itself, then its world
    // extracted (the cell types, their objects and the models).
    notice({"Loading the city..."}, false);
    std::optional<CityMap> original;
    en::World world;
    std::string error;
    {
        host::MachineConfig config;
        config.game_dir = game.root();
        config.save_dir = fs::temp_directory_path() / "vette2026_map_editor";
        config.cpu_hz = 140'000'000;
        host::Machine machine(config);
        bool extracted = false;
        if (machine.boot(error)) {
            for (int i = 0; i < 120 && !original; ++i) {
                machine.run_for(25'000'000);
                original = CityMap::read(machine.memory(), error);
            }
            for (int i = 0; original && i < 30 && !extracted; ++i) {
                machine.run_for(100'000'000);
                extracted = en::extract_world(machine, world, error);
            }
        }
        if (!original || !extracted) {
            notice({"The map editor couldn't read the city from VETTE.EXE: " + error}, true);
            return name;
        }
    }

    // The map being edited.
    CityMap map = *original;
    std::string map_name = name;  // the file it's saved as (empty: not yet)
    std::string message;
    std::uint64_t message_at = 0;
    const auto say = [&](std::string text) {
        message = std::move(text);
        message_at = SDL_GetTicksNS();
    };
    if (!name.empty()) {
        if (auto loaded = load_map(maps_dir, name, error)) {
            map = *loaded;
        } else {
            say("Couldn't open \"" + name + "\": " + error + ". This is the original map.");
            map_name.clear();
        }
    }

    // The picture, drawn from the extracted world with the map's cells.
    notice({"Drawing the map..."}, false);
    en::TopDown td(world, kPx);
    const auto sync_all = [&] {
        for (int cx = 0; cx < kCellsX; ++cx) {
            for (int cy = 0; cy < kCellsY; ++cy) {
                const CityMap::Cell c = map.cell(cx, cy);
                td.set_cell(cx, cy, en::Cell{c.type, c.elevation});
            }
        }
        for (int bt = 0; bt < CityMap::kBigTiles; ++bt) td.set_ground(bt, map.ground[static_cast<size_t>(bt)]);
        td.render();
        presenter.set_picture(td.pixels(), td.width(), td.height());
    };
    sync_all();
    const auto redraw = [&](int cx0, int cy0, int cx1, int cy1) {
        int x = 0, y = 0, w = 0, h = 0;
        td.render_cells(cx0, cy0, cx1, cy1, x, y, w, h);
        presenter.update_picture(td.pixels(), x, y, w, h);
    };

    // The palette: the cell types the original map uses, with how many cells use each.
    std::array<int, 256> uses{};
    for (int cx = 0; cx < kCellsX; ++cx) {
        for (int cy = 0; cy < kCellsY; ++cy) ++uses[original->cell(cx, cy).type];
    }
    std::vector<int> palette;
    std::vector<std::vector<uint32_t>> thumbs;
    for (int t = 0; t < 256; ++t) {
        if (!world.types[static_cast<size_t>(t)].used) continue;
        palette.push_back(t);
        thumbs.push_back(en::TopDown::thumbnail(world, t, kThumb, kThumbGround));
    }
    const auto palette_index = [&](int type) {
        const auto it = std::find(palette.begin(), palette.end(), type);
        return it == palette.end() ? -1 : static_cast<int>(it - palette.begin());
    };

    // Edits: the map, the picture (every big tile sharing the design), undo.
    std::vector<Stroke> undo, redo;
    Stroke stroke;
    bool dirty = false;
    const auto set_design_cell = [&](int design, int index, CityMap::Cell value) {
        map.designs[static_cast<size_t>(design)].cells[static_cast<size_t>(index)] = value;
        const int lx = index / kTileCells, ly = index % kTileCells;
        for (int bt = 0; bt < CityMap::kBigTiles; ++bt) {
            if (map.layout[static_cast<size_t>(bt)] != design) continue;
            const int cx = (bt / CityMap::kCols) * kTileCells + lx, cy = (bt % CityMap::kCols) * kTileCells + ly;
            td.set_cell(cx, cy, en::Cell{value.type, value.elevation});
            redraw(cx - 1, cy - 1, cx + 2, cy + 2);
        }
    };
    const auto set_layout = [&](int bt, uint8_t design) {
        map.layout[static_cast<size_t>(bt)] = design;
        const int cx0 = (bt / CityMap::kCols) * kTileCells, cy0 = (bt % CityMap::kCols) * kTileCells;
        for (int x = 0; x < kTileCells; ++x) {
            for (int y = 0; y < kTileCells; ++y) {
                const CityMap::Cell c = map.cell(cx0 + x, cy0 + y);
                td.set_cell(cx0 + x, cy0 + y, en::Cell{c.type, c.elevation});
            }
        }
        redraw(cx0 - 1, cy0 - 1, cx0 + kTileCells + 1, cy0 + kTileCells + 1);
    };
    const auto set_ground = [&](int bt, uint8_t colour) {
        map.ground[static_cast<size_t>(bt)] = colour;
        td.set_ground(bt, colour);
        const int cx0 = (bt / CityMap::kCols) * kTileCells, cy0 = (bt % CityMap::kCols) * kTileCells;
        redraw(cx0, cy0, cx0 + kTileCells, cy0 + kTileCells);
    };
    const auto apply = [&](const Change& c, bool forward) {
        switch (c.kind) {
        case Change::Kind::Cell: set_design_cell(c.where / 256, c.where % 256, forward ? c.after : c.before); break;
        case Change::Kind::Layout: set_layout(c.where, forward ? c.after_value : c.before_value); break;
        case Change::Kind::Ground: set_ground(c.where, forward ? c.after_value : c.before_value); break;
        }
    };
    const auto record = [&](Change c) {
        apply(c, true);
        stroke.push_back(c);
        dirty = true;
    };
    const auto end_stroke = [&] {
        if (stroke.empty()) return;
        undo.push_back(std::move(stroke));
        stroke.clear();
        redo.clear();
    };
    CityMap::Cell brush = original->cell(1, 2);
    const auto paint = [&](int cx, int cy) {
        const CityMap::Cell before = map.cell(cx, cy);
        if (before == brush) return;
        Change c;
        c.kind = Change::Kind::Cell;
        c.where = map.design_of(cx, cy) * 256 + (cx % kTileCells) * kTileCells + cy % kTileCells;
        c.before = before;
        c.after = brush;
        record(c);
    };
    const auto save = [&](const std::string& as) {
        CityMap copy = map;
        copy.name = as;
        if (save_map(maps_dir, as, copy, error)) {
            map.name = as;
            map_name = as;
            dirty = false;
            say("Saved as \"" + as + "\". Choose it as the launch menu's Map to play it.");
            return true;
        }
        say("Not saved: " + error);
        return false;
    };

    // View: zoom (output pixels per cell) and the cell at the map area's centre (x north, y east).
    double zoom = 0, centre_x = 24, centre_y = 24;
    bool grid = true, heights = false, help = false;
    bool left_down = false, right_down = false, middle_down = false, right_moved = false;
    float mouse_x = -1, mouse_y = -1, right_x = 0, right_y = 0;
    int last_cx = -1, last_cy = -1;
    int palette_scroll = 0;
    Prompt prompt = Prompt::None;
    std::string field;
    bool quit_after = false;
    bool leave_after_save = false;  // the save prompt came from leaving
    const float density = std::max(0.5f, SDL_GetWindowPixelDensity(presenter.window()));

    for (;;) {
        layout_canvas();
        canvas.reset(canvas.width, canvas.height, scale, kSeeThrough);
        const int W = canvas.width, H = canvas.height;
        const float ox = static_cast<float>((out_w - W * scale) / 2), oy = static_cast<float>((out_h - H * scale) / 2);
        // The map area (output pixels).
        const float ax0 = ox, ay0 = oy + static_cast<float>(kTopBar * scale);
        const float ax1 = ox + static_cast<float>((W - kPanelW) * scale), ay1 = oy + static_cast<float>((H - kBottomBar) * scale);
        if (zoom == 0) zoom = std::max(4.0, (ay1 - ay0) / 52.0);  // the drivable city in view
        const auto left = [&] { return (ax0 + ax1) / 2 - centre_y * zoom; };
        const auto top = [&] { return (ay0 + ay1) / 2 - (kCellsX - centre_x) * zoom; };
        const auto cell_at = [&](float sx, float sy, int& cx, int& cy) {
            if (sx < ax0 || sx >= ax1 || sy < ay0 || sy >= ay1) return false;
            cy = static_cast<int>(std::floor((sx - left()) / zoom));
            cx = static_cast<int>(std::floor(kCellsX - (sy - top()) / zoom));
            return cx >= 0 && cy >= 0 && cx < kCellsX && cy < kCellsY;
        };
        // The palette (canvas pixels).
        const int panel_x = W - kPanelW, palette_bottom = H - kBottomBar;
        const int rows = (static_cast<int>(palette.size()) + kPaletteCols - 1) / kPaletteCols;
        palette_scroll = std::clamp(palette_scroll, 0, std::max(0, rows * kThumbPitch - (palette_bottom - kPaletteTop)));
        const auto palette_at = [&](float sx, float sy) {
            const int px = static_cast<int>((sx - ox) / static_cast<float>(scale)) - panel_x - 6;
            const int py = static_cast<int>((sy - oy) / static_cast<float>(scale)) - kPaletteTop;
            if (px < 0 || py < 0 || py >= palette_bottom - kPaletteTop || px >= kPaletteCols * kThumbPitch) return -1;
            const int i = ((py + palette_scroll) / kThumbPitch) * kPaletteCols + px / kThumbPitch;
            return i < static_cast<int>(palette.size()) ? i : -1;
        };
        const auto show_brush = [&] {
            const int i = palette_index(brush.type);
            if (i < 0) return;
            const int y = (i / kPaletteCols) * kThumbPitch;
            const int view = palette_bottom - kPaletteTop;
            if (y < palette_scroll) palette_scroll = y;
            if (y + kThumbPitch > palette_scroll + view) palette_scroll = y + kThumbPitch - view;
        };
        const auto zoom_at = [&](double factor, float sx, float sy) {
            const double cy_at = (sx - left()) / zoom, cx_at = kCellsX - (sy - top()) / zoom;
            zoom = std::clamp(zoom * factor, 3.0, 160.0);
            centre_y = cy_at - (sx - (ax0 + ax1) / 2) / zoom;
            centre_x = cx_at + (sy - (ay0 + ay1) / 2) / zoom;
        };
        const auto leave = [&] {
            presenter.hide_overlay();
            SDL_StopTextInput(presenter.window());
            if (quit_after) {
                SDL_Event q{};
                q.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&q);
            }
            return map_name.empty() ? name : map_name;
        };
        int hover_cx = -1, hover_cy = -1;
        const bool on_map = cell_at(mouse_x, mouse_y, hover_cx, hover_cy);
        const int hover_bt = on_map ? map.big_tile(hover_cx, hover_cy) : -1;

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
                }
                break;
            case SDL_EVENT_KEY_DOWN: {
                const SDL_Keycode k = e.key.key;
                const bool ctrl = (e.key.mod & SDL_KMOD_CTRL) != 0, shift = (e.key.mod & SDL_KMOD_SHIFT) != 0;
                if (k == SDLK_F11 || ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && (e.key.mod & SDL_KMOD_ALT))) {
                    if (!e.key.repeat) presenter.toggle_fullscreen();
                    break;
                }
                if (prompt == Prompt::SaveAs) {
                    if (k == SDLK_ESCAPE) {
                        prompt = Prompt::None;
                        quit_after = leave_after_save = false;
                        SDL_StopTextInput(presenter.window());
                    } else if (k == SDLK_BACKSPACE && !field.empty()) {
                        field.pop_back();
                    } else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && !clean_name(field).empty()) {
                        const bool leaving = leave_after_save;
                        leave_after_save = false;
                        SDL_StopTextInput(presenter.window());
                        prompt = Prompt::None;
                        if (save(clean_name(field)) && leaving) return leave();
                    }
                    break;
                }
                if (prompt == Prompt::Leave) {
                    if (k == SDLK_Y) {
                        if (map_name.empty()) {
                            prompt = Prompt::SaveAs;
                            field = "My map";
                            leave_after_save = true;
                            SDL_StartTextInput(presenter.window());
                        } else if (save(map_name)) {
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
                if (prompt == Prompt::Restart) {
                    if (k == SDLK_Y) {
                        map = *original;
                        sync_all();
                        undo.clear();
                        redo.clear();
                        dirty = true;
                        say("This is the original map again.");
                    }
                    if (k == SDLK_Y || k == SDLK_N || k == SDLK_ESCAPE) prompt = Prompt::None;
                    break;
                }
                if (help) {
                    help = false;
                    break;
                }
                if (k == SDLK_ESCAPE) {
                    if (!dirty) return leave();
                    prompt = Prompt::Leave;
                } else if (k == SDLK_F1) {
                    help = true;
                } else if (ctrl && k == SDLK_Z && !shift) {
                    end_stroke();
                    if (!undo.empty()) {
                        for (auto it = undo.back().rbegin(); it != undo.back().rend(); ++it) apply(*it, false);
                        redo.push_back(std::move(undo.back()));
                        undo.pop_back();
                        dirty = true;
                    }
                } else if (ctrl && (k == SDLK_Y || (k == SDLK_Z && shift))) {
                    if (!redo.empty()) {
                        for (const Change& c : redo.back()) apply(c, true);
                        undo.push_back(std::move(redo.back()));
                        redo.pop_back();
                        dirty = true;
                    }
                } else if (ctrl && k == SDLK_S) {
                    if (map_name.empty() || shift) {
                        prompt = Prompt::SaveAs;
                        field = map_name.empty() ? "My map" : map_name;
                        SDL_StartTextInput(presenter.window());
                    } else {
                        save(map_name);
                    }
                } else if (ctrl && k == SDLK_N) {
                    prompt = Prompt::Restart;
                } else if (k >= SDLK_0 && k <= SDLK_3 && !ctrl) {
                    brush.elevation = static_cast<uint8_t>(k - SDLK_0);
                    say("Brush height " + std::to_string(brush.elevation) + ".");
                } else if ((k == SDLK_LEFTBRACKET || k == SDLK_RIGHTBRACKET) && hover_bt >= 0) {
                    const int d = map.layout[static_cast<size_t>(hover_bt)];
                    Change c;
                    c.kind = Change::Kind::Layout;
                    c.where = hover_bt;
                    c.before_value = static_cast<uint8_t>(d);
                    c.after_value = static_cast<uint8_t>((d + (k == SDLK_RIGHTBRACKET ? 1 : CityMap::kDesigns - 1)) %
                                                         CityMap::kDesigns);
                    record(c);
                    end_stroke();
                    say("Big tile " + std::to_string(hover_bt / CityMap::kCols) + "," +
                        std::to_string(hover_bt % CityMap::kCols) + " now uses design " + std::to_string(c.after_value) + ".");
                } else if (k == SDLK_W && hover_bt >= 0 && !ctrl) {
                    Change c;
                    c.kind = Change::Kind::Ground;
                    c.where = hover_bt;
                    c.before_value = map.ground[static_cast<size_t>(hover_bt)];
                    c.after_value = c.before_value == 9 ? 7 : 9;
                    record(c);
                    end_stroke();
                } else if (k == SDLK_G && !ctrl) {
                    grid = !grid;
                } else if (k == SDLK_E && !ctrl) {
                    heights = !heights;
                } else if (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT) {
                    const double step = std::max(1.0, 160.0 / zoom);
                    centre_x += k == SDLK_UP ? step : k == SDLK_DOWN ? -step : 0;
                    centre_y += k == SDLK_RIGHT ? step : k == SDLK_LEFT ? -step : 0;
                } else if (k == SDLK_EQUALS || k == SDLK_PLUS || k == SDLK_KP_PLUS) {
                    zoom_at(1.25, (ax0 + ax1) / 2, (ay0 + ay1) / 2);
                } else if (k == SDLK_MINUS || k == SDLK_KP_MINUS) {
                    zoom_at(0.8, (ax0 + ax1) / 2, (ay0 + ay1) / 2);
                } else if (k == SDLK_HOME) {
                    zoom = std::max(3.0, std::min(ax1 - ax0, ay1 - ay0) / 82.0);
                    centre_x = kCellsX / 2.0;
                    centre_y = kCellsY / 2.0;
                } else if (k == SDLK_PAGEUP || k == SDLK_PAGEDOWN) {
                    palette_scroll += (k == SDLK_PAGEDOWN ? 1 : -1) * (palette_bottom - kPaletteTop - kThumbPitch);
                } else if (k == SDLK_TAB) {
                    const int i = palette_index(brush.type), n = static_cast<int>(palette.size());
                    brush.type = static_cast<uint8_t>(palette[static_cast<size_t>((std::max(i, 0) + (shift ? n - 1 : 1)) % n)]);
                    show_brush();
                }
                break;
            }
            case SDL_EVENT_MOUSE_MOTION: {
                const float nx = e.motion.x * density, ny = e.motion.y * density;
                if ((right_down && (right_moved || std::abs(nx - right_x) + std::abs(ny - right_y) > 6)) || middle_down) {
                    right_moved = right_down;
                    centre_y -= (nx - mouse_x) / zoom;
                    centre_x += (ny - mouse_y) / zoom;
                }
                mouse_x = nx;
                mouse_y = ny;
                int cx = 0, cy = 0;
                if (left_down && prompt == Prompt::None && cell_at(mouse_x, mouse_y, cx, cy) && last_cx >= 0) {
                    // Every cell between the last and this one, so a quick stroke leaves no gaps.
                    const int steps = std::max(std::abs(cx - last_cx), std::abs(cy - last_cy));
                    for (int s = 1; s <= steps; ++s) {
                        paint(last_cx + static_cast<int>(std::lround(double(cx - last_cx) * s / steps)),
                              last_cy + static_cast<int>(std::lround(double(cy - last_cy) * s / steps)));
                    }
                    last_cx = cx;
                    last_cy = cy;
                }
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                mouse_x = e.button.x * density;
                mouse_y = e.button.y * density;
                if (prompt != Prompt::None || help) {
                    help = false;
                    break;
                }
                int cx = 0, cy = 0;
                if (e.button.button == SDL_BUTTON_LEFT) {
                    if (const int i = palette_at(mouse_x, mouse_y); i >= 0) {
                        brush.type = static_cast<uint8_t>(palette[static_cast<size_t>(i)]);
                    } else if (cell_at(mouse_x, mouse_y, cx, cy)) {
                        left_down = true;
                        last_cx = cx;
                        last_cy = cy;
                        paint(cx, cy);
                    }
                } else if (e.button.button == SDL_BUTTON_RIGHT) {
                    right_down = true;
                    right_moved = false;
                    right_x = mouse_x;
                    right_y = mouse_y;
                } else if (e.button.button == SDL_BUTTON_MIDDLE) {
                    middle_down = true;
                }
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                int cx = 0, cy = 0;
                if (e.button.button == SDL_BUTTON_LEFT) {
                    left_down = false;
                    last_cx = last_cy = -1;
                    end_stroke();
                } else if (e.button.button == SDL_BUTTON_RIGHT) {
                    if (right_down && !right_moved && cell_at(mouse_x, mouse_y, cx, cy)) {
                        brush = map.cell(cx, cy);  // pick
                        show_brush();
                    }
                    right_down = false;
                } else if (e.button.button == SDL_BUTTON_MIDDLE) {
                    middle_down = false;
                }
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
                if (palette_at(mouse_x, mouse_y) >= 0 || (mouse_x >= ox + static_cast<float>(panel_x * scale))) {
                    palette_scroll -= static_cast<int>(e.wheel.y * kThumbPitch * 2);
                } else if (mouse_x >= ax0 && mouse_x < ax1 && mouse_y >= ay0 && mouse_y < ay1) {
                    zoom_at(std::pow(1.15, static_cast<double>(e.wheel.y)), mouse_x, mouse_y);
                }
                break;
            default:
                break;
            }
        }
        centre_x = std::clamp(centre_x, 0.0, static_cast<double>(kCellsX));
        centre_y = std::clamp(centre_y, 0.0, static_cast<double>(kCellsY));

        // --- Drawing: the map with its marks, under the panels ---
        const SDL_FRect src{0, 0, static_cast<float>(td.width()), static_cast<float>(td.height())};
        const SDL_FRect dst{static_cast<float>(left()), static_cast<float>(top()), static_cast<float>(kCellsY * zoom),
                            static_cast<float>(kCellsX * zoom)};
        Presenter::PictureMarks marks;
        marks.grid_step = grid && zoom >= 6 ? static_cast<float>(zoom) : 0;
        marks.grid_colour = kGrid;
        const auto cell_rect = [&](int cx, int cy, int n) {
            return SDL_FRect{static_cast<float>(left() + cy * zoom), static_cast<float>(top() + (kCellsX - cx - n) * zoom),
                             static_cast<float>(n * zoom), static_cast<float>(n * zoom)};
        };
        if (on_map) {
            // The big tiles sharing the design under the pointer change together: outline them all.
            const int design = map.layout[static_cast<size_t>(hover_bt)];
            for (int bt = 0; bt < CityMap::kBigTiles; ++bt) {
                if (map.layout[static_cast<size_t>(bt)] == design) {
                    marks.boxes.push_back({cell_rect((bt / CityMap::kCols) * kTileCells, (bt % CityMap::kCols) * kTileCells, kTileCells),
                                           bt == hover_bt ? kGold : kShared, bt == hover_bt ? 2.0f : 1.0f});
                }
            }
            marks.boxes.push_back({cell_rect(hover_cx, hover_cy, 1), kHover, 2});
        }

        // Heights, on the cells (canvas pixels).
        if (heights && zoom >= 18) {
            for (int cx = 0; cx < kCellsX; ++cx) {
                for (int cy = 0; cy < kCellsY; ++cy) {
                    const SDL_FRect r = cell_rect(cx, cy, 1);
                    const int tx = static_cast<int>((r.x + r.w / 2 - ox) / static_cast<float>(scale)) - 4;
                    const int ty = static_cast<int>((r.y + r.h / 2 - oy) / static_cast<float>(scale)) - 4;
                    if (tx < 0 || ty < kTopBar || tx > panel_x - 8 || ty > H - kBottomBar - 8) continue;
                    const int el = map.cell(cx, cy).elevation;
                    if (el == 0) continue;  // (the ground level: most of the map)
                    canvas.fill_rect(tx - 1, ty - 1, 10, 10, kPanel);
                    canvas.text(tx, ty, std::to_string(el), kGold);
                }
            }
        }

        // Top bar.
        canvas.fill_rect(0, 0, W, kTopBar, kPanel);
        const std::string title = "Map editor: " + (map_name.empty() ? std::string("a copy of the original") : map_name) +
                                  (dirty ? " (changed)" : "");
        canvas.text(6, 3, fit_left(title, static_cast<size_t>((W - 12) / kGlyph)), kGold);

        // The palette.
        canvas.fill_rect(panel_x, kTopBar, kPanelW, H - kTopBar - kBottomBar, kPanel);
        canvas.fill_rect(panel_x, kTopBar, 1, H - kTopBar - kBottomBar, kRule);
        canvas.text(panel_x + 6, kTopBar + 4, "Cell types", kLabel);
        canvas.text(panel_x + 6, kTopBar + 16, "Brush: type " + hex2(brush.type), kValue);
        canvas.text(panel_x + 6, kTopBar + 27, "height " + std::to_string(brush.elevation) + " (0-3)", kValue);
        const int hover_palette = palette_at(mouse_x, mouse_y);
        for (size_t i = 0; i < palette.size(); ++i) {
            const int x = panel_x + 6 + static_cast<int>(i % kPaletteCols) * kThumbPitch;
            const int y = kPaletteTop + static_cast<int>(i / kPaletteCols) * kThumbPitch - palette_scroll;
            if (y + kThumbPitch <= kPaletteTop || y >= palette_bottom) continue;
            const std::vector<uint32_t>& t = thumbs[i];
            for (int j = 0; j < kThumb; ++j) {
                const int yy = y + 2 + j;
                if (yy < kPaletteTop || yy >= palette_bottom) continue;
                std::copy_n(t.begin() + j * kThumb, kThumb,
                            canvas.pixels.begin() + static_cast<std::ptrdiff_t>(yy * W + x + 2));
            }
            const bool sel = palette[i] == brush.type, hov = static_cast<int>(i) == hover_palette;
            if ((sel || hov) && y >= kPaletteTop && y + kThumbPitch <= palette_bottom)
                box(canvas, x, y, kThumbPitch, kThumbPitch, sel ? kGold : kValue);
        }

        // The status and the hints.
        canvas.fill_rect(0, H - kBottomBar, W, kBottomBar, kPanel);
        canvas.fill_rect(0, H - kBottomBar, W, 1, kRule);
        std::string status;
        if (on_map) {
            const CityMap::Cell c = map.cell(hover_cx, hover_cy);
            const int design = map.layout[static_cast<size_t>(hover_bt)], shared = map.sharing(design);
            status = "Cell " + std::to_string(hover_cx) + "," + std::to_string(hover_cy) + ": type " + hex2(c.type) +
                     ", height " + std::to_string(c.elevation) + ". Big tile " + std::to_string(hover_bt / CityMap::kCols) +
                     "," + std::to_string(hover_bt % CityMap::kCols) + ", design " + std::to_string(design) +
                     (shared > 1 ? " (shared by " + std::to_string(shared) + " big tiles: they change together)" : "") +
                     (map.ground[static_cast<size_t>(hover_bt)] == 9 ? ", water" : ", land");
        } else if (hover_palette >= 0) {
            const int t = palette[static_cast<size_t>(hover_palette)];
            status = "Type " + hex2(t) + ": " + std::to_string(uses[static_cast<size_t>(t)]) + " cells on the original map.";
        }
        const size_t chars = static_cast<size_t>((W - 12) / kGlyph);
        canvas.text(6, H - kBottomBar + 4, fit_left(status, chars), kValue);
        const bool fresh = !message.empty() && SDL_GetTicksNS() - message_at < kMessageNs;
        canvas.text(6, H - kBottomBar + 16,
                    fit_left(fresh ? message
                                   : "Left: paint   Right: pick   Right-drag: move   Wheel: zoom   F1: all keys   Esc: back",
                             chars),
                    fresh ? kGood : kHint);

        // Help, prompts.
        const auto panel_box = [&](const std::vector<std::pair<std::string, uint32_t>>& lines) {
            size_t longest = 0;
            for (const auto& l : lines) longest = std::max(longest, l.first.size());
            const int bw = std::min(W - 16, static_cast<int>(longest) * kGlyph + 24);
            const int bh = static_cast<int>(lines.size()) * 12 + 18;
            const int bx = (W - bw) / 2, by = (H - bh) / 2;
            canvas.fill_rect(bx, by, bw, bh, kPanel);
            box(canvas, bx, by, bw, bh, kRule);
            for (size_t i = 0; i < lines.size(); ++i) {
                canvas.text(bx + 12, by + 10 + static_cast<int>(i) * 12,
                            fit_left(lines[i].first, static_cast<size_t>((bw - 24) / kGlyph)), lines[i].second);
            }
        };
        if (help) {
            panel_box({{"Map editor keys", kGold},
                       {"Left button: paint the brush (drag for a line)", kHelp},
                       {"Right button: pick a cell's type and height as the brush", kHelp},
                       {"Right or middle drag, arrow keys: move   Wheel, + and -: zoom", kHelp},
                       {"Home: the whole map   Tab: the next type   Page Up/Down: the list", kHelp},
                       {"0-3: the brush's height (224 units a step)", kHelp},
                       {"[ and ]: the design of the big tile under the pointer", kHelp},
                       {"W: the big tile under the pointer is water or land", kHelp},
                       {"G: the cell grid   E: the cells' heights", kHelp},
                       {"Ctrl+Z, Ctrl+Y: undo, redo   Ctrl+S: save   Ctrl+Shift+S: save as", kHelp},
                       {"Ctrl+N: start again from the original map   Esc: back", kHelp},
                       {"Big tiles that share a design change together: pointing at one outlines them.", kHint}});
        } else if (prompt == Prompt::SaveAs) {
            panel_box({{"Save the map as:", kLabel}, {field + "_", kValue}, {"Enter: save   Esc: cancel", kHint}});
        } else if (prompt == Prompt::Leave) {
            panel_box({{"Save your changes" + (map_name.empty() ? std::string() : " to \"" + map_name + "\"") + "?", kLabel},
                       {"Y: save   N: don't save   Esc: keep editing", kHint}});
        } else if (prompt == Prompt::Restart) {
            panel_box({{"Start again from the original map? Your unsaved changes go.", kLabel},
                       {"Y: start again   Esc: keep editing", kHint}});
        }

        presenter.show_overlay(canvas, false);
        presenter.present_picture(src, dst, kOutside, marks);
        if (!presenter.visible()) SDL_Delay(10);
    }
}

}  // namespace vette::ui
