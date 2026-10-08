#include "assets/mac_files.h"
#include "assets/mac_sounds.h"
#include "assets/planar.h"
#include "assets/rle.h"
#include "core/game_dir.h"
#include "core/path_utf8.h"
#include "core/settings.h"
#include "enhanced/backdrop.h"
#include "enhanced/lanes.h"
#include "enhanced/scene.h"
#include "enhanced/world.h"
#include "game/driving.h"
#include "game/options.h"
#include "game/city_map.h"
#include "game/no_freeways.h"
#include "game/smooth.h"
#include "game/two_player.h"
#include "game/x86.h"
#include "graphics/art_files.h"
#include "graphics/substitution.h"
#include "host/loopback_link.h"
#include "host/machine.h"
#include "host/tcp_link.h"
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
#include "ui/key_sheet.h"
#include "ui/launcher.h"
#include "ui/map_editor.h"
#include "ui/online.h"
#include "ui/shortcuts.h"
#include "platform/url_scheme.h"
#ifdef VETTE_ONLINE
#include "net/online.h"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>  // UTF-8 argv on Windows

#include <algorithm>
#include <array>
#include <cctype>
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
    "                 [--skyline hills|painted] [--depth-buffer on|off] [--driving original|improved]\n"
    "                 [--lane-centering on|off] [--freeway-traffic smooth|original] [--manual-check]\n"
    "                 [--dump-frame <file.bmp>]\n"
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
    "  --depth-buffer on    (default) the Enhanced 3D view on the GPU with a depth buffer: nearer things\n"
    "                       always cover farther ones, with the whole city's traffic; off: the original's\n"
    "                       drawing order, with traffic and pedestrians only near the car\n"
    "  --driving original   (default) the original's driving; improved: the car drifts a little through\n"
    "                       fast corners and leaves the ground over crests at speed\n"
    "  --lane-centering on  a slight steering assist toward the lane's direction and centre (default off)\n"
    "  --freeway-traffic smooth   (default) new freeway cars come at the far end of the road and fade in\n"
    "                       and out; original: they appear a few hundred yards ahead and vanish far off\n"
    "  --map NAME           play a map made in the map editor (original: the original's city)\n"
    "  --freeways off       no freeways: roads join the city's parts, driven as one city (default on)\n"
    "  --scaling sharp      (default) the pictures simply enlarged, every pixel a solid block; smooth:\n"
    "                       the edges between pixels softened\n"
    "  --graphics dos       (default) the DOS screens; pc98 or mac: that version's art in their place\n"
    "                       (from Game/PC98 or Game/Mac)\n"
    "  --manual-check       show the original's manual-lookup question before the first race\n"
    "                       (skipped by default; this version accepts any answer anyway)\n"
    "  --dump-frame <file>  write the title screen to a 640x200 BMP and exit, without a window\n"
    "Testing (times are emulated seconds, and go on while the key sheet pauses the game):\n"
    "  --key T:SC, --hold A:B:SC   press scan code SC (hex, set 1) at second T for 100 ms, or hold it\n"
    "                       from second A to B, as vette_run does\n"
    "  --press T:KEYS       press a key at second T as the player would, through the window (this\n"
    "                       program's keys too): e.g. ctrl+h, alt+q, escape\n"
    "  --shot T             save the window's picture at second T to shot_T.bmp (repeatable)\n"
    "  --poke T:OFF:VAL     write VAL to the game's data segment at offset OFF (hex) at second T: a byte\n"
    "                       for two hex digits, else a word (e.g. a freeway on the next frame:\n"
    "                       --poke 39:2AD4:03 --poke 39:8156:0003)\n"
    "  --quit-after T       close at second T\n"
    "  --wav <file>         record the sound to a WAV file (16-bit mono)\n"
    "  --mute               make the sound (for --wav) but don't play it\n"
    "Two players over TCP, for development (the original's own two-player race; see\n"
    "re/notes/12-two-player.md):\n"
    "  --online-host        host an online race without the menu: the codes are logged, and the race\n"
    "                       starts when the friend joins (the course from the settings)\n"
    "  --online-lan-host    the same, a LAN race: on this network only\n"
    "  --online-join CODE   join the friend's online race without the menu (CODE \"lan\": the first LAN race)\n"
    "  --online-server URL  the relay server for this run (wss://...)\n"
    "  --link-listen PORT   be the host: wait for the other game on PORT\n"
    "  --link-connect HOST:PORT  be the guest: connect to the host\n"
    "                       Both games are taken through the original's menus into the race.\n"
    "  --link-course N      the host's course, 1-4 (default 1; the guest takes the host's)\n"
    "  --link-delay MS, --link-jitter MS   delay what arrives by MS plus a random 0..jitter, in order\n"
    "  --link-manual        don't drive the menus: Esc > Communications > Two players yourself\n"
    "\n"
    "Ctrl+H shows the keys during the game; Alt+Q quits to the desktop from any screen; F11 or\n"
    "Alt+Enter toggles fullscreen. Every other key, Esc included, goes to the game.\n"
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

