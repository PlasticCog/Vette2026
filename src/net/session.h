#pragma once
// One player's place in an online room, as a state machine without I/O or threads: what to send the
// server, what the server's messages mean, the serial stream (net/stream.h) on top, reconnecting
// within the grace period, and round-trip times. net/room_link.h drives it with a real WebSocket on
// its network thread; the tests drive it with a fake one and a fake clock. Times are microseconds.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "net/protocol.h"
#include "net/stream.h"

namespace vette::net {

// When the game's serial bytes go out. The game writes each packet as a burst of bytes; a burst should
// travel as one WebSocket message, since the server's free allowance counts messages (server/README.md).
struct Batching {
    std::int64_t gap_us = 2000;            // a pause this long ends a burst,
    std::int64_t max_hold_us = 8000;       // but no byte waits longer than this for the burst to end,
    std::int64_t min_interval_us = 33333;  // and messages go at least this far apart (30 a second)

    // When bytes queued from `first_us` to `last_us` may go, the previous message having gone at
    // `last_send_us`.
    std::int64_t due(std::int64_t first_us, std::int64_t last_us, std::int64_t last_send_us) const;
};

enum class LinkState {
    Connecting,    // reaching the server, creating or joining the room
    Waiting,       // in the room (host), waiting for the friend to join
    Connected,     // both players are in the room: the serial link is up
    Reconnecting,  // our connection dropped; getting back into the room (the stream resumes intact)
    PeerAway,      // the friend's connection dropped; waiting for them to come back
    PeerLeft,      // the friend left or was lost (reason); a host stays in the room for a new friend
    Failed,        // couldn't get into the room, or lost it for good (reason)
    Closed,        // we left, or the room closed (reason)
};
const char* to_string(LinkState state);

struct LinkStatus {
    LinkState state = LinkState::Connecting;
    bool host = false;
    std::string code;    // "VETTE-4KQ7", once the server has given it
    std::string reason;  // why: PeerLeft, Failed, Closed; while Reconnecting, what dropped
    double rtt_ms = -1;       // round trip to the friend's game and back (smoothed); -1: not measured yet
    double rtt_last_ms = -1;  // the latest measurement
    std::uint64_t rtt_samples = 0;
    double server_rtt_ms = -1;       // round trip to the server
    std::uint32_t settings_gen = 0;  // the race settings' generation; 0: none yet
    std::uint64_t bytes_sent = 0, bytes_received = 0;        // the serial stream (without resends)
    std::uint64_t messages_sent = 0, messages_received = 0;  // binary messages
    std::uint64_t bytes_resent = 0;
    int reconnects = 0;  // connections resumed after a drop
};

// What the session asks of its connection to the server.
class SessionIo {
public:
    virtual ~SessionIo() = default;
    // Open a WebSocket to the server, at this path and query; the driver reports back with on_open()
    // or on_closed(), after the session call that asked has returned.
    virtual void connect(const std::string& target) = 0;
    virtual void send_text(std::string_view text) = 0;
    virtual void send_binary(std::span<const std::uint8_t> message) = 0;
    // Drop the connection at once; no on_closed() follows.
    virtual void disconnect() = 0;
};

struct SessionConfig {
    std::string app_version;     // VETTE! 2026's version: both players need the same
    std::string game_build;      // the original game's version: both players need the same
    RaceSettings race_settings;  // host: what the guest gets before the serial stream starts
    int protocol = kProtocolVersion;  // only for testing the server's refusal
    std::int64_t ping_interval_us = 2'000'000;
    std::int64_t dead_after_us = 7'000'000;  // nothing from the server for this long: reconnect
    std::int64_t ack_delay_us = 100'000;     // received data is acknowledged within this
    std::int64_t resend_request_us = 300'000;
    std::size_t max_payload = 1024;          // serial bytes per message
    std::size_t max_unacked = 256 * 1024;    // more waiting for the friend's acknowledgement: fail
    int connect_attempts = 3;                // to reach the server at first
};

class Session {
public:
    // A new room (host), or the friend's room by its bare code (guest).
    Session(SessionIo& io, SessionConfig config, std::string join_code = {});

    void start(std::int64_t now_us);

