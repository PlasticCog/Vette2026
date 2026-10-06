// Online play: the session state machine (net/session.h) against a fake relay server that behaves like
// server/src/index.js: creating and joining a room, the race settings, relaying, drops and reconnects
// within the grace period, and giving up. Time is simulated.

#include <memory>
#include <string>

#include "net/session.h"
#include "test.h"

using namespace vette::net;

namespace {

struct FakeIo : SessionIo {
    std::vector<std::string> connects, texts;
    std::vector<std::vector<std::uint8_t>> binaries;
    int disconnects = 0;
    void connect(const std::string& target) override { connects.push_back(target); }
    void send_text(std::string_view text) override { texts.emplace_back(text); }
    void send_binary(std::span<const std::uint8_t> m) override { binaries.emplace_back(m.begin(), m.end()); }
    void disconnect() override { ++disconnects; }
};

struct Player {
    FakeIo io;
    std::unique_ptr<Session> s;
    bool open = false;
    bool deaf = false;  // what the server sends this player is lost (a half-open connection)
    std::size_t connects_seen = 0;
    std::string token;
    std::vector<std::uint8_t> wrote, got;  // the game's stream out, and what arrived
};

std::string query_value(const std::string& target, const std::string& key) {
    const std::size_t at = target.find(key + "=");
    if (at == std::string::npos) {
        return {};
    }
    std::string raw = target.substr(at + key.size() + 1);
    raw = raw.substr(0, raw.find('&'));
    std::string out;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '%' && i + 2 < raw.size()) {
            out += static_cast<char>(std::stoi(raw.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += raw[i];
        }
    }
    return out;
}

// The relay server, in memory, as server/src/index.js behaves.
class FakeServer {
public:
    std::int64_t now = 1'000'000;
    bool unreachable = false;
    bool answer_pings = true;
    std::string settings;
    int settings_gen = 1;
    Player host, guest;

    SessionConfig config(const std::string& settings_text = {}) {
        SessionConfig c;
        c.app_version = "0.1.6";
        c.game_build = "DOS 1.1";
        c.race_settings = *RaceSettings::parse(settings_text);
        return c;
    }
    void start_host(const std::string& settings_text = "improved_driving=1") {
        host.s = std::make_unique<Session>(host.io, config(settings_text));
        host.s->start(now);
        service();
    }
    void start_guest(const std::string& code = "4KQ7") {
        guest.s = std::make_unique<Session>(guest.io, config(), code);
        guest.s->start(now);
        service();
    }

    Player& other(Player& p) { return &p == &host ? guest : host; }

    void to(Player& p, const std::string& text) {
        if (p.open && !p.deaf) {
            p.s->on_text(text, now);
        }
    }

    // A connection drops (the server notices, unless `silently`).
    void drop(Player& p, bool silently = false) {
        p.open = false;
        p.deaf = false;
        p.io.binaries.clear();
        p.io.texts.clear();
        p.s->on_closed("dropped", now);
        if (!silently) {
            to(other(p), R"({"t":"peer","state":"away"})");
        }
        service();
    }

    void step(std::int64_t dt_us) {
        now += dt_us;
        for (Player* p : {&host, &guest}) {
            if (p->s) {
                p->s->tick(now);
            }
        }
        service();
    }

    void write(Player& p, const std::vector<std::uint8_t>& bytes) {
        if (p.s->write(bytes, now) && p.s->linked()) {
            p.wrote.insert(p.wrote.end(), bytes.begin(), bytes.end());
        }
        service();
    }

