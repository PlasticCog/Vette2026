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
    : io_(io),
      config_(std::move(config)),
      join_code_(std::move(join_code)),
      peer_({config_.max_payload, config_.max_unacked, config_.ack_delay_us, config_.resend_request_us},
            [this](std::span<const std::uint8_t> m) { io_.send_binary(m); }) {
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
    friend_ = peer == "here" ? Peer::Here : peer == "away" ? Peer::Away : Peer::None;
    peer_.set_open(can_send(), now_us);
    if (resumed) {
        // Whatever was in flight when the connection dropped may be lost: resend everything the friend
        // hasn't acknowledged, and tell them where our side is so they do the same.
        peer_.resume(now_us);
    }
    io_.send_text("ping");
    ping_sent_us_ = last_ping_us_ = now_us;
}

void Session::peer_event(const JsonObject& m, std::int64_t now_us) {
    const std::string state = field(m, "state");
    if (state == "joined") {
        peer_.reset();  // a new friend: a new cable
        friend_ = Peer::Here;
        status_.reason.clear();
        peer_.set_open(can_send(), now_us);
    } else if (state == "away") {
        friend_ = Peer::Away;
        peer_.set_open(false, now_us);
    } else if (state == "back") {
        friend_ = Peer::Here;
        peer_.set_open(can_send(), now_us);
        peer_.resume(now_us);
    } else if (state == "left") {
        friend_ = Peer::Left;
        peer_.set_open(false, now_us);
        status_.reason = field(m, "reason");
        if (guest_) {
            finish(LinkState::PeerLeft, status_.reason);  // the host's room closes with them
        }
    }
}

void Session::on_binary(std::span<const std::uint8_t> message, std::int64_t now_us) {
    last_rx_us_ = now_us;
    if (!final_) {
        peer_.on_message(message, now_us);
    }
}

void Session::on_closed(std::string_view why, std::int64_t now_us) {
    conn_ = Conn::Down;
    welcomed_here_ = false;
    ping_sent_us_ = -1;
    peer_.set_open(false, now_us);
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
    peer_.tick(now_us);
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
            t = std::min(t, peer_.next_timer());
        }
    }
    return std::max(t, now_us);
}

bool Session::write(std::span<const std::uint8_t> bytes, std::int64_t now_us) {
    if (final_ || bytes.empty()) {
        return !final_;
    }
    // With nobody at the other end (not joined yet, or gone), the bytes go nowhere, as on a cable.
    if (friend_ != Peer::Here && friend_ != Peer::Away) {
        return true;
    }
    if (!peer_.write(bytes, now_us)) {
        finish(LinkState::PeerLeft, "Your friend has been unreachable for too long.");
        return false;
    }
    return true;
}

void Session::take_received(std::vector<std::uint8_t>& out) { peer_.take_received(out); }

bool Session::send_side(std::string_view text, std::int64_t now_us) {
    return !final_ && peer_.send_side(text, now_us);
}

LinkStatus Session::status() const {
    LinkStatus s = status_;
    peer_.fill(s);
    return s;
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

void Session::finish(LinkState state, std::string reason) {
    final_ = true;
    peer_.set_open(false, 0);
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
        switch (friend_) {
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

bool Session::linked() const { return !final_ && welcomed_ && (friend_ == Peer::Here || friend_ == Peer::Away); }

}  // namespace vette::net
