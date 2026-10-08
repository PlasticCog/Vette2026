#pragma once
// The launch menu's online race: host a race, or join a friend's by the invite link or code they send
// (net/online.h: a room on the relay server, with an invite link, or a direct code needing no server;
// through a room the games connect straight to each other when they can). It returns once both players
// are connected; the game then takes both into the original's two-player race (game/two_player.h).
// Keyboard or gamepad.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "core/game_dir.h"
#include "core/settings.h"
#include "game/city_map.h"
#include "game/race_intro.h"
#include "game/two_player.h"
#include "host/serial_link.h"

namespace vette {
class Gamepad;
class Presenter;
}  // namespace vette
namespace vette::net {
class OnlineLink;
}

namespace vette::ui {

// Both players connected, ready to race.
struct OnlineSession {
    std::unique_ptr<host::SerialLink> link;  // a net::OnlineLink
    net::OnlineLink* online = nullptr;       // the same, for its status
    // The game's serial cable: `link` with the two games' intros (game/race_intro.h) already exchanged.
    std::unique_ptr<game::IntroLink> cable;
    bool host = false;
    game::TwoPlayerSetup setup;  // the host's choice: the course and the driving physics, for both
    std::string friend_name;     // the other player's name
    std::optional<game::CityMap> map;  // the host's city, for both (none: the original's)
    std::string map_name;
};

// A player's name as shown: theirs, or "Player" if they haven't chosen one.
std::string shown_name(std::string_view name);

// False when this build has no online play (built without libcurl).
bool online_available();

// Invite links that start a new copy of the program while a game runs (net/instance.h): the first copy
// takes them. Call accept_forwarded_invites() at the start, with argv[0]; take_forwarded_invite() each
// frame. leave_for_invite() makes way for a new copy of the program that joins `link` (the caller then
// quits); false if it couldn't start one.
bool forward_invite(const std::string& link);  // true: a running game took it
void accept_forwarded_invites(const char* program);
std::optional<std::string> take_forwarded_invite();
bool leave_for_invite(const std::string& link);

// Runs the online race screen. True: `session` is ready (the race can start); false: the player went
// back. Edits the player's name, the host's course and the other online choices in `settings`. A host
// that plays a map of its own (`settings.map_name`, in `maps_dir`) races its friend in it. `join_now`: an
// invite link or code to join straight away (a vette2026:// link the game was opened with).
bool run_online(Presenter& presenter, Gamepad& gamepad, Settings& settings, const GameDir& game,
                const std::filesystem::path& maps_dir, OnlineSession& session, std::string_view join_now = {});

// Without the menu (--online-host / --online-join, for testing): hosts (logging the codes) or joins by
// `code` (any code or invite link), and waits until both players are connected. False, with the reason
// in `error`, if that fails or takes longer than `timeout_s`.
bool connect_online(const Settings& settings, const GameDir& game, const std::filesystem::path& maps_dir, bool host,
                    std::string_view code, OnlineSession& session, std::string& error, double timeout_s = 300);

}  // namespace vette::ui
