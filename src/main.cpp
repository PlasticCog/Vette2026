#include "assets/planar.h"
#include "assets/rle.h"
#include "core/game_dir.h"
#include "core/path_utf8.h"
#include "game/options.h"
#include "host/machine.h"
#include "platform/audio.h"
#include "platform/framebuffer.h"
#include "platform/gamepad.h"
#include "platform/keymap.h"
#include "platform/mouse_pointer.h"
#include "platform/presenter.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>  // UTF-8 argv on Windows

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vette {
namespace {

constexpr const char* kAppName = "VETTE! 2026";

constexpr const char* kUsage =
    "Usage: vette2026 [--game <dir>] [--cpu-hz <n>] [--[no-]joystick] [--manual-check]\n"
    "                 [--dump-frame <file.bmp>]\n"
    "  --game <dir>         folder with the DOS VETTE! files (default: search for Game/)\n"
    "  --cpu-hz <n>         emulated CPU clock in Hz (default 12000000: a 12 MHz 286)\n"
    "  --joystick           give the PC a joystick even if no gamepad is connected yet\n"
    "  --no-joystick        no joystick, even with a gamepad connected\n"
    "  --manual-check       show the original's manual-lookup question before the first race\n"
    "                       (skipped by default; this version accepts any answer anyway)\n"
    "  --dump-frame <file>  write the title screen to a 640x200 BMP and exit, without a window\n"
    "\n"
    "F11 or Alt+Enter toggles fullscreen. Every other key, Esc included, goes to the game.\n"
    "A gamepad connected at launch becomes the PC's analog joystick. DOS games look for one only\n"
    "at startup, so whether the joystick exists is decided then (see README.md for the mapping).\n";

constexpr std::uint64_t kNsPerSecond = 1'000'000'000;
// Longest wall-clock step fed to the emulator, so it doesn't race to catch up after a stall
// (debugger break, window drag).
constexpr std::uint64_t kMaxStepNs = kNsPerSecond / 4;
constexpr int kAudioRate = 48000;

struct Options {
    std::optional<std::filesystem::path> game_dir;
    std::optional<std::string> dump_frame;  // UTF-8 path
    std::optional<std::uint64_t> cpu_hz;
    std::optional<bool> joystick;  // default: whether a gamepad is connected at launch
    bool manual_check = false;     // show the copy-protection question (skipped by default)
    bool help = false;
};

// Prints the problem and returns nullopt on bad usage.
std::optional<Options> parse_args(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--help" || arg == "-h") {
            opts.help = true;
        } else if (arg == "--joystick" || arg == "--no-joystick") {
            opts.joystick = arg == "--joystick";
        } else if (arg == "--manual-check") {
            opts.manual_check = true;
        } else if (arg == "--game" && has_value) {
            opts.game_dir = path_from_utf8(argv[++i]);
        } else if (arg == "--dump-frame" && has_value) {
            opts.dump_frame = argv[++i];
        } else if (arg == "--cpu-hz" && has_value) {
            opts.cpu_hz = std::strtoull(argv[++i], nullptr, 10);
            if (*opts.cpu_hz < 1'000'000) {
                std::fprintf(stderr, "--cpu-hz must be at least 1000000\n");
                return std::nullopt;
            }
        } else {
            const bool needs_value = arg == "--game" || arg == "--dump-frame" || arg == "--cpu-hz";
            std::fprintf(stderr, "%s: %s\n\n%s", needs_value ? "Missing value for" : "Unknown option", argv[i],
                         kUsage);
            return std::nullopt;
        }
    }
    return opts;
}

void report_error(const std::string& message, bool dialog) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", message.c_str());
    if (dialog)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kAppName, message.c_str(), nullptr);
}

std::string missing_files_message(const GameDirSearch& search) {
    std::string msg = "VETTE! 2026 needs the game files from your copy of VETTE! for DOS.\n\nMissing:";
    for (std::size_t i = 0; i < search.missing.size(); ++i)
        msg.append(i ? ", " : " ").append(search.missing[i]);
    msg += "\n\nSearched:\n";
    for (const auto& dir : search.searched)
        msg += "  " + path_to_utf8(dir) + "\n";
    msg += "\nCopy your DOS VETTE! files into the Game folder (see Game/README.md).";
    return msg;
}