// A scripted key press through the window (--press): the key's events, as SDL gives the player's.
struct ScriptedPress {
    std::uint64_t at_ns;
    SDL_Scancode scancode;
    SDL_Keymod mod;
};

void push_press(const ScriptedPress& p, SDL_Window* window) {
    for (const bool down : {true, false}) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.windowID = SDL_GetWindowID(window);
        e.key.scancode = p.scancode;
        e.key.key = SDL_GetKeyFromScancode(p.scancode, SDL_KMOD_NONE, false);
        e.key.mod = p.mod;
        e.key.down = down;
        SDL_PushEvent(&e);
    }
}

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
    std::optional<bool> depth_buffer;
    std::optional<bool> improved_driving;
    std::optional<bool> lane_centering;
    std::optional<bool> smooth_traffic;
    std::optional<std::string> map_name;
    std::optional<bool> freeways;
    std::vector<ScriptedKey> keys;
    std::vector<ScriptedPress> presses;  // sorted by time
    std::vector<ScriptedPoke> pokes;     // sorted by time
    std::vector<std::uint64_t> shots;    // emulated ns, sorted
    std::optional<std::uint64_t> quit_after;  // emulated ns
    std::optional<std::string> wav;           // UTF-8 path
    bool mute = false;  // sorted by time
    bool help = false;
    // An online race without the menu (--online-*).
    std::optional<std::string> online_server;
    bool online_host = false;
    std::optional<std::string> online_join;  // the code
    std::optional<std::string> invite;       // a vette2026:// link the game was opened with
    // The development link (--link-*).
    std::optional<std::uint16_t> link_listen;
    std::optional<std::string> link_connect;  // host:port
    int link_course = 1;
    double link_delay_ms = 0, link_jitter_ms = 0;
    bool link_manual = false;

    void apply_to(Settings& s) const {
        if (online_server)
            s.online_server = *online_server;
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
        if (depth_buffer)
            s.depth_buffer = *depth_buffer;
        if (improved_driving)
            s.improved_driving = *improved_driving;
        if (lane_centering)
            s.lane_centering = *lane_centering;
        if (smooth_traffic)
            s.smooth_traffic = *smooth_traffic;
        if (map_name)
            s.map_name = *map_name == "original" ? std::string() : *map_name;
        if (freeways)
            s.freeways = *freeways;
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
        } else if (is_invite_link(arg)) {  // opened from an invite link (Windows, Linux)
            opts.invite = std::string(arg);
        } else if (arg == "--online-server" && has_value) {
            opts.online_server = argv[++i];
        } else if (arg == "--online-host") {
            opts.online_host = true;
        } else if (arg == "--online-lan-host") {
            opts.online_host = true;
            opts.online_join = "lan";  // (connect_online: a LAN race)
        } else if (arg == "--online-join" && has_value) {
            opts.online_join = argv[++i];
        } else if (arg == "--link-listen" && has_value) {
            opts.link_listen = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--link-connect" && has_value && std::string_view(argv[i + 1]).find(':') != std::string_view::npos) {
            opts.link_connect = argv[++i];
        } else if (arg == "--link-course" && has_value && std::atoi(argv[i + 1]) >= 1 && std::atoi(argv[i + 1]) <= 4) {
            opts.link_course = std::atoi(argv[++i]);
        } else if ((arg == "--link-delay" || arg == "--link-jitter") && has_value) {
            (arg == "--link-delay" ? opts.link_delay_ms : opts.link_jitter_ms) = std::max(0.0, std::atof(argv[++i]));
        } else if (arg == "--link-manual") {
            opts.link_manual = true;
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
        } else if (arg == "--driving" && has_value && (std::string_view(argv[i + 1]) == "original" ||
                                                      std::string_view(argv[i + 1]) == "improved")) {
            opts.improved_driving = std::string_view(argv[++i]) == "improved";
        } else if (arg == "--lane-centering" && has_value && (std::string_view(argv[i + 1]) == "on" ||
                                                             std::string_view(argv[i + 1]) == "off")) {
            opts.lane_centering = std::string_view(argv[++i]) == "on";
        } else if (arg == "--freeways" && has_value && (std::string_view(argv[i + 1]) == "on" ||
                                                       std::string_view(argv[i + 1]) == "off")) {
            opts.freeways = std::string_view(argv[++i]) == "on";
        } else if (arg == "--map" && has_value) {
            opts.map_name = argv[++i];
        } else if (arg == "--freeway-traffic" && has_value && (std::string_view(argv[i + 1]) == "smooth" ||
                                                              std::string_view(argv[i + 1]) == "original")) {
            opts.smooth_traffic = std::string_view(argv[++i]) == "smooth";
        } else if (arg == "--depth-buffer" && has_value && (std::string_view(argv[i + 1]) == "on" ||
                                                           std::string_view(argv[i + 1]) == "off")) {
            opts.depth_buffer = std::string_view(argv[++i]) == "on";
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
        } else if (arg == "--press" && has_value) {
            // T:KEYS: ctrl+, alt+ and shift+ before a key's SDL name (case doesn't matter).
            const std::string v = argv[++i];
            const std::size_t c = v.find(':');
            ScriptedPress p{static_cast<std::uint64_t>(std::atof(v.substr(0, c).c_str()) * static_cast<double>(kNsPerSecond)),
                            SDL_SCANCODE_UNKNOWN, SDL_KMOD_NONE};
            std::string name = c == std::string::npos ? std::string() : v.substr(c + 1);
            for (std::size_t plus; (plus = name.find('+')) != std::string::npos && plus + 1 < name.size();) {
                std::string m = name.substr(0, plus);
                std::transform(m.begin(), m.end(), m.begin(), [](unsigned char ch) { return std::tolower(ch); });
                p.mod = static_cast<SDL_Keymod>(p.mod | (m == "ctrl" ? SDL_KMOD_LCTRL : m == "alt" ? SDL_KMOD_LALT
                                                         : m == "shift" ? SDL_KMOD_LSHIFT : SDL_KMOD_NONE));
                name = name.substr(plus + 1);
            }
            p.scancode = SDL_GetScancodeFromName(name.c_str());
            if (p.scancode == SDL_SCANCODE_UNKNOWN) {
                std::fprintf(stderr, "--press needs T:KEYS, e.g. 40:ctrl+h (not \"%s\")\n", v.c_str());
                return std::nullopt;
            }
            opts.presses.push_back(p);
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
                                     arg == "--press" ||
                                     arg == "--poke" ||
                                     arg == "--wav" || arg == "--effects" || arg == "--music" ||
                                     arg == "--graphics" || arg == "--scaling" ||
                                     arg == "--resolution" || arg == "--skyline" ||
                                     arg == "--depth-buffer" || arg == "--driving" || arg == "--lane-centering" ||
                                     arg == "--freeway-traffic" || arg == "--map" || arg == "--freeways" ||
                                     arg == "--link-listen" || arg == "--link-connect" || arg == "--link-course" ||
                                     arg == "--link-delay" || arg == "--link-jitter" || arg == "--online-server" ||
                                     arg == "--online-join";
            std::fprintf(stderr, "%s: %s\n\n%s", needs_value ? "Missing or invalid value for" : "Unknown option",
                         argv[i], kUsage);
            return std::nullopt;
        }
    }
    std::stable_sort(opts.keys.begin(), opts.keys.end(),
                     [](const ScriptedKey& a, const ScriptedKey& b) { return a.at_ns < b.at_ns; });
    std::stable_sort(opts.presses.begin(), opts.presses.end(),
                     [](const ScriptedPress& a, const ScriptedPress& b) { return a.at_ns < b.at_ns; });
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

