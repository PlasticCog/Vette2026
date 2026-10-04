#include "platform/framebuffer.h"

#include "platform/sdl_util.h"

#include <cstring>

namespace vette {

void save_bmp(const Framebuffer& fb, const std::string& path_utf8) {
    const SdlPtr<SDL_Surface> surface{SDL_CreateSurface(fb.width, fb.height, SDL_PIXELFORMAT_INDEX8)};
    if (!surface)
        throw_sdl_error("SDL_CreateSurface");
    SDL_Palette* palette = SDL_CreateSurfacePalette(surface.get());
    if (!palette)
        throw_sdl_error("SDL_CreateSurfacePalette");

    std::array<SDL_Color, 16> colors{};
    for (std::size_t i = 0; i < colors.size(); ++i)
        colors[i] = {fb.palette[i].r, fb.palette[i].g, fb.palette[i].b, 0xFF};
    if (!SDL_SetPaletteColors(palette, colors.data(), 0, static_cast<int>(colors.size())))
        throw_sdl_error("SDL_SetPaletteColors");

    auto* dst = static_cast<std::uint8_t*>(surface->pixels);
    for (int y = 0; y < fb.height; ++y)
        std::memcpy(dst + y * surface->pitch, fb.pixels.data() + y * fb.width, static_cast<std::size_t>(fb.width));

    if (!SDL_SaveBMP(surface.get(), path_utf8.c_str()))
        throw_sdl_error("SDL_SaveBMP");
}

}  // namespace vette
