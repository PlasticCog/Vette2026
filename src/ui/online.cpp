#include "ui/online.h"

#ifdef VETTE_ONLINE

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/crc32.h"
#include "net/default_server.h"
#include "net/invite.h"
#include "net/online.h"
#include "net/protocol.h"
#include "platform/gamepad.h"
#include "platform/presenter.h"
#include "platform/url_scheme.h"
#include "ui/canvas.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace vette::ui {
namespace {

using namespace theme;

enum Row { kHost, kJoin, kCourse, kServer, kBack, kRows };
enum class Page { Menu, Hosting, Joining, Server };

constexpr std::uint64_t kStartDelayNs = 800'000'000;  // "Starting the race..." shows this long

const char* course_name(int course) {
    switch (course) {
    case 1: return "Course 1";
    case 2: return "Course 2";
    case 3: return "Course 3";
    default: return "All three in a row";
    }
}

const char* row_label(int row) {
    switch (row) {
    case kHost: return "Host a race";
    case kJoin: return "Join a race";
    case kCourse: return "Course";
    case kServer: return "Server";
    default: return "Back";
    }
}

// The relay server in use: the player's, or the one this build comes with (empty: none, direct codes only).
std::string server_of(const Settings& s) { return s.online_server.empty() ? net::kDefaultServer : s.online_server; }

std::string_view row_help(int row) {
    switch (row) {
    case kHost:
        return "Start a race and send your friend the invite link (it's copied for you) or the code. The race "
               "begins as soon as they join, on your course and with your Driving setting for both of you.";
    case kJoin: return "Paste the invite link or code your friend sent you.";
    case kCourse: return "The course you race on when you host.";
    case kServer:
        return "Advanced: the relay server for room codes and invite links. Leave it empty to use the built-in one. "
               "Without a server, races use direct codes.";
    default: return "Back to the launch menu.";
    }
}

// What the player typed as a server address: wss://host[/path], from an https:// address or a bare host.
std::string normalize_server(std::string_view typed) {
    std::string s(typed);
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char c) { return !std::isspace(c); }));
    while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.back())) || s.back() == '/'))
        s.pop_back();
    if (s.empty())
        return s;
    if (s.rfind("https://", 0) == 0)
        return "wss://" + s.substr(8);
    if (s.rfind("http://", 0) == 0)
        return "ws://" + s.substr(7);
    if (s.find("://") == std::string::npos)
        return "wss://" + s;
    return s;
}

// The original game's build, which both players need the same of: VETTE.EXE's CRC32.
std::string game_build(const GameDir& game) {
    char text[16];
    std::snprintf(text, sizeof text, "%08X", static_cast<unsigned>(crc32(game.read("VETTE.EXE"))));
    return text;
}

net::OnlineOptions online_options(const Settings& s, const GameDir& game) {
    net::OnlineOptions o;
    o.server_url = server_of(s);
    o.app_version = VETTE_VERSION;
    o.game_build = game_build(game);
    return o;
}

game::TwoPlayerSetup host_setup(const Settings& s) {
    game::TwoPlayerSetup setup;
    setup.course = std::clamp(s.online_course, 1, 4);
    setup.improved_driving = s.improved_driving;
    return setup;
}

net::RaceSettings race_settings(const Settings& s) {
    net::RaceSettings settings;
    settings.set("setup", host_setup(s).encode());
    return settings;
}

// The guest's race: the host's setup; nullopt if it's not one this version knows.
std::optional<game::TwoPlayerSetup> guest_setup(const net::OnlineLink& link) {
    const auto settings = link.race_settings();
    const auto text = settings ? settings->get("setup") : std::nullopt;
    return text ? game::TwoPlayerSetup::decode(*text) : std::nullopt;
}

constexpr const char* kSetupMismatch = "Your friend's VETTE! 2026 sets up the race differently. Use the same version.";

bool trouble(net::LinkState state) {
    return state == net::LinkState::Failed || state == net::LinkState::Closed || state == net::LinkState::PeerLeft;
}