// The map editor's maps (ui/map_editor.h).
std::filesystem::path maps_dir() { return pref_dir("maps"); }

// The two-player setup screen opens on the link this program makes (game::link_config: Direct, COM1,
// 57.6k), so an online race's start just goes down it; unless the player has saved choices of their own.
void default_link_config(const GameDir& game, const std::filesystem::path& saves) {
    std::vector<std::uint8_t> shipped;
    try {
        shipped = game.read("CONFIG.BIN");
    } catch (const std::exception&) {
        return;  // (the game makes one when the screen is saved)
    }
    const std::filesystem::path path = saves / "CONFIG.BIN";
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        std::ifstream in(path, std::ios::binary);
        const std::vector<std::uint8_t> saved{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (saved != shipped)
            return;  // the player's own (or already the link's)
    }
    const std::vector<std::uint8_t> config = game::link_config(shipped);
    if (config.empty())
        return;
    std::filesystem::create_directories(saves, ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(config.data()), static_cast<std::streamsize>(config.size()));
}

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
    bool smooth_traffic = true;  // Settings::smooth_traffic: freeway cars fade in and out
    bool no_freeways = false;    // Settings::freeways off: traffic and pedestrians where game::no_freeway_placement puts them
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
        // With the depth buffer, all of the city's traffic and pedestrians (nothing pops in); without,
        // only those near the car, where the original draws them (none show through the scenery).
        options.depth = options.replicas = options.far_vehicles = presenter.depth_buffer();
        options.smooth_traffic = smooth_traffic;
        if (no_freeways)
            options.placement = game::no_freeway_placement;
        options.time_s = static_cast<double>(machine.emulated_ns()) / 1e9;
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

