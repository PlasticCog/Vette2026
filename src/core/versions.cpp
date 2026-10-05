#include "core/versions.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <system_error>
#include <utility>

#include "assets/mac_files.h"
#include "assets/mac_sounds.h"
#include "assets/pc98_disk.h"
#include "core/path_utf8.h"

namespace vette {
namespace fs = std::filesystem;

namespace {

constexpr int kMaxDepth = 3;
constexpr std::size_t kMaxFolders = 64;

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

bool iends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}

// `root` and the folders under it, shallowest first.
std::vector<fs::path> folders(const fs::path& root) {
    std::vector<fs::path> out{root};
    std::vector<int> depth{0};
    for (std::size_t i = 0; i < out.size() && out.size() < kMaxFolders; ++i) {
        if (depth[i] >= kMaxDepth)
            continue;
        std::vector<fs::path> children;
        std::error_code ec;
        for (fs::directory_iterator it(out[i], ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = path_to_utf8(it->path().filename());
            if (it->is_directory(ec) && !name.empty() && name.front() != '.')
                children.push_back(it->path());
        }
        std::sort(children.begin(), children.end());
        for (fs::path& c : children) {
            if (out.size() >= kMaxFolders)
                break;
            out.push_back(std::move(c));
            depth.push_back(depth[i] + 1);
        }
    }
    return out;
}

std::vector<std::string> file_names(const fs::path& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec))
            names.push_back(path_to_utf8(it->path().filename()));
    }
    return names;
}

std::vector<std::string_view> missing_dos_files(const std::vector<std::string>& names) {
    std::vector<std::string_view> missing;
    for (const std::string_view need : kRequiredGameFiles) {
        if (std::none_of(names.begin(), names.end(), [&](const std::string& n) { return iequals(n, need); }))
            missing.push_back(need);
    }
    return missing;
}

// The PC-98 version's files have its pictures (.PIC); the DOS version's VETTE.EXE alone doesn't count.
bool is_pc98(const assets::Pc98Files& files) {
    return std::any_of(files.names().begin(), files.names().end(),
                       [](const std::string& n) { return iends_with(n, ".PIC"); });
}

// What was found, briefly: "files", or the disk image and its kind ("VETTE.hdi: HDI hard disk").
std::string brief(const assets::Pc98Files& files) {
    if (files.image_listing().empty())
        return "files";
    const std::string& d = files.description();
    return d.substr(0, d.find(','));
}

// A PC-98 version in `dir`: its files, a disk image Pc98Files::open finds (the folder), or, where the
// folder also holds another version's VETTE.EXE (which stops that search), a disk image in it (the
// image file).
std::optional<std::pair<fs::path, std::string>> find_pc98(const fs::path& dir) {
    std::string error;
    if (const auto files = assets::Pc98Files::open(dir, error); files && is_pc98(*files))
        return std::pair{dir, brief(*files)};
    static constexpr std::string_view kImages[] = {".hdi", ".fdi", ".nhd", ".thd", ".d88", ".88d", ".d98",
                                                   ".98d", ".nfd", ".hdm", ".xdf", ".dim", ".tfd"};
    for (const std::string& name : file_names(dir)) {
        if (std::none_of(std::begin(kImages), std::end(kImages), [&](std::string_view e) { return iends_with(name, e); }))
            continue;
        const fs::path image = dir / path_from_utf8(name);
        if (const auto files = assets::Pc98Files::open_image(image, error); files && is_pc98(*files))
            return std::pair{image, brief(*files)};
    }
    return std::nullopt;
}

// A Mac version in `dir` (or below it): whatever holds VETTE!.Data's sounds and pictures.
std::optional<std::string> find_mac(const fs::path& dir) {
    const assets::MacFiles files = assets::MacFiles::open(dir);
    if (files.empty() || !assets::find_vette_data(files))
        return std::nullopt;
    const assets::MacFile* data = files.find("VETTE!.Data");
    return data ? data->source : std::string("Macintosh files");
}

}  // namespace

GameVersions scan_game_folder(const fs::path& root) {
    GameVersions v;
    v.root = root;
    const std::vector<fs::path> dirs = folders(root);

    std::optional<std::vector<std::string_view>> best;
    for (const fs::path& dir : dirs) {
        std::vector<std::string_view> missing = missing_dos_files(file_names(dir));
        if (missing.empty()) {
            v.dos = dir;
            break;
        }
        if (!best || missing.size() < best->size())
            best = std::move(missing);
    }
    if (!v.dos)
        v.dos_missing = std::move(best).value_or(std::vector<std::string_view>{});

    // The other versions: subfolders first (their own folders), the game folder itself last, as the
    // Mac and PC-98 readers also look below the folder they're given.
    std::vector<fs::path> order(dirs.begin() + 1, dirs.end());
    order.push_back(root);
    for (const fs::path& dir : order) {
        if (!v.pc98) {
            if (auto found = find_pc98(dir)) {
                v.pc98 = std::move(found->first);
                v.pc98_what = std::move(found->second);
                continue;  // one version per folder
            }
        }
        if (!v.mac) {
            if (std::optional<std::string> what = find_mac(dir)) {
                v.mac = dir;
                v.mac_what = std::move(*what);
            }
        }
        if (v.pc98 && v.mac)
            break;
    }
    return v;
}

std::optional<assets::Pc98Files> open_pc98(const GameVersions& versions, std::string& error) {
    if (!versions.pc98) {
        error = "the PC-98 version wasn't found";
        return std::nullopt;
    }
    std::error_code ec;
    return fs::is_regular_file(*versions.pc98, ec) ? assets::Pc98Files::open_image(*versions.pc98, error)
                                                    : assets::Pc98Files::open(*versions.pc98, error);
}

assets::MacFiles open_mac(const GameVersions& versions) {
    return versions.mac ? assets::MacFiles::open(*versions.mac) : assets::MacFiles{};
}

}  // namespace vette
