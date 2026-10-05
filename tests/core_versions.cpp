// The game folder scan: which version of VETTE! is where, whatever the folders are called.

#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/versions.h"
#include "test.h"

namespace fs = std::filesystem;
using vette::kRequiredGameFiles;
using vette::scan_game_folder;

namespace {

// A scratch game folder, removed afterwards.
struct TempDir {
    fs::path path;
    explicit TempDir(const char* name) : path(fs::temp_directory_path() / name) {
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void touch(const fs::path& file) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << "x";
}

void dos_files(const fs::path& dir, bool lower = false) {
    for (const std::string_view name : kRequiredGameFiles) {
        std::string n(name);
        if (lower)
            for (char& c : n)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        touch(dir / n);
    }
}

}  // namespace

TEST(versions_dos_in_the_game_folder) {
    TempDir game("vette_versions_root");
    dos_files(game.path);
    const auto v = scan_game_folder(game.path);
    CHECK(v.dos && fs::equivalent(*v.dos, game.path));
    CHECK(!v.pc98 && !v.mac);
}

TEST(versions_each_in_its_own_folder) {
    TempDir game("vette_versions_folders");
    dos_files(game.path / "My DOS copy", true);   // any name, any case
    touch(game.path / "Vette PC-98" / "VETTE.EXE");  // the PC-98's files: its VETTE.EXE and .PIC pictures
    touch(game.path / "Vette PC-98" / "TITLE.PIC");
    const auto v = scan_game_folder(game.path);
    CHECK(v.dos && v.dos->filename() == "My DOS copy");
    CHECK(v.pc98 && v.pc98->filename() == "Vette PC-98");
    CHECK(!v.mac);
}

TEST(versions_reports_what_dos_lacks) {
    TempDir game("vette_versions_missing");
    dos_files(game.path / "DOS");
    fs::remove(game.path / "DOS" / "TITLE.BIN");
    const auto v = scan_game_folder(game.path);
    CHECK(!v.dos);
    CHECK_EQ(v.dos_missing.size(), size_t{1});
    CHECK(v.dos_missing.size() == 1 && v.dos_missing[0] == "TITLE.BIN");
}

TEST(versions_dos_is_not_mistaken_for_pc98) {
    TempDir game("vette_versions_dos_only");
    dos_files(game.path / "DOS");  // its VETTE.EXE, but no PC-98 pictures
    const auto v = scan_game_folder(game.path);
    CHECK(v.dos);
    CHECK(!v.pc98);
}

// The player's real game folder, when there is one (the repository's Game/).
TEST(versions_real_game_folder) {
    const fs::path game = fs::path(__FILE__).parent_path().parent_path() / "Game";
    std::error_code ec;
    if (!fs::exists(game / "VETTE.EXE", ec) && !fs::exists(game / "DOS", ec)) {
        std::printf("  (skipped: no Game folder)\n");
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    const auto v = scan_game_folder(game);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("  scan %.0f ms: DOS %s, PC-98 %s, Mac %s\n", ms, v.dos ? "yes" : "no", v.pc98 ? v.pc98_what.c_str() : "no",
                v.mac ? v.mac_what.c_str() : "no");
    CHECK(v.dos);
}