// One line on how it's going.
std::string state_text(const net::OnlineStatus& st) {
    switch (st.state) {
    case net::LinkState::Connected: {
        std::string text = std::string("Connected (") + net::to_string(st.route);
        if (st.rtt_ms >= 0)
            text += ", " + std::to_string(static_cast<int>(st.rtt_ms + 0.5)) + " ms";
        return text + "). Starting the race...";
    }
    case net::LinkState::Failed:
    case net::LinkState::Closed:
    case net::LinkState::PeerLeft: {
        std::string text = st.reason.empty() ? std::string("Couldn't connect.") : st.reason;
        if (!st.suggestion.empty() && text.find(st.suggestion) == std::string::npos)
            text += " " + st.suggestion;
        return text;
    }
    case net::LinkState::Waiting: return st.activity.empty() ? "Waiting for your friend..." : st.activity;
    default: return st.activity.empty() ? "Connecting..." : st.activity;
    }
}

void copy_to_clipboard(const std::string& text) { SDL_SetClipboardText(text.c_str()); }

// An invite link or code on the clipboard (one line), or empty.
std::string clipboard_invite() {
    std::string found;
    if (char* clip = SDL_GetClipboardText()) {
        if (net::parse_invite(clip))
            found = clip;
        SDL_free(clip);
    }
    found.erase(std::remove_if(found.begin(), found.end(), [](char c) { return c == '\r' || c == '\n'; }), found.end());
    return found.size() <= 200 ? found : std::string();
}

void register_links() {
    std::string error;
    if (!register_url_scheme(error))
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Invite links: %s", error.c_str());
}

}  // namespace

bool online_available() { return true; }

bool connect_online(const Settings& s, const GameDir& game, bool host, std::string_view code, OnlineSession& session,
                    std::string& error, double timeout_s) {
    register_links();
    net::OnlineOptions o = online_options(s, game);
    std::unique_ptr<net::OnlineLink> link;
    if (host) {
        o.race_settings = race_settings(s);
        link = net::OnlineLink::host(std::move(o));
    } else {
        link = net::OnlineLink::join(std::move(o), code);
    }
    const std::uint64_t deadline = SDL_GetTicksNS() + static_cast<std::uint64_t>(timeout_s * 1e9);
    std::string shown, invite;
    for (;;) {
        const net::OnlineStatus st = link->status();
        if (host && invite.empty() && !link->invite_url().empty()) {
            invite = link->invite_url();
            SDL_Log("Online race: invite %s (room %s, direct %s)", invite.c_str(),
                    st.room.code.empty() ? "-" : st.room.code.c_str(), st.direct.code.empty() ? "-" : st.direct.code.c_str());
        }
        const std::string now = state_text(st);
        if (now != shown) {
            shown = now;
            SDL_Log("Online race: %s", now.c_str());
        }
        if (st.state == net::LinkState::Connected)
            break;
        if (st.state == net::LinkState::Failed || st.state == net::LinkState::Closed ||
            (!host && st.state == net::LinkState::PeerLeft)) {
            error = now;
            return false;
        }
        if (SDL_GetTicksNS() > deadline) {
            error = "Nobody joined in time.";
            return false;
        }
        SDL_Delay(20);
    }
    session.host = host;
    if (host) {
        session.setup = host_setup(s);
    } else if (const auto decoded = guest_setup(*link)) {
        session.setup = *decoded;
    } else {
        error = kSetupMismatch;
        return false;
    }
    session.online = link.get();
    session.link = std::move(link);
    return true;
}

