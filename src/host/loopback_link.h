#pragma once
// SerialLinks without a network, for tests and tools: a cable between two machines in one process, and
// a decorator that delays what arrives (network latency, jitter and stalls, in order).

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "host/serial_link.h"

namespace vette::host {

// The two ends of a cable: what one end sends, the other receives (at once). Both are connected while
// the cable is plugged in (the default); unplugged, sent bytes are lost, as on a real cable.
class LoopbackCable {
public:
    LoopbackCable();
    ~LoopbackCable();
    LoopbackCable(const LoopbackCable&) = delete;
    LoopbackCable& operator=(const LoopbackCable&) = delete;

    SerialLink& end(int i);
    void set_plugged(bool on) { plugged_ = on; }
    bool plugged() const { return plugged_; }

private:
    class End;
    std::array<std::unique_ptr<End>, 2> ends_;
    bool plugged_ = true;
};

// Delays the bytes arriving through `inner` as a network would: each chunk the peer sent (each of its
// send() calls) arrives `delay` plus a random 0..`jitter` later, but never before the chunk sent before
// it, so a slow one holds back those behind it and they then arrive together (bursts). With `stalls`
// per second (on average), the line also stops for up to `stall` at random moments. Times come from
// `clock` (ns): emulated time for machines stepped side by side, the wall clock for a real link.
class DelayedLink final : public SerialLink {
public:
    using Clock = std::function<uint64_t()>;
    struct Options {
        uint64_t delay_ns = 0;
        uint64_t jitter_ns = 0;
        uint64_t stall_ns = 0;
        double stalls = 0;  // per second
        uint32_t seed = 1;
    };
    DelayedLink(SerialLink& inner, Clock clock, Options options);

    void send(std::span<const uint8_t> bytes) override { inner_.send(bytes); }
    size_t receive(std::span<uint8_t> out) override;
    bool connected() const override { return inner_.connected(); }

    size_t in_flight() const;  // bytes received from the peer, not yet arrived

private:
    uint32_t random();
    uint64_t uniform(uint64_t max);  // 0..max

    SerialLink& inner_;
    Clock clock_;
    Options opt_;
    uint32_t rng_;
    uint64_t last_due_ = 0;
    uint64_t stall_until_ = 0, next_stall_ = 0;
    std::deque<std::pair<uint64_t, std::vector<uint8_t>>> queue_;  // (due, bytes), due ascending
    size_t front_used_ = 0;  // bytes of queue_.front() already handed out
};

} // namespace vette::host
