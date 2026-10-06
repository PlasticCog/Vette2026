#pragma once
// The launch menu's online race: host a race (a room on the relay server, with a code like VETTE-4KQ7 to
// tell a friend) or join a friend's by its code (server/README.md, net/room_link.h). It returns once both
// players are in the room; the game then takes both into the original's two-player race
// (game/two_player.h). Keyboard or gamepad (the D-pad picks the code's letters).

#include <memory>
#include <string>
#include <string_view>

#include "core/game_dir.h"
#include "core/settings.h"
#include "game/two_player.h"
#include "host/serial_link.h"

namespace vette {
class Gamepad;
class Presenter;
}  // namespace vette
namespace vette::net {
class RoomLink;
}

namespace vette::ui {

// Both players in the room, ready to race.
struct OnlineSession {
    std::unique_ptr<host::SerialLink> link;  // the room (a net::RoomLink): the game's serial cable
    net::RoomLink* room = nullptr;           // the same, for its status
    bool host = false;
    game::TwoPlayerSetup setup;  // the host's choice: the course and the driving physics, for both
};

// False when this build has no online play (built without libcurl).
bool online_available();

// Without the menu (--online-host / --online-join, for testing): creates the room (logging its code) or
// joins it by `code`, and waits until both players are in it. False, with the reason in `error`, if that
// fails or takes longer than `timeout_s`.
bool connect_online(const Settings& settings, const GameDir& game, bool host, std::string_view code,
                    OnlineSession& session, std::string& error, double timeout_s = 300);

// Runs the online race screen. True: `session` is ready (the race can start); false: the player went
// back to the launch menu. Edits the server address and the host's course in `settings`.
bool run_online(Presenter& presenter, Gamepad& gamepad, Settings& settings, const GameDir& game,
                OnlineSession& session);

}  // namespace vette::ui
