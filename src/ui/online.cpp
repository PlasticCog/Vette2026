#include "ui/online.h"

#ifdef VETTE_ONLINE

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "core/crc32.h"
#include "net/protocol.h"
#include "net/room_link.h"
#include "platform/gamepad.h"
#include "platform/presenter.h"
#include "ui/canvas.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace vette::ui {
namespace {

using namespace theme;

enum Row { kHost, kJoin, kCourse, kServer, kBack, kRows };
enum class Page { Menu, Hosting, Code, Server };

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

std::string_view row_help(int row) {
    switch (row) {
    case kHost:
        return "Start a race and get a code to tell your friend. The race begins as soon as they join. Your course "
               "and your Driving setting are used for both of you.";
    case kJoin: return "Type the code your friend got when they started a race.";
    case kCourse: return "The course you race on when you host.";
    case kServer:
        return "The relay server's address (wss://...workers.dev). See server/README.md to set one up. Both "
               "players need the same server.";
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

std::string state_text(const net::LinkStatus& st) {
    switch (st.state) {
    case net::LinkState::Connecting: return "Connecting to the server...";
    case net::LinkState::Waiting: return "Waiting for your friend...";
    case net::LinkState::Connected: return "Connected. Starting the race...";
    case net::LinkState::Reconnecting: return "The connection dropped. Reconnecting...";
    case net::LinkState::PeerAway: return "Your friend's connection dropped. Waiting for them...";
    case net::LinkState::PeerLeft:
        return st.host ? "Your friend left. Waiting for someone to join..." : "Your friend left. " + st.reason;
    case net::LinkState::Failed: return st.reason.empty() ? "Couldn't connect." : st.reason;
    case net::LinkState::Closed: return st.reason.empty() ? "The room closed." : st.reason;
    }
    return {};
}

bool trouble(net::LinkState state) {
    return state == net::LinkState::Failed || state == net::LinkState::Closed || state == net::LinkState::PeerLeft;
}

net::RoomOptions room_options(const Settings& s, const GameDir& game) {
    net::RoomOptions o;
    o.server_url = s.online_server;
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

// The guest's race: the host's setup, from the room; nullopt if it's not one this version knows.
std::optional<game::TwoPlayerSetup> guest_setup(const net::RoomLink& room) {
    const auto settings = room.race_settings();
    const auto text = settings ? settings->get("setup") : std::nullopt;
    return text ? game::TwoPlayerSetup::decode(*text) : std::nullopt;
}

constexpr const char* kSetupMismatch = "Your friend's VETTE! 2026 sets up the race differently. Use the same version.";

}  // namespace

using namespace theme;

bool online_available() { return true; }

bool connect_online(const Settings& s, const GameDir& game, bool host, std::string_view code, OnlineSession& session,
                    std::string& error, double timeout_s) {
    if (s.online_server.empty()) {
        error = "No server: set it in the launch menu (Online race, Server) or with --online-server.";
        return false;
    }
    std::unique_ptr<net::RoomLink> room;
    if (host) {
        net::RoomOptions o = room_options(s, game);
        o.race_settings.set("setup", host_setup(s).encode());
        room = net::RoomLink::create_room(std::move(o));
    } else {
        room = net::RoomLink::join_room(room_options(s, game), code);
    }
    const std::uint64_t deadline = SDL_GetTicksNS() + static_cast<std::uint64_t>(timeout_s * 1e9);
    net::LinkState shown = net::LinkState::Closed;
    for (;;) {
        const net::LinkStatus st = room->status();
        if (st.state != shown) {
            shown = st.state;
            SDL_Log("Online race%s%s: %s", st.code.empty() ? "" : " ", st.code.c_str(), state_text(st).c_str());
        }
        if (st.state == net::LinkState::Connected)
            break;
        if (st.state == net::LinkState::Failed || st.state == net::LinkState::Closed ||
            (!host && st.state == net::LinkState::PeerLeft)) {
            error = state_text(st);
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
    } else if (const auto decoded = guest_setup(*room)) {
        session.setup = *decoded;
    } else {
        error = kSetupMismatch;
        return false;
    }
    session.room = room.get();
    session.link = std::move(room);
    return true;
}

bool run_online(Presenter& presenter, Gamepad& gamepad, Settings& s, const GameDir& game, OnlineSession& session) {
    Page page = Page::Menu;
    int selected = kHost;
    std::string status;  // a problem to show on the menu
    std::unique_ptr<net::RoomLink> room;
    std::uint64_t connected_at = 0;  // when the friend was there (the race starts a moment later)
    std::string code(net::kCodeLength, ' ');  // the code being typed
    std::size_t code_pos = 0;
    std::string server_edit;
    Canvas canvas;
    std::vector<std::uint8_t> pad_keys;
    s.online_course = std::clamp(s.online_course, 1, 4);

    const auto setup = [&] { return host_setup(s); };
    const auto options = [&] { return room_options(s, game); };
    const auto text_input = [&](bool on) {
        if (on)
            SDL_StartTextInput(presenter.window());
        else
            SDL_StopTextInput(presenter.window());
    };
    const auto leave_room = [&] {
        room.reset();  // leaves the room
        connected_at = 0;
    };
    const auto host = [&] {
        if (s.online_server.empty()) {
            status = "Set the server address first.";
            selected = kServer;
            return;
        }
        net::RoomOptions o = options();
        o.race_settings.set("setup", setup().encode());
        room = net::RoomLink::create_room(std::move(o));
        page = Page::Hosting;
    };
    const auto join = [&] {
        const std::optional<std::string> typed = net::normalize_room_code(code);
        if (!typed) {
            status = "Type all " + std::to_string(net::kCodeLength) + " letters of the code.";
            return;
        }
        leave_room();
        status.clear();
        room = net::RoomLink::join_room(options(), *typed);
    };
    // The guest's race: the host's setup, from the room. False (with the reason in `status`) if it's
    // not one this version knows.
    const auto take_setup = [&](game::TwoPlayerSetup& out) {
        const auto decoded = guest_setup(*room);
        if (!decoded) {
            status = kSetupMismatch;
            return false;
        }
        out = *decoded;
        return true;
    };
    const auto activate = [&](int row, int dir) {
        status.clear();
        switch (row) {
        case kHost: host(); break;
        case kJoin:
            if (s.online_server.empty()) {
                status = "Set the server address first.";
                selected = kServer;
                break;
            }
            page = Page::Code;
            code.assign(net::kCodeLength, ' ');
            code_pos = 0;
            text_input(true);
            break;
        case kCourse: s.online_course = (s.online_course - 1 + dir + 4) % 4 + 1; break;
        case kServer:
            page = Page::Server;
            server_edit = s.online_server;
            text_input(true);
            break;
        default: break;
        }
    };
    // Esc, Back or the B button. True: back to the launch menu.
    const auto back = [&] {
        switch (page) {
        case Page::Menu:
            if (selected == kBack)
                return true;
            selected = kBack;
            return false;
        case Page::Hosting: leave_room(); page = Page::Menu; return false;
        case Page::Code:
            leave_room();
            text_input(false);
            page = Page::Menu;
            return false;
        case Page::Server: text_input(false); page = Page::Menu; return false;
        }
        return false;
    };
    const auto type_code = [&](char c) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (net::kCodeAlphabet.find(c) == std::string_view::npos || room)
            return;
        code[code_pos] = c;
        code_pos = std::min(code_pos + 1, net::kCodeLength - 1);
    };
    const auto cycle_code = [&](int dir) {  // the D-pad's way of picking a letter
        if (room)
            return;
        const std::string_view a = net::kCodeAlphabet;
        const std::size_t at = a.find(code[code_pos]);
        const auto n = static_cast<int>(a.size());
        const int next = at == std::string_view::npos ? (dir > 0 ? 0 : n - 1) : (static_cast<int>(at) + dir + n) % n;
        code[code_pos] = a[static_cast<std::size_t>(next)];
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
        case Page::Code:
            if (up_down)
                cycle_code(-up_down);
            if (left_right && !room)
                code_pos = static_cast<std::size_t>(
                    std::clamp(static_cast<int>(code_pos) + left_right, 0, static_cast<int>(net::kCodeLength) - 1));
            if (enter && !room)
                join();
            break;
        case Page::Server:
            if (enter) {
                s.online_server = normalize_server(server_edit);
                text_input(false);
                page = Page::Menu;
            }
            break;
        case Page::Hosting:
            if (left_right && room && room->status().state == net::LinkState::Waiting) {
                s.online_course = (s.online_course - 1 + left_right + 4) % 4 + 1;
                net::RaceSettings settings;
                settings.set("setup", setup().encode());
                room->set_race_settings(settings);
            }
            break;
        }
    };

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
            case SDL_EVENT_TEXT_INPUT:
                if (page == Page::Code) {
                    for (const char* c = e.text.text; *c; ++c)
                        type_code(*c);
                } else if (page == Page::Server) {
                    server_edit += e.text.text;
                }
                break;
            case SDL_EVENT_KEY_DOWN: {
                const SDL_Scancode k = e.key.scancode;
                const bool enter = (k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER) && !e.key.repeat;
                if (k == SDL_SCANCODE_ESCAPE && !e.key.repeat) {
                    done = back();
                } else if (k == SDL_SCANCODE_UP) {
                    key(-1, 0, false);
                } else if (k == SDL_SCANCODE_DOWN || (k == SDL_SCANCODE_TAB && page == Page::Menu)) {
                    key(1, 0, false);
                } else if (k == SDL_SCANCODE_LEFT || k == SDL_SCANCODE_RIGHT) {
                    key(0, k == SDL_SCANCODE_LEFT ? -1 : 1, false);
                } else if (enter || (k == SDL_SCANCODE_SPACE && page == Page::Menu && !e.key.repeat)) {
                    key(0, 0, true);
                } else if (k == SDL_SCANCODE_BACKSPACE) {
                    if (page == Page::Server && !server_edit.empty()) {
                        server_edit.pop_back();
                    } else if (page == Page::Code && !room) {
                        if (code[code_pos] == ' ' && code_pos > 0)
                            --code_pos;
                        code[code_pos] = ' ';
                    }
                } else if (k == SDL_SCANCODE_V && (e.key.mod & SDL_KMOD_CTRL)) {
                    if (char* clip = SDL_GetClipboardText()) {
                        if (page == Page::Server) {
                            server_edit += clip;
                        } else if (page == Page::Code && !room) {
                            if (const auto pasted = net::normalize_room_code(clip)) {
                                code = *pasted;
                                code_pos = net::kCodeLength - 1;
                            }
                        }
                        SDL_free(clip);
                    }
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

        // The room: on to the race once the friend is there.
        if (room) {
            const net::LinkStatus st = room->status();
            if (st.state == net::LinkState::Connected) {
                const std::uint64_t now = SDL_GetTicksNS();
                if (!connected_at)
                    connected_at = now;
                if (now - connected_at >= kStartDelayNs) {
                    session.host = page == Page::Hosting;
                    session.setup = setup();
                    if (session.host || take_setup(session.setup)) {
                        session.room = room.get();
                        session.link = std::move(room);
                        text_input(false);
                        return true;
                    }
                    leave_room();  // the host's setup is one this version doesn't know (status says why)
                }
            } else {
                connected_at = 0;
                if (page == Page::Code && trouble(st.state)) {  // a failed join: let the player try again
                    status = state_text(st);
                    leave_room();
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
        std::string help;
        std::string hints;
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
                    const std::string v = row == kCourse ? std::string("< ") + course_name(s.online_course) + " >"
                                          : s.online_server.empty() ? std::string("not set")
                                                                    : fit_left(s.online_server, room_chars);
                    canvas.text(vx, y, v, row == kServer && s.online_server.empty() ? kBad : kValue);
                } else {
                    canvas.text(m, y, std::string(sel ? "> " : "  ") + row_label(row), sel ? kGold : kValue);
                }
            }
            if (!status.empty())
                canvas.text(m, 64 + kRows * 14 + 20, fit_left(status, line_chars), kBad);
            help = std::string(row_help(selected));
            hints = "Up/Down choose   Left/Right change   Enter select   Esc back";
        } else if (page == Page::Hosting) {
            const net::LinkStatus st = room->status();
            canvas.text(m, 64, "Your race's code:", kLabel);
            const std::string shown = st.code.empty() ? "VETTE-...." : st.code;
            canvas.text(m, 80, shown, kGold, 3, true);
            const std::vector<std::string> tell =
                wrap("Tell your friend this code. They choose Online race, Join a race, and type it.", line_chars);
            for (size_t i = 0; i < tell.size() && i < 2; ++i)
                canvas.text(m, 112 + static_cast<int>(i) * 10, tell[i], kHelp);
            canvas.text(m, 136, fit_left(state_text(st), line_chars), trouble(st.state) ? kBad : kGood);
            canvas.text(m, 156,
                        std::string("Course: ") + course_name(s.online_course) + (st.state == net::LinkState::Waiting
                                                                                      ? "  (Left/Right to change)"
                                                                                      : ""),
                        kValue);
            canvas.text(m, 168, std::string("Driving: ") + (s.improved_driving ? "Improved" : "Original") +
                                    " (for both of you)",
                        kValue);
            hints = "Esc cancel";
        } else if (page == Page::Code) {
            canvas.text(m, 64, "Your friend's code:", kLabel);
            const int cx = m + text_width(std::string(net::kCodePrefix), 3);
            canvas.text(m, 80, std::string(net::kCodePrefix), kGold, 3, true);
            for (std::size_t i = 0; i < net::kCodeLength; ++i) {
                const int x = cx + static_cast<int>(i) * text_width("W", 3);
                if (i == code_pos && !room)
                    canvas.fill_rect(x - 1, 78, text_width("W", 3), 26, kSelection);
                canvas.text(x, 80, code[i] == ' ' ? "_" : std::string(1, code[i]), kGold, 3, true);
            }
            if (room) {
                const net::LinkStatus st = room->status();
                canvas.text(m, 136, fit_left(state_text(st), line_chars), trouble(st.state) ? kBad : kGood);
            } else if (!status.empty()) {
                canvas.text(m, 136, fit_left(status, line_chars), kBad);
            }
            help = "Type the 4 letters after VETTE-, or pick each one with Up/Down. Enter joins.";
            hints = room ? "Esc cancel" : "Type or Up/Down letter   Left/Right move   Enter join   Esc back";
        } else {
            canvas.text(m, 64, "Relay server address:", kLabel);
            canvas.fill_rect(m - 2, 78, canvas.width - 2 * m + 4, 14, kSelection);
            const size_t room_chars = line_chars > 1 ? line_chars - 1 : 0;
            canvas.text(m, 81, fit_left(server_edit, room_chars) + "_", kValue);
            help = "Paste (Ctrl+V) or type the address of your relay server, e.g. "
                   "wss://vette2026-relay.yourname.workers.dev. Enter saves.";
            hints = "Type or Ctrl+V paste   Backspace delete   Enter save   Esc cancel";
        }
        const int rule_y = canvas.height - 64;
        canvas.fill_rect(m, rule_y, canvas.width - 2 * m, 1, kRule);
        const std::vector<std::string> lines = wrap(help, line_chars);
        for (size_t i = 0; i < lines.size() && i < 3; ++i)
            canvas.text(m, rule_y + 8 + static_cast<int>(i) * 12, lines[i], kHelp);
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

bool run_online(Presenter&, Gamepad&, Settings&, const GameDir&, OnlineSession&) { return false; }

}  // namespace vette::ui

#endif
