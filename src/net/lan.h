#pragma once
// Races on the local network (LAN). A game looking for one broadcasts a question to UDP port 26990 on
// each network it's on; every hosting game answers with its race: the port its game listens on, the
// secret of its same-network code, the computer's name, the versions and the race settings. The asker
// joins with that code (net/invite.h), made from the answer's sender address. Nothing leaves the local
// network (routers don't pass broadcasts on), and only the hosts listen for anything: an answer comes
// back to the port the question left from, which firewalls let through.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/protocol.h"
#include "net/socket.h"

namespace vette::net {

inline constexpr std::uint16_t kLanPort = 26990;

struct LanRace {
    SocketAddress host;        // the host's address on the network, with the port its game listens on
    std::uint32_t secret = 0;  // its same-network code's
    std::string name;          // the host computer's name
    std::string app_version, game_build;
    RaceSettings settings;
    std::int64_t heard_ms = 0;  // LanSearch: when it last answered

    // The same-network code to join with: this address and port, and the secret.
    std::string code() const;
};

// The messages (for tests).
std::string lan_question();
std::string lan_answer(const LanRace& race);
// An answer that came from `from`: the race at that address; nullopt if it isn't one.
std::optional<LanRace> parse_lan_answer(std::string_view message, const SocketAddress& from);

// The host's side: answers the questions on `port`.
class LanHost {
public:
    // False (with `error`) if the port can't be had; the race can still be joined by its code.
    // `loopback_only`: questions from this computer only (tests).
    bool start(std::string& error, std::uint16_t port = kLanPort, bool loopback_only = false);
    void set_race(const LanRace& race);  // what to answer (its address isn't used: the asker sees it)
    void poll();                         // answers the questions waiting; never blocks
    bool running() const { return socket_.valid(); }

private:
    Socket socket_;
    std::string answer_;
};

// The asking side: asks about once a second on every network, and keeps the answers.
class LanSearch {
public:
    bool start(std::string& error, std::uint16_t port = kLanPort);
    void poll();  // asks again when it's time, takes the answers; never blocks
    // The races that answered in the last few seconds, in the order first heard.
    std::vector<LanRace> races() const;

private:
    void ask();
    std::uint16_t port_ = kLanPort;
    std::vector<Socket> sockets_;  // every interface, then one per network (bound to its address)
    std::vector<LanRace> races_;
    std::int64_t next_ask_ms_ = 0;
};

}  // namespace vette::net
