#include "net/session.h"

#include <algorithm>
#include <charconv>

namespace vette::net {

namespace {

std::string field(const JsonObject& m, std::string_view key) {
    const auto it = m.find(key);
    return it == m.end() ? std::string() : it->second;
}

std::int64_t number(const JsonObject& m, std::string_view key, std::int64_t fallback) {
    const std::string text = field(m, key);
    std::int64_t v = 0;
    const auto r = std::from_chars(text.data(), text.data() + text.size(), v);
    return r.ec == std::errc() && r.ptr == text.data() + text.size() ? v : fallback;
}

}  // namespace

std::int64_t Batching::due(std::int64_t first_us, std::int64_t last_us, std::int64_t last_send_us) const {
    return std::max(last_send_us + min_interval_us, std::min(last_us + gap_us, first_us + max_hold_us));
}

const char* to_string(LinkState state) {
    switch (state) {
    case LinkState::Connecting: return "connecting";
    case LinkState::Waiting: return "waiting for the friend";
    case LinkState::Connected: return "connected";
    case LinkState::Reconnecting: return "reconnecting";
    case LinkState::PeerAway: return "friend reconnecting";
    case LinkState::PeerLeft: return "friend gone";
    case LinkState::Failed: return "failed";
    case LinkState::Closed: return "closed";
    }
    return "?";
}

Session::Session(SessionIo& io, SessionConfig config, std::string join_code)
    : io_(io), config_(std::move(config)), join_code_(std::move(join_code)), stream_(config_.max_unacked) {
    guest_ = !join_code_.empty();
    status_.host = !guest_;
    if (guest_) {
        status_.code = format_room_code(join_code_);
    } else {
        // Generation 1 goes with the room's creation.
        settings_ = config_.race_settings;
        settings_gen_ = 1;
        status_.settings_gen = 1;
    }
}

std::string Session::hello() const {
    return "proto=" + std::to_string(config_.protocol) + "&app=" + percent_encode(config_.app_version) +
           "&game=" + percent_encode(config_.game_build);
}

std::string Session::target() const {
    if (welcomed_) {
        return "/v1/resume/" + room_code_ + "?" + hello() + "&token=" + percent_encode(token_);
    }
    if (guest_) {
        return "/v1/join/" + join_code_ + "?" + hello();
    }
    return "/v1/create?" + hello() + "&settings=" + percent_encode(settings_ ? settings_->serialize() : "");
}

void Session::start(std::int64_t now_us) {
    (void)now_us;
    conn_ = Conn::Connecting;
    io_.connect(target());
    update_state();
}

void Session::on_open(std::int64_t now_us) {
    conn_ = Conn::Open;
    last_rx_us_ = now_us;
    last_ping_us_ = now_us;
    update_state();
}

void Session::on_text(std::string_view text, std::int64_t now_us) {
    last_rx_us_ = now_us;
    if (text == "pong") {
        if (ping_sent_us_ >= 0) {
            status_.server_rtt_ms = static_cast<double>(now_us - ping_sent_us_) / 1000.0;
            ping_sent_us_ = -1;
        }
        return;
    }
    const auto m = parse_json_object(text);
    if (!m || final_) {
        return;
    }
    const std::string type = field(*m, "t");
    if (type == "welcome") {
        welcome(*m, now_us);
    } else if (type == "peer") {
        peer_event(*m, now_us);
    } else if (type == "settings") {
        const auto gen = number(*m, "gen", 0);
        if (guest_ && gen > settings_gen_) {  // never an older generation over a newer one
            if (auto s = RaceSettings::parse(field(*m, "data"))) {
                settings_ = std::move(*s);
                settings_gen_ = static_cast<std::uint32_t>(gen);
                status_.settings_gen = settings_gen_;
            }
        }
    } else if (type == "error") {
        // Refused: a wrong code, a full room, another version, too many attempts; or, when coming
        // back, the seat has expired.
        finish(welcomed_ ? LinkState::PeerLeft : LinkState::Failed, field(*m, "reason"));
    } else if (type == "closed") {
        finish(LinkState::Closed, field(*m, "reason"));
    }
    update_state();
}

void Session::welcome(const JsonObject& m, std::int64_t now_us) {
    const bool resumed = welcomed_;
    guest_ = field(m, "role") == "guest";
    status_.host = !guest_;
    token_ = field(m, "token");
    room_code_ = field(m, "code");
    status_.code = format_room_code(room_code_);
    grace_us_ = number(m, "grace", 30) * 1'000'000;
    welcomed_ = welcomed_here_ = true;
    failed_attempts_ = 0;
    backoff_us_ = 0;
    reconnect_at_us_ = give_up_at_us_ = -1;
    if (resumed) {
        ++status_.reconnects;
    }

    const auto gen = number(m, "settings_gen", 0);
    if (guest_) {
        // The server gives a guest the newest settings in every welcome: new ones are taken, the same
        // generation again (after a reconnect) changes nothing.
        if (gen > settings_gen_) {
            if (auto s = RaceSettings::parse(field(m, "settings"))) {
                settings_ = std::move(*s);
                settings_gen_ = static_cast<std::uint32_t>(gen);
                status_.settings_gen = settings_gen_;
            }
        }
    } else if (settings_gen_ > gen) {
        send_settings();  // a change the server hasn't had (made while we were away)
    }

    const std::string peer = field(m, "peer");
    peer_ = peer == "here" ? Peer::Here : peer == "away" ? Peer::Away : Peer::None;
    if (resumed) {
        // Whatever was in flight when the connection dropped may be lost: resend everything the friend
        // hasn't acknowledged, and tell them where our side is so they do the same.
        stream_.rewind();
        if (peer_ == Peer::Here) {
            send_message(MessageHeader::kAck, 0, {}, now_us);
        }
    }
    transmit(now_us);
    io_.send_text("ping");
    ping_sent_us_ = last_ping_us_ = now_us;
}

void Session::peer_event(const JsonObject& m, std::int64_t now_us) {
    const std::string state = field(m, "state");
    if (state == "joined") {
        new_stream();  // a new friend: a new cable
        peer_ = Peer::Here;
        status_.reason.clear();
        transmit(now_us);
    } else if (state == "away") {
        peer_ = Peer::Away;
    } else if (state == "back") {
        peer_ = Peer::Here;
        stream_.rewind();
        send_message(MessageHeader::kAck, 0, {}, now_us);
        transmit(now_us);
    } else if (state == "left") {
        peer_ = Peer::Left;
        status_.reason = field(m, "reason");
        if (guest_) {
            finish(LinkState::PeerLeft, status_.reason);  // the host's room closes with them
        }
    }
}

void Session::new_stream() {
    stream_.reset();
    sent_high_ = 0;
    received_.clear();
    have_echo_ = false;
    peer_ts_at_us_ = -1;
    ack_due_us_ = resend_requested_us_ = rewound_us_ = -1;
}

void Session::on_binary(std::span<const std::uint8_t> message, std::int64_t now_us) {
    last_rx_us_ = now_us;
    const auto h = MessageHeader::decode(message);
    if (!h || final_) {
        return;
    }
    ++status_.messages_received;

    peer_ts_ = h->ts_us;
    peer_ts_at_us_ = now_us;
    if (h->echo_hold_us != MessageHeader::kNoEcho && (!have_echo_ || h->echo_ts_us != last_echo_)) {
        have_echo_ = true;
        last_echo_ = h->echo_ts_us;
        const std::uint32_t elapsed = static_cast<std::uint32_t>(now_us) - h->echo_ts_us;
        if (elapsed >= h->echo_hold_us && elapsed - h->echo_hold_us < 60'000'000u) {
            const double ms = static_cast<double>(elapsed - h->echo_hold_us) / 1000.0;
            status_.rtt_last_ms = ms;
            status_.rtt_ms = status_.rtt_ms < 0 ? ms : status_.rtt_ms * 0.875 + ms * 0.125;
            ++status_.rtt_samples;
        }
    }

    stream_.on_ack(h->ack);
    if (h->flags & MessageHeader::kResend) {
        resend(now_us);
    }
    if (h->kind == MessageHeader::kData) {
        std::span<const std::uint8_t> fresh;
        switch (stream_.receive(h->offset, message.subspan(MessageHeader::kSize), fresh)) {
        case ReliableStream::Received::Applied:
            received_.insert(received_.end(), fresh.begin(), fresh.end());
            status_.bytes_received += fresh.size();
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
                send_message(MessageHeader::kAck, MessageHeader::kResend, {}, now_us);
            }
            break;
        }
    }
    transmit(now_us);
}

