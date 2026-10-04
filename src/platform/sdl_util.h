#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace vette {

struct SdlDeleter {
    void operator()(SDL_Window* p) const { SDL_DestroyWindow(p); }
    void operator()(SDL_Renderer* p) const { SDL_DestroyRenderer(p); }
    void operator()(SDL_Texture* p) const { SDL_DestroyTexture(p); }
    void operator()(SDL_Surface* p) const { SDL_DestroySurface(p); }
    void operator()(SDL_AudioStream* p) const { SDL_DestroyAudioStream(p); }
    void operator()(SDL_Gamepad* p) const { SDL_CloseGamepad(p); }
};

template <class T>
using SdlPtr = std::unique_ptr<T, SdlDeleter>;

[[noreturn]] inline void throw_sdl_error(const char* call) {
    throw std::runtime_error(std::string(call) + " failed: " + SDL_GetError());
}

}  // namespace vette
