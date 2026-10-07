#include "net/peer_stream.h"

#include <algorithm>

#include "net/session.h"

namespace vette::net {

void PeerStream::set_open(bool open, std::int64_t now_us) {
    open_ = open;
    transmit(now_us);
}

bool PeerStream::write(std::span<const std::uint8_t> bytes, std::int64_t now_us) {
    if (!stream_.write(bytes)) {
        return false;
    }
    transmit(now_us);
    return true;
}

void PeerStream::on_message(std::span<const std::uint8_t> message, std::int64_t now_us) {
    const auto h = MessageHeader::decode(message);
    if (!h) {
        return;
    }
    ++stats_.messages_received;

    peer_ts_ = h->ts_us;
    peer_ts_at_us_ = now_us;
    if (h->echo_hold_us != MessageHeader::kNoEcho && (!have_echo_ || h->echo_ts_us != last_echo_)) {
        have_echo_ = true;
        last_echo_ = h->echo_ts_us;
        const std::uint32_t elapsed = static_cast<std::uint32_t>(now_us) - h->echo_ts_us;
        if (elapsed >= h->echo_hold_us && elapsed - h->echo_hold_us < 60'000'000u) {
            const double ms = static_cast<double>(elapsed - h->echo_hold_us) / 1000.0;
            stats_.rtt_last_ms = ms;
            stats_.rtt_ms = stats_.rtt_ms < 0 ? ms : stats_.rtt_ms * 0.875 + ms * 0.125;
            ++stats_.rtt_samples;
        }
    }

    stream_.on_ack(h->ack);
    if (h->flags & MessageHeader::kResend) {
        resend(now_us);
    }
    const auto payload = message.subspan(MessageHeader::kSize);
    if (h->kind == MessageHeader::kSide) {
        side_.emplace_back(reinterpret_cast<const char*>(payload.data()), payload.size());
    } else if (h->kind == MessageHeader::kData) {
        std::span<const std::uint8_t> fresh;
        switch (stream_.receive(h->offset, payload, fresh)) {
        case ReliableStream::Received::Applied:
            received_.insert(received_.end(), fresh.begin(), fresh.end());
            stats_.bytes_received += fresh.size();
            [[fallthrough]];
        case ReliableStream::Received::Duplicate:
            if (ack_due_us_ < 0) {
                ack_due_us_ = now_us + config_.ack_delay_us;
            }
            break;
        case ReliableStream::Received::Gap:
            // Something before this is missing (lost in a drop): ask once for a resend from where we
            // are, and drop what follows until the resend arrives.
            if (resend_requested_us_ < 0 || stream_.received() != resend_requested_at_ ||
                now_us - resend_requested_us_ >= config_.resend_request_us) {
                resend_requested_us_ = now_us;
                resend_requested_at_ = stream_.received();
                if (open_) {
                    send_message(MessageHeader::kAck, MessageHeader::kResend, {}, now_us);
                }
            }
            break;
        }
    }
    transmit(now_us);
}

void PeerStream::resume(std::int64_t now_us) {
    stream_.rewind();
    if (open_) {
        send_message(MessageHeader::kAck, 0, {}, now_us);
    }
    transmit(now_us);
}

void PeerStream::resend(std::int64_t now_us) {
    // The other game sends one request per gap, but more of our messages may have crossed it.
    if (rewound_us_ >= 0 && now_us - rewound_us_ < config_.resend_request_us) {
        return;
    }
    rewound_us_ = now_us;
    stream_.rewind();
    transmit(now_us);
}

void PeerStream::reset() {
    stream_.reset();
    sent_high_ = 0;
    received_.clear();
    side_.clear();
    have_echo_ = false;
    peer_ts_at_us_ = -1;
    ack_due_us_ = resend_requested_us_ = rewound_us_ = -1;
}

void PeerStream::tick(std::int64_t now_us) {
    transmit(now_us);
    if (ack_due_us_ >= 0 && now_us >= ack_due_us_ && open_) {
        send_message(MessageHeader::kAck, 0, {}, now_us);
    }
}

bool PeerStream::send_side(std::string_view text, std::int64_t now_us) {
    if (!open_) {
        return false;
    }
    send_message(MessageHeader::kSide, 0, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()},
                 now_us);
    return true;
}

void PeerStream::take_received(std::vector<std::uint8_t>& out) {
    out.insert(out.end(), received_.begin(), received_.end());
    received_.clear();
}

void PeerStream::take_side(std::vector<std::string>& out) {
    for (auto& s : side_) {
        out.push_back(std::move(s));
    }
    side_.clear();
}

void PeerStream::fill(LinkStatus& status) const {
    status.rtt_ms = stats_.rtt_ms;
    status.rtt_last_ms = stats_.rtt_last_ms;
    status.rtt_samples = stats_.rtt_samples;
    status.bytes_sent = stats_.bytes_sent;
    status.bytes_received = stats_.bytes_received;
    status.messages_sent = stats_.messages_sent;
    status.messages_received = stats_.messages_received;
    status.bytes_resent = stats_.bytes_resent;
}

void PeerStream::transmit(std::int64_t now_us) {
    while (open_ && stream_.unsent() > 0) {
        const auto chunk = stream_.unsent_bytes(config_.max_payload);
        const std::uint64_t end = stream_.sent() + chunk.size();
        if (end > sent_high_) {
            stats_.bytes_sent += end - std::max(sent_high_, stream_.sent());
            stats_.bytes_resent += std::max(sent_high_, stream_.sent()) - stream_.sent();
            sent_high_ = end;
        } else {
            stats_.bytes_resent += chunk.size();
        }
        send_message(MessageHeader::kData, 0, chunk, now_us);
        stream_.mark_sent(chunk.size());
    }
}

void PeerStream::send_message(MessageHeader::Kind kind, std::uint8_t flags, std::span<const std::uint8_t> payload,
                              std::int64_t now_us) {
    MessageHeader h;
    h.kind = kind;
    h.flags = flags;
    h.ts_us = static_cast<std::uint32_t>(now_us);
    if (peer_ts_at_us_ >= 0) {
        h.echo_ts_us = peer_ts_;
        h.echo_hold_us = static_cast<std::uint32_t>(std::min<std::int64_t>(now_us - peer_ts_at_us_, 0xFFFFFFFE));
    }
    h.offset = static_cast<std::uint32_t>(stream_.sent());
    h.ack = static_cast<std::uint32_t>(stream_.received());
    scratch_.clear();
    h.append_to(scratch_);
    scratch_.insert(scratch_.end(), payload.begin(), payload.end());
    send_(scratch_);
    ++stats_.messages_sent;
    if (kind != MessageHeader::kSide) {
        ack_due_us_ = -1;
    }
}

}  // namespace vette::net