void Session::resend(std::int64_t now_us) {
    // The friend sends one request per gap, but more of our messages may have crossed it.
    if (rewound_us_ >= 0 && now_us - rewound_us_ < config_.resend_request_us) {
        return;
    }
    rewound_us_ = now_us;
    stream_.rewind();
    transmit(now_us);
}

void Session::on_closed(std::string_view why, std::int64_t now_us) {
    conn_ = Conn::Down;
    welcomed_here_ = false;
    ping_sent_us_ = -1;
    if (final_) {
        return;
    }
    if (!welcomed_) {
        if (++failed_attempts_ >= config_.connect_attempts) {
            finish(LinkState::Failed, std::string(why));  // e.g. "Can't connect to the server ..."
        } else {
            status_.reason = why;
            reconnect_at_us_ = now_us + 1'000'000LL * failed_attempts_;
        }
        update_state();
        return;
    }
    // A working connection dropped: get back into the room before the server gives our seat up.
    if (give_up_at_us_ < 0) {
        give_up_at_us_ = now_us + grace_us_;
    }
    reconnect_at_us_ = now_us + backoff_us_;
    backoff_us_ = backoff_us_ == 0 ? 250'000 : std::min<std::int64_t>(backoff_us_ * 2, 2'000'000);
    status_.reason = why;
    update_state();
}

