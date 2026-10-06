#include "assets/mac_files.h"
#include "assets/mac_sounds.h"
#include "assets/planar.h"
#include "assets/rle.h"
#include "core/game_dir.h"
#include "core/path_utf8.h"
#include "core/settings.h"
#include "enhanced/backdrop.h"
#include "enhanced/scene.h"
#include "enhanced/world.h"
#include "game/options.h"
#include "game/smooth.h"
#include "game/x86.h"
#include "graphics/art_files.h"
#include "graphics/substitution.h"
#include "host/machine.h"
#include "platform/audio.h"
#include "platform/framebuffer.h"
#include "platform/gamepad.h"
#include "platform/keymap.h"
#include "platform/mouse_pointer.h"
#include "platform/presenter.h"
#include "sound/game_audio.h"
#include "assets/pc98_disk.h"
#include "sound/mac_backend.h"
#include "sound/pc98_backend.h"
#include "sound/pc98_sound.h"
#include "sound/sfx_backend.h"
#include "sound/sfx_bank.h"
#include "ui/launcher.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>  // UTF-8 argv on Windows

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
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
    "                 [--effects off|speaker|adlib|mac] [--music off|original|pc98] [--no-sound]\n"
    "                 [--graphics dos|pc98|mac] [--scaling sharp|smooth] [--resolution display|original]\n"
    "                 [--skyline hills|painted] [--manual-check] [--dump-frame <file.bmp>]\n"
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
    "  --effects adlib      (default) the sound effects on an emulated AdLib FM card (bank: adlib.ini,\n"
    "                       edited with vette_sfx); speaker: the original PC speaker; mac: the Macintosh\n"
    "                       version's digitized sounds (Game/Mac); off\n"
    "  --music original     (default) the title and winner tunes on the effects' device; pc98: the PC-98\n"
    "                       version's FM songs (Game/PC98); off. --no-sound: no effects, no music\n"
    "  --resolution display (default) the Enhanced 3D view at the display's resolution; original: at the\n"
    "                       original's 320x200, enlarged like the rest of the picture\n"
    "  --skyline hills      (default) with the extended or maximum draw distance, the horizon backdrop\n"
    "                       keeps only the hills, trees and water, behind the real city; painted: the\n"
    "                       original's backdrop, with its painted skyline and bridges\n"
    "  --scaling sharp      (default) the pictures simply enlarged, every pixel a solid block; smooth:\n"
    "                       the edges between pixels softened\n"
    "  --graphics dos       (default) the DOS screens; pc98 or mac: that version's art in their place\n"
    "                       (from Game/PC98 or Game/Mac)\n"
    "  --manual-check       show the original's manual-lookup question before the first race\n"
    "                       (skipped by default; this version accepts any answer anyway)\n"
    "  --dump-frame <file>  write the title screen to a 640x200 BMP and exit, without a window\n"
    "Testing (times are emulated seconds):\n"
    "  --key T:SC, --hold A:B:SC   press scan code SC (hex, set 1) at second T for 100 ms, or hold it\n"
    "                       from second A to B, as vette_run does\n"
    "  --shot T             save the window's picture at second T to shot_T.bmp (repeatable)\n"
    "  --poke T:OFF:VAL     write VAL to the game's data segment at offset OFF (hex) at second T: a byte\n"
    "                       for two hex digits, else a word (e.g. a freeway on the next frame:\n"
    "                       --poke 39:2AD4:03 --poke 39:8156:0003)\n"
    "  --quit-after T       close at second T\n"
    "  --wav <file>         record the sound to a WAV file (16-bit mono)\n"
    "  --mute               make the sound (for --wav) but don't play it\n"
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

