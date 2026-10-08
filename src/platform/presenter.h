#pragma once

#include <array>
#include <cstdint>
#include <memory>
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
    ~Presenter();

    void present(const Framebuffer& fb);
    void present(const ui::Canvas& canvas);
    // The race view with the Enhanced 3D view: `under` (the game's view without its world), then
    // `scene` clipped to its viewport at the output's full resolution, then `inset` (the rear-view
    // mirror's scene, if any) clipped to its own viewport, then `over` wherever its pixels aren't
    // kTransparentPixel. All are in the frame's coordinates, placed as present() would. With the depth
    // buffer, the scenes' triangles hide each other by their SceneVertex::depth; else in their order.
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
    // Smooth: the 2D pictures' pixel edges softened ("sharp bilinear"); otherwise every pixel is a
    // solid block (nearest neighbour), as the original's graphics simply enlarged.
    void set_smooth_scaling(bool smooth) { smooth_ = smooth; }
    // On: the Enhanced 3D view (and its mirror) is drawn at the frame's own resolution (the original's
    // 320x200), then enlarged like the frame; frame_scale() is then 1.
    void set_original_resolution(bool on) { original_resolution_ = on; }
    bool original_resolution() const { return original_resolution_; }
    // The Enhanced 3D view drawn with a depth buffer on the GPU (SDL's GPU API: Direct3D 12, Vulkan or
    // Metal), when available and wanted.
    bool depth_buffer_available() const { return gpu_ != nullptr; }
    void set_depth_buffer(bool on) { depth_buffer_ = on; }
    bool depth_buffer() const { return depth_buffer_ && gpu_ != nullptr; }
    std::uint64_t depth_buffer_frames() const { return depth_frames_; }  // drawn with it so far
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
    // Drawn over every picture presented until hide_overlay() (the key sheet, the map editor's panels):
    // the picture darkened (if `dim`), then `canvas` (copied) at its scale, centered, without its pixels
    // of its background colour.
    void show_overlay(const ui::Canvas& canvas, bool dim = true);
    void hide_overlay() { overlay_on_ = false; }

    // A big picture kept on the GPU (the map editor's map, 0xRRGGBB pixels): set_picture uploads it whole,
    // update_picture the rectangle (x, y, w, h) that changed, from the same pixels. present_picture draws
    // `src` of it (picture pixels) into `dst` (output pixels; it may reach past the window), each pixel a
    // solid block, over `background`, with the marks, then the overlay.
    struct PictureMarks {
        float grid_step = 0;  // output pixels between grid lines, from dst's corner (0: none)
        uint32_t grid_colour = 0;
        struct Box {
            SDL_FRect rect;  // output pixels
            uint32_t colour;
            float thickness;
        };
        std::vector<Box> boxes;
    };
    // A 3D view in the window's own pixels (the object editor's): `scene`'s vertices are output pixels,
    // drawn over `background` (with the depth buffer when it's on, else in their order), then the overlay.
    void present_view(const enhanced::Scene& scene, std::uint32_t background);

    void set_picture(const std::vector<std::uint32_t>& pixels, int w, int h);
    void update_picture(const std::vector<std::uint32_t>& pixels, int x, int y, int w, int h);
    void present_picture(const SDL_FRect& src, const SDL_FRect& dst, std::uint32_t background, const PictureMarks& marks);

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
    SDL_Texture* enlarge(Layer& layer, bool transparency, const SDL_FRect& dst);
    void draw_scene(const Framebuffer& under, const enhanced::Scene& scene, const enhanced::Scene* inset,
                    const SDL_FRect& dst);
    void draw_scene_layers(SDL_Texture* under, int frame_w, int frame_h, const enhanced::Scene& scene,
                           const enhanced::Scene* inset, const SDL_FRect& dst);
    void draw_triangles(const enhanced::Scene& scene, int frame_w, int frame_h, const SDL_FRect& dst);
    struct Gpu;
    SDL_Texture* draw_depth_tested(const enhanced::Scene& scene, const enhanced::Scene* inset, int frame_w,
                                   int frame_h, int w, int h);
    void draw_composite(const graphics::Composite& c, const SDL_FRect& dst);
    SDL_Texture* art_texture(const graphics::Image& image);
    SDL_FRect fit() const;  // the 4:3 picture rect in render output pixels
    void finish_frame();    // SDL_RenderPresent, after a requested screenshot

    // Destroyed in reverse order: textures, then renderer, then window.
    SdlPtr<SDL_Window> window_;
    SdlPtr<SDL_Renderer> renderer_;
    std::unique_ptr<Gpu> gpu_;  // the depth-buffer pass, on the renderer's GPU device
    Layer base_;  // present()'s frame, and the layered view's `under`
    Layer over_;  // the layered view's `over`
    Layer art_base_, art_over_, art_moved_;  // a Composite's DOS pixels
    Layer low_under_, low_scene_;  // set_original_resolution(): `under`, and the view drawn at the frame's size
    std::unordered_map<const graphics::Image*, SdlPtr<SDL_Texture>> art_;  // its art, by image
    SdlPtr<SDL_Texture> canvas_;   // the last Canvas, at its own size
    SdlPtr<SDL_Texture> overlay_;  // show_overlay()'s canvas
    int overlay_w_ = 0, overlay_h_ = 0, overlay_scale_ = 1;
    bool overlay_on_ = false;
    bool overlay_dim_ = true;
    SdlPtr<SDL_Texture> picture_tex_;  // set_picture()
    int picture_tex_w_ = 0, picture_tex_h_ = 0;
    std::vector<float> scene_xy_;  // the scene's vertices in render output pixels
    std::string screenshot_;       // request_screenshot()
    int canvas_w_ = 0;
    int canvas_h_ = 0;
    SDL_FRect picture_{};  // where the last frame or canvas went, in render output pixels
    int picture_w_ = 0;    // and its size in its own pixels (for window_to_frame)
    int picture_h_ = 0;
    bool system_cursor_shown_ = true;
    bool smooth_ = false;
    bool original_resolution_ = false;
    bool depth_buffer_ = true;
    std::uint64_t depth_frames_ = 0;
};

}  // namespace vette
