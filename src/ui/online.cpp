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
#include "net/instance.h"
#include "net/invite.h"
#include "net/lan.h"
#include "net/online.h"
#include "net/protocol.h"
#include "platform/gamepad.h"
#include "platform/presenter.h"
#include "platform/url_scheme.h"
#include "ui/canvas.h"
#include "ui/shortcuts.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace vette::ui {
namespace {

using namespace theme;

enum Row { kHost, kJoin, kLanHost, kLanJoin, kCourse, kRouter, kAddress, kBack, kRows };
enum class Page { Menu, Hosting, Joining, Address, Lan };

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
    case kLanHost: return "Host a LAN race";
    case kLanJoin: return "Join a LAN race";
    case kCourse: return "Course";
    case kRouter: return "Router";
    case kAddress: return "Internet address";
    default: return "Back";
    }
}

// The relay server in use: the player's, or the one this build comes with (empty: none, direct codes only).
std::string server_of(const Settings& s) { return s.online_server.empty() ? net::kDefaultServer : s.online_server; }

std::string_view row_help(int row) {
    switch (row) {
    case kHost:
        return "Start a race and send your friend its code (it's copied for you). The race begins as soon as they "
               "join, on your course and with your Driving setting for both of you.";
    case kJoin: return "Paste the code your friend sent you.";
    case kLanHost:
        return "Race someone on the same network (the same Wi-Fi or router): no code to send, no router to set "
               "up. They choose Join a LAN race.";
    case kLanJoin: return "The races hosted on this network: pick one to join it.";
    case kCourse: return "The course you race on when you host.";
    case kRouter:
        return "To host over the internet, your router must let your friend in. Most open the game's port "
               "themselves (UPnP). If yours doesn't, forward TCP port 26989 to this computer in its settings, "
               "choose Forwarded by hand and enter your internet address.";
    case kAddress:
        return "Your internet address, for a port forwarded by hand (your router's status page shows it, as "
               "\"WAN\" or \"Internet\" address). Not needed when the router opens the port itself.";
    default: return "Back to the launch menu.";
    }
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
    o.use_stun = false;  // the internet address from the router (or the player), never from outside machines
    if (s.online_port_forwarded && net::parse_ipv4(s.online_address)) {
        o.manual_port_forward = true;
        o.code_address = s.online_address;  // the direct code with this address; the router isn't asked
    }
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

std::string g_program;  // argv[0], for leave_for_invite()

void register_links() {
    std::string error;
    if (!register_url_scheme(error))
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Invite links: %s", error.c_str());
}

}  // namespace

bool online_available() { return true; }

bool forward_invite(const std::string& link) { return net::InstanceChannel::forward(link); }

void accept_forwarded_invites(const char* program) {
    g_program = program ? program : "";
    if (!net::InstanceChannel::get().listen())
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Invite links: another game takes them (port %u in use)",
                    net::kInstancePort);
}

std::optional<std::string> take_forwarded_invite() { return net::InstanceChannel::get().take(); }

bool leave_for_invite(const std::string& link) {
    if (g_program.empty())
        return false;
    net::InstanceChannel::get().close();  // so the new copy doesn't hand the link straight back
    const char* args[] = {g_program.c_str(), link.c_str(), nullptr};
    SDL_Process* process = SDL_CreateProcess(args, false);
    if (!process) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Couldn't start the game for the invite: %s", SDL_GetError());
        net::InstanceChannel::get().listen();
        return false;
    }
    SDL_DestroyProcess(process);  // (it runs on)
    return true;
}

