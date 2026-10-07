#include "net/direct.h"

#include <algorithm>
#include <chrono>
#include <charconv>
#include <limits>
#include <map>
#include <random>

#include "net/peer_stream.h"
#include "net/websocket.h"

namespace vette::net {

namespace {

constexpr std::int64_t kNever = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kPingUs = 2'000'000, kDeadUs = 7'000'000, kGraceUs = 30'000'000;
constexpr std::int64_t kHandshakeUs = 8'000'000;
constexpr int kMaxWrong = 20;
constexpr std::size_t kMaxFrame = 65535;

std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string hex(std::span<const std::uint8_t> data) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (const auto b : data) {
        out += kHex[b >> 4];
        out += kHex[b & 15];
    }
    return out;
}

std::string random_hex(std::mt19937_64& rng, int bytes) {
    std::vector<std::uint8_t> b(static_cast<std::size_t>(bytes));
    for (auto& x : b) {
        x = static_cast<std::uint8_t>(rng());
    }
    return hex(b);
}

std::mt19937_64 seeded() {
    std::random_device rd;
    std::seed_seq seq{rd(), rd(), rd(), rd(), static_cast<unsigned>(now_us())};
    return std::mt19937_64(seq);
}

std::string sha1_hex(const std::string& text) {
    return hex(ws::sha1({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}));
}

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

// A TCP connection carrying frames.
class Framed {
public:
    struct Frame {
        char type = 'B';
        std::vector<std::uint8_t> data;
        std::string_view text() const { return {reinterpret_cast<const char*>(data.data()), data.size()}; }
    };

    explicit Framed(Socket s) : sock_(std::move(s)) {}

    const Socket& socket() const { return sock_; }
    void send_text(std::string_view t) { queue('T', {reinterpret_cast<const std::uint8_t*>(t.data()), t.size()}); }
    void send_binary(std::span<const std::uint8_t> d) { queue('B', d); }
    bool wants_write() const { return out_pos_ < out_.size(); }

    bool flush(std::string& error) {
        while (out_pos_ < out_.size()) {
            const long n = sock_.send(std::span(out_).subspan(out_pos_), error);
            if (n < 0) {
                return false;
            }
            if (n == 0) {
                break;
            }
            out_pos_ += static_cast<std::size_t>(n);
        }
        if (out_pos_ == out_.size()) {
            out_.clear();
            out_pos_ = 0;
        }
        return true;
    }

    // Appends the complete frames that have arrived (also those that came just before the connection
    // ended); false when it has ended.
    bool read(std::vector<Frame>& out, std::string& error) {
        std::uint8_t buf[16384];
        bool ended = false;
        for (;;) {
            const long n = sock_.receive(buf, error);
            if (n < 0) {
                ended = true;
                break;
            }
            if (n == 0) {
                break;
            }
            in_.insert(in_.end(), buf, buf + n);
        }
        std::size_t pos = 0;
        while (in_.size() - pos >= 3) {
            const std::size_t len = in_[pos] | static_cast<std::size_t>(in_[pos + 1]) << 8;
            if (len == 0 || (in_[pos + 2] != 'T' && in_[pos + 2] != 'B')) {
                error = "The other game sent something that isn't VETTE! 2026's.";
                return false;
            }
            if (in_.size() - pos < 2 + len) {
                break;
            }
            Frame f;
            f.type = static_cast<char>(in_[pos + 2]);
            f.data.assign(in_.begin() + static_cast<std::ptrdiff_t>(pos + 3),
                          in_.begin() + static_cast<std::ptrdiff_t>(pos + 2 + len));
            out.push_back(std::move(f));
            pos += 2 + len;
        }
        in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(pos));
        return !ended;
    }

private:
    void queue(char type, std::span<const std::uint8_t> data) {
        const std::size_t len = std::min(data.size(), kMaxFrame - 1) + 1;
        out_.push_back(static_cast<std::uint8_t>(len));
        out_.push_back(static_cast<std::uint8_t>(len >> 8));
        out_.push_back(static_cast<std::uint8_t>(type));
        out_.insert(out_.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(len - 1));
    }

    Socket sock_;
    std::vector<std::uint8_t> out_, in_;
    std::size_t out_pos_ = 0;
};

// The game's side: queues the emulation thread touches, and the status others read.
struct Shared {
    std::mutex tx_mutex;
    std::vector<std::uint8_t> tx;
    std::int64_t tx_first = 0, tx_last = 0;
    std::mutex rx_mutex;
    std::vector<std::uint8_t> rx;
    std::size_t rx_pos = 0;
    mutable std::mutex state_mutex;
    LinkStatus status;
    std::optional<RaceSettings> settings;
    std::optional<RaceSettings> new_settings;
    std::string via;                       // host: the purpose of the key the friend proved
    std::optional<SocketAddress> endpoint;  // guest: the address that answered
    std::string suggestion;                 // guest: advice when it failed
    Waker waker;