void load_title(const GameDir& game, Framebuffer& fb) {
    const auto planes = rle_decode(game.read("TITLE.BIN"));
    const std::size_t expected = planar_size(Framebuffer::kWidth, Framebuffer::kHeight);
    if (planes.size() != expected)
        throw std::runtime_error("TITLE.BIN decoded to " + std::to_string(planes.size()) + " bytes, expected " +
                                 std::to_string(expected));
    fb.pixels = decode_planar(planes, Framebuffer::kWidth, Framebuffer::kHeight);
}

// Saves (CONFIG.BIN, SCORE.BIN, ...) go here, never into the player's game folder.
std::filesystem::path save_dir() {
    char* pref = SDL_GetPrefPath("VETTE2026", "save");
    if (!pref)
        throw_sdl_error("SDL_GetPrefPath");
    std::filesystem::path dir = path_from_utf8(pref);
    SDL_free(pref);
    return dir;
}

void copy_frame(const host::Ega::Frame& in, Framebuffer& out) {
    if (in.width == 0) {  // text mode (startup/exit): not rendered
        std::fill(out.pixels.begin(), out.pixels.end(), std::uint8_t{0});
        return;
    }
    out.width = in.width;
    out.height = in.height;
    out.pixels = in.pixels;
    for (std::size_t i = 0; i < out.palette.size(); ++i) {
        const std::uint32_t c = in.palette[i];
        out.palette[i] = {static_cast<std::uint8_t>(c >> 16), static_cast<std::uint8_t>(c >> 8),
                          static_cast<std::uint8_t>(c)};
    }
}

// Runs the hosted game until the window closes or VETTE.EXE exits.
void main_loop(Presenter& presenter, host::Machine& machine, AudioOut* audio, Gamepad& gamepad) {
    Framebuffer fb;
    host::Ega::Frame frame;
    std::vector<std::uint8_t> scancodes;
    std::vector<std::int16_t> samples;
    std::uint8_t mouse_buttons = 0;
    float mouse_dx = 0;
    float mouse_dy = 0;
    std::uint64_t last = SDL_GetTicksNS();

    for (;;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            scancodes.clear();  // keyboard bytes this event produces, sent after the switch
            switch (event.type) {
            case SDL_EVENT_QUIT:
                return;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                const bool down = event.type == SDL_EVENT_KEY_DOWN;
                const SDL_Keycode key = event.key.key;
                const bool alt_enter = (key == SDLK_RETURN || key == SDLK_KP_ENTER) && (event.key.mod & SDL_KMOD_ALT);
                if (key == SDLK_F11 || alt_enter) {  // host keys; F11 didn't exist on 1989 keyboards
                    if (down && !event.key.repeat)
                        presenter.toggle_fullscreen();
                    break;
                }
                // Auto-repeat is forwarded too: a real keyboard repeats make codes while a key is held.
                xt_scancode(event.key.scancode, down, scancodes);
                break;
            }
            case SDL_EVENT_MOUSE_MOTION:
                mouse_dx += event.motion.xrel;
                mouse_dy += event.motion.yrel;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                const std::uint8_t bit = event.button.button == SDL_BUTTON_LEFT    ? 1
                                         : event.button.button == SDL_BUTTON_RIGHT ? 2
                                                                                   : 0;
                mouse_buttons = static_cast<std::uint8_t>(event.button.down ? (mouse_buttons | bit)
                                                                            : (mouse_buttons & ~bit));
                machine.mouse_buttons(mouse_buttons);
                break;
            }
            default:  // gamepad hot-plug, and D-pad/Start/Back as keys
                gamepad.handle_event(event, scancodes);
                break;
            }
            for (const std::uint8_t b : scancodes)
                machine.key(b);
        }
        // The mouse driver counts whole mickeys; carry the fractions into the next frame.
        const int dx = static_cast<int>(mouse_dx);
        const int dy = static_cast<int>(mouse_dy);
        if (dx != 0 || dy != 0) {
            machine.mouse_motion(dx, dy);
            mouse_dx -= static_cast<float>(dx);
            mouse_dy -= static_cast<float>(dy);
        }
        // Ignored by the machine when it has no game port (MachineConfig::joystick).
        const JoystickState stick = gamepad.joystick();
        machine.joystick_axes(stick.x, stick.y);
        machine.joystick_buttons(stick.buttons);

        const std::uint64_t now = SDL_GetTicksNS();
        machine.run_for(std::min(now - last, kMaxStepNs));
        last = now;
        if (!machine.fault().empty())
            throw std::runtime_error("VETTE.EXE stopped: " + machine.fault());
        if (machine.stopped())
            return;  // the player quit to DOS

        samples.clear();
        machine.take_audio(samples);
        if (audio)
            audio->push(samples);

        machine.render(frame);
        copy_frame(frame, fb);
        // The mouse driver's pointer goes on the copy, never into video memory. Text mode isn't shown.
        const host::Bios::Cursor pointer = machine.mouse_cursor();
        const bool show_pointer = pointer.visible && frame.width != 0;
        if (show_pointer)
            draw_mouse_pointer(fb, pointer.x, pointer.y);
        presenter.show_system_cursor(!show_pointer);
        if (presenter.visible())
            presenter.present(fb);
        else
            SDL_Delay(10);  // VSync doesn't pace a hidden window
    }
}

