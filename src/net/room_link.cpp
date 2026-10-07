#include "net/room_link.h"

#include <algorithm>
#include <chrono>
#include <limits>

#include "net/connection.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

namespace vette::net {

namespace {

constexpr std::int64_t kNever = std::numeric_limits<std::int64_t>::max();

std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Millisecond waits on Windows: its default timer tick is 15.6 ms, which would hold a burst of serial
// bytes that long before it goes out.
struct FineTimer {
#if defined(_WIN32)
    FineTimer() { timeBeginPeriod(1); }
    ~FineTimer() { timeEndPeriod(1); }
#endif
};

}  // namespace

// The session's requests, carried out on the network thread. Connecting is left to the loop, so the
// session never hears back while it is still in the call that asked.
class RoomLink::Io final : public SessionIo {
public:
    explicit Io(WebSocketConnection& ws) : ws_(ws) {}

    void connect(const std::string& target) override {
        target_ = target;
        want_connect_ = true;
    }
    void send_text(std::string_view text) override { ws_.send_text(text); }
    void send_binary(std::span<const std::uint8_t> message) override { ws_.send_binary(message); }
    void disconnect() override { ws_.close(); }

    bool want_connect_ = false;
    std::string target_;

private:
    WebSocketConnection& ws_;
};

std::unique_ptr<RoomLink> RoomLink::create_room(RoomOptions options) {
    return std::unique_ptr<RoomLink>(new RoomLink(std::move(options), {}, {}));
}

std::unique_ptr<RoomLink> RoomLink::join_room(RoomOptions options, std::string_view code) {
    const auto bare = normalize_room_code(code);
    if (!bare) {
        return std::unique_ptr<RoomLink>(new RoomLink(
            std::move(options), {},
            "\"" + std::string(code) + "\" isn't a room code. Codes look like VETTE-4KQ7."));
    }
    return std::unique_ptr<RoomLink>(new RoomLink(std::move(options), *bare, {}));
}

RoomLink::RoomLink(RoomOptions options, std::string join_code, std::string failure)
    : options_(std::move(options)), join_code_(std::move(join_code)), waker_(std::make_unique<Waker>()) {
    status_.host = join_code_.empty() && failure.empty();
    if (!join_code_.empty()) {
        status_.code = format_room_code(join_code_);
    }
    if (failure.empty() && !parse_server_url(options_.server_url)) {
        failure = "The online server address \"" + options_.server_url + "\" isn't valid.";
    }
    if (!failure.empty()) {
        status_.state = LinkState::Failed;
        status_.reason = std::move(failure);
        return;
    }
    if (status_.host) {
        settings_ = options_.race_settings;
        status_.settings_gen = 1;
    }
    thread_ = std::thread([this] { run(); });
}

RoomLink::~RoomLink() {
    close();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void RoomLink::send(std::span<const std::uint8_t> bytes) {
    // Nobody at the other end (not joined yet, or gone): the bytes go nowhere, as on a cable.
    if (bytes.empty() || !connected_.load()) {
        return;
    }
    bool wake;
    {
        std::lock_guard lock(tx_mutex_);
        const std::int64_t now = now_us();
        wake = tx_.empty();
        if (wake) {
            tx_first_us_ = now;
        }
        tx_last_us_ = now;
        tx_.insert(tx_.end(), bytes.begin(), bytes.end());
    }
    if (wake) {
        waker_->wake();  // the network thread then times the batch itself
    }
}

std::size_t RoomLink::receive(std::span<std::uint8_t> out) {
    std::lock_guard lock(rx_mutex_);
    const std::size_t n = std::min(out.size(), rx_.size() - rx_pos_);
    std::copy_n(rx_.begin() + static_cast<std::ptrdiff_t>(rx_pos_), n, out.begin());
    rx_pos_ += n;
    if (rx_pos_ == rx_.size()) {
        rx_.clear();
        rx_pos_ = 0;
    }
    return n;
}

bool RoomLink::connected() const { return connected_.load(); }

LinkStatus RoomLink::status() const {
    std::lock_guard lock(state_mutex_);
    return status_;
}

bool RoomLink::set_race_settings(const RaceSettings& settings) {
    {
        std::lock_guard lock(state_mutex_);
        if (!status_.host || settings.serialize().size() > RaceSettings::kMaxBytes) {
            return false;
        }
        new_settings_ = settings;
        settings_ = settings;
    }
    waker_->wake();
    return true;
}

std::optional<RaceSettings> RoomLink::race_settings() const {
    std::lock_guard lock(state_mutex_);
    return settings_;
}

void RoomLink::discard_received() {
    std::lock_guard lock(rx_mutex_);
    rx_.clear();
    rx_pos_ = 0;
}

void RoomLink::close() {
    stop_ = true;
    waker_->wake();
}

bool RoomLink::send_side(const std::string& text) {
    {
        std::lock_guard lock(state_mutex_);
        if (!connected_.load() || status_.state != LinkState::Connected) {
            return false;
        }
        side_out_.push_back(text);
    }
    waker_->wake();
    return true;
}

std::vector<std::string> RoomLink::take_side() {
    std::lock_guard lock(state_mutex_);
    std::vector<std::string> out;
    out.swap(side_in_);
    return out;
}

void RoomLink::simulate_drop(int offline_ms) {
    drop_offline_ms_ = offline_ms;
    drop_ = true;
    waker_->wake();
}

void RoomLink::publish(const Session& session) {
    std::lock_guard lock(state_mutex_);
    status_ = session.status();
    if (!new_settings_) {
        settings_ = session.race_settings();
    }
    connected_ = session.linked();
}

void RoomLink::run() {
    [[maybe_unused]] FineTimer fine_timer;
    WebSocketConnection ws;
    Io io(ws);
    SessionConfig config;
    config.app_version = options_.app_version;
    config.game_build = options_.game_build;
    config.race_settings = options_.race_settings;
    config.protocol = options_.protocol;
    Session session(io, config, join_code_);
    const ServerUrl server = *parse_server_url(options_.server_url);
    ConnectOptions connect_options;
    connect_options.ca_file = options_.ca_file;
    connect_options.user_agent = "VETTE2026/" + options_.app_version;
    connect_options.abort = &stop_;

    std::vector<ws::FrameDecoder::Message> messages;
    std::vector<std::uint8_t> batch, received;
    std::int64_t last_send_us = -options_.batching.min_interval_us;
    std::int64_t offline_until_us = 0;  // simulate_drop()
    session.start(now_us());
    publish(session);

    for (;;) {
        std::string error;
        if (stop_) {
            session.leave(now_us());  // "bye", so the friend hears at once, not after the grace period
            ws.shutdown(500);
            publish(session);
            return;
        }
        if (io.want_connect_) {
            io.want_connect_ = false;
            if (now_us() < offline_until_us) {
                session.on_closed("No network (test).", now_us());
            } else if (ws.connect(server, io.target_, connect_options, error)) {
                session.on_open(now_us());
            } else {
                session.on_closed(error, now_us());
            }
        }
        if (drop_.exchange(false) && ws.is_open()) {
            ws.close();
            offline_until_us = now_us() + drop_offline_ms_.load() * std::int64_t{1000};
            session.on_closed("The connection was dropped (test).", now_us());
        }
        std::optional<RaceSettings> new_settings;
        std::vector<std::string> side;
        {
            std::lock_guard lock(state_mutex_);
            new_settings.swap(new_settings_);
            side.swap(side_out_);
        }
        if (new_settings) {
            session.set_race_settings(*new_settings, now_us());
        }

        // From the server.
        if (ws.is_open()) {
            messages.clear();
            const bool open = ws.read(messages, error);
            for (const auto& m : messages) {
                if (m.op == ws::kText) {
                    session.on_text({reinterpret_cast<const char*>(m.data.data()), m.data.size()}, now_us());
                } else {
                    session.on_binary(m.data, now_us());
                }
            }
            if (!open) {
                session.on_closed(error, now_us());
            }
        }

        for (const auto& text : side) {
            session.send_side(text, now_us());
        }
        side.clear();
        session.take_side(side);
        if (!side.empty()) {
            std::lock_guard lock(state_mutex_);
            side_in_.insert(side_in_.end(), side.begin(), side.end());
        }

        // The game's bytes, a burst at a time (Batching).
        std::int64_t now = now_us();
        std::int64_t tx_due = kNever;
        {
            std::lock_guard lock(tx_mutex_);
            if (!tx_.empty()) {
                tx_due = options_.batching.due(tx_first_us_, tx_last_us_, last_send_us);
                if (now >= tx_due || tx_.size() >= 4096) {
                    batch.swap(tx_);
                    tx_due = kNever;
                }
            }
        }
        if (!batch.empty()) {
            session.write(batch, now);
            batch.clear();
            last_send_us = now;
        }
        session.tick(now);

        // To the server.
        if (ws.is_open() && !ws.flush(error)) {
            ws.close();
            session.on_closed(error, now_us());
        }

        received.clear();
        session.take_received(received);
        if (!received.empty()) {
            std::lock_guard lock(rx_mutex_);
            rx_.insert(rx_.end(), received.begin(), received.end());
        }
        publish(session);

        if (session.finished() && !io.want_connect_) {
            ws.shutdown(500);
            return;
        }
        if (io.want_connect_ || drop_) {
            continue;
        }
        now = now_us();
        const std::int64_t wake_at = std::min(session.next_timer(now), tx_due);
        const auto timeout_ms = static_cast<int>(std::clamp<std::int64_t>((wake_at - now + 999) / 1000, 0, 1000));
        ws.wait(*waker_, timeout_ms);
    }
}

}  // namespace vette::net