// A scripted write to the game's data (--poke).
struct ScriptedPoke {
    std::uint64_t at_ns;
    std::uint16_t offset, value;
    bool byte;
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
    std::optional<Settings::Effects> effects;
    std::optional<Settings::Music> music;
    std::optional<Settings::Graphics> graphics;
    std::optional<Settings::Scaling> scaling;
    std::optional<Settings::ViewResolution> view_resolution;
    std::optional<Settings::Skyline> skyline;
    std::vector<ScriptedKey> keys;
    std::vector<ScriptedPoke> pokes;     // sorted by time
    std::vector<std::uint64_t> shots;    // emulated ns, sorted
    std::optional<std::uint64_t> quit_after;  // emulated ns
    std::optional<std::string> wav;           // UTF-8 path
    bool mute = false;  // sorted by time
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
        if (effects)
            s.effects = *effects;
        if (music)
            s.music = *music;
        if (graphics)
            s.graphics = *graphics;
        if (scaling)
            s.scaling = *scaling;
        if (view_resolution)
            s.view_resolution = *view_resolution;
        if (skyline)
            s.skyline = *skyline;
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
        } else if (arg == "--wav" && has_value) {
            opts.wav = argv[++i];
        } else if (arg == "--mute") {
            opts.mute = true;
        } else if (arg == "--no-sound") {
            opts.effects = Settings::Effects::Off;
            opts.music = Settings::Music::Off;
        } else if (arg == "--effects" && has_value) {
            const std::string_view v = argv[++i];
            static constexpr std::string_view kNames[] = {"off", "speaker", "adlib", "mac"};
            const auto it = std::find(std::begin(kNames), std::end(kNames), v);
            if (it == std::end(kNames)) {
                std::fprintf(stderr, "--effects: off, speaker, adlib or mac\n");
                return std::nullopt;
            }
            opts.effects = static_cast<Settings::Effects>(it - std::begin(kNames));
        } else if (arg == "--resolution" && has_value && (std::string_view(argv[i + 1]) == "display" ||
                                                         std::string_view(argv[i + 1]) == "original")) {
            opts.view_resolution = std::string_view(argv[++i]) == "display" ? Settings::ViewResolution::Display
                                                                            : Settings::ViewResolution::Original;
        } else if (arg == "--skyline" && has_value && (std::string_view(argv[i + 1]) == "hills" ||
                                                      std::string_view(argv[i + 1]) == "painted")) {
            opts.skyline = std::string_view(argv[++i]) == "hills" ? Settings::Skyline::Hills : Settings::Skyline::Painted;
        } else if (arg == "--scaling" && has_value && (std::string_view(argv[i + 1]) == "sharp" ||
                                                      std::string_view(argv[i + 1]) == "smooth")) {
            opts.scaling = std::string_view(argv[++i]) == "sharp" ? Settings::Scaling::Sharp : Settings::Scaling::Smooth;
        } else if (arg == "--graphics" && has_value) {
            const std::string_view v = argv[++i];
            static constexpr std::string_view kNames[] = {"dos", "pc98", "mac"};
            const auto it = std::find(std::begin(kNames), std::end(kNames), v);
            if (it == std::end(kNames)) {
                std::fprintf(stderr, "--graphics: dos, pc98 or mac\n");
                return std::nullopt;
            }
            opts.graphics = static_cast<Settings::Graphics>(it - std::begin(kNames));
        } else if (arg == "--music" && has_value) {
            const std::string_view v = argv[++i];
            static constexpr std::string_view kNames[] = {"off", "original", "pc98"};
            const auto it = std::find(std::begin(kNames), std::end(kNames), v);
            if (it == std::end(kNames)) {
                std::fprintf(stderr, "--music: off, original or pc98\n");
                return std::nullopt;
            }
            opts.music = static_cast<Settings::Music>(it - std::begin(kNames));
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
        } else if (arg == "--poke" && has_value) {
            const std::string v = argv[++i];
            const std::size_t c1 = v.find(':');
            const std::size_t c2 = c1 == std::string::npos ? c1 : v.find(':', c1 + 1);
            if (c2 == std::string::npos) {
                std::fprintf(stderr, "--poke needs T:OFF:VAL\n");
                return std::nullopt;
            }
            const auto at = static_cast<std::uint64_t>(std::atof(v.substr(0, c1).c_str()) * static_cast<double>(kNsPerSecond));
            const auto offset = static_cast<std::uint16_t>(std::strtoul(v.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr, 16));
            const auto value = static_cast<std::uint16_t>(std::strtoul(v.substr(c2 + 1).c_str(), nullptr, 16));
            opts.pokes.push_back({at, offset, value, v.size() - c2 - 1 <= 2});
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
                                     arg == "--key" || arg == "--hold" || arg == "--shot" || arg == "--quit-after" ||
                                     arg == "--poke" ||
                                     arg == "--wav" || arg == "--effects" || arg == "--music" ||
                                     arg == "--graphics" || arg == "--scaling" ||
                                     arg == "--resolution" || arg == "--skyline";
            std::fprintf(stderr, "%s: %s\n\n%s", needs_value ? "Missing or invalid value for" : "Unknown option",
                         argv[i], kUsage);
            return std::nullopt;
        }
    }
    std::stable_sort(opts.keys.begin(), opts.keys.end(),
                     [](const ScriptedKey& a, const ScriptedKey& b) { return a.at_ns < b.at_ns; });
    std::stable_sort(opts.pokes.begin(), opts.pokes.end(),
                     [](const ScriptedPoke& a, const ScriptedPoke& b) { return a.at_ns < b.at_ns; });
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