    void service() {
        for (bool busy = true; busy;) {
            busy = false;
            for (Player* pp : {&host, &guest}) {
                Player& p = *pp;
                if (!p.s) {
                    continue;
                }
                if (p.io.connects.size() > p.connects_seen) {
                    busy = true;
                    p.connects_seen = p.io.connects.size();
                    accept(p, p.io.connects.back());
                }
                auto texts = std::move(p.io.texts);
                p.io.texts.clear();
                for (const auto& t : texts) {
                    busy = true;
                    control(p, t);
                }
                auto binaries = std::move(p.io.binaries);
                p.io.binaries.clear();
                for (const auto& m : binaries) {
                    busy = true;
                    Player& q = other(p);
                    if (p.open && q.open && !q.deaf) {
                        q.s->on_binary(m, now);
                    }
                }
                p.s->take_received(p.got);
            }
        }
    }

private:
    void accept(Player& p, const std::string& target) {
        if (unreachable) {
            p.s->on_closed("unreachable", now);
            return;
        }
        p.open = true;
        p.s->on_open(now);
        const bool is_host = &p == &host;
        const char* role = is_host ? "host" : "guest";
        Player& q = other(p);
        const std::string peer = !q.s || q.token.empty() ? "none" : q.open ? "here" : "away";
        std::string welcome = std::string(R"({"t":"welcome","role":")") + role +
                              R"(","code":"4KQ7","grace":30,"settings_gen":)" + std::to_string(settings_gen);
        if (target.starts_with("/v1/create")) {
            settings = query_value(target, "settings");
            p.token = "H";
            welcome += R"(,"token":"H","resumed":false,"peer":"none"})";
            to(p, welcome);
        } else if (target.starts_with("/v1/join/4KQ7")) {
            p.token = "G";
            welcome += R"(,"token":"G","resumed":false,"peer":")" + peer + R"(","settings":)" +
                       json_quote(settings) + "}";
            to(p, welcome);
            to(q, R"({"t":"peer","state":"joined"})");
        } else if (target.starts_with("/v1/resume/4KQ7") && query_value(target, "token") == p.token) {
            welcome += R"(,"token":")" + p.token + R"(","resumed":true,"peer":")" + peer + "\"";
            if (!is_host) {
                welcome += R"(,"settings":)" + json_quote(settings);
            }
            to(p, welcome + "}");
            to(q, R"({"t":"peer","state":"back"})");
        } else {
            to(p, R"({"t":"error","code":"no_room","reason":"There's no room VETTE-ZZZZ."})");
            p.open = false;
            p.s->on_closed("closed", now);
        }
    }

    void control(Player& p, const std::string& text) {
        if (!p.open) {
            return;
        }
        if (text == "ping") {
            if (answer_pings) {
                to(p, "pong");
            }
            return;
        }
        if (text == "bye") {
            p.open = false;
            Player& q = other(p);
            to(q, R"({"t":"peer","state":"left","reason":"Your friend left the room."})");
            return;
        }
        const auto m = parse_json_object(text);
        if (m && m->at("t") == "settings" && &p == &host && std::stoi(m->at("gen")) > settings_gen) {
            settings_gen = std::stoi(m->at("gen"));
            settings = m->at("data");
            to(guest, R"({"t":"settings","gen":)" + std::to_string(settings_gen) + R"(,"data":)" +
                          json_quote(settings) + "}");
        }
    }
};

std::vector<std::uint8_t> noise(std::uint32_t& state, int n) {
    std::vector<std::uint8_t> v;
    for (int i = 0; i < n; ++i) {
        state = state * 1664525u + 1013904223u;
        v.push_back(static_cast<std::uint8_t>(state >> 24));
    }
    return v;
}

}  // namespace

TEST(net_session_create_and_join) {
    FakeServer server;
    server.start_host("improved_driving=1");
    CHECK_EQ(server.host.io.connects.size(), std::size_t{1});
    CHECK_EQ(server.host.io.connects[0],
             std::string("/v1/create?proto=1&app=0.1.6&game=DOS%201.1&settings=improved_driving%3D1"));
    CHECK(server.host.s->status().state == LinkState::Waiting);
    CHECK_EQ(server.host.s->status().code, std::string("VETTE-4KQ7"));
    CHECK(server.host.s->status().host);
    CHECK(!server.host.s->linked());
    // Bytes the host's game sends before anyone joins go nowhere.
    server.write(server.host, {1, 2, 3});

    server.start_guest();
    CHECK_EQ(server.guest.io.connects[0], std::string("/v1/join/4KQ7?proto=1&app=0.1.6&game=DOS%201.1"));
    CHECK(server.guest.s->status().state == LinkState::Connected);
    CHECK(server.host.s->status().state == LinkState::Connected);
    CHECK(server.guest.s->linked() && server.host.s->linked());
    CHECK(!server.guest.s->status().host);
    // The settings are there with the welcome, before any serial byte.
    CHECK_EQ(server.guest.s->status().bytes_received, std::uint64_t{0});
    const auto settings = server.guest.s->race_settings();
    CHECK(settings && settings->get_bool("improved_driving", false));
    CHECK_EQ(server.guest.s->status().settings_gen, std::uint32_t{1});

    server.write(server.host, {10, 11, 12});
    server.write(server.guest, {20, 21});
    CHECK(server.guest.got == (std::vector<std::uint8_t>{10, 11, 12}));
    CHECK(server.host.got == (std::vector<std::uint8_t>{20, 21}));
    // Pings are answered.
    server.step(2'100'000);
    CHECK(server.host.s->status().server_rtt_ms >= 0);
}