    void send(std::span<const std::uint8_t> bytes) {
        bool wake;
        {
            std::lock_guard lock(tx_mutex);
            const std::int64_t now = now_us();
            wake = tx.empty();
            if (wake) {
                tx_first = now;
            }
            tx_last = now;
            tx.insert(tx.end(), bytes.begin(), bytes.end());
        }
        if (wake) {
            waker.wake();
        }
    }

    std::size_t receive(std::span<std::uint8_t> out) {
        std::lock_guard lock(rx_mutex);
        const std::size_t n = std::min(out.size(), rx.size() - rx_pos);
        std::copy_n(rx.begin() + static_cast<std::ptrdiff_t>(rx_pos), n, out.begin());
        rx_pos += n;
        if (rx_pos == rx.size()) {
            rx.clear();
            rx_pos = 0;
        }
        return n;
    }

    void deliver(PeerStream& stream) {
        std::vector<std::uint8_t> got;
        stream.take_received(got);
        if (!got.empty()) {
            std::lock_guard lock(rx_mutex);
            rx.insert(rx.end(), got.begin(), got.end());
        }
    }

    // The game's bytes when they're due (Batching); `tx_due` gets when they will be otherwise.
    void take_tx(const Batching& batching, std::int64_t now, std::int64_t& last_send, std::int64_t& tx_due,
                 std::vector<std::uint8_t>& batch) {
        std::lock_guard lock(tx_mutex);
        tx_due = kNever;
        if (!tx.empty()) {
            tx_due = batching.due(tx_first, tx_last, last_send);
            if (now >= tx_due || tx.size() >= 4096) {
                batch.swap(tx);
                tx_due = kNever;
                last_send = now;
            }
        }
    }

    void drop_tx() {
        std::lock_guard lock(tx_mutex);
        tx.clear();
    }
};

int timeout_ms(std::int64_t now, std::int64_t at) {
    return static_cast<int>(std::clamp<std::int64_t>((at - now + 999) / 1000, 0, 1000));
}

}  // namespace

std::string direct_guest_proof(std::string_view key, std::string_view host_nonce, std::string_view guest_nonce) {
    return sha1_hex(std::string(key) + ":" + std::string(host_nonce) + ":" + std::string(guest_nonce));
}

std::string direct_host_proof(std::string_view key, std::string_view host_nonce, std::string_view guest_nonce) {
    return sha1_hex(std::string(key) + ":" + std::string(guest_nonce) + ":" + std::string(host_nonce) + ":host");
}

std::string DirectOffer::encode() const {
    std::string out = "direct 1\nkey=" + key;
    for (const auto& ep : endpoints) {
        out += "\nep=" + ep.to_string();
    }
    return out;
}

std::optional<DirectOffer> DirectOffer::decode(std::string_view text) {
    if (!text.starts_with("direct 1\n")) {
        return std::nullopt;
    }
    DirectOffer offer;
    text.remove_prefix(9);
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        const std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        if (line.starts_with("key=")) {
            offer.key = line.substr(4);
        } else if (line.starts_with("ep=")) {
            if (auto a = SocketAddress::parse(line.substr(3)); a && a->port != 0 && offer.endpoints.size() < 16) {
                offer.endpoints.push_back(*a);
            }
        }
    }
    if (offer.key.empty()) {
        return std::nullopt;
    }
    return offer;
}

// ===== Host ============================================================================================

struct DirectHost::Impl {
    DirectHostOptions options;
    Shared shared;
    Socket listen4, listen6;
    std::mt19937_64 rng = seeded();

    std::mutex keys_mutex;
    std::map<std::string, std::string> keys;  // purpose -> key

    struct Pending {
        std::unique_ptr<Framed> conn;
        std::string nonce;
        std::int64_t deadline = 0;
        bool closing = false;  // an error is on its way; then close
    };
    std::vector<Pending> pending;

    enum class Friend { None, Here, Away, Left } friend_state = Friend::None;
    std::unique_ptr<Framed> conn;
    std::string session, via;
    std::int64_t away_deadline = 0, last_rx = 0, last_ping = 0, ping_sent = -1;
    double ping_rtt_ms = -1;
    std::uint32_t settings_gen = 1;
    RaceSettings settings;
    std::vector<Framed::Frame> carried;  // frames that came with the hello, for the friend's connection
    int reconnects = 0;
    std::string reason;
    bool friend_left = false;  // the last friend left (and nobody has joined since)
    bool locked = false;       // too many wrong proofs
    PeerStream stream{{}, [this](std::span<const std::uint8_t> m) {
                          if (conn) {
                              conn->send_binary(m);
                          }
                      }};

    void drop_friend_connection(std::int64_t now) {
        conn.reset();
        stream.set_open(false, now);
        if (friend_state == Friend::Here) {
            friend_state = Friend::Away;
            away_deadline = now + kGraceUs;
        }
    }

