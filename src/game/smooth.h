#pragma once
// Smooth frame rate for the race view: the display updates at the monitor's refresh rate while the
// game's simulation keeps its own cadence, untouched.
//
// Each game frame is captured just before the original draws its 3D world (3009:0356). For every
// display frame, the original's own 3D drawing code (3009:02DA-0373: camera matrix, sky and
// horizon, the cell window) runs again on a throwaway copy of the latest capture. The camera and the
// moving vehicles and pedestrians are interpolated between the last two game frames (one game frame
// of delay), and the traffic simulation calls inside that section are skipped. The replay never
// touches the real machine, so the drawing pass's simulation side effects (traffic cell binding,
// collision candidates) happen exactly once, in the real game.
//
// Whatever the game drew over its 3D view afterwards (rear-view mirror, messages, crash pictures) is
// carried over from the displayed frame. Those pixels are found by comparing the displayed page with
// the original's own 3D image of that page (captured at 3009:0374).

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "host/ega.h"
#include "host/machine.h"

namespace vette::game {

class SmoothRenderer {
public:
    // With `world_layers`, the replays stop short of the original's world drawing and render_layers()
    // hands over what an Enhanced renderer needs to draw the world itself; render() and self_check()
    // are then unavailable.
    explicit SmoothRenderer(host::Machine& machine, bool world_layers = false);
    ~SmoothRenderer();
    SmoothRenderer(const SmoothRenderer&) = delete;
    SmoothRenderer& operator=(const SmoothRenderer&) = delete;

    // Builds the frame to display at emulated time `now_ns` (normally Machine::emulated_ns()) and
    // returns true. Returns false, leaving `out` alone, when the 3D race view isn't on screen (menus,
    // highway mode, a stalled game), so the caller shows the game's own frame.
    bool render(uint64_t now_ns, host::Ega::Frame& out);

    // The race view in three layers, for an Enhanced renderer (world_layers only): `under`, then the
    // world drawn from `ram`, then `over`. Returns false, like render(), when the race view isn't on
    // screen.
    static constexpr uint8_t kTransparent = 0xFF;
    struct Layers {
        host::Ega::Frame under;  // the replayed view without its world: sky, ground, horizon
        host::Ega::Frame over;   // the displayed frame, kTransparent where the 3D view shows through
        std::vector<uint8_t> ram = std::vector<uint8_t>(host::Memory::kSize);  // at draw_world_cells' entry
    };
    bool render_layers(uint64_t now_ns, Layers& out);

    // Off: always show the latest game frame as it is (the Original frame rate), no blending.
    void set_interpolation(bool on) { interpolation_ = on; }

    // Diagnostics: replays the latest game frame without interpolation and compares its 3D view
    // with the original's own drawing of that frame. Returns the number of differing pixels, or -1
    // when no frame is available yet.
    int self_check();

    struct Stats {
        uint64_t game_frames = 0;  // game frames captured
        uint64_t replays = 0;      // replays run (render + self_check)
        double replay_ms = 0;      // total wall-clock time spent replaying
    };
    const Stats& stats() const { return stats_; }

private:
    struct Snapshot;
    class ScratchIo;

    void capture_frame();       // watch at 3009:0356
    void capture_pure_image();  // watch at 3009:0374
    int shown_page();           // render the displayed frame into shown_; its page (0/1), or -1
    double blend_alpha(uint64_t now_ns) const;
    bool replay(double alpha, host::Ega::Frame& out);
    void interpolate(double alpha);

    host::Machine& machine_;
    const bool world_layers_;
    bool interpolation_ = true;
    std::vector<uint8_t>* world_ram_ = nullptr;  // where the replay's world hook copies the memory to
    host::Cpu::WatchId capture_watch_ = 0, pure_watch_ = 0;
    std::unique_ptr<Snapshot> prev_, cur_;
    struct PureImage {
        bool valid = false;
        uint64_t frame = 0;  // game frame number it belongs to
        host::Ega::Frame image;
    };
    std::array<PureImage, 2> pure_;  // per page (CRTC start 0 / 2000h)

    // The scratch machine the replays run on.
    host::Memory scratch_mem_;
    host::Ega scratch_ega_;
    std::unique_ptr<ScratchIo> scratch_io_;
    std::unique_ptr<host::Cpu> scratch_cpu_;
    bool scratch_loaded_ = false;  // scratch RAM holds cur_'s RAM
    bool broken_ = false;          // a replay failed to finish; stop trying
    host::Ega::Frame replay_image_, shown_;
    Stats stats_;
};

} // namespace vette::game
