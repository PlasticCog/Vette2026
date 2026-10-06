#pragma once
// Online two-player races: the serial cable between two games, carried through a room on the relay
// server (server/README.md). The host creates a room and gets a code like VETTE-4KQ7 to tell a friend;
// the friend joins with it. From then on, what one game's UART sends arrives at the other's, in order
// and intact, through short network drops (reconnecting within the server's grace period).
//
//   auto link = vette::net::RoomLink::create_room(options);   // or join_room(options, "VETTE-4KQ7")
//   ... link->status().code, .state; uart.set_link(link.get()) once connected
//
// A network thread does all the I/O. send() and receive() (the SerialLink side, from the emulation
// thread) only touch queues; status() and the rest may be called from any thread.

#include <atomic>
#include <cstddef>
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
#include "net/protocol.h"
#include "net/session.h"

namespace vette::net {

class Waker;

struct RoomOptions {
    // The relay server: wss://<name>.<account>.workers.dev (or ws://127.0.0.1:8787 for `wrangler dev`).
    std::string server_url;
    std::string app_version;  // VETTE! 2026's version: both players need the same
    std::string game_build;   // the original game's version and build: both players need the same
    // Host: the settings the guest receives before the serial stream starts (net/protocol.h).
    RaceSettings race_settings;
    // Extra trusted certificates (PEM), for a test server with its own CA. Normally empty: the system's.
    std::string ca_file;
    Batching batching;  // how the game's serial bytes are grouped into messages
    int protocol = kProtocolVersion;  // only for testing the server's refusal
};

class RoomLink final : public host::SerialLink {
public:
    // Creates a room (this player is the host). Returns at once: status() shows the progress, and the
    // code once the server has given it (state Waiting).
    static std::unique_ptr<RoomLink> create_room(RoomOptions options);
    // Joins the friend's room by its code, as typed ("VETTE-4KQ7", "4kq7"). A code that can't be one
    // gives a link that has Failed, with the reason.
    static std::unique_ptr<RoomLink> join_room(RoomOptions options, std::string_view code);

    ~RoomLink() override;  // leaves the room (close())
    RoomLink(const RoomLink&) = delete;
    RoomLink& operator=(const RoomLink&) = delete;

    // host::SerialLink, for the emulation thread.
    void send(std::span<const std::uint8_t> bytes) override;
    std::size_t receive(std::span<std::uint8_t> out) override;
    // True while the friend is in the room and the byte stream is intact, including a reconnect in
    // progress on either side; false before the friend joins and after they're gone.
    bool connected() const override;

    LinkStatus status() const;
    double rtt_ms() const { return status().rtt_ms; }  // to the friend's game and back; -1: unknown

    // Race settings. Host: changes them; the guest gets the change in order with the serial stream.
    // False for a guest. Guest: the host's, readable once connected() (they arrive with the welcome,
    // ahead of any serial byte); status().settings_gen counts changes. Host: its own.
    bool set_race_settings(const RaceSettings& settings);
    std::optional<RaceSettings> race_settings() const;

    // Drops the friend's bytes received so far (that the game hasn't read), e.g. before its serial
    // port is opened, as a cable would not have delivered them.
    void discard_received();

    // Leaves the room for good (a host's room closes; a guest's seat is freed). status(): Closed.
    void close();

    // Testing: drops the connection as a network failure would (no goodbye), with the network gone for
    // `offline_ms` more; the link then reconnects.
    void simulate_drop(int offline_ms = 0);

private:
    class Io;

    RoomLink(RoomOptions options, std::string join_code, std::string failure);
    void run();
    void publish(const Session& session);

    RoomOptions options_;
    std::string join_code_;

    // Emulation thread -> network thread.
    std::mutex tx_mutex_;
    std::vector<std::uint8_t> tx_;
    std::int64_t tx_first_us_ = 0, tx_last_us_ = 0;

    // Network thread -> emulation thread.
    std::mutex rx_mutex_;
    std::vector<std::uint8_t> rx_;
    std::size_t rx_pos_ = 0;

    // Shared state and requests.
    mutable std::mutex state_mutex_;
    LinkStatus status_;
    std::optional<RaceSettings> settings_;
    std::optional<RaceSettings> new_settings_;  // requested by set_race_settings()
    std::atomic<bool> connected_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> drop_{false};
    std::atomic<int> drop_offline_ms_{0};

    std::unique_ptr<Waker> waker_;
    std::thread thread_;
};

}  // namespace vette::net
