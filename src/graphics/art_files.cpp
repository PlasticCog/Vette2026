#include "graphics/art_files.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>

#include "assets/mac_files.h"
#include "assets/pc98_disk.h"

namespace vette::graphics {

namespace {

// Color VETTE!'s resources as a PICT lookup. The B&W application's pictures are the fallback.
std::function<std::vector<std::uint8_t>(std::int16_t)> pict_lookup(std::shared_ptr<const assets::ResourceFork> fork) {
    return [fork](std::int16_t id) {
        const auto s = fork ? fork->get(assets::fourcc("PICT"), id) : std::span<const std::uint8_t>{};
        return std::vector<std::uint8_t>(s.begin(), s.end());
    };
}

}  // namespace

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    if (path.empty()) return {};
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {});
}

std::filesystem::path find_ci(const std::filesystem::path& dir, const std::string& name) {
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return {};
    const auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string want = lower(name);
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const auto u8 = e.path().filename().u8string();  // never throws on names the code page lacks
        if (e.is_regular_file(ec) && lower(std::string(u8.begin(), u8.end())) == want) return e.path();
    }
    return {};
}

ArtFiles ArtFiles::from_game_dir(const std::filesystem::path& game_dir, std::vector<std::string>* notes) {
    ArtFiles files;
    files.dos_file = [game_dir](const std::string& name) { return read_file(find_ci(game_dir, name)); };

    std::string error;
    auto pc98 = assets::Pc98Files::open(game_dir / "PC98", error);
    if (pc98) {
        if (notes) notes->push_back("PC-98: " + pc98->description());
        auto shared = std::make_shared<const assets::Pc98Files>(std::move(*pc98));
        files.pc98_file = [shared](const std::string& name) {
            std::vector<std::uint8_t> out;
            shared->read(name, out);
            return out;
        };
    } else {
        if (notes) notes->push_back("PC-98: " + error);
        files.pc98_file = [](const std::string&) { return std::vector<std::uint8_t>{}; };
    }

    std::shared_ptr<const assets::ResourceFork> fork;
    std::error_code ec;
    if (std::filesystem::exists(game_dir / "Mac", ec)) {
        const auto mac = assets::MacFiles::open(game_dir / "Mac");
        for (const char* app : {"Color VETTE!", "VETTE!"}) {
            for (const auto* file : mac.find_all(app)) {
                auto res = mac.resources(*file);
                if (res && res->find(assets::fourcc("PICT"), 24592)) {
                    if (notes) notes->push_back("Mac: " + file->path + " (" + file->source + ")");
                    fork = std::make_shared<const assets::ResourceFork>(std::move(*res));
                    break;
                }
            }
            if (fork) break;
        }
        if (!fork && notes) notes->push_back("Mac: no Color VETTE! application found");
    }
    files.mac_pict = pict_lookup(fork);
    return files;
}

ArtFiles ArtFiles::from_folders(const std::filesystem::path& dos_dir, const std::filesystem::path& pc98_dir,
                                const std::filesystem::path& mac_rsrc) {
    ArtFiles files;
    files.dos_file = [dos_dir](const std::string& name) { return read_file(find_ci(dos_dir, name)); };
    files.pc98_file = [pc98_dir](const std::string& name) { return read_file(find_ci(pc98_dir, name)); };
    auto fork = std::make_shared<assets::ResourceFork>();
    if (!fork->parse(read_file(mac_rsrc))) fork.reset();
    files.mac_pict = pict_lookup(fork);
    return files;
}

}  // namespace vette::graphics