    // The connection's events.
    void on_open(std::int64_t now_us);
    void on_text(std::string_view text, std::int64_t now_us);
    void on_binary(std::span<const std::uint8_t> message, std::int64_t now_us);
    void on_closed(std::string_view why, std::int64_t now_us);

    // Timers: pings, acknowledgements, reconnecting, giving up. next_timer() says when tick() is due.
    void tick(std::int64_t now_us);
    std::int64_t next_timer(std::int64_t now_us) const;

    // The game's serial bytes, to the friend. False if too much is waiting for the friend's
    // acknowledgement (they're gone for too long); the link then fails.
    bool write(std::span<const std::uint8_t> bytes, std::int64_t now_us);
    // Appends the friend's serial bytes that have arrived.
    void take_received(std::vector<std::uint8_t>& out);

    // The race settings. Host: replaces its settings; the guest gets them in order with the serial
    // stream (false for a guest, or if the settings are too long). Guest: the host's, from the moment
    // the session is connected (with the welcome, ahead of any serial byte); the host's own for a host.
    bool set_race_settings(const RaceSettings& settings, std::int64_t now_us);
    const std::optional<RaceSettings>& race_settings() const { return settings_; }

    // Leaves the room for good: says goodbye to the server (the driver then closes the connection).
    void leave(std::int64_t now_us);

    const LinkStatus& status() const { return status_; }
    // The friend is in the room (here, or briefly away) and the serial stream is intact.
    bool linked() const;
    bool finished() const { return final_; }
    bool connection_open() const { return conn_ == Conn::Open; }

private:
    enum class Conn { Down, Connecting, Open };
    enum class Peer { None, Here, Away, Left };

    std::string hello() const;
    std::string target() const;
    void welcome(const JsonObject& m, std::int64_t now_us);
    void peer_event(const JsonObject& m, std::int64_t now_us);
    void new_stream();
    bool can_send() const { return conn_ == Conn::Open && welcomed_here_ && peer_ == Peer::Here; }
    void transmit(std::int64_t now_us);
    void send_message(MessageHeader::Kind kind, std::uint8_t flags, std::span<const std::uint8_t> payload,
                      std::int64_t now_us);
    void send_settings();
    void resend(std::int64_t now_us);
    void finish(LinkState state, std::string reason);
    void update_state();

    SessionIo& io_;
    SessionConfig config_;
    std::string join_code_;  // empty: create a room
    std::string room_code_;  // the room's bare code, once welcomed
    std::string token_;      // our seat in the room, for reconnecting

    Conn conn_ = Conn::Down;
    bool welcomed_ = false;       // we have a seat in the room
    bool welcomed_here_ = false;  // ...and the current connection has been welcomed
    bool guest_ = false;
    bool final_ = false;
    Peer peer_ = Peer::None;
    LinkStatus status_;

    ReliableStream stream_;
    std::uint64_t sent_high_ = 0;         // the stream sent so far, resends not counted again
    std::vector<std::uint8_t> received_;  // for the game
    std::vector<std::uint8_t> scratch_;   // a message being built

    std::int64_t grace_us_ = 30'000'000;
    std::int64_t last_rx_us_ = 0;          // anything from the server
    std::int64_t last_ping_us_ = 0;
    std::int64_t ping_sent_us_ = -1;       // the ping awaiting its pong
    std::int64_t reconnect_at_us_ = -1;    // -1: no reconnect planned
    std::int64_t give_up_at_us_ = -1;
    std::int64_t backoff_us_ = 0;
    int failed_attempts_ = 0;

    // Round trip: the friend's newest clock reading and when it arrived; the echo last measured.
    std::uint32_t peer_ts_ = 0;
    std::int64_t peer_ts_at_us_ = -1;
    std::uint32_t last_echo_ = 0;
    bool have_echo_ = false;

    // Acknowledgements: when we must tell the friend how much has arrived (unless a message does first).
    std::int64_t ack_due_us_ = -1;
    std::int64_t resend_requested_us_ = -1;  // when we last asked the friend to resend
    std::uint64_t resend_requested_at_ = 0;  // ...from this position
    std::int64_t rewound_us_ = -1;           // when we last resent on the friend's request

    std::optional<RaceSettings> settings_;
    std::uint32_t settings_gen_ = 0;  // host: our newest; guest: the newest received
};

}  // namespace vette::net