// Improved Driving and Lane Centering (game/driving.h): the player's car on top of the original's
// physics. Lane Centering follows the lane markings of the extracted city: the Enhanced view's, or its
// own extraction when that view is off.
struct DrivingAids {
    game::Driving driving;
    std::uint64_t next_try_ns = kExtractFromNs;
    bool failed = false;

    DrivingAids(host::Machine& machine, game::Driving::Options options) : driving(machine, options) {}

    void set_lanes(const enhanced::World& world) {
        auto lanes = enhanced::lane_map(world);
        SDL_Log("Lane centering: %u lane markings", static_cast<unsigned>(lanes->size()));
        driving.set_lanes(std::move(lanes));
    }

    // Between emulation slices.
    void prepare(host::Machine& machine, const EnhancedView* view) {
        if (!driving.options().lane_centering || driving.has_lanes() || failed)
            return;
        if (view && !view->failed) {
            if (view->builder)
                set_lanes(view->world);
            return;
        }
        if (machine.emulated_ns() < next_try_ns)
            return;
        enhanced::World world;
        std::string error;
        if (enhanced::extract_world(machine, world, error)) {
            set_lanes(world);
        } else if (machine.emulated_ns() >= kExtractUntilNs) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No lane centering: %s", error.c_str());
            failed = true;
        } else {
            next_try_ns = machine.emulated_ns() + kExtractRetryNs;
        }
    }
};

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

// A two-player link, polled between emulation slices: the development link or an online race.
struct TwoPlayerLink {
    virtual ~TwoPlayerLink() = default;
    virtual void update(host::Machine& machine, DrivingAids* driving) = 0;
};

// --- Development two-player link (--link-listen / --link-connect) ----------------------------------------
// Two vette2026 processes race the original's two-player game over TCP, without the relay server
// (host/tcp_link.h). The listening side is the host: its course and driving physics go to the guest
// first, then both games are taken through the original's menus into the race (game/two_player.h).
struct DevLink final : TwoPlayerLink {
    std::unique_ptr<host::TcpLink> tcp;
    std::unique_ptr<host::DelayedLink> delayed;  // --link-delay / --link-jitter
    std::unique_ptr<game::LinkPacer> pacer;
    std::unique_ptr<game::TwoPlayerStart> start;
    bool host = false;
    bool manual = false;
    bool lane_centering = false;  // the guest's own
    game::TwoPlayerSetup setup;
    bool connected = false;
    bool got_setup = false;
    int jumps = 0;                 // logged so far: this car's jumps,
    bool remote_airborne = false;  // and whether the other car was last seen in the air

    // Between emulation slices: the setup from the host, the menus, the connection's state.
    void update(host::Machine& machine, DrivingAids* driving) override {
        tcp->poll();
        if (tcp->connected() != connected) {
            connected = tcp->connected();
            SDL_Log("Two players: %s", connected ? "the other game is connected" : tcp->closed_reason().c_str());
        }
        if (!host && !got_setup && tcp->hello()) {
            got_setup = true;
            const auto received = game::TwoPlayerSetup::decode(*tcp->hello());
            if (!received) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Two players: the host sent '%s', not a setup this version knows",
                            tcp->hello()->c_str());
            } else {
                setup = *received;
                SDL_Log("Two players: the host's setup: course %d, %s driving", setup.course,
                        setup.improved_driving ? "improved" : "original");
                if (driving)  // both games drive with the host's physics
                    driving->driving.set_options(game::Driving::Options{setup.improved_driving, lane_centering});
                if (!manual)
                    begin(machine);
            }
        }
        if (start) {
            const auto before = start->phase();
            start->poll();
            if (start->phase() != before && start->failed())
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", start->error().c_str());
        }
        // Jumps, this car's and the other's as shown here (Improved Driving).
        if (driving && driving->driving.options().improved) {
            const double t = static_cast<double>(machine.emulated_ns()) / 1e9;
            const auto& tm = driving->driving.telemetry();
            if (tm.jumps != jumps) {
                jumps = tm.jumps;
                SDL_Log("Two players: %.2fs this car leaves the ground (speed %d)", t, tm.speed);
            }
            const auto& remote = driving->driving.remote();
            if (remote.airborne != remote_airborne) {
                remote_airborne = remote.airborne;
                SDL_Log("Two players: %.2fs the other car is %s (z %.0f, ground %.0f)", t,
                        remote.airborne ? "in the air" : "back on the ground", remote.z, remote.ground);
            }
        }
    }

    void begin(host::Machine& machine) {
        start = std::make_unique<game::TwoPlayerStart>(
            machine, host ? game::TwoPlayerStart::Role::Host : game::TwoPlayerStart::Role::Guest, setup,
            game::TwoPlayerStart::Own{});
        start->on_log = [](const std::string& line) { SDL_Log("Two players: %s", line.c_str()); };
    }
};

