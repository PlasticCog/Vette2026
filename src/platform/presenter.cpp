#include "platform/presenter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "enhanced/scene.h"
#include "ui/app_icon.h"

namespace vette {
namespace {

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 960;

SdlPtr<SDL_Texture> create_texture(SDL_Renderer* renderer, SDL_TextureAccess access, int w, int h,
                                   SDL_ScaleMode scale_mode) {
    SdlPtr<SDL_Texture> texture{SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, access, w, h)};
    if (!texture)
        throw_sdl_error("SDL_CreateTexture");
    SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(texture.get(), scale_mode);
    return texture;
}

// Windows takes the icon from the exe's resources, in every size it has; elsewhere the window gets it here.
void set_window_icon(SDL_Window* window) {
#ifndef SDL_PLATFORM_WINDOWS
    constexpr int kSize = 128;
    std::vector<std::uint32_t> pixels = ui::app_icon(kSize);
    SdlPtr<SDL_Surface> icon{SDL_CreateSurfaceFrom(kSize, kSize, SDL_PIXELFORMAT_ARGB8888, pixels.data(), kSize * 4)};
    if (!icon || !SDL_SetWindowIcon(window, icon.get()))
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "No window icon: %s", SDL_GetError());
#else
    (void)window;
#endif
}

}  // namespace

Presenter::Presenter(const char* title) {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer(title, kWindowWidth, kWindowHeight,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &window, &renderer))
        throw_sdl_error("SDL_CreateWindowAndRenderer");
    window_.reset(window);
    renderer_.reset(renderer);
    set_window_icon(window);

    if (!SDL_SetRenderVSync(renderer, 1))
        SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "VSync unavailable: %s", SDL_GetError());
    SDL_Log("Renderer: %s", SDL_GetRendererName(renderer));
}

// Largest 4:3 rect that fits the output, centered: both EGA modes filled a 4:3 CRT.
SDL_FRect Presenter::fit() const {
    int out_w = 0;
    int out_h = 0;
    SDL_GetCurrentRenderOutputSize(renderer_.get(), &out_w, &out_h);
    const int w = std::min(out_w, out_h * 4 / 3);
    const int h = std::min(out_h, out_w * 3 / 4);
    return {static_cast<float>(out_w - w) / 2, static_cast<float>(out_h - h) / 2, static_cast<float>(w),
            static_cast<float>(h)};
}

void Presenter::frame_scale(int frame_w, int frame_h, float& sx, float& sy) const {
    const SDL_FRect dst = fit();
    sx = dst.w / static_cast<float>(frame_w);
    sy = dst.h / static_cast<float>(frame_h);
}

