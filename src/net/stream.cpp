#include "net/stream.h"

#include <algorithm>

namespace vette::net {

bool ReliableStream::write(std::span<const std::uint8_t> bytes) {
    if (buf_.size() - head_ + bytes.size() > max_unacked_) {
        return false;
    }
    buf_.insert(buf_.end(), bytes.begin(), bytes.end());
    return true;
}

std::span<const std::uint8_t> ReliableStream::unsent_bytes(std::size_t max) const {
    const std::size_t start = head_ + static_cast<std::size_t>(sent_ - acked_);
    return {buf_.data() + start, std::min(max, buf_.size() - start)};
}

void ReliableStream::mark_sent(std::size_t n) { sent_ += std::min<std::uint64_t>(n, unsent()); }

bool ReliableStream::on_ack(std::uint32_t ack) {
    const auto ahead = static_cast<std::int32_t>(ack - static_cast<std::uint32_t>(acked_));
    if (ahead <= 0) {
        return true;  // nothing new (or an older acknowledgement arriving late)
    }
    const std::uint64_t to = acked_ + static_cast<std::uint64_t>(ahead);
    if (to > written()) {
        return false;
    }
    head_ += static_cast<std::size_t>(to - acked_);
    acked_ = to;
    sent_ = std::max(sent_, acked_);
    if (head_ > 4096 && head_ * 2 > buf_.size()) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(head_));
        head_ = 0;
    }
    return true;
}

ReliableStream::Received ReliableStream::receive(std::uint32_t offset, std::span<const std::uint8_t> payload,
                                                 std::span<const std::uint8_t>& fresh) {
    fresh = {};
    const auto ahead = static_cast<std::int32_t>(offset - static_cast<std::uint32_t>(received_));
    if (ahead > 0) {
        return Received::Gap;
    }
    const auto already = static_cast<std::uint64_t>(-static_cast<std::int64_t>(ahead));
    if (already >= payload.size()) {
        return payload.empty() ? Received::Applied : Received::Duplicate;
    }
    fresh = payload.subspan(static_cast<std::size_t>(already));
    received_ += fresh.size();
    return Received::Applied;
}

void ReliableStream::reset() {
    buf_.clear();
    head_ = 0;
    acked_ = sent_ = received_ = 0;
}

}  // namespace vette::net