bool run_online(Presenter& presenter, Gamepad& gamepad, Settings& s, const GameDir& game, OnlineSession& session,
                std::string_view join_now) {
    register_links();  // so the invite links open this game
    Page page = Page::Menu;
    int selected = kHost;
    std::string status;  // a problem or a hint to show
    std::unique_ptr<net::OnlineLink> link;
    std::uint64_t connected_at = 0;  // when the friend was there (the race starts a moment later)
    bool invite_copied = false;
    std::string field;  // Joining: what's pasted or typed; Server: the address
    Canvas canvas;
    std::vector<std::uint8_t> pad_keys;
    s.online_course = std::clamp(s.online_course, 1, 4);

    const auto text_input = [&](bool on) {
        if (on)
            SDL_StartTextInput(presenter.window());
        else
            SDL_StopTextInput(presenter.window());
    };
    const auto leave = [&] {
        link.reset();  // leaves the room, removes the router's port forwarding
        connected_at = 0;
        invite_copied = false;
    };
    const auto host = [&] {
        net::OnlineOptions o = online_options(s, game);
        o.race_settings = race_settings(s);
        link = net::OnlineLink::host(std::move(o));
        page = Page::Hosting;
        status.clear();
    };
    const auto join = [&] {
        if (!net::parse_invite(field)) {
            status = "That isn't an invite link or a code. Paste exactly what your friend sent.";
            return;
        }
        leave();
        status.clear();
        link = net::OnlineLink::join(online_options(s, game), field);
    };
    const auto open_join = [&](std::string_view preset) {
        page = Page::Joining;
        leave();
        field = preset.empty() ? clipboard_invite() : std::string(preset);
        status = !preset.empty() || field.empty() ? "" : "From your clipboard. Press Enter to join.";
        text_input(true);
    };
    const auto activate = [&](int row, int dir) {
        status.clear();
        switch (row) {
        case kHost: host(); break;
        case kJoin: open_join({}); break;
        case kCourse: s.online_course = (s.online_course - 1 + dir + 4) % 4 + 1; break;
        case kServer:
            page = Page::Server;
            field = s.online_server;
            text_input(true);
            break;
        default: break;
        }
    };
    // Esc or the B button. True: back to the launch menu.
    const auto back = [&] {
        switch (page) {
        case Page::Menu:
            if (selected == kBack)
                return true;
            selected = kBack;
            return false;
        case Page::Hosting: leave(); page = Page::Menu; return false;
        case Page::Joining:
            if (link) {  // stop trying; the code stays to edit
                leave();
                status.clear();
                return false;
            }
            text_input(false);
            page = Page::Menu;
            return false;
        case Page::Server: text_input(false); page = Page::Menu; return false;
        }
        return false;
    };
    // The D-pad's way of typing a code: Up/Down change the last character, Right adds one, Left removes it.
    const auto pick = [&](int up_down, int left_right) {
        static constexpr std::string_view kPick = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ-";
        if (left_right > 0)
            field += 'A';
        else if (left_right < 0 && !field.empty())
            field.pop_back();
        if (up_down && !field.empty()) {
            const std::size_t at = kPick.find(static_cast<char>(std::toupper(static_cast<unsigned char>(field.back()))));
            const auto n = static_cast<int>(kPick.size());
            const int next = at == std::string_view::npos ? 0 : (static_cast<int>(at) - up_down + n) % n;
            field.back() = kPick[static_cast<std::size_t>(next)];
        }
    };
    // Up/Down, Left/Right, Enter: from the keyboard and the D-pad alike.
    const auto key = [&](int up_down, int left_right, bool enter) {
        switch (page) {
        case Page::Menu:
            if (up_down)
                selected = (selected + up_down + kRows) % kRows;
            if (left_right && selected == kCourse)
                activate(kCourse, left_right);
            if (enter)
                activate(selected, 1);
            break;
        case Page::Joining:
            if (link)
                break;
            if (up_down || left_right)
                pick(up_down, left_right);
            if (enter)
                join();
            break;
        case Page::Server:
            if (enter) {
                s.online_server = normalize_server(field);
                text_input(false);
                page = Page::Menu;
            }
            break;
        case Page::Hosting:
            if (left_right && link && link->status().state != net::LinkState::Connected) {
                s.online_course = (s.online_course - 1 + left_right + 4) % 4 + 1;
                link->set_race_settings(race_settings(s));
            }
            break;
        }
    };

    if (!join_now.empty()) {
        open_join(join_now);
        join();
    }

    for (;;) {
        int out_w = 0, out_h = 0;
        presenter.output_size(out_w, out_h);
        const int scale = std::max(1, std::min(out_w / 500, out_h / 340));
        canvas.reset(std::max(out_w / scale, 1), std::max(out_h / scale, 1), scale, kBackground);

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            pad_keys.clear();
            gamepad.handle_event(e, pad_keys);
            bool done = false;
            switch (e.type) {
            case SDL_EVENT_QUIT:
                SDL_PushEvent(&e);  // the launch menu quits too
                text_input(false);
                return false;
            case SDL_EVENT_DROP_FILE:  // macOS: a vette2026:// link opened while the game runs
                if (e.drop.data && is_invite_link(e.drop.data) && !(page == Page::Hosting && link)) {
                    open_join(e.drop.data);
                    join();
                }
                break;
            case SDL_EVENT_TEXT_INPUT:
                if ((page == Page::Joining && !link) || page == Page::Server)
                    field += e.text.text;
                break;
            case SDL_EVENT_KEY_DOWN: {
                const SDL_Scancode k = e.key.scancode;
                const bool enter = (k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER) && !e.key.repeat;
                const bool typing = page == Page::Server || (page == Page::Joining && !link);
                const bool arrow = k == SDL_SCANCODE_LEFT || k == SDL_SCANCODE_RIGHT || k == SDL_SCANCODE_UP ||
                                   k == SDL_SCANCODE_DOWN;
                if (k == SDL_SCANCODE_ESCAPE && !e.key.repeat) {
                    done = back();
                } else if (k == SDL_SCANCODE_V && (e.key.mod & SDL_KMOD_CTRL) && typing) {
                    if (char* clip = SDL_GetClipboardText()) {
                        field += clip;
                        field.erase(std::remove_if(field.begin(), field.end(),
                                                   [](char c) { return c == '\r' || c == '\n'; }),
                                    field.end());
                        SDL_free(clip);
                    }
                } else if (k == SDL_SCANCODE_BACKSPACE && typing) {
                    if (!field.empty())
                        field.pop_back();
                } else if (page == Page::Hosting && link && !e.key.repeat && (k == SDL_SCANCODE_C || k == SDL_SCANCODE_D)) {
                    const std::string what = k == SDL_SCANCODE_C ? link->invite_url() : link->status().direct.code;
                    if (!what.empty()) {
                        copy_to_clipboard(what);
                        status = k == SDL_SCANCODE_C ? "Invite copied." : "Direct code copied.";
                    }
                } else if (typing && arrow) {
                    // On the text pages the arrow keys don't edit (the D-pad's picker is for gamepads).
                } else if (k == SDL_SCANCODE_UP) {
                    key(-1, 0, false);
                } else if (k == SDL_SCANCODE_DOWN || (k == SDL_SCANCODE_TAB && page == Page::Menu)) {
                    key(1, 0, false);
                } else if (k == SDL_SCANCODE_LEFT || k == SDL_SCANCODE_RIGHT) {
                    key(0, k == SDL_SCANCODE_LEFT ? -1 : 1, false);
                } else if (enter || (k == SDL_SCANCODE_SPACE && page == Page::Menu && !e.key.repeat)) {
                    key(0, 0, true);
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH)
                    key(0, 0, true);
                else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)
                    done = back();
                break;
            default: break;
            }
            for (const std::uint8_t sc : pad_keys) {  // the D-pad, Start and Back
                switch (sc) {
                case 0x48: key(-1, 0, false); break;
                case 0x50: key(1, 0, false); break;
                case 0x4B: key(0, -1, false); break;
                case 0x4D: key(0, 1, false); break;
                case 0x1C: key(0, 0, true); break;
                case 0x01: done = done || back(); break;
                default: break;
                }
            }
            if (done) {
                text_input(false);
                return false;
            }
        }

        // The connection: the invite onto the clipboard once it's ready; on to the race once connected.
        if (link) {
            const net::OnlineStatus st = link->status();
            if (page == Page::Hosting && !invite_copied && !link->invite_url().empty()) {
                copy_to_clipboard(link->invite_url());
                invite_copied = true;
            }
            if (st.state == net::LinkState::Connected) {
                const std::uint64_t now = SDL_GetTicksNS();
                if (!connected_at)
                    connected_at = now;
                if (now - connected_at >= kStartDelayNs) {
                    session.host = page == Page::Hosting;
                    session.setup = host_setup(s);
                    const auto decoded = session.host ? std::nullopt : guest_setup(*link);
                    if (session.host || decoded) {
                        if (decoded)
                            session.setup = *decoded;
                        session.online = link.get();
                        session.link = std::move(link);
                        text_input(false);
                        return true;
                    }
                    leave();
                    status = kSetupMismatch;
                }
            } else {
                connected_at = 0;
                if (page == Page::Joining && trouble(st.state)) {  // a failed join: let the player try again
                    status = state_text(st);
                    leave();
                }
            }
        }

        // Drawing.
        const int m = 16;
        const size_t line_chars = static_cast<size_t>((canvas.width - 2 * m) / kGlyph);
        canvas.text(m, 12, "VETTE!", kGold, 3, true);
        const int sub_x = m + text_width("VETTE!", 3) + 32;
        canvas.text(sub_x, 14, "Online race", kSubtitle);
        canvas.text(sub_x, 26, "Race a friend over the internet", kSubtitle);
        const auto lines = [&](int y, std::string_view text, std::uint32_t color, size_t max_lines) {
            const std::vector<std::string> wrapped = wrap(text, line_chars);
            for (size_t i = 0; i < wrapped.size() && i < max_lines; ++i)
                canvas.text(m, y + static_cast<int>(i) * 10, wrapped[i], color);
            return y + static_cast<int>(std::min(wrapped.size(), max_lines)) * 10;
        };
        std::string help, hints;
        if (page == Page::Menu) {
            const int pitch = 14;
            for (int row = 0; row < kRows; ++row) {
                const int y = 64 + row * pitch + (row >= kCourse ? pitch / 2 : 0) + (row == kBack ? pitch / 2 : 0);
                const bool sel = row == selected;
                if (sel)
                    canvas.fill_rect(6, y - 3, canvas.width - 12, pitch, kSelection);
                if (row == kCourse || row == kServer) {
                    canvas.text(m, y, row_label(row), sel ? kGold : kLabel);
                    const int vx = m + 12 * kGlyph;
                    const size_t room_chars = static_cast<size_t>(std::max(0, (canvas.width - vx - m) / kGlyph));
                    std::string v;
                    if (row == kCourse)
                        v = std::string("< ") + course_name(s.online_course) + " >";
                    else if (!s.online_server.empty())
                        v = fit_left(s.online_server, room_chars);
                    else
                        v = *net::kDefaultServer ? "Built in" : "None (direct codes only)";
                    canvas.text(vx, y, v, kValue);
                } else {
                    canvas.text(m, y, std::string(sel ? "> " : "  ") + row_label(row), sel ? kGold : kValue);
                }
            }
            if (!status.empty())
                lines(64 + kRows * 14 + 20, status, kBad, 2);
            help = std::string(row_help(selected));
            hints = "Up/Down choose   Left/Right change   Enter select   Esc back";
        } else if (page == Page::Hosting) {
            const net::OnlineStatus st = link->status();
            const bool room = st.room.state == net::RouteStatus::State::Ready;
            const bool direct = st.direct.state == net::RouteStatus::State::Ready;
            int y = 60;
            if (room) {
                y = lines(y, "Send your friend this invite link. It's on your clipboard: paste it in a chat.", kHelp, 2);
                canvas.text(m, y + 2, fit_left(link->invite_url(), line_chars), kGood);
                canvas.text(m, y + 16, "Or tell them the code:", kLabel);
                canvas.text(m, y + 28, st.room.code, kGold, 3, true);
                y += 58;
            } else if (direct) {
                y = lines(y, "Send your friend this code. It's on your clipboard: paste it in a chat.", kHelp, 2);
                canvas.text(m, y + 4, st.direct.code, kGold, 3, true);
                y += 34;
            } else if (!trouble(st.state)) {
                canvas.text(m, y, "Getting your race ready...", kHelp);
                y += 14;
            }
            // The direct code, as a second way in (or why there isn't one).
            if (room && direct) {
                canvas.text(m, y, "Direct code (no server needed): " + st.direct.code, kValue);
                y += 12;
            } else if (st.direct.state == net::RouteStatus::State::Starting) {
                canvas.text(m, y, "Direct code: opening a port on your router...", kDim);
                y += 12;
            } else if (st.direct.state == net::RouteStatus::State::Failed && room) {
                y = lines(y, "Direct code not available: " + st.direct.reason, kDim, 2);
            }
            y += 6;
            y = lines(y, state_text(st), trouble(st.state) ? kBad : kGood, 3);
            if (!status.empty())
                y = lines(y, status, kGood, 1);
            canvas.text(m, y + 6,
                        std::string("Course: ") + course_name(s.online_course) +
                            (st.state != net::LinkState::Connected ? "  (Left/Right to change)" : ""),
                        kValue);
            canvas.text(m, y + 18,
                        std::string("Driving: ") + (s.improved_driving ? "Improved" : "Original") + " (for both of you)",
                        kValue);
            hints = std::string(room ? "C copy invite   " : "") + (direct ? "D copy direct code   " : "") + "Esc cancel";
        } else if (page == Page::Joining) {
            canvas.text(m, 64, "Paste the invite link or code your friend sent:", kLabel);
            canvas.fill_rect(m - 2, 78, canvas.width - 2 * m + 4, 14, kSelection);
            const size_t room_chars = line_chars > 1 ? line_chars - 1 : 0;
            canvas.text(m, 81, fit_left(field, room_chars) + (link ? "" : "_"), kValue);
            if (link) {
                const net::OnlineStatus st = link->status();
                lines(100, state_text(st), trouble(st.state) ? kBad : kGood, 3);
            } else if (!status.empty()) {
                lines(100, status, status.rfind("From your clipboard", 0) == 0 ? kGood : kBad, 3);
            }
            help = "Paste with Ctrl+V, or type the code. With a gamepad: Up/Down change the last letter, Right adds "
                   "one, Left removes one.";
            hints = link ? "Esc stop" : "Ctrl+V paste   Enter join   Esc back";
        } else {
            canvas.text(m, 64, "Relay server address (empty: the built-in one):", kLabel);
            canvas.fill_rect(m - 2, 78, canvas.width - 2 * m + 4, 14, kSelection);
            const size_t room_chars = line_chars > 1 ? line_chars - 1 : 0;
            canvas.text(m, 81, fit_left(field, room_chars) + "_", kValue);
            help = "Only if you run your own relay server (server/README.md): paste its address, e.g. "
                   "wss://vette2026-relay.yourname.workers.dev. Both players need the same server.";
            hints = "Ctrl+V paste   Backspace delete   Enter save   Esc cancel";
        }
        const int rule_y = canvas.height - 64;
        canvas.fill_rect(m, rule_y, canvas.width - 2 * m, 1, kRule);
        const std::vector<std::string> help_lines = wrap(help, line_chars);
        for (size_t i = 0; i < help_lines.size() && i < 3; ++i)
            canvas.text(m, rule_y + 8 + static_cast<int>(i) * 12, help_lines[i], kHelp);
        canvas.text(m, canvas.height - 14, hints, kHint);

        presenter.present(canvas);
        if (!presenter.visible())
            SDL_Delay(10);
    }
}

}  // namespace vette::ui

#else  // built without online play

namespace vette::ui {

bool online_available() { return false; }

bool connect_online(const Settings&, const GameDir&, bool, std::string_view, OnlineSession&, std::string& error,
                    double) {
    error = "This copy of VETTE! 2026 was built without online play.";
    return false;
}

bool run_online(Presenter&, Gamepad&, Settings&, const GameDir&, OnlineSession&, std::string_view) { return false; }

}  // namespace vette::ui

#endif