// Sharp bilinear: nearest-neighbor upscale by whole factors close to the output size, then a linear
// filter for the remaining non-integer stretch. Pixels stay crisp, and only the edges between them
// blend, so the uneven pixel aspect (2.4:1 at 640x200, 1.2:1 at 320x200) doesn't produce rows of
// visibly different heights. With `transparency`, kTransparentPixel pixels are see-through
// (premultiplied alpha, so the linear filter doesn't darken the edges).
SDL_Texture* Presenter::upload(Layer& layer, const Framebuffer& fb, bool transparency, const SDL_FRect& dst) {
    SDL_Renderer* renderer = renderer_.get();
    if (fb.width != layer.w || fb.height != layer.h) {  // the game switched video modes
        layer.frame = create_texture(renderer, SDL_TEXTUREACCESS_STREAMING, fb.width, fb.height, SDL_SCALEMODE_NEAREST);
        layer.w = fb.width;
        layer.h = fb.height;
        layer.scale_x = layer.scale_y = 0;
    }

    std::array<std::uint32_t, 16> argb{};
    for (std::size_t i = 0; i < argb.size(); ++i) {
        const Rgb c = fb.palette[i];
        argb[i] = 0xFF000000u | std::uint32_t{c.r} << 16 | std::uint32_t{c.g} << 8 | c.b;
    }
    void* pixels = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(layer.frame.get(), nullptr, &pixels, &pitch))
        throw_sdl_error("SDL_LockTexture");
    for (int y = 0; y < fb.height; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(pixels) + y * pitch);
        const std::uint8_t* src = fb.pixels.data() + y * fb.width;
        for (int x = 0; x < fb.width; ++x)
            row[x] = transparency && src[x] == kTransparentPixel ? 0 : argb[src[x] & 0x0F];
    }
    SDL_UnlockTexture(layer.frame.get());

    const int ix = std::max(1, static_cast<int>(dst.w) / fb.width);
    const int iy = std::max(1, static_cast<int>(dst.h) / fb.height);
    if (ix != layer.scale_x || iy != layer.scale_y) {
        layer.scaled = create_texture(renderer, SDL_TEXTUREACCESS_TARGET, fb.width * ix, fb.height * iy,
                                      SDL_SCALEMODE_LINEAR);
        if (transparency)
            SDL_SetTextureBlendMode(layer.scaled.get(), SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        layer.scale_x = ix;
        layer.scale_y = iy;
    }
    SDL_SetRenderTarget(renderer, layer.scaled.get());
    SDL_RenderTexture(renderer, layer.frame.get(), nullptr, nullptr);
    SDL_SetRenderTarget(renderer, nullptr);
    return layer.scaled.get();
}

void Presenter::present(const Framebuffer& fb) {
    SDL_Renderer* renderer = renderer_.get();
    const SDL_FRect dst = fit();
    SDL_Texture* picture = upload(base_, fb, false, dst);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, picture, nullptr, &dst);
    finish_frame();
    picture_ = dst;
    picture_w_ = fb.width;
    picture_h_ = fb.height;
}

void Presenter::present(const Framebuffer& under, const enhanced::Scene& scene, const Framebuffer& over) {
    SDL_Renderer* renderer = renderer_.get();
    const SDL_FRect dst = fit();
    SDL_Texture* below = upload(base_, under, false, dst);
    SDL_Texture* above = upload(over_, over, true, dst);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, below, nullptr, &dst);

    // The scene's triangles, from frame coordinates to output pixels, clipped to the 3D viewport.
    if (!scene.indices.empty()) {
        const float sx = dst.w / static_cast<float>(under.width);
        const float sy = dst.h / static_cast<float>(under.height);
        scene_xy_.resize(scene.vertices.size() * 2);
        for (std::size_t i = 0; i < scene.vertices.size(); ++i) {
            scene_xy_[2 * i] = dst.x + scene.vertices[i].x * sx;
            scene_xy_[2 * i + 1] = dst.y + scene.vertices[i].y * sy;
        }
        const auto edge = [](float v) { return static_cast<int>(std::lround(v)); };
        const int x0 = edge(dst.x + static_cast<float>(scene.view_x0) * sx);
        const int y0 = edge(dst.y + static_cast<float>(scene.view_y0) * sy);
        const SDL_Rect clip{x0, y0, edge(dst.x + static_cast<float>(scene.view_x1) * sx) - x0,
                            edge(dst.y + static_cast<float>(scene.view_y1) * sy) - y0};
        SDL_SetRenderClipRect(renderer, &clip);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);  // screen-door faces are translucent
        SDL_RenderGeometryRaw(renderer, nullptr, scene_xy_.data(), static_cast<int>(2 * sizeof(float)),
                              reinterpret_cast<const SDL_FColor*>(&scene.vertices[0].r),
                              static_cast<int>(sizeof(enhanced::SceneVertex)), nullptr, 0,
                              static_cast<int>(scene.vertices.size()), scene.indices.data(),
                              static_cast<int>(scene.indices.size()), static_cast<int>(sizeof(std::int32_t)));
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderClipRect(renderer, nullptr);
    }

    SDL_RenderTexture(renderer, above, nullptr, &dst);
    finish_frame();
    picture_ = dst;
    picture_w_ = under.width;
    picture_h_ = under.height;
}

