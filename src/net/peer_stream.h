#pragma once
// The serial cable's bytes between the two games, over whatever carries messages between them (the relay
// server's room, or a direct TCP connection): each game's stream kept intact across reconnects
// (net/stream.h), acknowledgements, resend requests, round-trip times from the echoed clocks
// (net/protocol.h, MessageHeader), and side messages (short texts outside the stream, e.g. a direct
// connection's offer). No I/O: messages go out through `send`. Times are microseconds.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "net/protocol.h"
#include "net/stream.h"

namespace vette::net {

struct LinkStatus;

struct PeerStreamConfig {
    std::size_t max_payload = 1024;        // serial bytes per message
    std::size_t max_unacked = 256 * 1024;  // more waiting for the other game's acknowledgement: overflow
    std::int64_t ack_delay_us = 100'000;   // received data is acknowledged within this
    std::int64_t resend_request_us = 300'000;
};

class PeerStream {
public:
    using Send = std::function<void(std::span<const std::uint8_t>)>;

    PeerStream(PeerStreamConfig config, Send send) : config_(config), send_(std::move(send)), stream_(config.max_unacked) {}

    // Whether messages can reach the other game now. Opening sends what's waiting.
    void set_open(bool open, std::int64_t now_us);
    bool open() const { return open_; }

    // The game's bytes; false if too much is waiting for the other game's acknowledgement.
    bool write(std::span<const std::uint8_t> bytes, std::int64_t now_us);
    // A binary message from the other game.
    void on_message(std::span<const std::uint8_t> message, std::int64_t now_us);
    // After a reconnect (either side's): resend what isn't acknowledged and tell the other game where
    // ours is, so it does the same.
    void resume(std::int64_t now_us);
    // A new peer: both directions start over (statistics are kept).
    void reset();

    void tick(std::int64_t now_us);  // acknowledgements due
    std::int64_t next_timer() const { return ack_due_us_ < 0 ? INT64_MAX : ack_due_us_; }

    // A short text outside the stream; dropped unless open() (the receiver can't tell it was lost).
    bool send_side(std::string_view text, std::int64_t now_us);

    void take_received(std::vector<std::uint8_t>& out);
    void take_side(std::vector<std::string>& out);
    bool has_received() const { return !received_.empty(); }

    // Copies the counters and round-trip times into a status.
    void fill(LinkStatus& status) const;

private:
    void transmit(std::int64_t now_us);
    void send_message(MessageHeader::Kind kind, std::uint8_t flags, std::span<const std::uint8_t> payload,
                      std::int64_t now_us);
    void resend(std::int64_t now_us);

    PeerStreamConfig config_;
    Send send_;
    bool open_ = false;
    ReliableStream stream_;
    std::uint64_t sent_high_ = 0;  // the stream sent so far, resends not counted again
    std::vector<std::uint8_t> received_;
    std::vector<std::string> side_;
    std::vector<std::uint8_t> scratch_;

    // Round trip: the other game's newest clock reading and when it arrived; the echo last measured.
    std::uint32_t peer_ts_ = 0;
    std::int64_t peer_ts_at_us_ = -1;
    std::uint32_t last_echo_ = 0;
    bool have_echo_ = false;

    std::int64_t ack_due_us_ = -1;
    std::int64_t resend_requested_us_ = -1;  // when we last asked for a resend
    std::uint64_t resend_requested_at_ = 0;  // ...from this position
    std::int64_t rewound_us_ = -1;           // when we last resent on request

    struct {
        double rtt_ms = -1, rtt_last_ms = -1;
        std::uint64_t rtt_samples = 0;
        std::uint64_t bytes_sent = 0, bytes_received = 0, messages_sent = 0, messages_received = 0;
        std::uint64_t bytes_resent = 0;
    } stats_;
};

}  // namespace vette::net