// The Graphics option: the PC-98's or the Mac's art in place of the DOS pictures it recognises on
// screen (graphics/substitution.h); every other frame is shown as the game drew it.
struct Artwork {
    std::unique_ptr<graphics::Substitution> substitution;
    graphics::Composite composite;
    std::array<std::uint32_t, 16> palette{};
    std::uint64_t frames = 0;
    double compose_ms = 0;

    // A composite for this frame (the game's frame, or the Enhanced view's `over`), if one applies.
    bool compose(const Framebuffer& fb) {
        if (fb.width == 0)
            return false;
        for (std::size_t i = 0; i < palette.size(); ++i)
            palette[i] = std::uint32_t{fb.palette[i].r} << 16 | std::uint32_t{fb.palette[i].g} << 8 | fb.palette[i].b;
        const std::uint64_t start = SDL_GetTicksNS();
        const bool replaced =
            substitution->compose({fb.pixels.data(), fb.width, fb.height, &palette}, composite);
        compose_ms += static_cast<double>(SDL_GetTicksNS() - start) / 1e6;
        ++frames;
        return replaced;
    }
};

struct EnhancedView {
    int radius = enhanced::kMapCells;
    bool hills = true;  // Settings::Skyline::Hills: the backdrop without its painted city
    enhanced::Backdrop backdrop;
    enhanced::World world;
    std::unique_ptr<enhanced::SceneBuilder> builder;  // once the world is extracted
    std::uint64_t next_try_ns = kExtractFromNs;
    bool failed = false;
    game::SmoothRenderer::Layers layers;
    enhanced::Scene scene;
    enhanced::Scene mirror;  // the rear-view mirror's, while it's on
    Framebuffer under, over;
    std::uint64_t frames = 0;
    double build_ms = 0;
    std::uint64_t vehicles = 0;  // drawn in the main view, over all frames

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