    void refuse(Pending& p, const std::string& code, const std::string& why) {
        p.conn->send_text(R"({"t":"error","code":)" + json_quote(code) + R"(,"reason":)" + json_quote(why) + "}");
        p.closing = true;
        p.deadline = now_us() + 1'000'000;
    }

    void hello(Pending& p, const Framed::Frame& f, std::atomic<int>& wrong, std::int64_t now) {
        const auto m = parse_json_object(f.text());
        if (f.type != 'T' || !m || field(*m, "t") != "hello") {
            p.conn.reset();
            return;
        }
        if (number(*m, "proto", 0) != kProtocolVersion) {
            return refuse(p, "version", number(*m, "proto", 0) < kProtocolVersion
                                            ? "Your VETTE! 2026 is older than your friend's. You both need the same version."
                                            : "Your VETTE! 2026 is newer than your friend's. You both need the same version.");
        }
        if (field(*m, "app") != options.app_version) {
            return refuse(p, "app", "Your friend has VETTE! 2026 " + options.app_version + " and you have " +
                                        field(*m, "app") + ". You both need the same version.");
        }
        if (field(*m, "game") != options.game_build) {
            return refuse(p, "game", "Your friend's copy of VETTE! is " + options.game_build + " and yours is " +
                                         field(*m, "game") + ". You both need the same version of the original game.");
        }
        if (locked) {
            return refuse(p, "closed", "Your friend's game isn't taking players any more. Ask them to host again.");
        }
        // The key the guest proves it has. A direct code's guest can't tell which of the host's codes it
        // has: the internet one ("code") or the same-network one ("lan").
        const std::string asked_via = field(*m, "via");
        const std::string guest_nonce = field(*m, "nonce");
        std::string key, key_via;
        if (guest_nonce.size() >= 16) {
            std::lock_guard lock(keys_mutex);
            for (const auto& [purpose, k] : keys) {
                if ((purpose == asked_via || (asked_via == "code" && purpose == "lan")) &&
                    field(*m, "proof") == direct_guest_proof(k, p.nonce, guest_nonce)) {
                    key = k;
                    key_via = purpose;
                    break;
                }
            }
        }
        if (key.empty()) {
            if (++wrong >= kMaxWrong) {
                locked = true;
                reason = "Someone tried to join with a wrong code " + std::to_string(kMaxWrong) +
                         " times, so your game stopped taking players. Host again for a new code.";
            }
            return refuse(p, "code", "That code doesn't match your friend's game. Check it with your friend: "
                                     "they get a new code each time they host.");
        }
        const std::string asked = field(*m, "session");
        const bool resume = !asked.empty() && asked == session &&
                            (friend_state == Friend::Here || friend_state == Friend::Away);
        if (!resume && (friend_state == Friend::Here || friend_state == Friend::Away)) {
            return refuse(p, "full", "Your friend's game already has a player.");
        }
        if (!resume) {
            session = random_hex(rng, 8);
            via = key_via;
            stream.reset();
            friend_left = false;
            reason.clear();
        }
        conn = std::move(p.conn);
        friend_state = Friend::Here;
        last_rx = last_ping = now;
        conn->send_text(R"({"t":"welcome","session":")" + session + R"(","resumed":)" + (resume ? "true" : "false") +
                        R"(,"proof":")" + direct_host_proof(key, p.nonce, guest_nonce) +
                        R"(","settings_gen":)" + std::to_string(settings_gen) + R"(,"settings":)" +
                        json_quote(settings.serialize()) + "}");
        stream.set_open(true, now);
        if (resume) {
            ++reconnects;
            stream.resume(now);
        }
    }

    void publish(std::atomic<bool>& connected) {
        LinkStatus s;
        s.host = true;
        switch (friend_state) {
        case Friend::None:
            s.state = locked ? LinkState::Failed : friend_left ? LinkState::PeerLeft : LinkState::Waiting;
            break;
        case Friend::Here: s.state = LinkState::Connected; break;
        case Friend::Away: s.state = LinkState::PeerAway; break;
        case Friend::Left: s.state = LinkState::PeerLeft; break;
        }
        s.reason = reason;
        s.settings_gen = settings_gen;
        s.reconnects = reconnects;
        s.server_rtt_ms = ping_rtt_ms;
        stream.fill(s);
        std::lock_guard lock(shared.state_mutex);
        shared.status = s;
        if (!shared.new_settings) {
            shared.settings = settings;
        }
        shared.via = via;
        connected = friend_state == Friend::Here || friend_state == Friend::Away;
    }
};