// Sets up the development link from the command line (nullptr without --link-*).
std::unique_ptr<DevLink> make_dev_link(const Options& opts, const Settings& settings, host::Machine& machine) {
    if (!opts.link_listen && !opts.link_connect)
        return nullptr;
    auto link = std::make_unique<DevLink>();
    std::string error;
    link->host = opts.link_listen.has_value();
    link->manual = opts.link_manual;
    link->lane_centering = settings.lane_centering;
    if (link->host) {
        link->setup.course = opts.link_course;
        link->setup.improved_driving = settings.improved_driving;
        link->tcp = host::TcpLink::listen(*opts.link_listen, error);
        if (link->tcp) {
            link->tcp->set_hello(link->setup.encode());
            SDL_Log("Two players: waiting for the other game on port %u (%s)", *opts.link_listen,
                    link->setup.encode().c_str());
        }
    } else {
        const std::string& v = *opts.link_connect;
        const std::size_t colon = v.rfind(':');
        link->tcp = host::TcpLink::connect(v.substr(0, colon),
                                           static_cast<std::uint16_t>(std::strtoul(v.c_str() + colon + 1, nullptr, 10)), error);
        if (link->tcp)
            SDL_Log("Two players: connecting to %s", v.c_str());
    }
    if (!link->tcp)
        throw std::runtime_error("Two players: " + error);
    host::SerialLink* cable = link->tcp.get();
    if (opts.link_delay_ms > 0 || opts.link_jitter_ms > 0) {
        host::DelayedLink::Options lag;
        lag.delay_ns = static_cast<std::uint64_t>(opts.link_delay_ms * 1e6);
        lag.jitter_ns = static_cast<std::uint64_t>(opts.link_jitter_ms * 1e6);
        lag.seed = link->host ? 1 : 2;
        link->delayed = std::make_unique<host::DelayedLink>(*link->tcp, [] { return SDL_GetTicksNS(); }, lag);
        cable = link->delayed.get();
        SDL_Log("Two players: arrivals delayed by %.0f ms + up to %.0f ms", opts.link_delay_ms, opts.link_jitter_ms);
    }
    link->pacer = std::make_unique<game::LinkPacer>(machine, *cable);
    machine.attach_serial(link->pacer.get());
    if (link->host && !link->manual)
        link->begin(machine);
    return link;
}

// --- Online two-player race (ui/online.h) ----------------------------------------------------------------
// Both players are in a room on the relay server (net/room_link.h), which is the serial cable between the
// two games; both are taken through the original's menus into the race, with the host's course and
// driving physics. The window's title shows the room's state.
struct OnlineRace final : TwoPlayerLink {
    ui::OnlineSession session;
    std::unique_ptr<game::LinkPacer> pacer;
    std::unique_ptr<game::TwoPlayerStart> start;
    SDL_Window* window = nullptr;
    std::uint64_t next_title_ns = 0;

    void update(host::Machine&, DrivingAids*) override {
        const auto before = start->phase();
        start->poll();
        if (start->phase() != before && start->failed())
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Online race: %s", start->error().c_str());
#ifdef VETTE_ONLINE
        const std::uint64_t now = SDL_GetTicksNS();
        if (window && session.online && now >= next_title_ns) {
            next_title_ns = now + 1'000'000'000;
            const net::OnlineStatus st = session.online->status();
            std::string title = std::string(kAppName) + " - online race: ";
            switch (st.state) {
            case net::LinkState::Connected:
                title += std::string("racing your friend, ") + net::to_string(st.route);
                if (st.rtt_ms >= 0)
                    title += ", " + std::to_string(static_cast<int>(st.rtt_ms + 0.5)) + " ms";
                break;
            case net::LinkState::Reconnecting: title += "reconnecting..."; break;
            case net::LinkState::PeerAway: title += "your friend's connection dropped"; break;
            default: title += "your friend has gone"; break;
            }
            SDL_SetWindowTitle(window, title.c_str());
        }
#endif
    }
};