void Session::tick(std::int64_t now_us) {
    if (final_) {
        return;
    }
    if (conn_ == Conn::Down && reconnect_at_us_ >= 0 && now_us >= reconnect_at_us_) {
        if (welcomed_ && now_us >= give_up_at_us_) {
            finish(LinkState::PeerLeft, "The connection to the online server was lost.");
            return;
        }
        reconnect_at_us_ = -1;
        conn_ = Conn::Connecting;
        io_.connect(target());
        update_state();
        return;
    }
    if (conn_ != Conn::Open) {
        return;
    }
    if (now_us - last_rx_us_ >= config_.dead_after_us) {
        io_.disconnect();
        on_closed("The connection stopped responding.", now_us);
        return;
    }
    if (welcomed_here_ && now_us - last_ping_us_ >= config_.ping_interval_us) {
        io_.send_text("ping");
        last_ping_us_ = now_us;
        if (ping_sent_us_ < 0) {
            ping_sent_us_ = now_us;
        }
    }
    transmit(now_us);
    if (ack_due_us_ >= 0 && now_us >= ack_due_us_ && can_send()) {
        send_message(MessageHeader::kAck, 0, {}, now_us);
    }
}

std::int64_t Session::next_timer(std::int64_t now_us) const {
    std::int64_t t = now_us + 1'000'000;
    if (!final_) {
        if (conn_ == Conn::Down && reconnect_at_us_ >= 0) {
            t = std::min(t, reconnect_at_us_);
        }
        if (conn_ == Conn::Open) {
            t = std::min(t, last_rx_us_ + config_.dead_after_us);
            if (welcomed_here_) {
                t = std::min(t, last_ping_us_ + config_.ping_interval_us);
            }
            if (ack_due_us_ >= 0) {
                t = std::min(t, ack_due_us_);
            }
        }
    }
    return std::max(t, now_us);
}

bool Session::write(std::span<const std::uint8_t> bytes, std::int64_t now_us) {
    if (final_ || bytes.empty()) {
        return !final_;
    }
    // With nobody at the other end (not joined yet, or gone), the bytes go nowhere, as on a cable.
    if (peer_ != Peer::Here && peer_ != Peer::Away) {
        return true;
    }
    if (!stream_.write(bytes)) {
        finish(LinkState::PeerLeft, "Your friend has been unreachable for too long.");
        return false;
    }
    transmit(now_us);
    return true;
}

void Session::take_received(std::vector<std::uint8_t>& out) {
    out.insert(out.end(), received_.begin(), received_.end());
    received_.clear();
}

bool Session::set_race_settings(const RaceSettings& settings, std::int64_t now_us) {
    (void)now_us;
    if (guest_ || settings.serialize().size() > RaceSettings::kMaxBytes) {
        return false;
    }
    settings_ = settings;
    status_.settings_gen = ++settings_gen_;
    if (conn_ == Conn::Open && welcomed_here_) {
        send_settings();  // otherwise with the next welcome
    }
    return true;
}

void Session::send_settings() {
    io_.send_text(R"({"t":"settings","gen":)" + std::to_string(settings_gen_) + R"(,"data":)" +
                  json_quote(settings_ ? settings_->serialize() : "") + "}");
}

void Session::leave(std::int64_t now_us) {
    (void)now_us;
    if (final_) {
        return;
    }
    if (conn_ == Conn::Open) {
        io_.send_text("bye");
    }
    finish(LinkState::Closed, "You left the room.");
}

void Session::transmit(std::int64_t now_us) {
    while (can_send() && stream_.unsent() > 0) {
        const auto chunk = stream_.unsent_bytes(config_.max_payload);
        const std::uint64_t end = stream_.sent() + chunk.size();
        if (end > sent_high_) {
            status_.bytes_sent += end - std::max(sent_high_, stream_.sent());
            status_.bytes_resent += std::max(sent_high_, stream_.sent()) - stream_.sent();
            sent_high_ = end;
        } else {
            status_.bytes_resent += chunk.size();
        }
        send_message(MessageHeader::kData, 0, chunk, now_us);
        stream_.mark_sent(chunk.size());
    }
}

void Session::send_message(MessageHeader::Kind kind, std::uint8_t flags, std::span<const std::uint8_t> payload,
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
    io_.send_binary(scratch_);
    ++status_.messages_sent;
    ack_due_us_ = -1;
}

void Session::finish(LinkState state, std::string reason) {
    final_ = true;
    status_.state = state;
    status_.reason = std::move(reason);
    reconnect_at_us_ = -1;
}

void Session::update_state() {
    if (final_) {
        return;
    }
    LinkState s;
    if (!welcomed_) {
        s = LinkState::Connecting;
    } else if (conn_ != Conn::Open || !welcomed_here_) {
        s = LinkState::Reconnecting;
    } else {
        switch (peer_) {
        case Peer::None: s = LinkState::Waiting; break;
        case Peer::Here: s = LinkState::Connected; break;
        case Peer::Away: s = LinkState::PeerAway; break;
        default: s = LinkState::PeerLeft; break;
        }
    }
    if (s == LinkState::Connected || s == LinkState::Waiting) {
        if (s != status_.state) {
            status_.reason.clear();
        }
    }
    status_.state = s;
}

bool Session::linked() const { return !final_ && welcomed_ && (peer_ == Peer::Here || peer_ == Peer::Away); }

}  // namespace vette::net
