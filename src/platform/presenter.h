#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "platform/framebuffer.h"
#include "platform/sdl_util.h"
#include "ui/canvas.h"

namespace vette::enhanced {
struct Scene;
}
namespace vette::graphics {
struct Composite;
struct Image;
}

namespace vette {

// Owns the window. Shows the game's Framebuffer 4:3, letterboxed, "sharp bilinear" scaled; or one of
// the app's own screens (a ui::Canvas) with square pixels at an integer scale.
// Requires SDL_INIT_VIDEO. Throws std::runtime_error if the window or renderer can't be created.
class Presenter {
public:
    explicit Presenter(const char* title);

    void present(const Framebuffer& fb);
    void present(const ui::Canvas& canvas);
    // The race view with the Enhanced 3D view: `under` (the game's view without its world), then
    // `scene` clipped to its viewport at the output's full resolution, then `inset` (the rear-view
    // mirror's scene, if any) clipped to its own viewport, then `over` wherever its pixels aren't
    // kTransparentPixel. All are in the frame's coordinates, placed as present() would.
    static constexpr std::uint8_t kTransparentPixel = 0xFF;
    void present(const Framebuffer& under, const enhanced::Scene& scene, const Framebuffer& over,
                 const enhanced::Scene* inset = nullptr);
    // A frame with the PC-98's or the Mac's art in place of the DOS pictures (graphics/composite.h):
    // the backdrop or the DOS pixels under the art, the art at the output's resolution, then the DOS
    // pixels kept on top and the pieces moved into the art's layout. With `under` and `scene`, the
    // Enhanced 3D view is drawn first and the composite (made from its `over`) on top of it.
    void present(const graphics::Composite& composite);
    void present(const Framebuffer& under, const enhanced::Scene& scene, const graphics::Composite& composite,
                 const enhanced::Scene* inset = nullptr);
    // Output pixels per frame pixel, horizontally and vertically, for a frame of this size.
    void frame_scale(int frame_w, int frame_h, float& sx, float& sy) const;
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
    // Saves the next presented picture, as the window shows it, to a BMP file (UTF-8 path).
    void request_screenshot(std::string path_utf8) { screenshot_ = std::move(path_utf8); }

private:
    // A Framebuffer on its way to the screen: converted to ARGB at its own size, then upscaled by whole
    // factors (nearest-neighbor) for the final linear stretch.
    struct Layer {
        SdlPtr<SDL_Texture> frame;
        SdlPtr<SDL_Texture> scaled;
        int w = 0, h = 0;              // frame's size
        int scale_x = 0, scale_y = 0;  // scaled's factors
    };
    SDL_Texture* upload(Layer& layer, const Framebuffer& fb, bool transparency, const SDL_FRect& dst);
    SDL_Texture* upload(Layer& layer, const std::uint8_t* pixels, int w, int h,
                        const std::array<std::uint32_t, 16>& argb, bool transparency, const SDL_FRect& dst);
    void draw_scene(const Framebuffer& under, const enhanced::Scene& scene, const enhanced::Scene* inset,
                    const SDL_FRect& dst);
    void draw_triangles(const enhanced::Scene& scene, int frame_w, int frame_h, const SDL_FRect& dst);
    void draw_composite(const graphics::Composite& c, const SDL_FRect& dst);
    SDL_Texture* art_texture(const graphics::Image& image);
    SDL_FRect fit() const;  // the 4:3 picture rect in render output pixels
    void finish_frame();    // SDL_RenderPresent, after a requested screenshot

    // Destroyed in reverse order: textures, then renderer, then window.
    SdlPtr<SDL_Window> window_;
    SdlPtr<SDL_Renderer> renderer_;
    Layer base_;  // present()'s frame, and the layered view's `under`
    Layer over_;  // the layered view's `over`
    Layer art_base_, art_over_, art_moved_;  // a Composite's DOS pixels
    std::unordered_map<const graphics::Image*, SdlPtr<SDL_Texture>> art_;  // its art, by image
    SdlPtr<SDL_Texture> canvas_;   // the last Canvas, at its own size
    std::vector<float> scene_xy_;  // the scene's vertices in render output pixels
    std::string screenshot_;       // request_screenshot()
    int canvas_w_ = 0;
    int canvas_h_ = 0;
    SDL_FRect picture_{};  // where the last frame or canvas went, in render output pixels
    int picture_w_ = 0;    // and its size in its own pixels (for window_to_frame)
    int picture_h_ = 0;
    bool system_cursor_shown_ = true;
};

}  // namespace vette