void Presenter::present(const ui::Canvas& canvas) {
    SDL_Renderer* renderer = renderer_.get();
    if (canvas.width != canvas_w_ || canvas.height != canvas_h_) {
        canvas_ = create_texture(renderer, SDL_TEXTUREACCESS_STREAMING, canvas.width, canvas.height,
                                 SDL_SCALEMODE_NEAREST);
        canvas_w_ = canvas.width;
        canvas_h_ = canvas.height;
    }
    void* pixels = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(canvas_.get(), nullptr, &pixels, &pitch))
        throw_sdl_error("SDL_LockTexture");
    for (int y = 0; y < canvas.height; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(pixels) + y * pitch);
        const std::uint32_t* src = canvas.pixels.data() + y * canvas.width;
        for (int x = 0; x < canvas.width; ++x)
            row[x] = 0xFF000000u | src[x];
    }
    SDL_UnlockTexture(canvas_.get());

    int out_w = 0;
    int out_h = 0;
    SDL_GetCurrentRenderOutputSize(renderer, &out_w, &out_h);
    const int w = canvas.width * canvas.scale;
    const int h = canvas.height * canvas.scale;
    const SDL_FRect dst{static_cast<float>((out_w - w) / 2), static_cast<float>((out_h - h) / 2), static_cast<float>(w),
                        static_cast<float>(h)};
    SDL_SetRenderDrawColor(renderer, static_cast<std::uint8_t>(canvas.background >> 16),
                           static_cast<std::uint8_t>(canvas.background >> 8), static_cast<std::uint8_t>(canvas.background),
                           SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, canvas_.get(), nullptr, &dst);
    finish_frame();
    picture_ = dst;
    picture_w_ = canvas.width;
    picture_h_ = canvas.height;
}

void Presenter::finish_frame() {
    SDL_Renderer* renderer = renderer_.get();
    if (!screenshot_.empty()) {
        SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr);
        if (!shot || !SDL_SaveBMP(shot, screenshot_.c_str()))
            SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "Screenshot %s: %s", screenshot_.c_str(), SDL_GetError());
        SDL_DestroySurface(shot);
        screenshot_.clear();
    }
    SDL_RenderPresent(renderer);
}

void Presenter::output_size(int& w, int& h) const { SDL_GetCurrentRenderOutputSize(renderer_.get(), &w, &h); }

void Presenter::toggle_fullscreen() { set_fullscreen(!fullscreen()); }

void Presenter::set_fullscreen(bool on) { SDL_SetWindowFullscreen(window_.get(), on); }

bool Presenter::fullscreen() const { return (SDL_GetWindowFlags(window_.get()) & SDL_WINDOW_FULLSCREEN) != 0; }

bool Presenter::window_to_frame(float wx, float wy, int& fx, int& fy) const {
    float rx = 0;
    float ry = 0;
    if (picture_w_ == 0 || picture_.w <= 0 || picture_.h <= 0 ||
        !SDL_RenderCoordinatesFromWindow(renderer_.get(), wx, wy, &rx, &ry))
        return false;
    const float u = (rx - picture_.x) / picture_.w;
    const float v = (ry - picture_.y) / picture_.h;
    if (u < 0 || u >= 1 || v < 0 || v >= 1)
        return false;
    fx = static_cast<int>(u * static_cast<float>(picture_w_));
    fy = static_cast<int>(v * static_cast<float>(picture_h_));
    return true;
}

bool Presenter::visible() const {
    return (SDL_GetWindowFlags(window_.get()) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED)) == 0;
}

// SDL's cursor visibility only applies over SDL's own windows, and this app has just the one.
void Presenter::show_system_cursor(bool show) {
    if (show != system_cursor_shown_ && (show ? SDL_ShowCursor() : SDL_HideCursor()))
        system_cursor_shown_ = show;
}

}  // namespace vette