int run(int argc, char** argv) {
    const auto opts = parse_args(argc, argv);
    if (!opts)
        return 2;
    if (opts->help) {
        std::fputs(kUsage, stdout);
        return 0;
    }
    const bool headless = opts->dump_frame.has_value();

    try {
        const GameDirSearch search = find_game_dir(opts->game_dir);
        if (!search.dir) {
            report_error(missing_files_message(search), !headless);
            return 1;
        }
        const GameDir& game = *search.dir;
        SDL_Log("Game folder: %s", path_to_utf8(game.root()).c_str());
        identify_vette_exe(game.read("VETTE.EXE"));

        if (headless) {
            Framebuffer fb;
            load_title(game, fb);
            save_bmp(fb, *opts->dump_frame);
            SDL_Log("Wrote %s", opts->dump_frame->c_str());
            return 0;
        }

        if (!SDL_Init(SDL_INIT_VIDEO))
            throw_sdl_error("SDL_Init");
        std::optional<AudioOut> audio;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            try {
                audio.emplace(kAudioRate);
            } catch (const std::exception& e) {
                SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "No sound: %s", e.what());
            }
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "No sound: %s", SDL_GetError());
        }

        Gamepad gamepad;

        host::MachineConfig config;
        config.game_dir = game.root();
        config.save_dir = save_dir();
        config.audio_rate = kAudioRate;
        if (opts->cpu_hz)
            config.cpu_hz = *opts->cpu_hz;
        // Fixed for the session: DOS games detect the game port once, at startup.
        config.joystick = opts->joystick.value_or(gamepad.connected());
        SDL_Log("Joystick: %s", !config.joystick      ? "none"
                                : gamepad.connected() ? "gamepad"
                                                      : "game port present, no gamepad connected (centered)");
        host::Machine machine(config);
        machine.set_log([](const std::string& msg) { SDL_Log("%s", msg.c_str()); });
        std::string error;
        if (!machine.boot(error))
            throw std::runtime_error("Couldn't start VETTE.EXE: " + error);
        SDL_Log("Saves: %s", path_to_utf8(config.save_dir).c_str());
        if (!opts->manual_check)
            game::install_skip_manual_check(machine.cpu());

        Presenter presenter(kAppName);
        main_loop(presenter, machine, audio ? &*audio : nullptr, gamepad);
        return 0;
    } catch (const std::exception& e) {
        report_error(e.what(), !headless);
        return 1;
    }
}

}  // namespace
}  // namespace vette

int main(int argc, char* argv[]) {
    const int code = vette::run(argc, argv);
    SDL_Quit();
    return code;
}
