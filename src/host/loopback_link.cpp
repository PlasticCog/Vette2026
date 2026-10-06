#include "host/loopback_link.h"

#include <algorithm>

namespace vette::host {

// One end: an inbox of the chunks the other end sent (one send() each), handed out one chunk (or what
// fits of it) per receive().
class LoopbackCable::End final : public SerialLink {
public:
    explicit End(LoopbackCable& cable) : cable_(cable) {}
    void set_peer(End* peer) { peer_ = peer; }

    void send(std::span<const uint8_t> bytes) override {
        if (cable_.plugged_ && !bytes.empty()) {
            peer_->inbox_.emplace_back(bytes.begin(), bytes.end());
        }
    }
    size_t receive(std::span<uint8_t> out) override {
        if (inbox_.empty() || out.empty()) {
            return 0;
        }
        std::vector<uint8_t>& chunk = inbox_.front();
        const size_t n = std::min(out.size(), chunk.size() - used_);
        std::copy_n(chunk.begin() + static_cast<std::ptrdiff_t>(used_), n, out.begin());
        used_ += n;
        if (used_ == chunk.size()) {
            inbox_.pop_front();
            used_ = 0;
        }
        return n;
    }
    bool connected() const override { return cable_.plugged_; }

private:
    LoopbackCable& cable_;
    End* peer_ = nullptr;
    std::deque<std::vector<uint8_t>> inbox_;
    size_t used_ = 0;
};

LoopbackCable::LoopbackCable() {
    ends_[0] = std::make_unique<End>(*this);
    ends_[1] = std::make_unique<End>(*this);
    ends_[0]->set_peer(ends_[1].get());
    ends_[1]->set_peer(ends_[0].get());
}

LoopbackCable::~LoopbackCable() = default;

SerialLink& LoopbackCable::end(int i) { return *ends_[static_cast<size_t>(i & 1)]; }

DelayedLink::DelayedLink(SerialLink& inner, Clock clock, Options options)
    : inner_(inner), clock_(std::move(clock)), opt_(options), rng_(options.seed ? options.seed : 1) {}

uint32_t DelayedLink::random() {  // xorshift32
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

uint64_t DelayedLink::uniform(uint64_t max) {
    if (max == 0) {
        return 0;
    }
    const uint64_t r = (static_cast<uint64_t>(random()) << 32) | random();
    return r % (max + 1);
}

size_t DelayedLink::in_flight() const {
    size_t n = 0;
    for (const auto& [due, bytes] : queue_) {
        n += bytes.size();
    }
    return n - front_used_;
}

size_t DelayedLink::receive(std::span<uint8_t> out) {
    const uint64_t now = clock_();
    // Everything the peer has sent so far, stamped with its arrival time.
    std::array<uint8_t, 4096> buf;
    for (size_t n; (n = inner_.receive(buf)) > 0;) {
        if (opt_.stalls > 0 && opt_.stall_ns > 0) {
            const auto mean = static_cast<uint64_t>(1e9 / opt_.stalls);
            if (next_stall_ == 0) {
                next_stall_ = now + uniform(2 * mean);
            }
            if (now >= next_stall_) {
                stall_until_ = now + uniform(opt_.stall_ns);
                next_stall_ = now + uniform(2 * mean);
            }
        }
        uint64_t due = now + opt_.delay_ns + uniform(opt_.jitter_ns);
        if (now < stall_until_) {
            due = std::max(due, stall_until_ + opt_.delay_ns);
        }
        due = std::max(due, last_due_);  // in order
        last_due_ = due;
        queue_.emplace_back(due, std::vector<uint8_t>(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n)));
    }
    // What has arrived.
    size_t written = 0;
    while (written < out.size() && !queue_.empty() && queue_.front().first <= now) {
        const std::vector<uint8_t>& chunk = queue_.front().second;
        const size_t n = std::min(out.size() - written, chunk.size() - front_used_);
        std::copy_n(chunk.begin() + static_cast<std::ptrdiff_t>(front_used_), n,
                    out.begin() + static_cast<std::ptrdiff_t>(written));
        written += n;
        front_used_ += n;
        if (front_used_ == chunk.size()) {
            queue_.pop_front();
            front_used_ = 0;
        }
    }
    return written;
}

} // namespace vette::host