    // The race view in layers, with the world drawn from the extracted city (or, in highway mode, the
    // freeway built from the game's route data), and the rear-view mirror's view while it's on. False
    // when the race view isn't on screen (or the city isn't ready): the caller shows the game's own frame.
    bool render(host::Machine& machine, game::SmoothRenderer& smooth, const Presenter& presenter) {
        if (!builder || !smooth.render_layers(machine.emulated_ns(), layers))
            return false;
        enhanced::SceneOptions options;
        options.radius = radius;
        options.replicas = radius >= enhanced::kMapCells;  // Maximum: the whole city, all its traffic included
        presenter.frame_scale(layers.under.width, layers.under.height, options.pixel_w, options.pixel_h);
        if (presenter.original_resolution())
            options.line_width = 1;  // the original's one-pixel lines
        builder->build(layers.ram.data(), options, scene);
        build_ms += scene.stats.milliseconds;
        vehicles += static_cast<std::uint64_t>(scene.stats.vehicles);
        // The Hills skyline wherever the real city is drawn (not on a freeway alone, whose painted
        // skyline is the only city there is).
        if (hills && scene.stats.city && layers.horizon.rows > 0) {
            backdrop.apply(machine.ega(), layers.horizon.rows, layers.horizon.source, layers.horizon.dest,
                           layers.under.pixels.data(), layers.under.width, layers.under.height);
        }
        if (layers.mirror) {
            options.mirror = true;
            builder->build(layers.ram.data(), options, mirror);
            build_ms += mirror.stats.milliseconds;
        }
        ++frames;
        copy_frame(layers.under, under);
        copy_frame(layers.over, over);
        return true;
    }
    const enhanced::Scene* inset() const { return layers.mirror ? &mirror : nullptr; }
};

// The game's sound put together from the player's choices (sound/game_audio.h): the effects from the
// speaker, AdLib or the Mac's samples (AdLib for what the Mac lacks), the music from the same, from the
// PC-98's FM songs, or off. The AdLib bank is read from adlib.ini in the settings folder, again whenever
// it changes (saved from the sound editor while the game runs).
// Sounds playing together can add up past full scale (the engine under a crash): bend the peaks
// smoothly instead of clipping them.
float soft_limit(float x) {
    constexpr float kKnee = 0.8f;
    const float a = std::fabs(x);
    return a <= kKnee ? x : std::copysign(kKnee + (1 - kKnee) * std::tanh((a - kKnee) / (1 - kKnee)), x);
}

struct GameSound {
    sound::AdlibBackend adlib{kAudioRate};
    std::unique_ptr<sound::SfxBackend> mac, pc98;
    std::unique_ptr<sound::GameAudio> audio;
    std::filesystem::path bank_file;
    std::filesystem::file_time_type bank_time{};
    std::uint64_t next_check_ns = 0;
    std::vector<float> mix;

    GameSound(host::Machine& machine, const Settings& settings, std::unique_ptr<sound::SfxBackend> mac_sound,
              std::unique_ptr<sound::SfxBackend> pc98_music, std::filesystem::path bank)
        : mac(std::move(mac_sound)), pc98(std::move(pc98_music)), bank_file(std::move(bank)) {
        sound::GameAudio::Sources src;
        src.effects_off = settings.effects == Settings::Effects::Off;
        src.effects = settings.effects == Settings::Effects::AdLib ? &adlib
                      : settings.effects == Settings::Effects::Mac ? (mac ? mac.get() : &adlib)
                                                                   : nullptr;
        src.music_off = settings.music == Settings::Music::Off;
        src.music = pc98.get();
        src.fallback = &adlib;
        audio = std::make_unique<sound::GameAudio>(machine, kAudioRate, src);
        load_bank();
    }

    void load_bank() {
        std::error_code ec;
        bank_time = std::filesystem::last_write_time(bank_file, ec);
        std::ifstream f(bank_file, std::ios::binary);
        if (!f) {
            adlib.adlib().set_bank(sound::SfxBank::defaults());
            return;
        }
        std::stringstream text;
        text << f.rdbuf();
        adlib.adlib().set_bank(sound::SfxBank::parse(text.str()));
        SDL_Log("AdLib sound bank: %s", path_to_utf8(bank_file).c_str());
    }