std::unique_ptr<DirectHost> DirectHost::start(DirectHostOptions options, std::string& error) {
    std::unique_ptr<DirectHost> h(new DirectHost());
    h->impl_ = std::make_unique<Impl>();
    Impl& im = *h->impl_;
    im.options = std::move(options);
    im.settings = im.options.race_settings;
    // The port: the usual one (a short code), else one of the next 9, else any.
    std::vector<std::uint16_t> ports;
    for (int i = 0; i < 10 && im.options.port != 0; ++i) {
        ports.push_back(static_cast<std::uint16_t>(im.options.port + i));
    }
    ports.push_back(0);
    for (const auto port : ports) {
        std::string why;
        im.listen4 = Socket::listen_tcp(false, port, im.options.loopback_only, why);
        if (im.listen4.valid()) {
            h->port_ = im.listen4.local_address() ? im.listen4.local_address()->port : port;
            break;
        }
        error = "Can't listen for your friend's game: " + why + ".";
    }
    if (!im.listen4.valid()) {
        return nullptr;
    }
    error.clear();
    std::string ignored;
    im.listen6 = Socket::listen_tcp(true, h->port_, im.options.loopback_only, ignored);
    h->ipv6_ = im.listen6.valid();
    im.publish(h->connected_);
    h->thread_ = std::thread([p = h.get()] { p->run(); });
    return h;
}

