#include "ui/launcher.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/path_utf8.h"
#include "platform/gamepad.h"
#include "platform/presenter.h"
#include "ui/canvas.h"

#ifndef VETTE_VERSION
#define VETTE_VERSION "dev"
#endif

namespace vette::ui {
namespace {

enum Row {
    kFolder, kPreset, kFrameRate, kPc, kDrawDistance, kManualCheck, kJoystick, kDisplay, kSound, kLauncher,
    kPlay, kQuit, kRows
};

// Colours (0xRRGGBB).
constexpr uint32_t kBackground = 0x0E1324, kSelection = 0x28345A, kRule = 0x2E3A5C;
constexpr uint32_t kGold = 0xFFC23C, kSubtitle = 0xB9BECB, kLabel = 0xC5CAD6, kValue = 0xEEF0F4;
constexpr uint32_t kHelp = 0xD6DAE4, kHint = 0x7C8396, kGood = 0x62D96B, kBad = 0xFF6464;

constexpr int kValueColumn = 20;  // characters from the left margin

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
    case kSound: return "Sound";
    case kLauncher: return "This screen";
    case kPlay: return "Play";
    default: return "Quit";
    }
}

std::string value(int row, const Settings& s, const std::optional<GameDir>& game) {
    switch (row) {
    case kFolder: return game ? path_to_utf8(game->root()) : "Not found - Enter to choose";
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
    case kSound: return s.sound ? "On (PC speaker)" : "Off";
    case kLauncher: return s.show_launcher ? "Show at start" : "Skip at start";
    default: return "";
    }
}

std::string_view help(int row, const Settings& s) {
    switch (row) {
    case kFolder:
        return "The folder with your DOS VETTE! files: by default the folder Game next to vette2026 "
               "(Game/README.md lists the files). Enter: choose another folder.";
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
                   ? "The 3D view at your display's resolution, eight blocks around you."
                   : "The 3D view at your display's resolution, with the whole city in view to the horizon.";
    case kManualCheck:
        return "Copy protection: the original asks a question from the manual before the first race. This "
               "version of the game accepts any answer.";
    case kJoystick:
        return "Auto: a gamepad connected at startup becomes the PC's joystick. On: always there. Off: none "
               "(the pad still works in menus).";
    case kDisplay: return "F11 or Alt+Enter switches at any time.";
    case kSound: return "The original's PC speaker sound.";
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
        pitch = height >= 360 ? 14 : 12;
        list_y = 56;
        actions_y = list_y + kPlay * pitch + pitch / 2;
        status_y = actions_y + 2 * pitch + pitch / 2;
        rule_y = status_y + pitch;
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
                          const GameDirSearch& search) {
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
        case kDisplay:
            s.fullscreen = !s.fullscreen;
            presenter.set_fullscreen(s.fullscreen);
            break;
        case kSound: s.sound = !s.sound; break;
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
                const GameDirSearch chosen = find_game_dir(path_from_utf8(pick.path));
                if (chosen.dir) {
                    game = chosen.dir;
                    s.game_folder = pick.path;
                    status.clear();
                    selected = kPlay;
                } else {
                    status = describe_problem(chosen);
                }
            }
        }

        // Canvas: the largest integer scale that keeps at least ~500x300 logical pixels.
        int out_w = 0, out_h = 0;
        presenter.output_size(out_w, out_h);
        const int scale = std::max(1, std::min(out_w / 500, out_h / 300));
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
            std::string v = fit_left(value(row, s, game), value_chars);
            if (row != kFolder)
                v = "< " + v + " >";
            canvas.text(value_x, y, v, row == kFolder && !game ? kBad : kValue);
        }

        // Status, help and key hints.
        const size_t line_chars = static_cast<size_t>((canvas.width - 2 * m) / kGlyph);
        if (!status.empty())
            canvas.text(m, lay.status_y, fit_left(status, line_chars), kBad);
        else if (game)
            canvas.text(m, lay.status_y, fit_left("Game found: " + path_to_utf8(game->root()), line_chars), kGood);
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