TEST(net_session_drops_keep_the_stream_intact) {
    FakeServer server;
    server.start_host();
    server.start_guest();
    std::uint32_t rng = 7;
    for (int i = 0; i < 400; ++i) {
        server.write(server.host, noise(rng, 1 + i % 50));
        server.write(server.guest, noise(rng, 1 + i % 23));
        if (i == 100) {
            // The guest's connection drops with messages in flight both ways.
            const auto in_flight = noise(rng, 5);
            server.host.s->write(in_flight, server.now);  // lost on the way to the guest
            server.host.wrote.insert(server.host.wrote.end(), in_flight.begin(), in_flight.end());
            server.drop(server.guest);
            CHECK(server.guest.s->status().state == LinkState::Reconnecting);
            CHECK(server.host.s->status().state == LinkState::PeerAway);
            CHECK(server.guest.s->linked() && server.host.s->linked());  // the cable stays plugged in
        }
        if (i == 200) {
            server.drop(server.host);
        }
        if (i == 300) {
            // A half-open connection: what the server relays to the guest vanishes, and the server
            // doesn't notice; the guest's client reconnects when the server stops answering.
            server.guest.deaf = true;
        }
        if (i == 310) {
            server.drop(server.guest, true);
        }
        server.step(10'000);
    }
    for (int i = 0; i < 100; ++i) {
        server.step(10'000);
    }
    CHECK(server.host.s->status().state == LinkState::Connected);
    CHECK(server.guest.s->status().state == LinkState::Connected);
    CHECK_EQ(server.guest.s->status().reconnects, 2);
    CHECK_EQ(server.host.s->status().reconnects, 1);
    CHECK(server.guest.io.connects.back().starts_with("/v1/resume/4KQ7?proto=1&"));
    CHECK(server.guest.io.connects.back().ends_with("&token=G"));
    // Everything each game wrote has arrived, once, in order (the bytes lost in flight were resent).
    CHECK_EQ(server.guest.got.size(), server.host.wrote.size());
    CHECK_EQ(server.host.got.size(), server.guest.wrote.size());
    CHECK(server.guest.got == server.host.wrote);
    CHECK(server.host.got == server.guest.wrote);
    CHECK(server.guest.s->status().bytes_resent > 0);
}

TEST(net_session_settings_never_go_stale) {
    FakeServer server;
    server.start_host("improved_driving=1");
    server.start_guest();
    RaceSettings s2 = *RaceSettings::parse("improved_driving=0");
    CHECK(server.host.s->set_race_settings(s2, server.now));
    server.service();
    CHECK(server.guest.s->race_settings() == s2);
    CHECK_EQ(server.guest.s->status().settings_gen, std::uint32_t{2});
    CHECK(!server.guest.s->set_race_settings(s2, server.now));  // only the host has settings to give

    // The guest drops; the host changes the settings meanwhile; the guest's welcome has the new ones.
    server.drop(server.guest);
    RaceSettings s3 = *RaceSettings::parse("improved_driving=1\nlaps=2");
    CHECK(server.host.s->set_race_settings(s3, server.now));
    server.service();
    server.step(10'000);
    CHECK(server.guest.s->status().state == LinkState::Connected);
    CHECK(server.guest.s->race_settings() == s3);
    CHECK_EQ(server.guest.s->status().settings_gen, std::uint32_t{3});
    // An old generation arriving late changes nothing.
    server.guest.s->on_text(R"({"t":"settings","gen":2,"data":"improved_driving=0"})", server.now);
    CHECK(server.guest.s->race_settings() == s3);
    // Reconnecting again: the same generation, nothing re-applied.
    server.drop(server.guest);
    server.step(10'000);
    CHECK_EQ(server.guest.s->status().settings_gen, std::uint32_t{3});

    // The host changes them while its own connection is down: they go once it's back.
    server.drop(server.host);
    RaceSettings s4 = *RaceSettings::parse("improved_driving=0\nlaps=5");
    CHECK(server.host.s->set_race_settings(s4, server.now));
    CHECK_EQ(server.settings_gen, 3);
    server.step(10'000);
    CHECK(server.host.s->status().state == LinkState::Connected);
    CHECK_EQ(server.settings_gen, 4);
    CHECK(server.guest.s->race_settings() == s4);
}

TEST(net_session_gives_up_after_the_grace_period) {
    FakeServer server;
    server.start_host();
    server.start_guest();
    server.unreachable = true;
    server.drop(server.guest);
    for (int i = 0; i < 29; ++i) {
        server.step(1'000'000);
    }
    CHECK(server.guest.s->status().state == LinkState::Reconnecting);
    CHECK(server.guest.s->linked());
    server.step(2'000'000);
    CHECK(server.guest.s->status().state == LinkState::PeerLeft);
    CHECK(server.guest.s->finished() && !server.guest.s->linked());
    CHECK(!server.guest.s->status().reason.empty());
}

TEST(net_session_unreachable_server) {
    FakeServer server;
    server.unreachable = true;
    server.start_host();
    for (int i = 0; i < 10; ++i) {
        server.step(1'000'000);
    }
    CHECK(server.host.s->status().state == LinkState::Failed);
    CHECK_EQ(server.host.io.connects.size(), std::size_t{3});
    CHECK(server.host.s->status().reason.find("unreachable") != std::string::npos);
}

TEST(net_session_refused_join) {
    FakeServer server;
    server.start_host();
    server.start_guest("ZZZZ");
    CHECK(server.guest.s->status().state == LinkState::Failed);
    CHECK_EQ(server.guest.s->status().reason, std::string("There's no room VETTE-ZZZZ."));
    CHECK(server.guest.s->finished());
}

TEST(net_session_friend_leaves) {
    FakeServer server;
    server.start_host();
    server.start_guest();
    server.guest.s->leave(server.now);
    server.service();
    CHECK(server.guest.s->status().state == LinkState::Closed);
    CHECK(server.host.s->status().state == LinkState::PeerLeft);
    CHECK(!server.host.s->finished());  // a host stays in the room: a new friend may join
    CHECK(!server.host.s->linked());
    CHECK_EQ(server.host.s->status().reason, std::string("Your friend left the room."));
    server.host.s->on_text(R"({"t":"peer","state":"joined"})", server.now);
    CHECK(server.host.s->status().state == LinkState::Connected);
}

TEST(net_session_dead_connection_reconnects) {
    FakeServer server;
    server.start_host();
    server.start_guest();
    server.answer_pings = false;
    server.guest.deaf = true;  // nothing reaches the guest any more
    for (int i = 0; i < 8; ++i) {
        server.step(1'000'000);
    }
    CHECK(server.guest.io.disconnects >= 1);
    CHECK(server.guest.io.connects.size() >= 2);  // it went back in
}

TEST(net_session_round_trip_time) {
    FakeServer server;
    server.start_host();
    server.start_guest();
    // The host's message reaches the guest 10 ms later; the guest answers 5 ms after that; the answer
    // reaches the host 10 ms later: 20 ms on the network, the 5 ms hold not counted.
    Session& host = *server.host.s;
    Session& guest = *server.guest.s;
    host.write(std::vector<std::uint8_t>{1}, server.now);
    const auto to_guest = server.host.io.binaries.back();
    server.host.io.binaries.clear();
    guest.on_binary(to_guest, server.now + 10'000);
    guest.write(std::vector<std::uint8_t>{2}, server.now + 15'000);
    const auto to_host = server.guest.io.binaries.back();
    server.guest.io.binaries.clear();
    host.on_binary(to_host, server.now + 25'000);
    CHECK(host.status().rtt_samples >= 1);
    CHECK(host.status().rtt_last_ms > 19.9 && host.status().rtt_last_ms < 20.1);
}

TEST(net_session_gap_asks_for_a_resend) {
    FakeServer server;
    server.start_host();
    server.start_guest();
    Session& guest = *server.guest.s;
    // A data message from beyond what has arrived: the guest asks for a resend from where it is.
    MessageHeader h;
    h.offset = 100;
    std::vector<std::uint8_t> m;
    h.append_to(m);
    m.push_back(42);
    guest.on_binary(m, server.now);
    CHECK(!server.guest.io.binaries.empty());
    const auto request = MessageHeader::decode(server.guest.io.binaries.back());
    CHECK(request && request->kind == MessageHeader::kAck && (request->flags & MessageHeader::kResend));
    CHECK(request && request->ack == 0);
    CHECK(server.guest.got.empty());
    server.guest.io.binaries.clear();

    // The host, asked, sends what isn't acknowledged again.
    Session& host = *server.host.s;
    host.write(std::vector<std::uint8_t>{1, 2, 3}, server.now);
    server.host.io.binaries.clear();  // lost
    MessageHeader ask;
    ask.kind = MessageHeader::kAck;
    ask.flags = MessageHeader::kResend;
    std::vector<std::uint8_t> request_bytes;
    ask.append_to(request_bytes);
    host.on_binary(request_bytes, server.now);
    CHECK(!server.host.io.binaries.empty());
    const auto resent = MessageHeader::decode(server.host.io.binaries.back());
    CHECK(resent && resent->kind == MessageHeader::kData && resent->offset == 0);
    CHECK_EQ(server.host.io.binaries.back().size(), MessageHeader::kSize + 3);
}