DirectHost::~DirectHost() {
    close();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void DirectHost::allow(const std::string& purpose, const std::string& key) {
    std::lock_guard lock(impl_->keys_mutex);
    impl_->keys[purpose] = key;
}

void DirectHost::disallow(const std::string& purpose) {
    std::lock_guard lock(impl_->keys_mutex);
    impl_->keys.erase(purpose);
}

void DirectHost::send(std::span<const std::uint8_t> bytes) {
    if (!bytes.empty() && connected_.load()) {
        impl_->shared.send(bytes);
    }
}

std::size_t DirectHost::receive(std::span<std::uint8_t> out) { return impl_->shared.receive(out); }

LinkStatus DirectHost::status() const {
    std::lock_guard lock(impl_->shared.state_mutex);
    return impl_->shared.status;
}

std::string DirectHost::joined_via() const {
    std::lock_guard lock(impl_->shared.state_mutex);
    return impl_->shared.via;
}

bool DirectHost::set_race_settings(const RaceSettings& settings) {
    {
        std::lock_guard lock(impl_->shared.state_mutex);
        impl_->shared.new_settings = settings;
        impl_->shared.settings = settings;
    }
    impl_->shared.waker.wake();
    return true;
}

std::optional<RaceSettings> DirectHost::race_settings() const {
    std::lock_guard lock(impl_->shared.state_mutex);
    return impl_->shared.settings;
}

void DirectHost::simulate_drop() {
    drop_ = true;
    impl_->shared.waker.wake();
}

void DirectHost::close() {
    stop_ = true;
    if (impl_) {
        impl_->shared.waker.wake();
    }
}

void DirectHost::run() {
    Impl& im = *impl_;
    std::vector<std::uint8_t> batch;
    std::int64_t last_send = std::numeric_limits<std::int64_t>::min() / 2, tx_due = kNever;
    std::vector<Framed::Frame> frames;
    std::vector<PollItem> items;

    for (;;) {
        std::int64_t now = now_us();
        std::string error;
        if (stop_) {
            if (im.conn) {
                im.conn->send_text("bye");
                const std::int64_t until = now + 500'000;
                while (im.conn->wants_write() && now_us() < until && im.conn->flush(error)) {
                    PollItem it{&im.conn->socket(), false, true};
                    wait_sockets({&it, 1}, nullptr, 10);
                }
            }
            return;
        }

        // New connections: each gets a challenge.
        for (Socket* l : {&im.listen4, &im.listen6}) {
            while (l->valid() && im.pending.size() < 8) {
                Socket s = l->accept();
                if (!s.valid()) {
                    break;
                }
                Impl::Pending p;
                p.conn = std::make_unique<Framed>(std::move(s));
                p.nonce = random_hex(im.rng, 16);
                p.deadline = now + kHandshakeUs;
                p.conn->send_text(R"({"t":"challenge","proto":)" + std::to_string(kProtocolVersion) +
                                  R"(,"nonce":")" + p.nonce + "\"}");
                im.pending.push_back(std::move(p));
            }
        }
        // Handshakes.
        for (auto& p : im.pending) {
            if (!p.conn) {
                continue;
            }
            frames.clear();
            const bool open = p.conn->read(frames, error);
            for (std::size_t i = 0; i < frames.size(); ++i) {
                if (!p.conn) {
                    // In: what came after the hello is the friend's.
                    im.carried.assign(frames.begin() + static_cast<std::ptrdiff_t>(i), frames.end());
                    break;
                }
                if (p.closing) {
                    break;
                }
                im.hello(p, frames[i], wrong_, now);
            }
            if (p.conn && (!open || !p.conn->flush(error) || now >= p.deadline || (p.closing && !p.conn->wants_write()))) {
                p.conn.reset();
            }
        }
        std::erase_if(im.pending, [](const Impl::Pending& p) { return !p.conn; });

        // Settings from the game's thread.
        std::optional<RaceSettings> new_settings;
        {
            std::lock_guard lock(im.shared.state_mutex);
            new_settings.swap(im.shared.new_settings);
        }
        if (new_settings) {
            im.settings = *new_settings;
            ++im.settings_gen;
            if (im.conn) {
                im.conn->send_text(R"({"t":"settings","gen":)" + std::to_string(im.settings_gen) + R"(,"data":)" +
                                   json_quote(im.settings.serialize()) + "}");
            }
        }

        if (drop_.exchange(false) && im.conn) {
            im.drop_friend_connection(now);
        }

        // The friend.
        if (im.conn) {
            frames.swap(im.carried);
            im.carried.clear();
            bool open = im.conn->read(frames, error);
            for (const auto& f : frames) {
                im.last_rx = now;
                if (f.type == 'B') {
                    im.stream.on_message(f.data, now);
                } else if (f.text() == "ping") {
                    im.conn->send_text("pong");
                } else if (f.text() == "pong" && im.ping_sent >= 0) {
                    im.ping_rtt_ms = static_cast<double>(now - im.ping_sent) / 1000.0;
                    im.ping_sent = -1;
                } else if (f.text() == "bye") {
                    im.friend_state = Impl::Friend::Left;
                    im.reason = "Your friend left the race.";
                    open = false;
                }
            }
            if (open && now - im.last_rx >= kDeadUs) {
                open = false;
            }
            if (!open) {
                im.drop_friend_connection(now);  // away: the game's bytes wait for them
            }
        }
        if (im.friend_state == Impl::Friend::Away && now >= im.away_deadline) {
            im.friend_state = Impl::Friend::Left;
            im.reason = "Your friend's connection was lost.";
        }
        if (im.friend_state == Impl::Friend::Left) {
            // Gone for good: another friend may join.
            im.stream.reset();
            im.session.clear();
            im.via.clear();
            im.friend_state = Impl::Friend::None;
            im.friend_left = true;
            im.reconnects = 0;
            im.shared.drop_tx();
        }

        // The game's bytes.
        im.shared.take_tx(im.options.batching, now, last_send, tx_due, batch);
        if (!batch.empty()) {
            if (im.friend_state == Impl::Friend::Here || im.friend_state == Impl::Friend::Away) {
                if (!im.stream.write(batch, now)) {
                    im.friend_state = Impl::Friend::Left;
                    im.reason = "Your friend has been unreachable for too long.";
                }
            }
            batch.clear();
        }
        if (im.conn && now - im.last_ping >= kPingUs) {
            im.conn->send_text("ping");
            im.last_ping = im.ping_sent = now;
        }
        im.stream.tick(now);
        if (im.conn && !im.conn->flush(error)) {
            im.drop_friend_connection(now);
        }
        im.shared.deliver(im.stream);
        im.publish(connected_);

        // Wait.
        items.clear();
        for (const Socket* l : {&im.listen4, &im.listen6}) {
            if (l->valid()) {
                items.push_back({l, true, false});
            }
        }
        std::int64_t wake = now + 1'000'000;
        for (const auto& p : im.pending) {
            items.push_back({&p.conn->socket(), true, p.conn->wants_write()});
            wake = std::min(wake, p.deadline);
        }
        if (im.conn) {
            items.push_back({&im.conn->socket(), true, im.conn->wants_write()});
            wake = std::min({wake, im.last_ping + kPingUs, im.last_rx + kDeadUs});
        }
        if (im.friend_state == Impl::Friend::Away) {
            wake = std::min(wake, im.away_deadline);
        }
        wake = std::min({wake, im.stream.next_timer(), tx_due});
        now = now_us();
        wait_sockets(items, &im.shared.waker, timeout_ms(now, wake));
    }
}

// ===== Guest ===========================================================================================

struct DirectJoin::Impl {
    DirectJoinOptions options;
    Shared shared;
    std::mt19937_64 rng = seeded();

    struct Dial {
        SocketAddress endpoint;
        std::unique_ptr<Framed> conn;
        bool tcp_up = false;
        std::string guest_nonce, host_nonce;
        std::string error;
    };
    std::vector<Dial> dials;

    enum class Phase { Dialing, Up, Reconnecting, Done } phase = Phase::Dialing;
    std::unique_ptr<Framed> conn;
    std::optional<SocketAddress> endpoint;
    std::string session;
    std::int64_t deadline = 0, give_up_at = 0, next_attempt = 0, backoff = 0;
    std::int64_t last_rx = 0, last_ping = 0, ping_sent = -1;
    double ping_rtt_ms = -1;
    std::uint32_t settings_gen = 0;
    std::optional<RaceSettings> settings;
    LinkState final_state = LinkState::Connecting;
    std::string reason, suggestion, refusal;
    bool refused_for_good = false;  // the right game said no (versions): no point trying the others
    bool lost = false;              // came back to a game that no longer knows us
    int reconnects = 0;
    std::vector<Framed::Frame> carried;  // frames that came with the welcome
    PeerStream stream{{}, [this](std::span<const std::uint8_t> m) {
                          if (conn) {
                              conn->send_binary(m);
                          }
                      }};

