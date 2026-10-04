#pragma once

#include "platform/framebuffer.h"
#include "platform/sdl_util.h"

namespace vette {

// Owns the window and shows a Framebuffer in it: 4:3, letterboxed, "sharp bilinear" scaled.
// Requires SDL_INIT_VIDEO. Throws std::runtime_error if the window or renderer can't be created.
class Presenter {
public:
    explicit Presenter(const char* title);

    void present(const Framebuffer& fb);
    void toggle_fullscreen();
    void set_fullscreen(bool on);
    bool fullscreen() const;
    // Maps a window position (mouse event coordinates) to a pixel of the last presented frame.
    // Returns false when it lies outside the picture.
    bool window_to_frame(float wx, float wy, int& fx, int& fy) const;
    // False while minimized or hidden, when VSync can't be relied on to pace the loop.
    bool visible() const;
    // Shows or hides the OS mouse pointer while it's over the window (hidden while the game's own
    // pointer is on screen). Outside the window it is unaffected.
    void show_system_cursor(bool show);

private:
    // Destroyed in reverse order: textures, then renderer, then window.
    SdlPtr<SDL_Window> window_;
    SdlPtr<SDL_Renderer> renderer_;
    SdlPtr<SDL_Texture> frame_;   // Framebuffer converted to ARGB, at its size (frame_w_ x frame_h_)
    SdlPtr<SDL_Texture> scaled_;  // frame_ upscaled by (scale_x_, scale_y_), nearest-neighbor
    int frame_w_ = 0;
    int frame_h_ = 0;
    SDL_FRect picture_{};  // where the last frame went, in render output pixels
    int scale_x_ = 0;
    int scale_y_ = 0;
    bool system_cursor_shown_ = true;
};

}  // namespace vette
