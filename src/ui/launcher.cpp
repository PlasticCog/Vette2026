#include "ui/launcher.h"

#include <SDL3/SDL.h>

#include <string>
#include <string_view>
#include <vector>

#include "platform/gamepad.h"
#include "platform/presenter.h"
#include "ui/draw.h"

namespace vette::ui {
namespace {

enum Row { kPreset, kFrameRate, kPc, kDrawDistance, kManualCheck, kJoystick, kDisplay, kSound, kLauncher, kStart, kQuit, kRows };

// EGA colours (default palette).
constexpr uint8_t kBlack = 0, kBlue = 1, kRed = 4, kLightGray = 7, kDarkGray = 8, kLightBlue = 9, kYellow = 14, kWhite = 15;

// Layout in the 640x200 frame (8x8 font).
constexpr int kPanelX = 104, kPanelY = 12, kPanelW = 432, kPanelH = 176;
constexpr int kTextX = kPanelX + 12, kValueRight = kPanelX + kPanelW - 12;
constexpr int kRowStep = 10;
constexpr int kHelpY = 150, kHelpLines = 3, kHelpChars = (kPanelW - 24) / kGlyph;

int row_y(int row) {
    if (row < kStart)
        return kPanelY + 18 + row * kRowStep;
    return kPanelY + 18 + kStart * kRowStep + 4 + (row - kStart) * kRowStep;
}

bool selectable(int row) { return row != kDrawDistance; }  // in development

const char* label(int row) {
    switch (row) {
    case kPreset: return "Preset";
    case kFrameRate: return "Frame rate";
    case kPc: return "PC speed";
    case kDrawDistance: return "Draw distance";
    case kManualCheck: return "Manual check (copy protection)";
    case kJoystick: return "Joystick";
    case kDisplay: return "Display";
    case kSound: return "Sound";
    case kLauncher: return "Show this menu at startup";
    case kStart: return "Start game";
    default: return "Quit";
    }
}

std::string value(int row, const Settings& s) {
    switch (row) {
    case kPreset:
        return s.preset() == Settings::Preset::Classic    ? "Classic (1989)"
               : s.preset() == Settings::Preset::Enhanced ? "Enhanced"
                                                          : "Custom";
    case kFrameRate: return s.frame_rate == Settings::FrameRate::Smooth ? "Smooth (display rate)" : "Original";
    case kPc: return s.pc == Settings::Pc::Fast ? "Fast PC (30 fps)" : "1989 PC/AT (12 MHz)";
    case kDrawDistance: return "Original (more soon)";
    case kManualCheck: return s.manual_check ? "Show" : "Skip";
    case kJoystick:
        return s.joystick == Settings::Joystick::Auto ? "Auto" : s.joystick == Settings::Joystick::On ? "On" : "Off";
    case kDisplay: return s.fullscreen ? "Fullscreen" : "Window";
    case kSound: return s.sound ? "On" : "Off";
    case kLauncher: return s.show_launcher ? "Yes" : "No";
    default: return "";
    }
}

std::string_view help(int row, const Settings& s) {
    switch (row) {
    case kPreset:
        return "Classic is VETTE! as it was in 1989. Enhanced turns on the smooth frame rate and a fast PC, and "
               "skips the manual question. Changing an option below makes it Custom.";
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
                   : "A 12 MHz PC/AT as in 1989: 12-17 frames per second; the mirror and building windows start "
                     "off (F6 and W toggle them).";
    case kDrawDistance:
        return "In development: Extended, and Maximum, which draws the whole city to the horizon.";
    case kManualCheck:
        return "The original asks a question from the manual before the first race. This version of the game "
               "accepts any answer.";
    case kJoystick:
        return "Auto: a gamepad connected at startup becomes the PC's joystick. On: always there. Off: none "
               "(the pad still works in menus).";
    case kDisplay: return "F11 or Alt+Enter switches at any time.";
    case kSound: return "The original's PC speaker sound.";
    case kLauncher:
        return "No: the game starts straight away next time. Run vette2026 --launcher to see this menu again.";
    case kStart: return "Start VETTE! with these settings. They are saved for next time.";
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

void draw(Framebuffer& fb, const Framebuffer& background, const Settings& s, int selected) {
    fb = background;
    fill_rect(fb, kPanelX, kPanelY, kPanelW, kPanelH, kBlue);
    frame_rect(fb, kPanelX, kPanelY, kPanelW, kPanelH, kWhite);
    fill_rect(fb, kPanelX + 1, kPanelY + 1, kPanelW - 2, 11, kRed);
    draw_text(fb, kTextX, kPanelY + 3, "VETTE! 2026", kYellow);
    draw_text(fb, kValueRight - 14 * kGlyph, kPanelY + 3, "Launch options", kWhite);

    for (int row = 0; row < kRows; ++row) {
        const int y = row_y(row);
        if (row == selected)
            fill_rect(fb, kPanelX + 2, y - 1, kPanelW - 4, kRowStep, kLightBlue);
        const uint8_t text = !selectable(row) ? kDarkGray : kWhite;
        if (row >= kStart) {
            const std::string button = std::string("[ ") + label(row) + " ]";
            draw_text(fb, kPanelX + (kPanelW - static_cast<int>(button.size()) * kGlyph) / 2, y, button, text);
            continue;
        }
        draw_text(fb, kTextX, y, label(row), text);
        const std::string v = selectable(row) ? "< " + value(row, s) + " >" : value(row, s);
        draw_text(fb, kValueRight - static_cast<int>(v.size()) * kGlyph, y, v, selectable(row) ? kYellow : kDarkGray);
    }

    fill_rect(fb, kPanelX + 8, kHelpY - 4, kPanelW - 16, 1, kLightBlue);
    const std::vector<std::string> lines = wrap(help(selected, s), static_cast<size_t>(kHelpChars));
    for (size_t i = 0; i < lines.size() && i < kHelpLines; ++i)
        draw_text(fb, kTextX, kHelpY + static_cast<int>(i) * kRowStep, lines[i], kLightGray);

    fill_rect(fb, 0, fb.height - 10, fb.width, 10, kBlack);
    draw_text(fb, 8, fb.height - 9, "Up/Down: choose  Left/Right/Enter: change  Esc: quit  (mouse, gamepad too)",
              kLightGray);
}

int row_at(int fx, int fy) {
    if (fx < kPanelX || fx >= kPanelX + kPanelW)
        return -1;
    for (int row = 0; row < kRows; ++row) {
        if (fy >= row_y(row) - 1 && fy < row_y(row) - 1 + kRowStep)
            return row;
    }
    return -1;
}

}  // namespace

LaunchChoice run_launcher(Presenter& presenter, Gamepad& gamepad, const Framebuffer& background, Settings& s) {
    int selected = kStart;
    Framebuffer fb;
    std::vector<std::uint8_t> pad_keys;

    const auto move = [&](int dir) {
        do {
            selected = (selected + dir + kRows) % kRows;
        } while (!selectable(selected));
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
    // Returns true once the player has chosen to start or quit (`choice` set).
    LaunchChoice choice = LaunchChoice::Quit;
    const auto activate = [&](int row, int dir) {
        if (row == kStart || row == kQuit) {
            choice = row == kStart ? LaunchChoice::Start : LaunchChoice::Quit;
            return true;
        }
        change(row, dir);
        return false;
    };
    const auto escape = [&]() {
        if (selected == kQuit)
            return true;  // a second Esc quits; the first one only moves to Quit
        selected = kQuit;
        return false;
    };

    for (;;) {
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
                    if (selected < kStart)
                        change(selected, key == SDL_SCANCODE_LEFT || key == SDL_SCANCODE_KP_4 ? -1 : 1);
                } else if ((enter || key == SDL_SCANCODE_SPACE) && !e.key.repeat) {
                    done = activate(selected, 1);
                } else if (key == SDL_SCANCODE_ESCAPE && !e.key.repeat) {
                    done = escape();
                }
                break;
            }
            case SDL_EVENT_MOUSE_MOTION: {
                int fx = 0, fy = 0;
                if (presenter.window_to_frame(e.motion.x, e.motion.y, fx, fy)) {
                    const int row = row_at(fx, fy);
                    if (row >= 0 && selectable(row))
                        selected = row;
                }
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                int fx = 0, fy = 0;
                if (presenter.window_to_frame(e.button.x, e.button.y, fx, fy)) {
                    const int row = row_at(fx, fy);
                    if (row >= 0 && selectable(row)) {
                        selected = row;
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
                    if (selected < kStart)
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
        draw(fb, background, s, selected);
        presenter.present(fb);
        if (!presenter.visible())
            SDL_Delay(10);
    }
}

}  // namespace vette::ui