    void dial(const SocketAddress& ep) {
        Dial d;
        d.endpoint = ep;
        std::string error;
        Socket s = Socket::connect_tcp(ep, error);
        if (!s.valid()) {
            d.error = error;
        } else {
            d.conn = std::make_unique<Framed>(std::move(s));
        }
        d.guest_nonce = random_hex(rng, 16);
        dials.push_back(std::move(d));
    }

    void finish(LinkState state, std::string why, std::string advice = {}) {
        phase = Phase::Done;
        final_state = state;
        reason = std::move(why);
        suggestion = std::move(advice);
        conn.reset();
        dials.clear();
        stream.set_open(false, 0);
    }

    // A frame on a connection being set up; true when it's in (welcomed).
    bool handshake(Dial& d, const Framed::Frame& f, std::int64_t now) {
        const auto m = parse_json_object(f.text());
        if (f.type != 'T' || !m) {
            d.error = "It isn't a VETTE! 2026 game.";
            d.conn.reset();
            return false;
        }
        const std::string t = field(*m, "t");
        if (t == "challenge" && d.host_nonce.empty()) {
            d.host_nonce = field(*m, "nonce");
            d.conn->send_text(R"({"t":"hello","proto":)" + std::to_string(kProtocolVersion) + R"(,"app":)" +
                              json_quote(options.app_version) + R"(,"game":)" + json_quote(options.game_build) +
                              R"(,"via":)" + json_quote(options.purpose) + R"(,"nonce":")" + d.guest_nonce +
                              R"(","proof":")" + direct_guest_proof(options.key, d.host_nonce, d.guest_nonce) +
                              R"(","session":")" + session + "\"}");
            return false;
        }
        if (t == "error") {
            refusal = field(*m, "reason");
            const std::string code = field(*m, "code");
            d.conn.reset();
            d.error = refusal;
            // Final, whatever the other addresses say: it's the right game, and it said no.
            refused_for_good = refused_for_good || code == "version" || code == "app" || code == "game" ||
                               code == "closed";
            return false;
        }
        if (t == "welcome" && !d.host_nonce.empty()) {
            if (field(*m, "proof") != direct_host_proof(options.key, d.host_nonce, d.guest_nonce)) {
                d.error = "It isn't your friend's game.";
                d.conn.reset();
                return false;
            }
            const bool resumed = field(*m, "resumed") == "true";
            if (!session.empty() && !resumed) {
                // Our friend's game no longer knows us: the cable's bytes in between are lost.
                lost = true;
                d.conn.reset();
                return false;
            }
            session = field(*m, "session");
            const auto gen = number(*m, "settings_gen", 0);
            if (gen > settings_gen) {
                if (auto s = RaceSettings::parse(field(*m, "settings"))) {
                    settings = std::move(*s);
                    settings_gen = static_cast<std::uint32_t>(gen);
                }
            }
            conn = std::move(d.conn);
            endpoint = d.endpoint;
            last_rx = last_ping = now;
            phase = Phase::Up;
            backoff = 0;
            stream.set_open(true, now);
            if (resumed) {
                ++reconnects;
                stream.resume(now);
            }
            return true;
        }
        return false;
    }

    void failed_to_reach() {
        std::string where;
        bool refused = false;
        for (const auto& d : dials) {
            if (!where.empty()) {
                where += ", ";
            }
            where += d.endpoint.to_string();
            refused = refused || d.error.find("refused") != std::string::npos;
        }
        if (!refusal.empty()) {
            finish(LinkState::Failed, refusal, "Check the code with your friend.");
            return;
        }
        finish(LinkState::Failed,
               std::string("Your friend's game ") + (refused ? "refused the connection" : "didn't answer") + " at " +
                   (where.empty() ? std::string("the code's address") : where) + ".",
               "Check the code with your friend. If it's right, their router or firewall is blocking the "
               "connection: host the race yourself and send them your code instead.");
    }

    void publish(std::atomic<bool>& connected) {
        LinkStatus s;
        switch (phase) {
        case Phase::Dialing: s.state = LinkState::Connecting; break;
        case Phase::Up: s.state = LinkState::Connected; break;
        case Phase::Reconnecting: s.state = LinkState::Reconnecting; break;
        case Phase::Done: s.state = final_state; break;
        }
        s.reason = reason;
        s.settings_gen = settings_gen;
        s.reconnects = reconnects;
        s.server_rtt_ms = ping_rtt_ms;
        stream.fill(s);
        std::lock_guard lock(shared.state_mutex);
        shared.status = s;
        shared.settings = settings;
        shared.endpoint = endpoint;
        shared.suggestion = suggestion;
        connected = phase == Phase::Up || phase == Phase::Reconnecting;
    }
};

