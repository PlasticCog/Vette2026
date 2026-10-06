#pragma once
// One direction of the serial link kept intact across reconnects: the sender keeps every byte until
// the other game acknowledges it, and resends from the acknowledged position after a reconnect; the
// receiver places each message's bytes by their stream offset, so it drops what it already has and
// notices what's missing. Positions are 64-bit here and travel as their low 32 bits
// (net/protocol.h, MessageHeader), which is unambiguous within 2 GiB of each other.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vette::net {

class ReliableStream {
public:
    explicit ReliableStream(std::size_t max_unacked = 256 * 1024) : max_unacked_(max_unacked) {}

    // --- Sending ---
    // Queues the game's bytes; false (and nothing queued) if more than max_unacked bytes would be
    // waiting for the other game's acknowledgement.
    bool write(std::span<const std::uint8_t> bytes);
    std::uint64_t written() const { return acked_ + buf_.size() - head_; }
    std::uint64_t acked() const { return acked_; }
    std::uint64_t sent() const { return sent_; }  // transmitted (since the last rewind)
    std::size_t unsent() const { return static_cast<std::size_t>(written() - sent_); }
    // The next bytes to transmit (from sent()), at most `max`; mark_sent() once they're on their way.
    std::span<const std::uint8_t> unsent_bytes(std::size_t max) const;
    void mark_sent(std::size_t n);
    // The other game has received our stream up to `ack`; false if that's impossible (beyond what was
    // written). Stale acknowledgements are ignored.
    bool on_ack(std::uint32_t ack);
    // Transmits everything not yet acknowledged again (after a reconnect, or when asked to).
    void rewind() { sent_ = acked_; }

    // --- Receiving ---
    enum class Received { Applied, Duplicate, Gap };
    // Places a data message's payload at `offset`. Applied: `fresh` is the part not seen before (maybe
    // all of it). Duplicate: nothing new. Gap: it starts beyond what has arrived (something before it
    // is missing); nothing is taken and the sender must resend from received().
    Received receive(std::uint32_t offset, std::span<const std::uint8_t> payload,
                     std::span<const std::uint8_t>& fresh);
    std::uint64_t received() const { return received_; }

    // Both directions start over (a new peer).
    void reset();

private:
    std::size_t max_unacked_;
    std::vector<std::uint8_t> buf_;  // unacknowledged bytes from buf_[head_], at stream position acked_
    std::size_t head_ = 0;
    std::uint64_t acked_ = 0, sent_ = 0, received_ = 0;
};

}  // namespace vette::net
