#include "assets/planar.h"
#include "assets/rle.h"
#include "core/game_dir.h"
#include "core/path_utf8.h"
#include "platform/framebuffer.h"
#include "platform/presenter.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>  // UTF-8 argv on Windows

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace vette {
namespace {

constexpr const char* kAppName = "VETTE! 2026";

constexpr const char* kUsage =
    "Usage: vette2026 [--game <dir>] [--dump-frame <file.bmp>]\n"
    "  --game <dir>         folder with the DOS VETTE! files (default: search for Game/)\n"
    "  --dump-frame <file>  write the title screen to a 640x200 BMP and exit, without a window\n";

// Timer rate. VETTE.EXE reprograms PIT channel 0 with divisor 0x1000 (~291.27 Hz); every IRQ0 bumps
// the tick counter its variable-timestep frame loop reads. See re/notes/01-startup-and-timing.md.
constexpr std::uint64_t kPitClockHz = 1193182;
constexpr std::uint64_t kPitDivisor = 0x1000;

constexpr std::uint64_t kNsPerSecond = 1'000'000'000;
// Longest wall-clock step fed to the simulation, so it doesn't race to catch up after a stall
// (debugger break, window drag).
constexpr std::uint64_t kMaxStepNs = kNsPerSecond / 4;

struct Options {
    std::optional<std::filesystem::path> game_dir;
    std::optional<std::string> dump_frame;  // UTF-8 path
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
        } else if (arg == "--game" && has_value) {
            opts.game_dir = path_from_utf8(argv[++i]);
        } else if (arg == "--dump-frame" && has_value) {
            opts.dump_frame = argv[++i];
        } else {
            const bool needs_value = arg == "--game" || arg == "--dump-frame";
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

void tick() {
    // One PIT interrupt period. Phase 1 runs the hosted VETTE.EXE here up to its next IRQ0.
}

void main_loop(Presenter& presenter, const Framebuffer& fb) {
    // Accumulate elapsed time in units of ns * kPitClockHz, so ticks follow the PIT rate exactly.
    constexpr std::uint64_t kTickCost = kPitDivisor * kNsPerSecond;
    std::uint64_t accumulator = 0;
    std::uint64_t last = SDL_GetTicksNS();

    for (;;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT)
                return;
            if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat)
                continue;
            const SDL_Keycode key = event.key.key;
            const bool alt_enter = (key == SDLK_RETURN || key == SDLK_KP_ENTER) && (event.key.mod & SDL_KMOD_ALT);
            if (key == SDLK_ESCAPE)
                return;
            if (key == SDLK_F11 || alt_enter)
                presenter.toggle_fullscreen();
        }

        const std::uint64_t now = SDL_GetTicksNS();
        accumulator += std::min(now - last, kMaxStepNs) * kPitClockHz;
        last = now;
        while (accumulator >= kTickCost) {
            tick();
            accumulator -= kTickCost;
        }

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

        Framebuffer fb;
        load_title(game, fb);

        if (headless) {
            save_bmp(fb, *opts->dump_frame);
            SDL_Log("Wrote %s", opts->dump_frame->c_str());
            return 0;
        }

        if (!SDL_Init(SDL_INIT_VIDEO))
            throw_sdl_error("SDL_Init");
        Presenter presenter(kAppName);
        main_loop(presenter, fb);
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
