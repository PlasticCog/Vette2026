#include "platform/presenter.h"

#include <algorithm>
#include <array>
#include <cstdint>

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

}  // namespace

Presenter::Presenter(const char* title) {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer(title, kWindowWidth, kWindowHeight,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &window, &renderer))
        throw_sdl_error("SDL_CreateWindowAndRenderer");
    window_.reset(window);
    renderer_.reset(renderer);

    if (!SDL_SetRenderVSync(renderer, 1))
        SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "VSync unavailable: %s", SDL_GetError());
    SDL_Log("Renderer: %s", SDL_GetRendererName(renderer));
}

// Sharp bilinear: nearest-neighbor upscale by whole factors close to the output size, then a linear
// filter for the remaining non-integer stretch. Pixels stay crisp, and only the edges between them
// blend, so the uneven pixel aspect (2.4:1 at 640x200, 1.2:1 at 320x200) doesn't produce rows of
// visibly different heights.
void Presenter::present(const Framebuffer& fb) {
    SDL_Renderer* renderer = renderer_.get();
    if (fb.width != frame_w_ || fb.height != frame_h_) {  // the game switched video modes
        frame_ = create_texture(renderer, SDL_TEXTUREACCESS_STREAMING, fb.width, fb.height, SDL_SCALEMODE_NEAREST);
        frame_w_ = fb.width;
        frame_h_ = fb.height;
        scale_x_ = scale_y_ = 0;
    }

    std::array<std::uint32_t, 16> argb{};
    for (std::size_t i = 0; i < argb.size(); ++i) {
        const Rgb c = fb.palette[i];
        argb[i] = 0xFF000000u | std::uint32_t{c.r} << 16 | std::uint32_t{c.g} << 8 | c.b;
    }
    void* pixels = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(frame_.get(), nullptr, &pixels, &pitch))
        throw_sdl_error("SDL_LockTexture");
    for (int y = 0; y < fb.height; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(pixels) + y * pitch);
        const std::uint8_t* src = fb.pixels.data() + y * fb.width;
        for (int x = 0; x < fb.width; ++x)
            row[x] = argb[src[x] & 0x0F];
    }
    SDL_UnlockTexture(frame_.get());

    // Largest 4:3 rect that fits the output, centered: both EGA modes filled a 4:3 CRT.
    int out_w = 0;
    int out_h = 0;
    SDL_GetCurrentRenderOutputSize(renderer, &out_w, &out_h);
    const int w = std::min(out_w, out_h * 4 / 3);
    const int h = std::min(out_h, out_w * 3 / 4);
    const SDL_FRect dst{static_cast<float>(out_w - w) / 2, static_cast<float>(out_h - h) / 2,
                        static_cast<float>(w), static_cast<float>(h)};

    const int ix = std::max(1, w / fb.width);
    const int iy = std::max(1, h / fb.height);
    if (ix != scale_x_ || iy != scale_y_) {
        scaled_ = create_texture(renderer, SDL_TEXTUREACCESS_TARGET, fb.width * ix, fb.height * iy,
                                 SDL_SCALEMODE_LINEAR);
        scale_x_ = ix;
        scale_y_ = iy;
    }
    SDL_SetRenderTarget(renderer, scaled_.get());
    SDL_RenderTexture(renderer, frame_.get(), nullptr, nullptr);
    SDL_SetRenderTarget(renderer, nullptr);

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, scaled_.get(), nullptr, &dst);
    SDL_RenderPresent(renderer);
    picture_ = dst;
}

void Presenter::toggle_fullscreen() { set_fullscreen(!fullscreen()); }

void Presenter::set_fullscreen(bool on) { SDL_SetWindowFullscreen(window_.get(), on); }

bool Presenter::fullscreen() const { return (SDL_GetWindowFlags(window_.get()) & SDL_WINDOW_FULLSCREEN) != 0; }

bool Presenter::window_to_frame(float wx, float wy, int& fx, int& fy) const {
    float rx = 0;
    float ry = 0;
    if (frame_w_ == 0 || picture_.w <= 0 || picture_.h <= 0 ||
        !SDL_RenderCoordinatesFromWindow(renderer_.get(), wx, wy, &rx, &ry))
        return false;
    const float u = (rx - picture_.x) / picture_.w;
    const float v = (ry - picture_.y) / picture_.h;
    if (u < 0 || u >= 1 || v < 0 || v >= 1)
        return false;
    fx = static_cast<int>(u * static_cast<float>(frame_w_));
    fy = static_cast<int>(v * static_cast<float>(frame_h_));
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