std::unique_ptr<DirectJoin> DirectJoin::start(DirectJoinOptions options) {
    std::unique_ptr<DirectJoin> j(new DirectJoin());
    j->impl_ = std::make_unique<Impl>();
    j->impl_->options = std::move(options);
    j->impl_->publish(j->connected_);
    j->thread_ = std::thread([p = j.get()] { p->run(); });
    return j;
}

DirectJoin::~DirectJoin() {
    close();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void DirectJoin::send(std::span<const std::uint8_t> bytes) {
    if (!bytes.empty() && connected_.load()) {
        impl_->shared.send(bytes);
    }
}

std::size_t DirectJoin::receive(std::span<std::uint8_t> out) { return impl_->shared.receive(out); }

LinkStatus DirectJoin::status() const {
    std::lock_guard lock(impl_->shared.state_mutex);
    return impl_->shared.status;
}

std::optional<SocketAddress> DirectJoin::endpoint() const {
    std::lock_guard lock(impl_->shared.state_mutex);
    return impl_->shared.endpoint;
}

std::optional<RaceSettings> DirectJoin::race_settings() const {
    std::lock_guard lock(impl_->shared.state_mutex);
    return impl_->shared.settings;
}

std::string DirectJoin::suggestion() const {
    std::lock_guard lock(impl_->shared.state_mutex);
    return impl_->shared.suggestion;
}

void DirectJoin::simulate_drop(int offline_ms) {
    drop_offline_ms_ = offline_ms;
    drop_ = true;
    impl_->shared.waker.wake();
}

void DirectJoin::close() {
    stop_ = true;
    if (impl_) {
        impl_->shared.waker.wake();
    }
}

void DirectJoin::run() {
    Impl& im = *impl_;
    std::vector<std::uint8_t> batch;
    std::int64_t last_send = std::numeric_limits<std::int64_t>::min() / 2, tx_due = kNever;
    std::vector<Framed::Frame> frames;
    std::vector<PollItem> items;

    const std::int64_t start = now_us();
    im.deadline = start + std::int64_t{im.options.timeout_ms} * 1000;
    for (const auto& ep : im.options.endpoints) {
        im.dial(ep);
    }
    if (im.options.endpoints.empty()) {
        im.finish(LinkState::Failed, "There's no address to connect to.");
    }

    for (;;) {
        std::int64_t now = now_us();
        std::string error;
        if (stop_) {
            if (im.conn) {
                im.conn->send_text("bye");
                const std::int64_t until = now + 500'000;
                while (im.conn->wants_write() && now_us() < until && im.conn->flush(error)) {
                    PollItem it{&im.conn->socket(), false, true};
                    wait_sockets({&it, 1}, nullptr, 10);
                }
            }
            im.finish(LinkState::Closed, "You left the race.");
            im.publish(connected_);
            return;
        }

        // Getting in: every address at once.
        if (im.phase == Impl::Phase::Dialing || im.phase == Impl::Phase::Reconnecting) {
            if (im.phase == Impl::Phase::Reconnecting && im.dials.empty() && now >= im.next_attempt) {
                if (now >= im.give_up_at) {
                    im.finish(LinkState::PeerLeft, "The connection to your friend was lost.");
                } else {
                    im.dial(*im.endpoint);
                    im.deadline = std::min(im.give_up_at, now + 5'000'000);
                }
            }
            for (auto& d : im.dials) {
                if (!d.conn) {
                    continue;
                }
                if (!d.tcp_up) {
                    const auto st = d.conn->socket().finish_connect(error);
                    if (st == Socket::ConnectState::Failed) {
                        d.error = error;
                        d.conn.reset();
                        continue;
                    }
                    d.tcp_up = st == Socket::ConnectState::Connected;
                    if (!d.tcp_up) {
                        continue;
                    }
                }
                frames.clear();
                const bool open = d.conn->read(frames, error);
                for (std::size_t i = 0; i < frames.size() && d.conn; ++i) {
                    if (im.handshake(d, frames[i], now)) {
                        im.carried.assign(frames.begin() + static_cast<std::ptrdiff_t>(i) + 1, frames.end());
                        break;
                    }
                }
                if (d.conn && (!open || !d.conn->flush(error))) {
                    d.error = error;
                    d.conn.reset();
                    continue;
                }
                if (im.phase == Impl::Phase::Up) {
                    break;
                }
            }
            if (im.phase == Impl::Phase::Up) {
                im.dials.clear();  // the others lose
            } else if (im.lost) {
                im.finish(LinkState::PeerLeft, "The connection to your friend was lost.");
            } else if (im.refused_for_good) {
                im.finish(im.phase == Impl::Phase::Dialing ? LinkState::Failed : LinkState::PeerLeft, im.refusal);
            } else if (im.phase == Impl::Phase::Dialing || !im.dials.empty()) {
                // (Reconnecting with no attempt under way: waiting for the next one.)
                const bool all_failed =
                    std::all_of(im.dials.begin(), im.dials.end(), [](const Impl::Dial& d) { return !d.conn; });
                if ((all_failed && !im.dials.empty()) || now >= im.deadline) {
                    if (im.phase == Impl::Phase::Dialing) {
                        im.failed_to_reach();
                    } else {
                        im.dials.clear();
                        im.next_attempt = now + im.backoff;
                        im.backoff = std::min<std::int64_t>(im.backoff == 0 ? 250'000 : im.backoff * 2, 2'000'000);
                    }
                }
            }
        }

        if (drop_.exchange(false) && im.phase == Impl::Phase::Up) {
            im.conn.reset();
            im.stream.set_open(false, now);
            im.phase = Impl::Phase::Reconnecting;
            im.give_up_at = now + kGraceUs;
            im.next_attempt = now + std::int64_t{drop_offline_ms_.load()} * 1000;
            im.backoff = 250'000;
        }

        // Connected.
        if (im.phase == Impl::Phase::Up) {
            frames.swap(im.carried);
            im.carried.clear();
            bool open = im.conn->read(frames, error);
            for (const auto& f : frames) {
                im.last_rx = now;
                if (f.type == 'B') {
                    im.stream.on_message(f.data, now);
                } else if (f.text() == "ping") {
                    im.conn->send_text("pong");
                } else if (f.text() == "pong" && im.ping_sent >= 0) {
                    im.ping_rtt_ms = static_cast<double>(now - im.ping_sent) / 1000.0;
                    im.ping_sent = -1;
                } else if (f.text() == "bye") {
                    im.finish(LinkState::PeerLeft, "Your friend left the race.");
                    open = true;
                    break;
                } else if (const auto m = parse_json_object(f.text()); m && field(*m, "t") == "settings") {
                    const auto gen = number(*m, "gen", 0);
                    if (gen > im.settings_gen) {
                        if (auto s = RaceSettings::parse(field(*m, "data"))) {
                            im.settings = std::move(*s);
                            im.settings_gen = static_cast<std::uint32_t>(gen);
                        }
                    }
                }
            }
            if (im.phase == Impl::Phase::Up && (!open || now - im.last_rx >= kDeadUs)) {
                // Dropped: back in within the grace period, at the address that worked.
                im.conn.reset();
                im.stream.set_open(false, now);
                im.phase = Impl::Phase::Reconnecting;
                im.give_up_at = now + kGraceUs;
                im.next_attempt = now;
                im.backoff = 250'000;
            }
        }

        im.shared.take_tx(im.options.batching, now, last_send, tx_due, batch);
        if (!batch.empty()) {
            if ((im.phase == Impl::Phase::Up || im.phase == Impl::Phase::Reconnecting) && !im.stream.write(batch, now)) {
                im.finish(LinkState::PeerLeft, "Your friend has been unreachable for too long.");
            }
            batch.clear();
        }
        if (im.phase == Impl::Phase::Up) {
            if (now - im.last_ping >= kPingUs) {
                im.conn->send_text("ping");
                im.last_ping = im.ping_sent = now;
            }
            im.stream.tick(now);
            if (!im.conn->flush(error)) {
                im.conn.reset();
                im.stream.set_open(false, now);
                im.phase = Impl::Phase::Reconnecting;
                im.give_up_at = now + kGraceUs;
                im.next_attempt = now;
            }
        }
        im.shared.deliver(im.stream);
        im.publish(connected_);
        if (im.phase == Impl::Phase::Done) {
            return;
        }

        // Wait.
        items.clear();
        std::int64_t wake = now + 1'000'000;
        for (const auto& d : im.dials) {
            if (d.conn) {
                items.push_back({&d.conn->socket(), d.tcp_up, !d.tcp_up || d.conn->wants_write()});
            }
        }
        if (im.phase == Impl::Phase::Dialing || im.phase == Impl::Phase::Reconnecting) {
            wake = std::min(wake, im.dials.empty() ? im.next_attempt : im.deadline);
        }
        if (im.phase == Impl::Phase::Up) {
            items.push_back({&im.conn->socket(), true, im.conn->wants_write()});
            wake = std::min({wake, im.last_ping + kPingUs, im.last_rx + kDeadUs, im.stream.next_timer()});
        }
        wake = std::min(wake, tx_due);
        now = now_us();
        wait_sockets(items, &im.shared.waker, timeout_ms(now, wake));
    }
}

}  // namespace vette::net