    // Replaces one stretch of the speaker's samples (emulated time from t0) with the replacement's.
    void process(std::uint64_t t0_ns, std::vector<std::int16_t>& samples) {
        if (SDL_GetTicksNS() >= next_check_ns) {
            next_check_ns = SDL_GetTicksNS() + kNsPerSecond;
            std::error_code ec;
            const auto t = std::filesystem::last_write_time(bank_file, ec);
            if (!ec && t != bank_time)
                load_bank();
        }
        audio->render(t0_ns, samples, mix);
        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = static_cast<std::int16_t>(soft_limit(mix[i]) * 32767.0f);
    }
};

// --wav: the sound as played, to a 16-bit mono WAV (the header is completed on close).
class WavWriter {
public:
    explicit WavWriter(const std::string& path_utf8) : file_(path_from_utf8(path_utf8), std::ios::binary) {
        file_.write(std::string(44, '\0').data(), 44);
    }
    ~WavWriter() {
        const auto u32 = [](std::uint32_t v) {
            return std::string{static_cast<char>(v), static_cast<char>(v >> 8), static_cast<char>(v >> 16),
                               static_cast<char>(v >> 24)};
        };
        const auto u16 = [](std::uint16_t v) { return std::string{static_cast<char>(v), static_cast<char>(v >> 8)}; };
        const std::string header = "RIFF" + u32(36 + bytes_) + "WAVEfmt " + u32(16) + u16(1) + u16(1) + u32(kAudioRate) +
                                   u32(kAudioRate * 2) + u16(2) + u16(16) + "data" + u32(bytes_);
        file_.seekp(0);
        file_.write(header.data(), static_cast<std::streamsize>(header.size()));
    }
    void write(const std::vector<std::int16_t>& samples) {
        for (const std::int16_t v : samples) {
            const char b[2] = {static_cast<char>(v), static_cast<char>(static_cast<std::uint16_t>(v) >> 8)};
            file_.write(b, 2);
        }
        bytes_ += static_cast<std::uint32_t>(samples.size() * 2);
    }

private:
    std::ofstream file_;
    std::uint32_t bytes_ = 0;
};

// The Mac version's sounds from the player's copy (nullptr, with the reason logged, if not).
std::unique_ptr<sound::SfxBackend> mac_sound(const GameVersions& versions) {
    if (!versions.mac) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Mac sound: the Mac version isn't in the game folder");
        return nullptr;
    }
    const std::optional<std::filesystem::path>& folder = versions.mac;
    const assets::MacFiles files = open_mac(versions);
    const std::optional<assets::ResourceFork> data = assets::find_vette_data(files);
    std::vector<assets::MacSound> sounds = data ? assets::decode_mac_sounds(*data) : std::vector<assets::MacSound>{};
    if (sounds.empty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Mac sound: VETTE!.Data's sounds not found in %s",
                    path_to_utf8(*folder).c_str());
        for (const std::string& note : files.notes())
            SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "  %s", note.c_str());
        return nullptr;
    }
    SDL_Log("Mac sound: %u sounds from %s", static_cast<unsigned>(sounds.size()), path_to_utf8(*folder).c_str());
    return std::make_unique<sound::MacBackend>(std::move(sounds), kAudioRate);
}

// The PC-98 version's FM songs from the player's copy (nullptr, with the reason logged, if not).
std::unique_ptr<sound::SfxBackend> pc98_music(const GameVersions& versions, host::Machine& machine) {
    if (!versions.pc98) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "PC-98 music: the PC-98 version isn't in the game folder");
        return nullptr;
    }
    const std::optional<std::filesystem::path>& folder = versions.pc98;
    std::string error;
    const std::optional<assets::Pc98Files> files = open_pc98(versions, error);
    std::unique_ptr<sound::Pc98Sound> fm = files ? sound::Pc98Sound::create(*files, kAudioRate, error) : nullptr;
    if (!fm) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "PC-98 music: %s", error.c_str());
        return nullptr;
    }
    SDL_Log("PC-98 music: from %s", path_to_utf8(*folder).c_str());
    return std::make_unique<sound::Pc98Backend>(machine, std::move(fm));
}

