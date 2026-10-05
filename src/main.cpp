#include "assets/planar.h"
#include "assets/rle.h"
#include "core/game_dir.h"
#include "core/path_utf8.h"
#include "core/settings.h"
#include "enhanced/scene.h"
#include "enhanced/world.h"
#include "game/options.h"
#include "game/smooth.h"
#include "host/machine.h"
#include "platform/audio.h"
#include "platform/framebuffer.h"
#include "platform/gamepad.h"
#include "platform/keymap.h"
#include "platform/mouse_pointer.h"
#include "platform/presenter.h"
#include "ui/launcher.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>  // UTF-8 argv on Windows

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vette {
namespace {

constexpr const char* kAppName = "VETTE! 2026";

constexpr const char* kUsage =
    "Usage: vette2026 [--[no-]launcher] [--game <dir>] [--fps smooth|original] [--pc fast|286]\n"
    "                 [--draw-distance original|extended|maximum] [--cpu-hz <n>] [--[no-]joystick]\n"
    "                 [--[no-]sound] [--manual-check] [--dump-frame <file.bmp>]\n"
    "Settings come from the launch menu (saved in settings.ini); these flags override them for one run.\n"
    "  --launcher           show the launch menu even if it's switched off (--no-launcher: skip it)\n"
    "  --game <dir>         folder with the DOS VETTE! files (default: search for Game/)\n"
    "  --fps smooth         (default) the race view is drawn at the display's refresh rate, blending\n"
    "                       between the game's own frames; the game logic is unchanged\n"
    "  --fps original       show only the frames the game itself draws\n"
    "  --pc fast            (default) a fast PC: the game runs at its own 30 fps limit\n"
    "  --pc 286             a 12 MHz PC/AT, as in 1989: about 12-17 fps\n"
    "  --cpu-hz <n>         emulated CPU clock in Hz (overrides --pc)\n"
    "  --draw-distance maximum   (default) the race's 3D view at the display's resolution, with the\n"
    "                       whole city in view; extended: eight blocks around; original: the game's\n"
    "                       own 320x200 view, about two blocks ahead\n"
    "  --joystick           give the PC a joystick even if no gamepad is connected yet\n"
    "  --no-joystick        no joystick, even with a gamepad connected\n"
    "  --no-sound           no sound (--sound: sound on)\n"
    "  --manual-check       show the original's manual-lookup question before the first race\n"
    "                       (skipped by default; this version accepts any answer anyway)\n"
    "  --dump-frame <file>  write the title screen to a 640x200 BMP and exit, without a window\n"
    "Testing (times are emulated seconds):\n"
    "  --key T:SC, --hold A:B:SC   press scan code SC (hex, set 1) at second T for 100 ms, or hold it\n"
    "                       from second A to B, as vette_run does\n"
    "  --shot T             save the window's picture at second T to shot_T.bmp (repeatable)\n"
    "  --quit-after T       close at second T\n"
    "\n"
    "F11 or Alt+Enter toggles fullscreen. Every other key, Esc included, goes to the game.\n"
    "A gamepad connected at launch becomes the PC's analog joystick. DOS games look for one only\n"
    "at startup, so whether the joystick exists is decided then (see README.md for the mapping).\n";

constexpr std::uint64_t kNsPerSecond = 1'000'000'000;
// Longest wall-clock step fed to the emulator, so it doesn't race to catch up after a stall
// (debugger break, window drag).
constexpr std::uint64_t kMaxStepNs = kNsPerSecond / 4;
constexpr int kAudioRate = 48000;

// Emulated CPU clocks. The game measures its own frame time, and its frame rate is capped by its
// double vertical-retrace wait at half the EGA refresh (~30 fps). 140 MHz of our 286 timing reaches
// that cap even with the mirror and window detail that the game enables on fast CPUs.
constexpr std::uint64_t kFastPcHz = 140'000'000;
constexpr std::uint64_t kAtHz = 12'000'000;

// A scripted key (--key, --hold): a make or break code at an emulated time.
struct ScriptedKey {
    std::uint64_t at_ns;
    std::uint8_t scancode;
};

// Command-line overrides of the saved settings (unset = use the setting).
struct Options {
    std::optional<std::filesystem::path> game_dir;
    std::optional<std::string> dump_frame;  // UTF-8 path
    std::optional<std::uint64_t> cpu_hz;
    std::optional<Settings::FrameRate> frame_rate;
    std::optional<Settings::Pc> pc;
    std::optional<Settings::DrawDistance> draw_distance;
    std::optional<bool> joystick;
    std::optional<bool> manual_check;
    std::optional<bool> launcher;
    std::optional<bool> sound;
    std::vector<ScriptedKey> keys;
    std::vector<std::uint64_t> shots;    // emulated ns, sorted
    std::optional<std::uint64_t> quit_after;  // emulated ns  // sorted by time
    bool help = false;

    void apply_to(Settings& s) const {
        if (frame_rate)
            s.frame_rate = *frame_rate;
        if (pc)
            s.pc = *pc;
        if (draw_distance)
            s.draw_distance = *draw_distance;
        if (joystick)
            s.joystick = *joystick ? Settings::Joystick::On : Settings::Joystick::Off;
        if (manual_check)
            s.manual_check = *manual_check;
        if (sound)
            s.sound = *sound;
    }
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
        } else if ((arg == "--shot" || arg == "--quit-after") && has_value) {
            const auto ns = static_cast<std::uint64_t>(std::atof(argv[++i]) * static_cast<double>(kNsPerSecond));
            if (arg == "--shot")
                opts.shots.push_back(ns);
            else
                opts.quit_after = ns;
        } else if (arg == "--sound" || arg == "--no-sound") {
            opts.sound = arg == "--sound";
        } else if (arg == "--launcher" || arg == "--no-launcher") {
            opts.launcher = arg == "--launcher";
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
        } else if (arg == "--fps" && has_value && (std::string_view(argv[i + 1]) == "smooth" ||
                                                  std::string_view(argv[i + 1]) == "original")) {
            opts.frame_rate = std::string_view(argv[++i]) == "smooth" ? Settings::FrameRate::Smooth
                                                                     : Settings::FrameRate::Original;
        } else if (arg == "--pc" && has_value && (std::string_view(argv[i + 1]) == "fast" ||
                                                 std::string_view(argv[i + 1]) == "286")) {
            opts.pc = std::string_view(argv[++i]) == "fast" ? Settings::Pc::Fast : Settings::Pc::At286;
        } else if ((arg == "--key" || arg == "--hold") && has_value) {
            // T:SC or A:B:SC, seconds as decimals, the scan code in hex.
            const std::string v = argv[++i];
            const std::size_t c1 = v.find(':');
            const std::size_t c2 = arg == "--hold" && c1 != std::string::npos ? v.find(':', c1 + 1) : c1;
            if (c2 == std::string::npos) {
                std::fprintf(stderr, "%s needs %s\n", argv[i - 1], arg == "--key" ? "T:SC" : "A:B:SC");
                return std::nullopt;
            }
            const auto ns = [](const std::string& sec) {
                return static_cast<std::uint64_t>(std::atof(sec.c_str()) * static_cast<double>(kNsPerSecond));
            };
            const std::uint64_t from = ns(v.substr(0, c1));
            const std::uint64_t to = arg == "--key" ? from + kNsPerSecond / 10 : ns(v.substr(c1 + 1, c2 - c1 - 1));
            const auto sc = static_cast<std::uint8_t>(std::strtoul(v.substr(c2 + 1).c_str(), nullptr, 16) & 0x7F);
            opts.keys.push_back({from, sc});
            opts.keys.push_back({to, static_cast<std::uint8_t>(sc | 0x80)});
        } else if (arg == "--draw-distance" && has_value && (std::string_view(argv[i + 1]) == "original" ||
                                                            std::string_view(argv[i + 1]) == "extended" ||
                                                            std::string_view(argv[i + 1]) == "maximum")) {
            const std::string_view v = argv[++i];
            opts.draw_distance = v == "original"   ? Settings::DrawDistance::Original
                                 : v == "extended" ? Settings::DrawDistance::Extended
                                                   : Settings::DrawDistance::Maximum;
        } else {
            const bool needs_value = arg == "--game" || arg == "--dump-frame" || arg == "--cpu-hz" ||
                                     arg == "--fps" || arg == "--pc" || arg == "--draw-distance" ||
                                     arg == "--key" || arg == "--hold" || arg == "--shot" || arg == "--quit-after";
            std::fprintf(stderr, "%s: %s\n\n%s", needs_value ? "Missing or invalid value for" : "Unknown option",
                         argv[i], kUsage);
            return std::nullopt;
        }
    }
    std::stable_sort(opts.keys.begin(), opts.keys.end(),
                     [](const ScriptedKey& a, const ScriptedKey& b) { return a.at_ns < b.at_ns; });
    std::sort(opts.shots.begin(), opts.shots.end());
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

std::filesystem::path pref_dir(const char* app) {
    char* pref = SDL_GetPrefPath("VETTE2026", app);
    if (!pref)
        throw_sdl_error("SDL_GetPrefPath");
    std::filesystem::path dir = path_from_utf8(pref);
    SDL_free(pref);
    return dir;
}

// The game's saves (CONFIG.BIN, SCORE.BIN, ...) go here, never into the player's game folder.
std::filesystem::path save_dir() { return pref_dir("save"); }

// settings.ini (the launch menu's choices).
std::filesystem::path settings_dir() { return pref_dir("config"); }

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

// The Enhanced 3D view (the Extended and Maximum draw distances): the city is extracted from the
// running game once VETTE.EXE has unpacked itself, then every race frame's world is drawn from it at
// the display's resolution (enhanced/scene.h), between the game's own background and overlays.
constexpr int kExtendedRadius = 8;  // cells, about a city block each
constexpr std::uint64_t kExtractFromNs = kNsPerSecond;  // emulated time; earlier tries are retried
constexpr std::uint64_t kExtractRetryNs = kNsPerSecond / 2;
constexpr std::uint64_t kExtractUntilNs = 10 * kNsPerSecond;
static_assert(Presenter::kTransparentPixel == game::SmoothRenderer::kTransparent);

struct EnhancedView {
    int radius = enhanced::kMapCells;
    enhanced::World world;
    std::unique_ptr<enhanced::SceneBuilder> builder;  // once the world is extracted
    std::uint64_t next_try_ns = kExtractFromNs;
    bool failed = false;
    game::SmoothRenderer::Layers layers;
    enhanced::Scene scene;
    Framebuffer under, over;
    std::uint64_t frames = 0;
    double build_ms = 0;

    void prepare(host::Machine& machine) {
        if (builder || failed || machine.emulated_ns() < next_try_ns)
            return;
        std::string error;
        if (enhanced::extract_world(machine, world, error)) {
            SDL_Log("City extracted: %d cell types, %d objects, %d models (%.0f ms)", world.stats.types_used,
                    world.stats.routines, world.stats.models, world.stats.milliseconds);
            builder = std::make_unique<enhanced::SceneBuilder>(world);
        } else if (machine.emulated_ns() >= kExtractUntilNs) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No Enhanced 3D view (the game's own is shown): %s",
                        error.c_str());
            failed = true;
        } else {
            next_try_ns = machine.emulated_ns() + kExtractRetryNs;
        }
    }

    // The race view in layers, with the world drawn from the extracted city. False when the race view
    // isn't on screen (or the city isn't ready): the caller shows the game's own frame.
    bool render(host::Machine& machine, game::SmoothRenderer& smooth, const Presenter& presenter) {
        if (!builder || !smooth.render_layers(machine.emulated_ns(), layers))
            return false;
        enhanced::SceneOptions options;
        options.radius = radius;
        presenter.frame_scale(layers.under.width, layers.under.height, options.pixel_w, options.pixel_h);
        builder->build(layers.ram.data(), options, scene);
        ++frames;
        build_ms += scene.stats.milliseconds;
        copy_frame(layers.under, under);
        copy_frame(layers.over, over);
        return true;
    }
};

