#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace vette {

// SDL, argv (via SDL_main) and our logs are UTF-8, but std::filesystem::path's char interface uses
// the ANSI code page on Windows. Go through char8_t instead.
inline std::filesystem::path path_from_utf8(std::string_view utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

inline std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string s = path.u8string();
    return std::string(s.begin(), s.end());
}

}  // namespace vette
