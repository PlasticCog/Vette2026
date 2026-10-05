#include "core/game_dir.h"

#include "core/crc32.h"
#include "core/path_utf8.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace vette {
namespace fs = std::filesystem;

namespace {

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iequals(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) { return ascii_lower(x) == ascii_lower(y); });
}

std::vector<fs::path> candidate_dirs(const std::optional<fs::path>& override_dir) {
    if (override_dir)
        return {*override_dir};

    std::vector<fs::path> dirs;
    if (const char* base = SDL_GetBasePath()) {
        fs::path dir = path_from_utf8(base);
        if (!dir.has_filename())
            dir = dir.parent_path();  // SDL_GetBasePath ends with a separator
        for (int up = 0; up <= 4; ++up) {
            dirs.push_back(dir / "Game");
            if (dir.parent_path() == dir)
                break;
            dir = dir.parent_path();
        }
    }
    std::error_code ec;
    dirs.push_back(fs::current_path(ec) / "Game");

    // Normalize for display and drop duplicates (the CWD is often one of the exe's parents).
    std::vector<fs::path> unique;
    for (const fs::path& d : dirs) {
        fs::path abs = fs::absolute(d, ec);
        abs = ec ? d : abs.lexically_normal();
        if (std::ranges::find(unique, abs) == unique.end())
            unique.push_back(std::move(abs));
    }
    return unique;
}

}  // namespace

std::optional<fs::path> find_subfolder(const fs::path& dir, std::string_view name) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec) && iequals(path_to_utf8(it->path().filename()), name))
            return it->path();
    }
    return std::nullopt;
}

GameDir::GameDir(fs::path root) : root_(std::move(root)) {
    std::error_code ec;
    for (fs::directory_iterator it(root_, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec))
            names_.push_back(path_to_utf8(it->path().filename()));
}

std::optional<fs::path> GameDir::find(std::string_view name) const {
    const auto it = std::ranges::find_if(names_, [&](const std::string& n) { return iequals(n, name); });
    if (it == names_.end())
        return std::nullopt;
    return root_ / path_from_utf8(*it);
}

std::vector<std::uint8_t> GameDir::read(std::string_view name) const {
    const auto path = find(name);
    if (!path)
        throw std::runtime_error(std::string(name) + " not found in " + path_to_utf8(root_));

    const std::string utf8 = path_to_utf8(*path);
    std::size_t size = 0;
    void* data = SDL_LoadFile(utf8.c_str(), &size);
    if (!data)
        throw std::runtime_error("Can't read " + utf8 + ": " + SDL_GetError());
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::vector<std::uint8_t> result(bytes, bytes + size);
    SDL_free(data);
    return result;
}

GameDirSearch find_game_dir(const std::optional<fs::path>& override_dir) {
    GameDirSearch search;
    search.searched = candidate_dirs(override_dir);
    std::optional<GameVersions> best;
    for (const fs::path& path : search.searched) {
        GameVersions v = scan_game_folder(path);
        if (v.dos) {
            search.dir = GameDir(*v.dos);
            search.versions = std::move(v);
            return search;
        }
        if (!best || v.dos_missing.size() < best->dos_missing.size())
            best = std::move(v);
    }
    if (best) {
        search.missing = best->dos_missing;
        search.versions = std::move(*best);
    }
    return search;
}

bool identify_vette_exe(std::span<const std::uint8_t> exe) {
    if (exe.size() < 2 || exe[0] != 'M' || exe[1] != 'Z')
        throw std::runtime_error("VETTE.EXE is not a DOS executable (no MZ signature)");

    const std::uint32_t crc = crc32(exe);
    SDL_Log("VETTE.EXE: %u bytes, CRC32 %08X", static_cast<unsigned>(exe.size()), static_cast<unsigned>(crc));
    if (exe.size() == kVetteExeSize && crc == kVetteExeCrc32) {
        SDL_Log("VETTE.EXE: DOS v1.1 (English)");
        return true;
    }
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "VETTE.EXE: unknown version (expected DOS v1.1, %u bytes, CRC32 %08X); it may not work",
                static_cast<unsigned>(kVetteExeSize), static_cast<unsigned>(kVetteExeCrc32));
    return false;
}

}  // namespace vette
