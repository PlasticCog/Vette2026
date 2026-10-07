// Online play: hosting and joining through net::OnlineLink, by a direct code over the loopback interface
// (no server, no router). Built as vette_online_tests (it needs vette_net).

#include <chrono>
#include <functional>
#include <thread>

#include "net/online.h"
#include "test.h"

using namespace vette::net;

namespace {

bool wait_for(const std::function<bool()>& cond, int ms = 8000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (cond()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

OnlineOptions options() {
    OnlineOptions o;
    o.app_version = "0.1.6";
    o.game_build = "DOS 1.1";
    o.room = false;
    o.port = 0;
    o.loopback_only = true;
    o.code_address = "127.0.0.1";
    return o;
}

}  // namespace

TEST(online_direct_code_host_and_join) {
    OnlineOptions ho = options();
    ho.race_settings.set("setup", "vette2p/1 course=2 improved=1");
    auto host = OnlineLink::host(ho);
    CHECK(wait_for([&] { return host->status().state == LinkState::Waiting; }));
    const OnlineStatus hs = host->status();
    CHECK(hs.host && hs.direct.state == RouteStatus::State::Ready);
    CHECK(hs.room.state == RouteStatus::State::Off);
    CHECK_EQ(host->invite_url(), hs.direct.code);  // no server: the code itself

    auto guest = OnlineLink::join(options(), "Race me: vette2026://direct/" + hs.direct.code);
    CHECK(wait_for([&] { return guest->connected() && host->connected(); }));
    CHECK(guest->status().route == Route::Direct && host->status().route == Route::Direct);
    const auto settings = guest->race_settings();
    CHECK(settings && settings->get("setup") == std::optional<std::string>("vette2p/1 course=2 improved=1"));

    const std::vector<std::uint8_t> out = {'I', 'D', 'N', 1, 2, 3};
    host->send(out);
    std::vector<std::uint8_t> got;
    CHECK(wait_for([&] {
        std::uint8_t buf[64];
        const std::size_t n = guest->receive(buf);
        got.insert(got.end(), buf, buf + n);
        return got.size() >= out.size();
    }));
    CHECK(got == out);
    CHECK(wait_for([&] { return guest->status().rtt_ms >= 0; }));

    guest->close();
    CHECK(wait_for([&] { return host->status().state == LinkState::PeerLeft; }));
}

TEST(online_join_refusals) {
    auto bad = OnlineLink::join(options(), "not a code");
    CHECK(wait_for([&] { return bad->status().state == LinkState::Failed; }));
    CHECK(!bad->status().reason.empty() && !bad->status().suggestion.empty());
    auto no_server = OnlineLink::join(options(), "VETTE-4KQ7");  // a room code, but no server is set
    CHECK(wait_for([&] { return no_server->status().state == LinkState::Failed; }));
    CHECK(no_server->status().reason.find("server") != std::string::npos);
}
