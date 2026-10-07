#pragma once
// The serial cable straight between two games over TCP, with no server: the host listens, the guest
// connects to it (by a direct code, or to the addresses the host offers through a room), and they carry
// the same messages as through the relay (net/peer_stream.h), so short drops are survived the same way:
// the guest reconnects, and each side resends what the other hasn't acknowledged.
//
// On the wire: frames of [length: u16 little-endian][type: 'T' text or 'B' binary][payload]. The host
// speaks first:
//
//   host  -> {"t":"challenge","proto":1,"nonce":H}
//   guest -> {"t":"hello","proto":1,"app":..,"game":..,"via":"code"|"room","nonce":G,"proof":P,"session":S}
//   host  -> {"t":"welcome","session":..,"resumed":..,"proof":Q,"settings_gen":..,"settings":..}
//            or {"t":"error","code":..,"reason":..}
//
// P = SHA-1(key ":" H ":" G) and Q = SHA-1(key ":" G ":" H ":host"), in hex: both prove they know the key
// (the direct code's secret, or the one the host gave through the room) without sending it, and nothing
// the host says before the guest's proof depends on it. Versions must match, as with the relay. After 20
// wrong proofs the host stops taking guests (someone is guessing). Then: binary frames are the
// MessageHeader messages; text "ping"/"pong" every 2 s, "bye" to leave, and the host's
// {"t":"settings","gen":..,"data":..}.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "host/serial_link.h"
#include "net/protocol.h"
#include "net/session.h"
#include "net/socket.h"

namespace vette::net {

// The handshake's proofs (exposed for the tests).
std::string direct_guest_proof(std::string_view key, std::string_view host_nonce, std::string_view guest_nonce);
std::string direct_host_proof(std::string_view key, std::string_view host_nonce, std::string_view guest_nonce);

// What a room's host offers its guest for going direct: where to connect, and the key to prove.
// As text (a side message through the room): "direct 1\nkey=...\nep=192.168.1.20:26989\nep=...".
struct DirectOffer {
    std::string key;
    std::vector<SocketAddress> endpoints;

    std::string encode() const;
    static std::optional<DirectOffer> decode(std::string_view text);
};

struct DirectHostOptions {
    std::string app_version, game_build;
    RaceSettings race_settings;
    std::uint16_t port = 26989;  // tried first; then the next 9; then any free port
    bool loopback_only = false;  // listen on 127.0.0.1 / ::1 only (tests)
    Batching batching{2000, 8000, 0};  // no server counting messages: no minimum interval
};

// Listens for the friend's game. The first guest that proves one of the allowed keys becomes the peer;
// it may reconnect within the grace period. If it leaves, another may join.
class DirectHost final : public host::SerialLink {
public:
    // Null, with `error`, if it can't listen.
    static std::unique_ptr<DirectHost> start(DirectHostOptions options, std::string& error);
    ~DirectHost() override;
    DirectHost(const DirectHost&) = delete;
    DirectHost& operator=(const DirectHost&) = delete;

    std::uint16_t port() const { return port_; }
    bool ipv6() const { return ipv6_; }  // listening on IPv6 too

    // Keys a guest may prove, by purpose ("code": the direct code; "room": the room's offer).
    void allow(const std::string& purpose, const std::string& key);
    void disallow(const std::string& purpose);

    void send(std::span<const std::uint8_t> bytes) override;
    std::size_t receive(std::span<std::uint8_t> out) override;
    bool connected() const override { return connected_.load(); }

    // Waiting while nobody has joined; Connected; PeerAway; PeerLeft; Failed (too many wrong attempts).
    LinkStatus status() const;
    std::string joined_via() const;  // the purpose of the key the current friend proved
    bool set_race_settings(const RaceSettings& settings);
    std::optional<RaceSettings> race_settings() const;
    int wrong_attempts() const { return wrong_.load(); }

    void close();  // says goodbye to the friend and stops listening
    void simulate_drop();  // testing: drops the friend's connection as a network failure would

private:
    struct Impl;
    DirectHost() = default;
    void run();

    std::unique_ptr<Impl> impl_;
    std::uint16_t port_ = 0;
    bool ipv6_ = false;
    std::atomic<bool> connected_{false}, stop_{false}, drop_{false};
    std::atomic<int> wrong_{0};
    std::thread thread_;
};

struct DirectJoinOptions {
    std::string app_version, game_build;
    std::string purpose = "code";  // which key: "code" or "room"
    std::string key;
    std::vector<SocketAddress> endpoints;  // all tried at once; the first to welcome us wins
    int timeout_ms = 8000;                 // to get in at first
    Batching batching{2000, 8000, 0};
};

class DirectJoin final : public host::SerialLink {
public:
    static std::unique_ptr<DirectJoin> start(DirectJoinOptions options);
    ~DirectJoin() override;
    DirectJoin(const DirectJoin&) = delete;
    DirectJoin& operator=(const DirectJoin&) = delete;

    void send(std::span<const std::uint8_t> bytes) override;
    std::size_t receive(std::span<std::uint8_t> out) override;
    bool connected() const override { return connected_.load(); }

    // Connecting; Connected; Reconnecting; PeerLeft; Failed (reason); Closed.
    LinkStatus status() const;
    std::optional<SocketAddress> endpoint() const;  // the address that answered
    std::optional<RaceSettings> race_settings() const;
    // Why it failed, as advice for the player (empty unless Failed).
    std::string suggestion() const;

    void close();
    // Testing: drops the connection as a network failure would, with the network gone for `offline_ms`.
    void simulate_drop(int offline_ms = 0);

private:
    struct Impl;
    DirectJoin() = default;
    void run();

    std::unique_ptr<Impl> impl_;
    std::atomic<bool> connected_{false}, stop_{false}, drop_{false};
    std::atomic<int> drop_offline_ms_{0};
    std::thread thread_;
};

}  // namespace vette::net
