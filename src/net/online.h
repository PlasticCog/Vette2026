#pragma once
// Online races, by whichever way works: the one place the game hosts or joins (net/room_link.h and
// net/direct.h are the ways underneath).
//
// Hosting offers, at once:
//  - a room code (VETTE-4KQ7) on the relay server, if one is configured, with an invite link;
//  - a direct code (7K3M-QX9P-2HDA): this computer's internet address, with the port opened on the router
//    (UPnP, NAT-PMP or PCP), for joining with no server at all.
// The friend joins with either (or an invite link). Through a room, the games then try to connect
// straight to each other (the host's addresses on its local network, its internet address, IPv6),
// for a moment, before the race starts; if they can, the room is left behind ("direct"), otherwise the
// race goes through the server ("via server"). Either way the link is the game's serial cable.
//
//   auto online = net::OnlineLink::host(options);        // or ::join(options, pasted_text)
//   ... online->status(): codes, how each way in is doing, then Connected and which route
//   ... uart.set_link(online.get())
//
// Hosting listens for connections: the first time, Windows Firewall asks whether to allow the game on
// private and public networks (the direct code needs it allowed; the room code doesn't).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "host/serial_link.h"
#include "net/invite.h"
#include "net/lan.h"
#include "net/protocol.h"
#include "net/session.h"

namespace vette::net {

class DirectHost;
class DirectJoin;
class PortMapper;
class RoomLink;

// The way the serial bytes go between the games.
enum class Route { None, Server, Direct };
const char* to_string(Route route);  // "", "via server", "direct"

// How one way in is doing.
struct RouteStatus {
    enum class State { Off, Starting, Ready, Failed };
    State state = State::Off;
    std::string code;        // Ready: room "VETTE-4KQ7", direct "7K3M-QX9P-2HDA"
    std::string detail;      // e.g. "UPnP, 203.0.113.5:26989"
    std::string log;         // direct, host: what the router was asked and said (for a details view)
    std::string reason;      // Failed: why, in plain English
    std::string suggestion;  // Failed: what to do instead
};

struct OnlineStatus {
    // Connecting; Waiting (host: nobody yet); Connected (the race can start); Reconnecting; PeerAway;
    // PeerLeft; Failed; Closed (net/session.h).
    LinkState state = LinkState::Connecting;
    bool host = false;
    Route route = Route::None;  // once Connected
    std::string activity;       // what's happening, for the screen ("Trying a direct connection...")
    std::string reason, suggestion;  // Failed, PeerLeft, Closed
    double rtt_ms = -1;              // to the friend's game and back, on the route in use (-1: not yet)
    RouteStatus room, direct;        // host: both ways in; guest: the one it's joining by
    RouteStatus lan;                 // host: a direct code with this computer's address on its own network
    LinkStatus link;                 // the route in use: counters, reconnects
};

struct OnlineOptions {
    std::string server_url;  // the relay server (wss://...); empty: no room codes or invite links
    std::string app_version, game_build;  // both players need the same
    RaceSettings race_settings;           // host: what the guest gets before the serial stream starts
    // Host: the ways in.
    bool room = true;         // a room code (if there's a server)
    bool direct_code = true;  // a direct code
    bool go_direct = true;    // through a room, try connecting straight to each other first
    std::uint16_t port = kDirectPort;
    bool manual_port_forward = false;  // the player forwarded the port on the router themselves
    bool use_stun = true;  // ask STUN servers for the internet address when the router doesn't say
    // Guest: how long to try going direct before racing through the server.
    int direct_wait_ms = 2500;
    // Host: make the direct code from this IPv4 address instead of the internet address, with no router
    // involved: for a local network, or tests ("127.0.0.1").
    std::string code_address;
    // Host: only on the local network (a LAN race): the same-network code and the answers to the games
    // looking for races there (net/lan.h), no room, router or internet address.
    bool lan_only = false;
    std::uint16_t lan_port = kLanPort;  // where the LAN questions come (0: answer none)
    // Testing.
    bool loopback_only = false;  // the host listens on loopback only
    std::vector<std::string> offer_endpoints;  // host: offer these addresses instead (e.g. unreachable ones)
    std::string ca_file;         // extra trusted certificates for the server (net/connection.h)
};

class OnlineLink final : public host::SerialLink {
public:
    static std::unique_ptr<OnlineLink> host(OnlineOptions options);
    // By what the friend gave: a room code, a direct code or an invite link (net/invite.h). Something
    // that's none of these gives a link that has Failed, with the reason.
    static std::unique_ptr<OnlineLink> join(OnlineOptions options, std::string_view pasted);

    ~OnlineLink() override;  // leaves (close())
    OnlineLink(const OnlineLink&) = delete;
    OnlineLink& operator=(const OnlineLink&) = delete;

    // host::SerialLink: the race's serial cable, once Connected.
    void send(std::span<const std::uint8_t> bytes) override;
    std::size_t receive(std::span<std::uint8_t> out) override;
    bool connected() const override;

    OnlineStatus status() const;

    // Host: what to give the friend. The invite link through the server if there is one (room code if
    // it's ready, else the direct code), else the direct code itself. Empty until a way in is ready.
    std::string invite_url() const;

    // Host: changes the race settings (the guest gets them in order with the serial stream). Guest: the
    // host's, readable once Connected.
    bool set_race_settings(const RaceSettings& settings);
    std::optional<RaceSettings> race_settings() const;

    void close();
    // Testing: drops the route's connection as a network failure would (net/room_link.h, net/direct.h).
    void simulate_drop(int offline_ms = 0);

private:
    OnlineLink(OnlineOptions options, bool host);
    void run();
    void run_host();
    void run_guest();
    void choose(Route route, host::SerialLink* link);
    void publish_active();

    OnlineOptions options_;
    bool host_ = false;
    std::optional<Invite> invite_;  // guest: what it joins by
    std::string failure_;           // guest: a pasted text that isn't a code

    std::unique_ptr<RoomLink> room_;
    std::unique_ptr<DirectHost> direct_host_;
    std::unique_ptr<DirectJoin> direct_join_;
    std::unique_ptr<PortMapper> mapper_;
    std::atomic<host::SerialLink*> active_{nullptr};
    std::vector<std::thread> cleanups_;  // closing what's no longer needed (removing the port forwarding)

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    OnlineStatus status_;
    std::optional<RaceSettings> settings_;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace vette::net