std::unique_ptr<OnlineRace> make_online_race(ui::OnlineSession session, const Settings& settings, host::Machine& machine,
                                             DrivingAids* driving, SDL_Window* window) {
    auto race = std::make_unique<OnlineRace>();
    race->session = std::move(session);
    race->window = window;
    race->pacer = std::make_unique<game::LinkPacer>(machine, *race->session.link);
    machine.attach_serial(race->pacer.get());
    const game::TwoPlayerSetup& setup = race->session.setup;
    if (driving)  // both games drive with the host's physics; lane centering is each player's own
        driving->driving.set_options(game::Driving::Options{setup.improved_driving, settings.lane_centering});
    race->start = std::make_unique<game::TwoPlayerStart>(
        machine, race->session.host ? game::TwoPlayerStart::Role::Host : game::TwoPlayerStart::Role::Guest, setup,
        game::TwoPlayerStart::Own{});
    race->start->on_log = [](const std::string& line) { SDL_Log("Online race: %s", line.c_str()); };
    SDL_Log("Online race: %s, course %d, %s driving", race->session.host ? "hosting" : "joined", setup.course,
            setup.improved_driving ? "improved" : "original");
    return race;
}

// Runs the hosted game until the window closes or VETTE.EXE exits.
// `smooth` (optional) draws the race view at the display's refresh rate (game/smooth.h), and `view`
// (optional, with `smooth` in world-layers mode) draws its world with the Enhanced renderer. `script`:
// the testing options (keys, screenshots, quit time).
void main_loop(Presenter& presenter, host::Machine& machine, AudioOut* audio, Gamepad& gamepad,
               game::SmoothRenderer* smooth, EnhancedView* view, GameSound* game_sound, Artwork* art,
               DrivingAids* driving, const Options& script, TwoPlayerLink* link) {
    std::size_t next_key = 0;
    std::size_t next_press = 0;
    std::size_t next_poke = 0;
    std::uint64_t paused_ns = 0;  // the key sheet's pauses: the testing options' clock goes on through them
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
    // The key sheet (Ctrl+H). The keys pressed while it's open stay out of the game, releases and all;
    // the releases of keys held from before go in, so none sticks. It pauses the game, except an online
    // race (the other game goes on).
    bool keys_open = false;
    std::array<bool, SDL_SCANCODE_COUNT> kept_out{};
    ui::Canvas sheet;
    const auto close_keys = [&] {
        keys_open = false;
        presenter.hide_overlay();
    };

    for (;;) {
        for (const std::uint64_t t = machine.emulated_ns() + paused_ns;
             next_press < script.presses.size() && script.presses[next_press].at_ns <= t; ++next_press)
            push_press(script.presses[next_press], presenter.window());
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            scancodes.clear();  // keyboard bytes this event produces, sent after the switch
            if (ui::quit_shortcut(event))
                continue;  // (the quit it posts comes next)
            if (ui::keys_shortcut(event)) {
                if (!event.key.repeat) {
                    if (keys_open)
                        close_keys();
                    else
                        keys_open = true;
                }
                kept_out[event.key.scancode] = true;
                continue;
            }
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
                const SDL_Scancode sc = event.key.scancode;
                if (!down && kept_out[sc]) {
                    kept_out[sc] = false;
                    break;
                }
                if (down && keys_open) {
                    if (!event.key.repeat) {
                        kept_out[sc] = true;
                        if (key == SDLK_ESCAPE || key == SDLK_RETURN || key == SDLK_KP_ENTER)
                            close_keys();
                    }
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
                if (keys_open && event.button.down)
                    break;
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
                if (keys_open && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    scancodes.clear();  // (its release goes in: a key the game never saw pressed)
                    if (event.gbutton.button == SDL_GAMEPAD_BUTTON_START || event.gbutton.button == SDL_GAMEPAD_BUTTON_BACK)
                        close_keys();
                }
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

        const std::uint64_t t = machine.emulated_ns() + paused_ns;
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
        const bool paused = keys_open && !link;
        if (paused)
            paused_ns += std::min(now - last, kMaxStepNs);
        else
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
        if (driving)
            driving->prepare(machine, view);
        if (link)
            link->update(machine, driving);
        // An invite link opened while a game runs (handed over by the new copy it started): leave and join?
        if (const auto invite = ui::take_forwarded_invite()) {
            SDL_RaiseWindow(presenter.window());
            const SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Join now"},
                                                        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Keep playing"}};
            const SDL_MessageBoxData box{SDL_MESSAGEBOX_INFORMATION, presenter.window(), kAppName,
                                         "A friend invited you to an online race. Leave this game and join it?", 2,
                                         buttons, nullptr};
            int chosen = 0;
            if (SDL_ShowMessageBox(&box, &chosen) && chosen == 1 && ui::leave_for_invite(*invite))
                return;  // this copy closes; the new one joins
            last = SDL_GetTicksNS();  // (the emulation doesn't catch up on the time the question took)
        }
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
        if (keys_open) {
            int out_w = 0;
            int out_h = 0;
            presenter.output_size(out_w, out_h);
            ui::draw_key_sheet(sheet, out_w, out_h, paused);
            presenter.show_overlay(sheet);
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
        const Settings saved = load_settings(settings_file);
        Settings settings = saved;
        opts->apply_to(settings);
        const Settings launched = settings;
        // The player's choices are kept for next time, whether they play or quit: what they changed in the
        // launch menu, and full screen or the window as they left it. This run's overrides aren't.
        const auto keep_settings = [&](const Presenter& presenter) {
            settings.fullscreen = presenter.fullscreen();
            if (!save_settings(settings_file, with_changes(saved, launched, settings)))
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Couldn't save %s", path_to_utf8(settings_file).c_str());
        };

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

        // Opened by an invite link while a game is running: that game takes it, and this copy is done.
        if (opts->invite && ui::online_available() && ui::forward_invite(*opts->invite)) {
            SDL_Log("Invite: handed to the game that's already running.");
            return 0;
        }
        ui::accept_forwarded_invites(argv[0]);

        Presenter presenter(kAppName);
        presenter.set_fullscreen(settings.fullscreen);
        presenter.set_smooth_scaling(settings.scaling == Settings::Scaling::Smooth);
        presenter.set_original_resolution(settings.view_resolution == Settings::ViewResolution::Original);
        presenter.set_depth_buffer(settings.depth_buffer);
        Gamepad gamepad;

        // An invite link: on macOS it comes as an event shortly after the start (SDL: a dropped "file").
        std::optional<std::string> invite = opts->invite;
#ifdef __APPLE__
        for (const std::uint64_t until = SDL_GetTicksNS() + 500'000'000; !invite && SDL_GetTicksNS() < until;) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_DROP_FILE && e.drop.data && is_invite_link(e.drop.data))
                    invite = std::string(e.drop.data);
                else if (e.type == SDL_EVENT_QUIT)
                    return 0;
            }
            SDL_Delay(10);
        }
#endif
        // The launch menu: when it's switched on, or to let the player find the game files. Opened with
        // an invite link, the game goes straight to joining that race (and to the menu if that's cancelled).
        ui::OnlineSession online;  // set when the player chose an online race
        if (invite && game && ui::online_available()) {
            SDL_Log("Invite: %s", invite->c_str());
            const bool quit =
                !ui::run_online(presenter, gamepad, settings, *game, online, *invite) && !online.link &&
                ui::run_launcher(presenter, gamepad, settings, game, search, &online, maps_dir()) == ui::LaunchChoice::Quit;
            keep_settings(presenter);
            if (quit)
                return 0;
        } else if (opts->launcher.value_or(settings.show_launcher) || !game) {
            const bool quit =
                ui::run_launcher(presenter, gamepad, settings, game, search, &online, maps_dir()) == ui::LaunchChoice::Quit;
            keep_settings(presenter);
            if (quit)
                return 0;
        }
        // An online race from the command line (testing): the room first, then the game.
        if (!online.link && (opts->online_host || opts->online_join)) {
            Settings run = settings;
            if (opts->online_server)
                run.online_server = *opts->online_server;
            std::string error;
            if (!ui::connect_online(run, *game, opts->online_host, opts->online_join.value_or(""), online, error))
                throw std::runtime_error("Online race: " + error);
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
        default_link_config(*game, config.save_dir);
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
        // A map of the player's own (ui/map_editor.h), except in two-player races: both need the same city.
        if (!settings.map_name.empty()) {
            if (online.link || opts->link_listen || opts->link_connect) {
                SDL_Log("Map: the original (two-player races use it)");
            } else if (const auto map = ui::load_map(maps_dir(), settings.map_name, error)) {
                game::install_city_map(machine, *map);
                SDL_Log("Map: %s", settings.map_name.c_str());
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Map %s: %s; the original instead",
                            settings.map_name.c_str(), error.c_str());
            }
        }
        // Without the freeways (after the map, which their roads go into); an online race as its host chose.
        const bool no_freeways = !(online.link ? online.setup.freeways : settings.freeways);
        if (no_freeways) {
            game::install_no_freeways(machine);
            SDL_Log("Freeways: off (one connected city)");
        }
        SDL_Log("Saves: %s", path_to_utf8(config.save_dir).c_str());
        if (!settings.manual_check || online.link)  // (an online race's menus are driven: no question then)
            game::install_skip_manual_check(machine.cpu());
        game::install_idle_skip(machine);  // the fast PC spends most cycles waiting for retrace
        if (settings.smooth_traffic)
            game::install_far_freeway_spawns(machine.cpu());
        const bool smooth_fps = settings.frame_rate == Settings::FrameRate::Smooth;
        const bool enhanced_view = settings.draw_distance != Settings::DrawDistance::Original;
        std::optional<game::SmoothRenderer> smooth;
        std::unique_ptr<EnhancedView> view;
        if (smooth_fps || enhanced_view) {
            smooth.emplace(machine, enhanced_view);
            smooth->set_interpolation(smooth_fps);
            if (no_freeways)
                game::install_no_freeway_drawing(smooth->replay_cpu());
        }
        if (enhanced_view) {
            view = std::make_unique<EnhancedView>();
            if (settings.draw_distance == Settings::DrawDistance::Extended)
                view->radius = kExtendedRadius;
            view->hills = settings.skyline == Settings::Skyline::Hills;
            view->smooth_traffic = settings.smooth_traffic;
            view->no_freeways = no_freeways;
        }
        SDL_Log("Frame rate: %s; draw distance: %s%s%s; emulated CPU %.0f MHz",
                smooth_fps ? "smooth (display refresh)" : "original",
                !enhanced_view ? "original" : view->radius == kExtendedRadius ? "extended" : "maximum",
                !enhanced_view ? "" : view->hills ? ", hills skyline" : ", painted skyline",
                !enhanced_view ? "" : presenter.depth_buffer() ? ", depth buffer" : ", original order",
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

        // Driving: the original's physics, or Improved Driving and Lane Centering layered on them.
        std::unique_ptr<DrivingAids> driving;
        // (With a two-player link, the guest may take the host's physics.)
        const bool two_player = opts->link_listen || opts->link_connect || online.link;
        if (settings.improved_driving || settings.lane_centering || two_player) {
            driving = std::make_unique<DrivingAids>(machine, game::Driving::Options{settings.improved_driving,
                                                                                    settings.lane_centering});
            if (game_sound) {
                game::SoundEvents& events = game_sound->audio->events();
                driving->driving.on_hard_landing = [&events](float) { events.report(game::Sfx::Thud); };
                events.thud_hold = [aids = driving.get()] { return aids->driving.flying(); };
            }
        }
        SDL_Log("Driving: %s physics; lane centering %s", settings.improved_driving ? "improved" : "original",
                settings.lane_centering ? "on" : "off");
        std::unique_ptr<TwoPlayerLink> link;
        if (online.link)
            link = make_online_race(std::move(online), settings, machine, driving.get(), presenter.window());
        else
            link = make_dev_link(*opts, settings, machine);
        main_loop(presenter, machine, audio ? &*audio : nullptr, gamepad, smooth ? &*smooth : nullptr, view.get(),
                  game_sound.get(), art.get(), driving.get(), *opts, link.get());
        keep_settings(presenter);  // (Alt+Enter in the game)
        if (art && art->frames)
            SDL_Log("Graphics: %.2f ms per frame to compose", art->compose_ms / static_cast<double>(art->frames));
        if (smooth && smooth->stats().replays)
            SDL_Log("Smooth: %llu game frames, %llu display frames, %.2f ms per replay",
                    static_cast<unsigned long long>(smooth->stats().game_frames),
                    static_cast<unsigned long long>(smooth->stats().replays),
                    smooth->stats().replay_ms / static_cast<double>(smooth->stats().replays));
        if (view && view->frames)
            SDL_Log("Enhanced view: %llu frames (%llu with the depth buffer), %.2f ms per scene, %.0f vehicles and "
                    "pedestrians drawn",
                    static_cast<unsigned long long>(view->frames),
                    static_cast<unsigned long long>(presenter.depth_buffer_frames()),
                    view->build_ms / static_cast<double>(view->frames),
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