bool connect_online(const Settings& s, const GameDir& game, bool host, std::string_view code, OnlineSession& session,
                    std::string& error, double timeout_s) {
    register_links();
    net::OnlineOptions o = online_options(s, game);
    std::unique_ptr<net::OnlineLink> link;
    const bool lan_race = code == "lan";
    const std::uint64_t deadline = SDL_GetTicksNS() + static_cast<std::uint64_t>(timeout_s * 1e9);
    if (host) {
        o.race_settings = race_settings(s);
        o.lan_only = lan_race;
        link = net::OnlineLink::host(std::move(o));
    } else if (lan_race) {
        // The first race on this network that this game can join.
        net::LanSearch search;
        if (!search.start(error)) {
            error = "Can't look for races on this network: " + error;
            return false;
        }
        const std::string build = game_build(game);
        std::string found;
        while (found.empty()) {
            search.poll();
            for (const net::LanRace& r : search.races()) {
                if (r.app_version == VETTE_VERSION && r.game_build == build) {
                    found = r.code();
                    SDL_Log("Online race: %s's race on this network (%s)", r.name.c_str(), r.host.to_string().c_str());
                    break;
                }
            }
            if (found.empty() && SDL_GetTicksNS() > deadline) {
                error = "No race found on this network.";
                return false;
            }
            SDL_Delay(50);
        }
        link = net::OnlineLink::join(std::move(o), found);
    } else {
        link = net::OnlineLink::join(std::move(o), code);
    }
    std::string shown, invite, lan;
    for (;;) {
        const net::OnlineStatus st = link->status();
        if (host && lan.empty() && st.lan.state == net::RouteStatus::State::Ready) {
            lan = st.lan.code;
            SDL_Log("Online race: same-network code %s (%s)", lan.c_str(), st.lan.detail.c_str());
        }
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
    bool lan_hosting = false;                // Hosting: a LAN race
    bool joined_from_lan = false;            // Joining: picked from the LAN list (Esc goes back there)
    std::unique_ptr<net::LanSearch> search;  // Lan: the races on this network
    int lan_selected = 0;
    const std::string my_build = game_build(game);
    const std::string my_name = net::computer_name();
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
    const auto host = [&](bool lan) {
        net::OnlineOptions o = online_options(s, game);
        o.race_settings = race_settings(s);
        o.lan_only = lan;
        link = net::OnlineLink::host(std::move(o));
        page = Page::Hosting;
        lan_hosting = lan;
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
        joined_from_lan = false;
        text_input(true);
    };
    const auto open_lan = [&] {
        leave();
        text_input(false);
        page = Page::Lan;
        lan_selected = 0;
        status.clear();
        search = std::make_unique<net::LanSearch>();
        std::string error;
        if (!search->start(error))
            status = "Can't look for races on this network: " + error;
    };
    const auto activate = [&](int row, int dir) {
        status.clear();
        switch (row) {
        case kHost:
            if (s.online_port_forwarded && !net::parse_ipv4(s.online_address)) {
                status = "Enter your internet address first (or set Router to the automatic way).";
                selected = kAddress;
                break;
            }
            host(false);
            break;
        case kJoin: open_join({}); break;
        case kLanHost: host(true); break;
        case kLanJoin: open_lan(); break;
        case kCourse: s.online_course = (s.online_course - 1 + dir + 4) % 4 + 1; break;
        case kRouter: s.online_port_forwarded = !s.online_port_forwarded; break;
        case kAddress:
            page = Page::Address;
            field = s.online_address;
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
            if (joined_from_lan)
                open_lan();
            else
                page = Page::Menu;
            return false;
        case Page::Address: text_input(false); page = Page::Menu; return false;
        case Page::Lan: search.reset(); page = Page::Menu; return false;
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
            if (left_right && (selected == kCourse || selected == kRouter))
                activate(selected, left_right);
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
        case Page::Address:
            if (enter) {
                std::string typed = field;
                typed.erase(std::remove_if(typed.begin(), typed.end(), [](unsigned char c) { return std::isspace(c); }),
                            typed.end());
                if (!typed.empty() && !net::parse_ipv4(typed)) {
                    status = "That isn't an internet address: four numbers with dots, like 203.0.113.5.";
                } else {
                    s.online_address = typed;
                    status.clear();
                    text_input(false);
                    page = Page::Menu;
                }
            }
            break;
        case Page::Lan: {
            const std::vector<net::LanRace> races = search ? search->races() : std::vector<net::LanRace>{};
            const int n = static_cast<int>(races.size());
            if (n == 0)
                break;
            lan_selected = (std::clamp(lan_selected, 0, n - 1) + up_down + n) % n;
            if (enter) {
                const net::LanRace& r = races[static_cast<std::size_t>(lan_selected)];
                if (r.app_version != VETTE_VERSION) {
                    status = r.name + " has VETTE! 2026 " + r.app_version + " and you have " VETTE_VERSION
                             ". You both need the same version.";
                } else if (r.game_build != my_build) {
                    status = r.name + " has another version of the original game: you both need DOS VETTE! 1.1.";
                } else {
                    search.reset();
                    open_join(r.code());
                    joined_from_lan = true;
                    text_input(false);
                    join();
                }
            }
            break;
        }
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
            if (quit_shortcut(e))
                continue;  // (the quit it posts comes next)
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
                if ((page == Page::Joining && !link) || page == Page::Address)
                    field += e.text.text;
                break;
            case SDL_EVENT_KEY_DOWN: {
                const SDL_Scancode k = e.key.scancode;
                const bool enter = (k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER) && !e.key.repeat;
                const bool typing = page == Page::Address || (page == Page::Joining && !link);
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
                } else if (page == Page::Hosting && link && !e.key.repeat &&
                           (k == SDL_SCANCODE_C || k == SDL_SCANCODE_L)) {
                    const net::OnlineStatus st = link->status();
                    const std::string what = k == SDL_SCANCODE_L ? st.lan.code
                                             : !link->invite_url().empty() ? link->invite_url()
                                                                            : st.direct.code;
                    if (!what.empty()) {
                        copy_to_clipboard(what);
                        status = k == SDL_SCANCODE_L ? "Same-network code copied." : "Code copied.";
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

        if (search)
            search->poll();

        // An invite link opened while this screen is up: join it (unless a friend is already connected).
        if (const auto forwarded = take_forwarded_invite()) {
            SDL_RaiseWindow(presenter.window());
            if (!(link && link->status().state == net::LinkState::Connected)) {
                open_join(*forwarded);
                join();
            }
        }

        // The connection: the invite onto the clipboard once it's ready; on to the race once connected.
        if (link) {
            const net::OnlineStatus st = link->status();
            if (page == Page::Hosting && !invite_copied) {
                if (!link->invite_url().empty()) {
                    copy_to_clipboard(link->invite_url());
                    invite_copied = true;
                } else if ((st.direct.state == net::RouteStatus::State::Failed || lan_hosting) &&
                           st.lan.state == net::RouteStatus::State::Ready) {
                    copy_to_clipboard(st.lan.code);
                    invite_copied = true;
                }
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
        canvas.text(sub_x, 26, "Race a friend online or on your network", kSubtitle);
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
                const int y = 64 + row * pitch + (row >= kLanHost ? pitch / 2 : 0) + (row >= kCourse ? pitch / 2 : 0) +
                              (row == kBack ? pitch / 2 : 0);
                const bool sel = row == selected;
                if (sel)
                    canvas.fill_rect(6, y - 3, canvas.width - 12, pitch, kSelection);
                if (row == kCourse || row == kRouter || row == kAddress) {
                    canvas.text(m, y, row_label(row), sel ? kGold : kLabel);
                    const int vx = m + 18 * kGlyph;
                    const size_t room_chars = static_cast<size_t>(std::max(0, (canvas.width - vx - m) / kGlyph));
                    std::string v;
                    std::uint32_t color = kValue;
                    if (row == kCourse) {
                        v = std::string("< ") + course_name(s.online_course) + " >";
                    } else if (row == kRouter) {
                        v = s.online_port_forwarded ? "< Port forwarded by hand >" : "< Opens the port itself (UPnP) >";
                    } else if (!s.online_port_forwarded) {
                        v = "(not needed)";
                        color = kDim;
                    } else {
                        v = s.online_address.empty() ? std::string("not set") : fit_left(s.online_address, room_chars);
                        color = net::parse_ipv4(s.online_address) ? kValue : kBad;
                    }
                    canvas.text(vx, y, v, color);
                } else {
                    canvas.text(m, y, std::string(sel ? "> " : "  ") + row_label(row), sel ? kGold : kValue);
                }
            }
            if (!status.empty())
                lines(64 + kRows * 14 + 27, status, kBad, 2);
            help = std::string(row_help(selected));
            hints = "Up/Down choose   Left/Right change   Enter select   Esc back";
        } else if (page == Page::Hosting) {
            const net::OnlineStatus st = link->status();
            const bool room = st.room.state == net::RouteStatus::State::Ready;
            const bool direct = st.direct.state == net::RouteStatus::State::Ready;
            const bool lan = st.lan.state == net::RouteStatus::State::Ready;
            const bool direct_failed = st.direct.state == net::RouteStatus::State::Failed;
            int y = 60;
            if (lan_hosting) {
                if (lan) {
                    y = lines(y,
                              "Hosting on this network as " + my_name +
                                  ". Your friend chooses Online race > Join a LAN race and picks you, or types this "
                                  "code (it's on your clipboard):",
                              kHelp, 3);
                    canvas.text(m, y + 4, st.lan.code, kGold, 3, true);
                    y += 34;
                } else if (!trouble(st.state)) {
                    canvas.text(m, y, "Getting your race ready...", kHelp);
                    y += 14;
                }
            } else if (room) {
                y = lines(y, "Send your friend this invite link. It's on your clipboard: paste it in a chat.", kHelp, 2);
                canvas.text(m, y + 2, fit_left(link->invite_url(), line_chars), kGood);
                canvas.text(m, y + 16, "Or tell them the code:", kLabel);
                canvas.text(m, y + 28, st.room.code, kGold, 3, true);
                y += 58;
            } else if (direct) {
                y = lines(y, "Send your friend this code. It's on your clipboard: paste it in a chat.", kHelp, 2);
                canvas.text(m, y + 4, st.direct.code, kGold, 3, true);
                y += 34;
            } else if (direct_failed && lan) {
                y = lines(y, st.direct.reason, kBad, 3);
                y = lines(y + 2,
                          "To race over the internet, ask your friend to host, or forward TCP port 26989 to this "
                          "computer in your router's settings and set Router to Port forwarded by hand. A friend on "
                          "the same network can join with this code (on your clipboard):",
                          kHelp, 4);
                canvas.text(m, y + 4, st.lan.code, kGold, 3, true);
                y += 34;
            } else if (!trouble(st.state)) {
                canvas.text(m, y, st.direct.state == net::RouteStatus::State::Starting
                                      ? "Asking your router to open the game's port..."
                                      : "Getting your race ready...",
                            kHelp);
                y += 14;
            }
            if (lan && (direct || room)) {
                canvas.text(m, y, "Friend on the same network? This code: " + st.lan.code, kValue);
                y += 12;
            }
            y += 6;
            y = lines(y, state_text(st), trouble(st.state) ? kBad : kGood, 4);
            if (!status.empty())
                y = lines(y, status, kGood, 1);
            canvas.text(m, y + 6,
                        std::string("Course: ") + course_name(s.online_course) +
                            (st.state != net::LinkState::Connected ? "  (Left/Right to change)" : ""),
                        kValue);
            canvas.text(m, y + 18,
                        std::string("Driving: ") + (s.improved_driving ? "Improved" : "Original") + " (for both of you)",
                        kValue);
            hints = std::string(room || direct ? "C copy code   " : "") + (lan ? "L copy same-network code   " : "") +
                    "Esc cancel";
        } else if (page == Page::Lan) {
            const std::vector<net::LanRace> races = search ? search->races() : std::vector<net::LanRace>{};
            canvas.text(m, 64, "Races on this network:", kLabel);
            int y = 82;
            if (races.empty()) {
                canvas.text(m, y, "Looking...", kGood);
                y = lines(y + 16,
                          "None yet. Your friend chooses Online race > Host a LAN race (or Host a race) on the same "
                          "Wi-Fi or router.",
                          kHelp, 3);
            }
            lan_selected = std::clamp(lan_selected, 0, std::max(0, static_cast<int>(races.size()) - 1));
            const int what_x = m + 26 * kGlyph;
            const size_t what_chars = static_cast<size_t>(std::max(0, (canvas.width - what_x - m) / kGlyph));
            for (size_t i = 0; i < races.size() && i < 8; ++i) {
                const net::LanRace& r = races[i];
                const bool sel = static_cast<int>(i) == lan_selected;
                if (sel)
                    canvas.fill_rect(6, y - 3, canvas.width - 12, 14, kSelection);
                const bool same = r.app_version == VETTE_VERSION && r.game_build == my_build;
                std::string what;
                if (r.app_version != VETTE_VERSION) {
                    what = "VETTE! 2026 " + r.app_version + ": can't join";
                } else if (!same) {
                    what = "another original game: can't join";
                } else if (const auto text = r.settings.get("setup")) {
                    if (const auto setup = game::TwoPlayerSetup::decode(*text))
                        what = std::string(course_name(setup->course)) + ", " +
                               (setup->improved_driving ? "Improved" : "Original") + " driving";
                }
                canvas.text(m, y, std::string(sel ? "> " : "  ") + fit_left(r.name, 22), sel ? kGold : kValue);
                canvas.text(what_x, y, fit_left(what, what_chars), same ? kLabel : kBad);
                y += 14;
            }
            if (!status.empty())
                lines(y + 8, status, kBad, 2);
            help = "Races hosted on the same network as this computer (the same Wi-Fi or router). Not listed? Both "
                   "computers must be on that network, and the host's firewall must let VETTE! 2026 in.";
            hints = races.empty() ? "Esc back" : "Up/Down choose   Enter join   Esc back";
        } else if (page == Page::Joining) {
            canvas.text(m, 64, "Paste the code your friend sent:", kLabel);
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
            canvas.text(m, 64, "Your internet address (for a port forwarded by hand):", kLabel);
            canvas.fill_rect(m - 2, 78, canvas.width - 2 * m + 4, 14, kSelection);
            const size_t room_chars = line_chars > 1 ? line_chars - 1 : 0;
            canvas.text(m, 81, fit_left(field, room_chars) + "_", kValue);
            if (!status.empty())
                lines(100, status, kBad, 2);
            help = std::string(row_help(kAddress));
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

bool forward_invite(const std::string&) { return false; }

void accept_forwarded_invites(const char*) {}

std::optional<std::string> take_forwarded_invite() { return std::nullopt; }

bool leave_for_invite(const std::string&) { return false; }

bool connect_online(const Settings&, const GameDir&, bool, std::string_view, OnlineSession&, std::string& error,
                    double) {
    error = "This copy of VETTE! 2026 was built without online play.";
    return false;
}

bool run_online(Presenter&, Gamepad&, Settings&, const GameDir&, OnlineSession&, std::string_view) { return false; }

}  // namespace vette::ui

#endif