// Runs the hosted game until the window closes or VETTE.EXE exits.
// `smooth` (optional) draws the race view at the display's refresh rate (game/smooth.h), and `view`
// (optional, with `smooth` in world-layers mode) draws its world with the Enhanced renderer. `script`:
// the testing options (keys, screenshots, quit time).
void main_loop(Presenter& presenter, host::Machine& machine, AudioOut* audio, Gamepad& gamepad,
               game::SmoothRenderer* smooth, EnhancedView* view, const Options& script) {
    std::size_t next_key = 0;
    std::size_t next_shot = 0;
    struct FrameCount {
        std::uint64_t frames = 0;
        std::uint64_t start_ns = SDL_GetTicksNS();
        ~FrameCount() {
            const double seconds = static_cast<double>(SDL_GetTicksNS() - start_ns) / 1e9;
            if (frames && seconds > 0)
                SDL_Log("Display: %llu frames, %.1f fps", static_cast<unsigned long long>(frames),
                        static_cast<double>(frames) / seconds);
        }
    } presented;
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

        const std::uint64_t t = machine.emulated_ns();
        for (; next_key < script.keys.size() && script.keys[next_key].at_ns <= t; ++next_key)
            machine.key(script.keys[next_key].scancode);
        if (script.quit_after && t >= *script.quit_after)
            return;
        for (; next_shot < script.shots.size() && script.shots[next_shot] <= t; ++next_shot) {
            char name[48];
            std::snprintf(name, sizeof name, "shot_%g.bmp", static_cast<double>(script.shots[next_shot]) / 1e9);
            presenter.request_screenshot(name);
        }
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

        if (view)
            view->prepare(machine);
        const bool layered = view && smooth && view->render(machine, *smooth, presenter);
        if (!layered) {
            if (!smooth || !smooth->render(machine.emulated_ns(), frame))
                machine.render(frame);
            copy_frame(frame, fb);
        }
        // The mouse driver's pointer goes on the copy, never into video memory. Text mode isn't shown.
        Framebuffer& top = layered ? view->over : fb;
        const host::Bios::Cursor pointer = machine.mouse_cursor();
        const bool show_pointer = pointer.visible && (layered || frame.width != 0);
        if (show_pointer)
            draw_mouse_pointer(top, pointer.x, pointer.y);
        presenter.show_system_cursor(!show_pointer);
        if (!presenter.visible()) {
            SDL_Delay(10);  // VSync doesn't pace a hidden window
            continue;
        }
        if (layered)
            presenter.present(view->under, view->scene, view->over);
        else
            presenter.present(fb);
        ++presented.frames;
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
        if (headless) {
            const GameDirSearch search = find_game_dir(opts->game_dir);
            if (!search.dir) {
                report_error(missing_files_message(search), false);
                return 1;
            }
            identify_vette_exe(search.dir->read("VETTE.EXE"));
            Framebuffer fb;
            load_title(*search.dir, fb);
            save_bmp(fb, *opts->dump_frame);
            SDL_Log("Wrote %s", opts->dump_frame->c_str());
            return 0;
        }

        if (!SDL_Init(SDL_INIT_VIDEO))
            throw_sdl_error("SDL_Init");

        // Saved settings, then this run's command-line overrides.
        const std::filesystem::path settings_file = settings_dir() / "settings.ini";
        Settings settings = load_settings(settings_file);
        opts->apply_to(settings);

        // The game folder: --game, else the one chosen in the launch menu, else Game/ near the program.
        GameDirSearch search;
        if (opts->game_dir) {
            search = find_game_dir(opts->game_dir);
        } else {
            if (!settings.game_folder.empty())
                search = find_game_dir(path_from_utf8(settings.game_folder));
            if (!search.dir)
                search = find_game_dir(std::nullopt);
        }
        std::optional<GameDir> game = search.dir;

        Presenter presenter(kAppName);
        presenter.set_fullscreen(settings.fullscreen);
        Gamepad gamepad;

        // The launch menu: when it's switched on, or to let the player find the game files.
        if (opts->launcher.value_or(settings.show_launcher) || !game) {
            if (ui::run_launcher(presenter, gamepad, settings, game, search) == ui::LaunchChoice::Quit)
                return 0;
            if (!save_settings(settings_file, settings))
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Couldn't save %s", path_to_utf8(settings_file).c_str());
        }
        SDL_Log("Game folder: %s", path_to_utf8(game->root()).c_str());
        identify_vette_exe(game->read("VETTE.EXE"));

        std::optional<AudioOut> audio;
        if (!settings.sound) {
            SDL_Log("Sound: off");
        } else if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            try {
                audio.emplace(kAudioRate);
            } catch (const std::exception& e) {
                SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "No sound: %s", e.what());
            }
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "No sound: %s", SDL_GetError());
        }

        host::MachineConfig config;
        config.game_dir = game->root();
        config.save_dir = save_dir();
        config.audio_rate = kAudioRate;
        config.cpu_hz = opts->cpu_hz.value_or(settings.pc == Settings::Pc::Fast ? kFastPcHz : kAtHz);
        // Fixed for the session: DOS games detect the game port once, at startup.
        config.joystick = settings.joystick == Settings::Joystick::Auto ? gamepad.connected()
                                                                         : settings.joystick == Settings::Joystick::On;
        SDL_Log("Joystick: %s", !config.joystick      ? "none"
                                : gamepad.connected() ? "gamepad"
                                                      : "game port present, no gamepad connected (centered)");
        host::Machine machine(config);
        machine.set_log([](const std::string& msg) { SDL_Log("%s", msg.c_str()); });
        std::string error;
        if (!machine.boot(error))
            throw std::runtime_error("Couldn't start VETTE.EXE: " + error);
        SDL_Log("Saves: %s", path_to_utf8(config.save_dir).c_str());
        if (!settings.manual_check)
            game::install_skip_manual_check(machine.cpu());
        game::install_idle_skip(machine);  // the fast PC spends most cycles waiting for retrace
        const bool smooth_fps = settings.frame_rate == Settings::FrameRate::Smooth;
        const bool enhanced_view = settings.draw_distance != Settings::DrawDistance::Original;
        std::optional<game::SmoothRenderer> smooth;
        std::unique_ptr<EnhancedView> view;
        if (smooth_fps || enhanced_view) {
            smooth.emplace(machine, enhanced_view);
            smooth->set_interpolation(smooth_fps);
        }
        if (enhanced_view) {
            view = std::make_unique<EnhancedView>();
            if (settings.draw_distance == Settings::DrawDistance::Extended)
                view->radius = kExtendedRadius;
        }
        SDL_Log("Frame rate: %s; draw distance: %s; emulated CPU %.0f MHz",
                smooth_fps ? "smooth (display refresh)" : "original",
                !enhanced_view ? "original" : view->radius == kExtendedRadius ? "extended" : "maximum",
                static_cast<double>(config.cpu_hz) / 1e6);

        main_loop(presenter, machine, audio ? &*audio : nullptr, gamepad, smooth ? &*smooth : nullptr, view.get(),
                  *opts);
        if (smooth && smooth->stats().replays)
            SDL_Log("Smooth: %llu game frames, %llu display frames, %.2f ms per replay",
                    static_cast<unsigned long long>(smooth->stats().game_frames),
                    static_cast<unsigned long long>(smooth->stats().replays),
                    smooth->stats().replay_ms / static_cast<double>(smooth->stats().replays));
        if (view && view->frames)
            SDL_Log("Enhanced view: %llu frames, %.2f ms per scene", static_cast<unsigned long long>(view->frames),
                    view->build_ms / static_cast<double>(view->frames));
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
