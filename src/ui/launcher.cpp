#include "ui/launcher.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/path_utf8.h"
#include "platform/gamepad.h"
#include "platform/presenter.h"
#include "ui/canvas.h"
#include "ui/theme.h"

#ifndef VETTE_VERSION
#define VETTE_VERSION "dev"
#endif

namespace vette::ui {
namespace {

enum Row {
    kFolder, kPreset, kFrameRate, kPc, kDrawDistance, kViewResolution, kDepthBuffer, kSkyline, kGraphics, kEffects, kMusic, kDriving, kLaneCentering, kManualCheck, kJoystick, kDisplay, kScaling, kLauncher,
    kPlay, kQuit, kRows
};

using namespace theme;

constexpr int kValueColumn = 20;  // characters from the left margin
constexpr int kStatusPitch = 11;  // the versions-found lines

const char* label(int row) {
    switch (row) {
    case kFolder: return "Game folder";
    case kPreset: return "Preset";
    case kFrameRate: return "Frame rate";
    case kPc: return "PC speed";
    case kDrawDistance: return "Draw distance";
    case kManualCheck: return "Manual check";
    case kJoystick: return "Joystick";
    case kDisplay: return "Display";
    case kScaling: return "Scaling";
    case kSkyline: return "Skyline";
    case kViewResolution: return "Resolution";
    case kDepthBuffer: return "Depth buffer";
    case kDriving: return "Driving";
    case kLaneCentering: return "Lane centering";
    case kGraphics: return "Graphics";
    case kEffects: return "Sound effects";
    case kMusic: return "Music";
    case kLauncher: return "This screen";
    case kPlay: return "Play";
    default: return "Quit";
    }
}

// The other versions' files, in subfolders of the game folder (Game/PC98, Game/Mac).
struct Extras {
    bool pc98 = false;
    bool mac = false;
    bool depth_buffer = true;  // the GPU can draw the 3D view with one (Presenter::depth_buffer_available)
};

Extras find_extras(const GameDirSearch& search) {
    return {search.versions.pc98.has_value(), search.versions.mac.has_value()};
}

// A choice that needs another version's files, when they aren't there.
bool missing(int row, const Settings& s, const Extras& x) {
    if (row == kGraphics)
        return (s.graphics == Settings::Graphics::Pc98 && !x.pc98) || (s.graphics == Settings::Graphics::Mac && !x.mac);
    if (row == kEffects)
        return s.effects == Settings::Effects::Mac && !x.mac;
    if (row == kMusic)
        return s.music == Settings::Music::Pc98 && !x.pc98;
    return false;
}

std::string value(int row, const Settings& s, const GameDirSearch& search, const Extras& x) {
    const std::optional<GameDir>& game = search.dir;
    switch (row) {
    case kFolder: return game ? path_to_utf8(search.versions.root) : "Not found - Enter to choose";
    case kPreset:
        return s.preset() == Settings::Preset::Classic    ? "Classic (1989)"
               : s.preset() == Settings::Preset::Enhanced ? "Enhanced"
                                                          : "Custom";
    case kFrameRate: return s.frame_rate == Settings::FrameRate::Smooth ? "Smooth (display rate)" : "Original";
    case kPc: return s.pc == Settings::Pc::Fast ? "Fast PC (30 fps)" : "1989 PC/AT (12 MHz)";
    case kDrawDistance:
        return s.draw_distance == Settings::DrawDistance::Original   ? "Original"
               : s.draw_distance == Settings::DrawDistance::Extended ? "Extended"
                                                                     : "Maximum (whole city)";
    case kManualCheck: return s.manual_check ? "Show" : "Skip";
    case kJoystick:
        return s.joystick == Settings::Joystick::Auto ? "Auto" : s.joystick == Settings::Joystick::On ? "On" : "Off";
    case kDisplay: return s.fullscreen ? "Full screen" : "Window";
    case kScaling: return s.scaling == Settings::Scaling::Sharp ? "Sharp pixels" : "Smooth";
    case kSkyline: return s.skyline == Settings::Skyline::Hills ? "Hills only" : "Painted (original)";
    case kViewResolution:
        return s.view_resolution == Settings::ViewResolution::Display ? "Display" : "Original 320x200";
    case kDriving: return s.improved_driving ? "Improved (drifts, jumps)" : "Original";
    case kLaneCentering: return s.lane_centering ? "On (slight)" : "Off";
    case kDepthBuffer:
        return !s.depth_buffer ? "Off (original order)" : x.depth_buffer ? "On" : "On - not available here";
    case kGraphics: {
        const std::string v = s.graphics == Settings::Graphics::Dos    ? "DOS (original)"
                              : s.graphics == Settings::Graphics::Pc98 ? "PC-98"
                                                                       : "Macintosh";
        return missing(row, s, x) ? v + " - files missing" : v;
    }
    case kEffects: {
        static constexpr const char* kNames[] = {"Off", "PC speaker (original)", "AdLib (FM)", "Macintosh (digitized)"};
        const std::string v = kNames[static_cast<int>(s.effects)];
        return missing(row, s, x) ? v + " - files missing" : v;
    }
    case kMusic: {
        static constexpr const char* kNames[] = {"Off", "Original", "PC-98 (YM2203 FM)"};
        const std::string v = kNames[static_cast<int>(s.music)];
        return missing(row, s, x) ? v + " - files missing" : v;
    }
    case kLauncher: return s.show_launcher ? "Show at start" : "Skip at start";
    default: return "";
    }
}

std::string_view help(int row, const Settings& s) {
    switch (row) {
    case kFolder:
        return "The folder with your copies of VETTE!: the DOS files, and the PC-98 and Mac versions if you "
               "have them, each in a folder of its own (any names). Enter: choose another folder.";
    case kPreset:
        return "Classic is VETTE! as it was in 1989. Enhanced turns on the smooth frame rate, a fast PC and "
               "the whole city in view, and skips the manual question. Changing an option below makes it Custom.";
    case kFrameRate:
        return s.frame_rate == Settings::FrameRate::Smooth
                   ? "The race is drawn at your display's refresh rate, blending between the game's own frames. "
                     "The game logic is unchanged."
                   : "Only the frames the game draws itself: about 30 per second on the fast PC, 12-17 on the "
                     "1989 PC/AT.";
    case kPc:
        return s.pc == Settings::Pc::Fast
                   ? "A fast 386/486: the game runs at its own 30 fps limit and turns on its rear-view mirror "
                     "and building windows."
                   : "A 12 MHz PC/AT as in 1989: 12-17 frames per second; mirror and building windows start "
                     "off (F6 and W toggle them).";
    case kDrawDistance:
        return s.draw_distance == Settings::DrawDistance::Original
                   ? "The game's own 3D view: about two blocks ahead, at 320x200."
               : s.draw_distance == Settings::DrawDistance::Extended
                   ? "The long-distance 3D view, eight blocks around you."
                   : "The whole city at once, to the horizon, with all its traffic and pedestrians: nothing "
                     "pops in.";
    case kManualCheck:
        return "Copy protection: the original asks a question from the manual before the first race. This "
               "version of the game accepts any answer.";
    case kJoystick:
        return "Auto: a gamepad connected at startup becomes the PC's joystick. On: always there. Off: none "
               "(the pad still works in menus).";
    case kDisplay: return "F11 or Alt+Enter switches at any time.";
    case kViewResolution:
        return s.view_resolution == Settings::ViewResolution::Display
                   ? "The long-distance 3D view is drawn at your display's full resolution: smooth edges and "
                     "fine lines."
                   : "The long-distance 3D view is drawn at the original's 320x200 and enlarged like the rest "
                     "of the game, with every enhancement kept.";
    case kDriving:
        return s.improved_driving
                   ? "Your car drifts a little through fast corners, and flies over the crest of a hill when "
                     "it's going fast enough."
                   : "The original's driving.";
    case kLaneCentering:
        return s.lane_centering ? "A slight steering assist that keeps your car straight in its lane. Steer to "
                                  "override it."
                                : "No steering assist, as in the original.";
    case kDepthBuffer:
        return s.depth_buffer
                   ? "Nearer things always cover farther ones, and the whole city's traffic and pedestrians are "
                     "drawn. Needs Direct3D 12, Vulkan or Metal; without, it works as Off."
                   : "The original's back-to-front drawing. Traffic and pedestrians appear as you near them, as "
                     "in the original, so none show through the scenery.";
    case kSkyline:
        return s.skyline == Settings::Skyline::Hills
                   ? "The backdrop behind the long-distance 3D view keeps only the hills: the real city stands in "
                     "front of it, so its painted buildings and bridges are left out."
                   : "The original painted backdrop, skyline and bridges included, behind the 3D view.";
    case kScaling:
        return s.scaling == Settings::Scaling::Sharp
                   ? "The original's pictures simply enlarged: every pixel a solid block."
                   : "The original's pictures enlarged with the edges between pixels softened.";
    case kGraphics:
        return s.graphics == Settings::Graphics::Dos
                   ? "The DOS version's EGA screens, as in 1989."
                   : s.graphics == Settings::Graphics::Pc98
                         ? "The PC-98 version's art: its colours, and sharper race pictures and opponent screen. "
                           "Needs its files in Game/PC98."
                         : "The Macintosh version's colour art for the title, garage, opponents, dashboard and crash "
                           "pictures. Needs its files in Game/Mac.";
    case kEffects:
        switch (s.effects) {
        case Settings::Effects::Off: return "No sound effects.";
        case Settings::Effects::Speaker: return "The original's PC speaker sounds.";
        case Settings::Effects::AdLib:
            return "An AdLib FM sound card, which the original never supported: each sound on its own instrument "
                   "(edit them with vette_sfx). The engine keeps running under a skid or the siren.";
        case Settings::Effects::Mac:
            return "The Macintosh version's digitized sounds, including its opening song. Needs its files in "
                   "Game/Mac; AdLib plays the two it doesn't have.";
        }
        return "";
    case kMusic:
        switch (s.music) {
        case Settings::Music::Off: return "No title or winner tunes.";
        case Settings::Music::Original: return "The original's title and winner tunes, on the sound effects' device.";
        case Settings::Music::Pc98:
            return "The PC-98 version's FM soundtrack: title, menus, winner and loser songs. Needs its files in "
                   "Game/PC98.";
        }
        return "";
    case kLauncher:
        return "Skip: the game starts straight away next time. Run vette2026 --launcher to see this screen "
               "again.";
    case kPlay: return "Start VETTE! with these settings. They are saved for next time.";
    default: return "Leave without starting.";
    }
}

// Splits text into lines of at most `width` characters at spaces.
std::vector<std::string> wrap(std::string_view text, size_t width) {
    std::vector<std::string> lines;
    std::string line;
    while (!text.empty()) {
        const size_t end = text.find(' ');
        const std::string_view word = text.substr(0, end);
        if (!line.empty() && line.size() + 1 + word.size() > width) {
            lines.push_back(line);
            line.clear();
        }
        line += line.empty() ? std::string(word) : " " + std::string(word);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    }
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

// Fits text into `chars` characters by dropping the start (paths keep their informative end).
std::string fit_left(std::string s, size_t chars) {
    if (s.size() <= chars || chars < 4)
        return s;
    return "..." + s.substr(s.size() - (chars - 3));
}

// Where everything goes on a canvas of the given size.
struct Layout {
    int margin, pitch, list_y, actions_y, status_y, rule_y, help_y, hints_y;

    explicit Layout(int height) {
        margin = 16;
        list_y = 56;
        // Rows as far apart as the height allows (9 to 14 pixels), with room below them for the
        // versions found, the help (3 lines) and the key hints.
        const int below = 3 * kStatusPitch + 2 + 8 + 3 * 12 + 20;
        pitch = std::clamp((height - list_y - below) / (kPlay + 3), 9, 14);
        actions_y = list_y + kPlay * pitch + pitch / 2;
        status_y = actions_y + 2 * pitch + pitch / 2;
        rule_y = status_y + 3 * kStatusPitch + 2;  // a line per version
        help_y = rule_y + 8;
        hints_y = height - 14;
    }
    int row_y(int row) const { return row < kPlay ? list_y + row * pitch : actions_y + (row - kPlay) * pitch; }
};

// The native folder dialog reports on any thread; the launcher picks the result up each frame.
struct FolderPick {
    std::mutex mutex;
    bool open = false;
    bool done = false;
    std::string path;
};

void SDLCALL on_folder_picked(void* user, const char* const* list, int) {
    auto* pick = static_cast<FolderPick*>(user);
    const std::lock_guard lock(pick->mutex);
    pick->open = false;
    if (list && list[0]) {
        pick->path = list[0];
        pick->done = true;
    }
}

std::string describe_problem(const GameDirSearch& search) {
    if (search.missing.empty())
        return "Game files not found. Choose the folder with your DOS VETTE! files.";
    std::string missing;
    for (size_t i = 0; i < search.missing.size() && i < 3; ++i)
        missing += (i ? ", " : "") + std::string(search.missing[i]);
    if (search.missing.size() > 3)
        missing += ", ...";
    return "Not all VETTE! files are there (missing " + missing + ").";
}

}  // namespace

LaunchChoice run_launcher(Presenter& presenter, Gamepad& gamepad, Settings& s, std::optional<GameDir>& game,
                          GameDirSearch& search) {
    static FolderPick pick;  // static: a dialog left open must not outlive what its callback writes to
    int selected = game ? kPlay : kFolder;
    std::string status = game ? "" : describe_problem(search);
    Canvas canvas;
    std::vector<std::uint8_t> pad_keys;
    LaunchChoice choice = LaunchChoice::Quit;
    // The window can open under a resting pointer. Hovering selects only once the pointer has really
    // moved, so the selection doesn't start on whatever row happens to be under it. Measured on the
    // desktop: the window itself can still move while it opens.
    SDL_FPoint pointer_start{};
    SDL_GetGlobalMouseState(&pointer_start.x, &pointer_start.y);
    bool pointer_moved = false;

    const auto open_folder_dialog = [&] {
        {
            const std::lock_guard lock(pick.mutex);
            if (pick.open)
                return;
            pick.open = true;
        }
        const std::string start = game ? path_to_utf8(game->root()) : std::string();
        SDL_ShowOpenFolderDialog(on_folder_picked, &pick, presenter.window(), start.empty() ? nullptr : start.c_str(),
                                 false);
    };
    const auto move = [&](int dir) {
        selected = (selected + dir + kRows) % kRows;
    };
    const auto change = [&](int row, int dir) {
        switch (row) {
        case kPreset:
            s.apply(s.preset() == Settings::Preset::Enhanced ? Settings::Preset::Classic : Settings::Preset::Enhanced);
            break;
        case kFrameRate:
            s.frame_rate = s.frame_rate == Settings::FrameRate::Smooth ? Settings::FrameRate::Original
                                                                       : Settings::FrameRate::Smooth;
            break;
        case kPc: s.pc = s.pc == Settings::Pc::Fast ? Settings::Pc::At286 : Settings::Pc::Fast; break;
        case kDrawDistance:
            s.draw_distance = static_cast<Settings::DrawDistance>((static_cast<int>(s.draw_distance) + dir + 3) % 3);
            break;
        case kManualCheck: s.manual_check = !s.manual_check; break;
        case kJoystick: s.joystick = static_cast<Settings::Joystick>((static_cast<int>(s.joystick) + dir + 3) % 3); break;
        case kViewResolution:
            s.view_resolution = s.view_resolution == Settings::ViewResolution::Display
                                    ? Settings::ViewResolution::Original
                                    : Settings::ViewResolution::Display;
            break;
        case kDepthBuffer:
            s.depth_buffer = !s.depth_buffer;
            break;
        case kDriving: s.improved_driving = !s.improved_driving; break;
        case kLaneCentering: s.lane_centering = !s.lane_centering; break;
        case kSkyline:
            s.skyline = s.skyline == Settings::Skyline::Hills ? Settings::Skyline::Painted : Settings::Skyline::Hills;
            break;
        case kScaling:
            s.scaling = s.scaling == Settings::Scaling::Sharp ? Settings::Scaling::Smooth : Settings::Scaling::Sharp;
            presenter.set_smooth_scaling(s.scaling == Settings::Scaling::Smooth);
            break;
        case kDisplay:
            s.fullscreen = !s.fullscreen;
            presenter.set_fullscreen(s.fullscreen);
            break;
        case kGraphics: s.graphics = static_cast<Settings::Graphics>((static_cast<int>(s.graphics) + dir + 3) % 3); break;
        case kEffects: s.effects = static_cast<Settings::Effects>((static_cast<int>(s.effects) + dir + 4) % 4); break;
        case kMusic: s.music = static_cast<Settings::Music>((static_cast<int>(s.music) + dir + 3) % 3); break;
        case kLauncher: s.show_launcher = !s.show_launcher; break;
        default: break;
        }
    };
    // Enter, click or the A button. Returns true once the player has chosen to play or quit.
    const auto activate = [&](int row, int dir) {
        if (row == kFolder) {
            open_folder_dialog();
        } else if (row == kPlay) {
            if (game) {
                choice = LaunchChoice::Start;
                return true;
            }
            status = "Choose the game folder first.";
            selected = kFolder;
        } else if (row == kQuit) {
            choice = LaunchChoice::Quit;
            return true;
        } else {
            change(row, dir);
        }
        return false;
    };
    const auto escape = [&] {
        if (selected == kQuit)
            return true;  // a second Esc quits; the first only moves to Quit
        selected = kQuit;
        return false;
    };

    for (;;) {
        // A folder chosen in the dialog: use it if VETTE!'s files are all there.
        {
            const std::lock_guard lock(pick.mutex);
            if (pick.done) {
                pick.done = false;
                GameDirSearch chosen = find_game_dir(path_from_utf8(pick.path));
                if (chosen.dir) {
                    game = chosen.dir;
                    search = std::move(chosen);
                    s.game_folder = pick.path;
                    status.clear();
                    selected = kPlay;
                } else {
                    status = describe_problem(chosen);
                }
            }
        }

        // Canvas: the largest integer scale that keeps at least ~500x340 logical pixels.
        int out_w = 0, out_h = 0;
        presenter.output_size(out_w, out_h);
        const int scale = std::max(1, std::min(out_w / 500, out_h / 340));
        canvas.reset(std::max(out_w / scale, 1), std::max(out_h / scale, 1), scale, kBackground);
        const Layout lay(canvas.height);

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            pad_keys.clear();
            gamepad.handle_event(e, pad_keys);  // hot-plug; D-pad/Start/Back arrive as scan codes
            bool done = false;
            switch (e.type) {
            case SDL_EVENT_QUIT:
                return LaunchChoice::Quit;
            case SDL_EVENT_KEY_DOWN: {
                // Physical keys, so the keypad's arrows (8/2/4/6) work as well, whatever Num Lock says.
                const SDL_Scancode key = e.key.scancode;
                const bool enter = key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER;
                if (key == SDL_SCANCODE_F11 || (enter && (e.key.mod & SDL_KMOD_ALT))) {
                    if (!e.key.repeat)
                        change(kDisplay, 1);
                } else if (key == SDL_SCANCODE_UP || key == SDL_SCANCODE_KP_8) {
                    move(-1);
                } else if (key == SDL_SCANCODE_DOWN || key == SDL_SCANCODE_KP_2 || key == SDL_SCANCODE_TAB) {
                    move(1);
                } else if (key == SDL_SCANCODE_LEFT || key == SDL_SCANCODE_KP_4 || key == SDL_SCANCODE_RIGHT ||
                           key == SDL_SCANCODE_KP_6) {
                    if (selected != kFolder && selected < kPlay)
                        change(selected, key == SDL_SCANCODE_LEFT || key == SDL_SCANCODE_KP_4 ? -1 : 1);
                } else if ((enter || key == SDL_SCANCODE_SPACE) && !e.key.repeat) {
                    done = activate(selected, 1);
                } else if (key == SDL_SCANCODE_ESCAPE && !e.key.repeat) {
                    done = escape();
                }
                break;
            }
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                const bool click = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                if (!click && !pointer_moved) {
                    SDL_FPoint p{};
                    SDL_GetGlobalMouseState(&p.x, &p.y);
                    pointer_moved = std::abs(p.x - pointer_start.x) + std::abs(p.y - pointer_start.y) >= 8;
                    if (!pointer_moved)
                        break;
                }
                int fx = 0, fy = 0;
                if (!presenter.window_to_frame(click ? e.button.x : e.motion.x, click ? e.button.y : e.motion.y, fx,
                                               fy))
                    break;
                for (int row = 0; row < kRows; ++row) {
                    const int y = lay.row_y(row) - 3;
                    if (fy >= y && fy < y + lay.pitch) {
                        selected = row;
                        if (click)
                            done = activate(row, e.button.button == SDL_BUTTON_RIGHT ? -1 : 1);
                    }
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH)
                    done = activate(selected, 1);
                break;
            default:
                break;
            }
            for (const std::uint8_t code : pad_keys) {  // make codes only
                switch (code) {
                case 0x48: move(-1); break;
                case 0x50: move(1); break;
                case 0x4B:
                case 0x4D:
                    if (selected != kFolder && selected < kPlay)
                        change(selected, code == 0x4B ? -1 : 1);
                    break;
                case 0x1C: done = done || activate(selected, 1); break;
                case 0x01: done = done || escape(); break;
                default: break;
                }
            }
            if (done)
                return choice;
        }

        // Title and subtitle.
        const int m = lay.margin;
        canvas.text(m, 12, "VETTE!", kGold, 3, true);
        const int sub_x = m + text_width("VETTE!", 3) + 32;
        canvas.text(sub_x, 14, "Spectrum HoloByte, 1989", kSubtitle);
        canvas.text(sub_x, 26, "C++ / SDL3 rebuild, version " VETTE_VERSION, kSubtitle);

        // Options and actions.
        const int value_x = m + kValueColumn * kGlyph;
        const size_t value_chars = static_cast<size_t>(std::max(0, (canvas.width - value_x - m) / kGlyph - 4));
        Extras extras = find_extras(search);
        extras.depth_buffer = presenter.depth_buffer_available();
        for (int row = 0; row < kRows; ++row) {
            const int y = lay.row_y(row);
            const bool sel = row == selected;
            if (sel)
                canvas.fill_rect(6, y - 3, canvas.width - 12, lay.pitch, kSelection);
            if (row >= kPlay) {
                canvas.text(m, y, std::string(sel ? "> " : "  ") + label(row), sel ? kGold : kValue);
                continue;
            }
            canvas.text(m, y, label(row), sel ? kGold : kLabel);
            std::string v = fit_left(value(row, s, search, extras), value_chars);
            if (row != kFolder)
                v = "< " + v + " >";
            canvas.text(value_x, y, v, (row == kFolder && !game) || missing(row, s, extras) ? kBad : kValue);
        }

        // Status, help and key hints.
        const size_t line_chars = static_cast<size_t>((canvas.width - 2 * m) / kGlyph);
        // The versions found in the game folder (a problem, if there is one, in place of the DOS line).
        const auto where = [&](const std::filesystem::path& p, const std::string& what) {
            std::error_code ec;
            const std::filesystem::path base = search.versions.root.parent_path();
            const std::filesystem::path rel = std::filesystem::relative(p, base, ec);
            std::string text = path_to_utf8(ec || rel.empty() ? p : rel);
            return what.empty() ? text : text + "  (" + what + ")";
        };
        const int label_w = 11;
        const auto version_line = [&](int i, const char* name, const std::optional<std::filesystem::path>& at,
                                      const std::string& what) {
            const int y = lay.status_y + i * kStatusPitch;
            canvas.text(m, y, name, kLabel);
            const size_t room = line_chars > static_cast<size_t>(label_w) ? line_chars - label_w : 0;
            if (at)
                canvas.text(m + label_w * kGlyph, y, fit_left(where(*at, what), room), kGood);
            else
                canvas.text(m + label_w * kGlyph, y, "not found", kDim);
        };
        if (!status.empty())
            canvas.text(m, lay.status_y, fit_left(status, line_chars), kBad);
        else
            version_line(0, "DOS", game ? std::optional{game->root()} : std::nullopt, "");
        version_line(1, "PC-98", search.versions.pc98, search.versions.pc98_what);
        version_line(2, "Macintosh", search.versions.mac, search.versions.mac_what);
        canvas.fill_rect(m, lay.rule_y, canvas.width - 2 * m, 1, kRule);
        const std::vector<std::string> lines = wrap(help(selected, s), line_chars);
        for (size_t i = 0; i < lines.size() && i < 3; ++i)
            canvas.text(m, lay.help_y + static_cast<int>(i) * 12, lines[i], kHelp);
        canvas.text(m, lay.hints_y, "Up/Down choose   Left/Right change   Enter select   Esc quit", kHint);

        presenter.present(canvas);
        if (!presenter.visible())
            SDL_Delay(10);
    }
}

}  // namespace vette::ui
