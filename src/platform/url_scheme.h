#pragma once
// vette2026:// links: a race invite opened from a browser starts the game and joins the race
// (vette2026://join/<room code>, vette2026://direct/<direct code>; the relay's invite pages link to them).
// Windows passes the link as the program's argument, as does Linux through a desktop entry; macOS sends
// it to the running app, which SDL delivers as an SDL_EVENT_DROP_FILE with the link as its "file", and
// the app bundle declares the scheme in its Info.plist (cmake/MacOSXBundleInfo.plist.in).

#include <string>
#include <string_view>

namespace vette {

inline constexpr std::string_view kUrlScheme = "vette2026://";

// Makes this program the handler of vette2026:// links for the current user (Windows: the registry's
// HKEY_CURRENT_USER classes; Linux: a desktop entry in ~/.local/share/applications, made the default with
// xdg-mime; macOS: nothing to do, the bundle declares it). Rewritten each time, so a moved program keeps working. False,
// with the reason, if it couldn't.
bool register_url_scheme(std::string& error);

inline bool is_invite_link(std::string_view text) {
    return text.size() > kUrlScheme.size() && text.substr(0, kUrlScheme.size()) == kUrlScheme;
}

}  // namespace vette
