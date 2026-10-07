#include "platform/url_scheme.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

extern char** environ;
#endif

#include <string>

namespace vette {

#if defined(_WIN32)

namespace {

bool set_value(const wchar_t* subkey, const wchar_t* name, const std::wstring& value, std::string& error) {
    const LSTATUS rc = RegSetKeyValueW(HKEY_CURRENT_USER, subkey, name, REG_SZ, value.c_str(),
                                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    if (rc != ERROR_SUCCESS) {
        error = "Couldn't register vette2026:// links (registry error " + std::to_string(rc) + ").";
        return false;
    }
    return true;
}

}  // namespace

bool register_url_scheme(std::string& error) {
    std::wstring exe(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
        if (n == 0) {
            error = "Couldn't find the program's own path.";
            return false;
        }
        if (n < exe.size()) {
            exe.resize(n);
            break;
        }
        exe.resize(exe.size() * 2);
    }
    const wchar_t* root = L"Software\\Classes\\vette2026";
    return set_value(root, nullptr, L"URL:VETTE! 2026 race invite", error) &&
           set_value(root, L"URL Protocol", L"", error) &&
           set_value(L"Software\\Classes\\vette2026\\DefaultIcon", nullptr, L"\"" + exe + L"\",0", error) &&
           set_value(L"Software\\Classes\\vette2026\\shell\\open\\command", nullptr, L"\"" + exe + L"\" \"%1\"", error);
}

#elif defined(__linux__)

namespace {

// A desktop entry's Exec argument, quoted (the Desktop Entry Specification's rules).
std::string quote_exec(const std::string& path) {
    std::string q = "\"";
    for (const char c : path) {
        if (c == '"' || c == '`' || c == '$' || c == '\\')
            q += '\\';
        q += c;
    }
    return q + "\"";
}

}  // namespace

bool register_url_scheme(std::string& error) {
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        error = "Couldn't find the program's own path.";
        return false;
    }
    std::filesystem::path dir;
    if (const char* data = std::getenv("XDG_DATA_HOME"); data && *data)
        dir = data;
    else if (const char* home = std::getenv("HOME"); home && *home)
        dir = std::filesystem::path(home) / ".local" / "share";
    else {
        error = "No home folder to register vette2026:// links in.";
        return false;
    }
    dir /= "applications";
    std::filesystem::create_directories(dir, ec);
    const char* name = "vette2026-invite.desktop";
    {
        std::ofstream out(dir / name, std::ios::trunc);
        out << "[Desktop Entry]\n"
               "Type=Application\n"
               "Name=VETTE! 2026\n"
               "Comment=Opens VETTE! 2026 race invites\n"
               "Exec="
            << quote_exec(exe.string())
            << " %u\n"
               "MimeType=x-scheme-handler/vette2026;\n"
               "NoDisplay=true\n"
               "Terminal=false\n";
        if (!out) {
            error = "Couldn't write " + (dir / name).string() + ".";
            return false;
        }
    }
    // Make it the default handler (xdg-utils); without xdg-mime, desktops still find the entry.
    char arg0[] = "xdg-mime", arg1[] = "default", arg3[] = "x-scheme-handler/vette2026";
    std::string entry = name;
    char* argv[] = {arg0, arg1, entry.data(), arg3, nullptr};
    pid_t pid = 0;
    if (posix_spawnp(&pid, "xdg-mime", nullptr, nullptr, argv, environ) == 0) {
        int status = 0;
        waitpid(pid, &status, 0);
    }
    return true;
}

#else  // macOS: the app bundle's Info.plist declares the scheme

bool register_url_scheme(std::string&) { return true; }

#endif

}  // namespace vette
