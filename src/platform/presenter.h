#pragma once

#include "platform/framebuffer.h"
#include "platform/sdl_util.h"
#include "ui/canvas.h"

namespace vette {

// Owns the window. Shows the game's Framebuffer 4:3, letterboxed, "sharp bilinear" scaled; or one of
// the app's own screens (a ui::Canvas) with square pixels at an integer scale.
// Requires SDL_INIT_VIDEO. Throws std::runtime_error if the window or renderer can't be created.
class Presenter {
public:
    explicit Presenter(const char* title);

    void present(const Framebuffer& fb);
    void present(const ui::Canvas& canvas);
    // The window's drawable size in pixels (what a Canvas should be laid out for).
    void output_size(int& w, int& h) const;
    SDL_Window* window() const { return window_.get(); }
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
    SdlPtr<SDL_Texture> canvas_;  // the last Canvas, at its own size
    int frame_w_ = 0;
    int frame_h_ = 0;
    int canvas_w_ = 0;
    int canvas_h_ = 0;
    SDL_FRect picture_{};  // where the last frame or canvas went, in render output pixels
    int picture_w_ = 0;    // and its size in its own pixels (for window_to_frame)
    int picture_h_ = 0;
    int scale_x_ = 0;
    int scale_y_ = 0;
    bool system_cursor_shown_ = true;
};

}  // namespace vette