// Runs the hosted game until the window closes or VETTE.EXE exits.
// `smooth` (optional) draws the race view at the display's refresh rate (game/smooth.h), and `view`
// (optional, with `smooth` in world-layers mode) draws its world with the Enhanced renderer. `script`:
// the testing options (keys, screenshots, quit time).
void main_loop(Presenter& presenter, host::Machine& machine, AudioOut* audio, Gamepad& gamepad,
               game::SmoothRenderer* smooth, EnhancedView* view, GameSound* game_sound, Artwork* art,
               const Options& script) {
    std::size_t next_key = 0;
    std::size_t next_poke = 0;
    std::size_t next_shot = 0;
    std::optional<WavWriter> wav;
    if (script.wav)
        wav.emplace(*script.wav);
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
        for (; next_poke < script.pokes.size() && script.pokes[next_poke].at_ns <= t; ++next_poke) {
            const ScriptedPoke& pk = script.pokes[next_poke];
            const std::uint32_t at = host::Cpu::linear(game::kDataSeg, pk.offset);
            if (pk.byte)
                machine.memory().write8(at, static_cast<std::uint8_t>(pk.value));
            else
                machine.memory().write16(at, pk.value);
        }
        if (script.quit_after && t >= *script.quit_after)
            return;
        for (; next_shot < script.shots.size() && script.shots[next_shot] <= t; ++next_shot) {
            char name[48];
            std::snprintf(name, sizeof name, "shot_%g.bmp", static_cast<double>(script.shots[next_shot]) / 1e9);
            presenter.request_screenshot(name);
        }
        const std::uint64_t now = SDL_GetTicksNS();
        const std::uint64_t t0 = machine.emulated_ns();
        machine.run_for(std::min(now - last, kMaxStepNs));
        last = now;
        if (!machine.fault().empty())
            throw std::runtime_error("VETTE.EXE stopped: " + machine.fault());
        if (machine.stopped())
            return;  // the player quit to DOS

        samples.clear();
        machine.take_audio(samples);
        if (game_sound)
            game_sound->process(t0, samples);
        if (wav)
            wav->write(samples);
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
        const bool replaced = art && art->compose(top);
        if (layered && replaced)
            presenter.present(view->under, view->scene, art->composite, view->inset());
        else if (layered)
            presenter.present(view->under, view->scene, view->over, view->inset());
        else if (replaced)
            presenter.present(art->composite);
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
        presenter.set_smooth_scaling(settings.scaling == Settings::Scaling::Smooth);
        presenter.set_original_resolution(settings.view_resolution == Settings::ViewResolution::Original);
        Gamepad gamepad;

        // The launch menu: when it's switched on, or to let the player find the game files.
        if (opts->launcher.value_or(settings.show_launcher) || !game) {
            if (ui::run_launcher(presenter, gamepad, settings, game, search) == ui::LaunchChoice::Quit)
                return 0;
            if (!save_settings(settings_file, settings))
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Couldn't save %s", path_to_utf8(settings_file).c_str());
        }
        SDL_Log("Game folder: %s", path_to_utf8(search.versions.root).c_str());
        SDL_Log("  DOS: %s", path_to_utf8(game->root()).c_str());
        if (search.versions.pc98)
            SDL_Log("  PC-98: %s (%s)", path_to_utf8(*search.versions.pc98).c_str(), search.versions.pc98_what.c_str());
        if (search.versions.mac)
            SDL_Log("  Mac: %s (%s)", path_to_utf8(*search.versions.mac).c_str(), search.versions.mac_what.c_str());
        identify_vette_exe(game->read("VETTE.EXE"));

        std::optional<AudioOut> audio;
        if (settings.effects == Settings::Effects::Off && settings.music == Settings::Music::Off) {
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
            view->hills = settings.skyline == Settings::Skyline::Hills;
        }
        SDL_Log("Frame rate: %s; draw distance: %s%s; emulated CPU %.0f MHz",
                smooth_fps ? "smooth (display refresh)" : "original",
                !enhanced_view ? "original" : view->radius == kExtendedRadius ? "extended" : "maximum",
                !enhanced_view ? "" : view->hills ? ", hills skyline" : ", painted skyline",
                static_cast<double>(config.cpu_hz) / 1e6);

        // Sound: the emulated PC speaker, or a replacement driven by the game's sound events.
        if (opts->mute)
            audio.reset();
        std::unique_ptr<GameSound> game_sound;
        const bool silent = settings.effects == Settings::Effects::Off && settings.music == Settings::Music::Off;
        if (!silent &&
            (settings.effects != Settings::Effects::Speaker || settings.music != Settings::Music::Original)) {
            std::unique_ptr<sound::SfxBackend> mac =
                settings.effects == Settings::Effects::Mac ? mac_sound(search.versions) : nullptr;
            std::unique_ptr<sound::SfxBackend> pc98 =
                settings.music == Settings::Music::Pc98 ? pc98_music(search.versions, machine) : nullptr;
            game_sound = std::make_unique<GameSound>(machine, settings, std::move(mac), std::move(pc98),
                                                     settings_dir() / "adlib.ini");
        }
        static constexpr const char* kEffects[] = {"off", "PC speaker", "AdLib", "Macintosh"};
        static constexpr const char* kMusic[] = {"off", "original", "PC-98 FM"};
        SDL_Log("Sound effects: %s; music: %s%s", kEffects[static_cast<int>(settings.effects)],
                kMusic[static_cast<int>(settings.music)], audio || silent ? "" : " (not played)");

        // Graphics: the PC-98's or the Mac's art, from the player's copies in Game/PC98 and Game/Mac.
        std::unique_ptr<Artwork> art;
        if (settings.graphics != Settings::Graphics::Dos) {
            const graphics::Art which =
                settings.graphics == Settings::Graphics::Pc98 ? graphics::Art::Pc98 : graphics::Art::Mac;
            std::vector<std::string> notes;
            const graphics::ArtFiles files = graphics::ArtFiles::from_versions(search.versions, &notes);
            auto substitution = std::make_unique<graphics::Substitution>(which, files);
            for (const std::string& w : substitution->warnings())
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Graphics: %s", w.c_str());
            if (substitution->available().empty()) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Graphics: no %s art found; the DOS screens are shown",
                            graphics::art_name(which));
            } else {
                substitution->attach(machine);  // program memory, and the draw tracker's watches
                art = std::make_unique<Artwork>();
                art->substitution = std::move(substitution);
                SDL_Log("Graphics: %s art for %u screens", graphics::art_name(which),
                        static_cast<unsigned>(art->substitution->available().size()));
            }
        }

        // Another version's art may lay the dash out differently: the mirror then stays the game's own.
        if (smooth && art)
            smooth->set_mirror_inset(false);
        main_loop(presenter, machine, audio ? &*audio : nullptr, gamepad, smooth ? &*smooth : nullptr, view.get(),
                  game_sound.get(), art.get(), *opts);
        if (art && art->frames)
            SDL_Log("Graphics: %.2f ms per frame to compose", art->compose_ms / static_cast<double>(art->frames));
        if (smooth && smooth->stats().replays)
            SDL_Log("Smooth: %llu game frames, %llu display frames, %.2f ms per replay",
                    static_cast<unsigned long long>(smooth->stats().game_frames),
                    static_cast<unsigned long long>(smooth->stats().replays),
                    smooth->stats().replay_ms / static_cast<double>(smooth->stats().replays));
        if (view && view->frames)
            SDL_Log("Enhanced view: %llu frames, %.2f ms per scene, %.0f vehicles and pedestrians drawn",
                    static_cast<unsigned long long>(view->frames), view->build_ms / static_cast<double>(view->frames),
                    static_cast<double>(view->vehicles) / static_cast<double>(view->frames));
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
