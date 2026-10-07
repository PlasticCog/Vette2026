// Online play: the direct connection between two games (net/direct.h), for real over the loopback
// interface: the handshake, refusals, the serial stream, drops and reconnects, leaving.

#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>

#include "net/direct.h"
#include "test.h"

using namespace vette::net;

namespace {

using Clock = std::chrono::steady_clock;

bool wait_for(const std::function<bool()>& cond, int ms = 5000) {
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < until) {
        if (cond()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return cond();
}

std::unique_ptr<DirectHost> start_host(const std::string& key, const std::string& settings = "improved_driving=1") {
    DirectHostOptions o;
    o.app_version = "0.1.6";
    o.game_build = "DOS 1.1";
    o.race_settings = *RaceSettings::parse(settings);
    o.port = 0;  // any free port
    o.loopback_only = true;
    std::string error;
    auto h = DirectHost::start(o, error);
    if (h) {
        h->allow("code", key);
    }
    return h;
}

std::unique_ptr<DirectJoin> start_join(std::uint16_t port, const std::string& key, const std::string& app = "0.1.6",
                                       std::vector<SocketAddress> extra = {}) {
    DirectJoinOptions o;
    o.app_version = app;
    o.game_build = "DOS 1.1";
    o.key = key;
    o.endpoints = std::move(extra);
    o.endpoints.push_back(SocketAddress::ipv4(0x7F000001, port));
    o.timeout_ms = 1000;  // short: reconnecting after a longer outage mustn't depend on it
    return DirectJoin::start(o);
}

// Sends `n` bytes of a known sequence from `from` and checks they arrive at `to` (starting at `*at`).
bool exchange(vette::host::SerialLink& from, vette::host::SerialLink& to, std::uint64_t& sent, std::uint64_t& got, int n) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(n));
    for (auto& b : out) {
        b = static_cast<std::uint8_t>(sent++ * 7 + 3);
    }
    from.send(out);
    bool ok = true;
    const bool arrived = wait_for([&] {
        std::uint8_t buf[512];
        for (std::size_t k; (k = to.receive(buf)) > 0;) {
            for (std::size_t i = 0; i < k; ++i, ++got) {
                ok = ok && buf[i] == static_cast<std::uint8_t>(got * 7 + 3);
            }
        }
        return got >= sent;
    });
    if (!(arrived && ok && got == sent)) {
        std::fprintf(stderr, "  exchange: sent %llu, got %llu, %s\n", static_cast<unsigned long long>(sent),
                     static_cast<unsigned long long>(got), ok ? "in order" : "WRONG BYTES");
    }
    return arrived && ok && got == sent;
}

}  // namespace

TEST(net_direct_connects_and_carries_bytes) {
    auto host = start_host("code:1234");
    CHECK(host != nullptr);
    if (!host) {
        return;
    }
    CHECK(host->status().state == LinkState::Waiting);
    auto guest = start_join(host->port(), "code:1234");
    CHECK(wait_for([&] { return guest->connected() && host->connected(); }));
    CHECK(guest->status().state == LinkState::Connected);
    CHECK_EQ(host->joined_via(), std::string("code"));
    const auto settings = guest->race_settings();
    CHECK(settings && settings->get_bool("improved_driving", false));  // with the welcome
    CHECK(guest->endpoint() && guest->endpoint()->port == host->port());

    std::uint64_t hs = 0, gr = 0, gs = 0, hr = 0;
    for (int i = 0; i < 20; ++i) {
        CHECK(exchange(*host, *guest, hs, gr, 50));
        CHECK(exchange(*guest, *host, gs, hr, 37));
    }
    CHECK(wait_for([&] { return guest->status().rtt_samples > 0 && host->status().rtt_samples > 0; }));

    // Settings changes follow in order.
    CHECK(host->set_race_settings(*RaceSettings::parse("improved_driving=0")));
    CHECK(wait_for([&] { return guest->status().settings_gen == 2; }));
    CHECK(!guest->race_settings()->get_bool("improved_driving", true));

    // Drops on either side: the guest comes back, and nothing is lost or doubled.
    guest->simulate_drop(1500);  // offline for longer than the first connection's timeout
    CHECK(exchange(*host, *guest, hs, gr, 400));  // written while the guest is away
    CHECK(wait_for([&] { return guest->status().reconnects == 1; }));
    host->simulate_drop();
    CHECK(exchange(*guest, *host, gs, hr, 300));
    CHECK(wait_for([&] { return guest->status().reconnects == 2 && host->status().state == LinkState::Connected; }));

    // The guest leaves: the host may take another.
    guest->close();
    CHECK(wait_for([&] { return host->status().state == LinkState::PeerLeft; }));
    CHECK(!host->connected());
    auto second = start_join(host->port(), "code:1234");
    CHECK(wait_for([&] { return second->connected() && host->connected(); }));
    std::uint64_t s2 = 0, r2 = 0;
    CHECK(exchange(*host, *second, s2, r2, 64));
}

TEST(net_direct_refusals) {
    auto host = start_host("code:42");
    if (!host) {
        CHECK(false);
        return;
    }
    auto wrong = start_join(host->port(), "code:43");
    CHECK(wait_for([&] { return wrong->status().state == LinkState::Failed; }));
    CHECK(wrong->status().reason.find("doesn't match") != std::string::npos);
    CHECK_EQ(host->wrong_attempts(), 1);
    CHECK(!host->connected());

    auto old = start_join(host->port(), "code:42", "0.1.5");
    CHECK(wait_for([&] { return old->status().state == LinkState::Failed; }));
    CHECK(old->status().reason.find("same version") != std::string::npos);

    // One of the addresses is wrong: the right one wins.
    auto good = start_join(host->port(), "code:42", "0.1.6", {SocketAddress::ipv4(0x7F000001, 9)});
    CHECK(wait_for([&] { return good->connected(); }));
    // A third player is refused while the friend is in.
    auto third = start_join(host->port(), "code:42");
    CHECK(wait_for([&] { return third->status().state == LinkState::Failed; }));
    CHECK(third->status().reason.find("already has a player") != std::string::npos);
}

TEST(net_direct_unreachable) {
    // Nothing listens on port 9 of the loopback interface: refused at once.
    DirectJoinOptions o;
    o.app_version = "0.1.6";
    o.game_build = "DOS 1.1";
    o.key = "code:1";
    o.endpoints = {SocketAddress::ipv4(0x7F000001, 9)};
    o.timeout_ms = 3000;
    auto j = DirectJoin::start(o);
    CHECK(wait_for([&] { return j->status().state == LinkState::Failed; }, 4000));
    CHECK(j->status().reason.find("127.0.0.1:9") != std::string::npos);
    CHECK(j->suggestion().find("room code") != std::string::npos);
}

TEST(net_direct_lockout) {
    auto host = start_host("code:7");
    if (!host) {
        CHECK(false);
        return;
    }
    for (int i = 0; i < 20; ++i) {
        auto j = start_join(host->port(), "code:8");
        wait_for([&] { return j->status().state == LinkState::Failed; });
    }
    CHECK(wait_for([&] { return host->status().state == LinkState::Failed; }));
    auto right = start_join(host->port(), "code:7");
    CHECK(wait_for([&] { return right->status().state == LinkState::Failed; }));  // too late: locked
}
