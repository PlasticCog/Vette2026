#pragma once
// The opponent screen's turning car at the display's resolution (the Enhanced view). The original draws
// it with the race renderer into a 216 x 99 box at 320 x 200 (3009:E074's loop: the box's background
// copied from the picture kept at A800h, then 3009:E44E draws the chosen opponent's model, then the pages
// flip). Each time it draws, the model's camera-space vertices are taken (draw_model, 3009:B765, once it
// has transformed them, B7DD) and its faces drawn from them as the original orders them, unclipped by its
// pixels: the picture under them is the frame with the box as the background picture has it.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "enhanced/scene.h"
#include "game/model_pack.h"

namespace vette::host {
class Machine;
}

namespace vette::enhanced {

class MenuCar {
public:
    // The box the game redraws each frame, in the frame's pixels.
    static constexpr int kBoxX = 104, kBoxY = 101, kBoxW = 216, kBoxH = 99;
    // Where the box's background is kept: video memory A800h, as a CRTC start address.
    static constexpr uint16_t kBackgroundStart = 0x8000;

    explicit MenuCar(host::Machine& machine);
    ~MenuCar();
    MenuCar(const MenuCar&) = delete;
    MenuCar& operator=(const MenuCar&) = delete;

    // The car as last drawn, in the frame's coordinates (viewport: the box), if the opponent screen is on
    // and has drawn one lately. `line_width`: the outlines' width in frame pixels. `smooth`: turning
    // between the last two drawings, one drawing behind (the Smooth frame rate), not in its steps.
    bool build(uint64_t now_ns, float line_width, bool smooth, Scene& out) const;

private:
    struct Capture;
    host::Machine& machine_;
    std::shared_ptr<Capture> capture_;
};

}  // namespace vette::enhanced
