// Races on the local network (net/lan.h): the messages, and a search finding a host over the loopback.

#include <chrono>
#include <optional>
#include <string>
#include <thread>

#include "net/invite.h"
#include "net/lan.h"
#include "test.h"

using namespace vette::net;

TEST(net_lan_messages) {
    LanRace r;
    r.host = SocketAddress::ipv4(0x0A000005, kDirectPort);
    r.secret = 123456;
    r.name = "DESK\nTOP";  // a line break can't get into the message
    r.app_version = "0.1.8";
    r.game_build = "C013A5B5";
    CHECK(r.settings.set("setup", "vette2p/1 course=2 improved=1"));
    const SocketAddress from = SocketAddress::ipv4(0xC0A80105, 50000);
    const auto back = parse_lan_answer(lan_answer(r), from);
    CHECK(back.has_value());
    if (!back)
        return;
    CHECK(back->host == SocketAddress::ipv4(0xC0A80105, kDirectPort));  // the sender's address, the race's port
    CHECK(back->secret == 123456);
    CHECK_EQ(back->name, std::string("DESKTOP"));
    CHECK_EQ(back->app_version, std::string("0.1.8"));
    CHECK_EQ(back->game_build, std::string("C013A5B5"));
    CHECK(back->settings.get("setup") == std::optional<std::string>("vette2p/1 course=2 improved=1"));
    // Its code joins that address.
    const auto code = DirectCode::decode(back->code());
    CHECK(code && code->ipv4 == 0xC0A80105 && code->port == kDirectPort && code->secret == 123456);

    CHECK(!parse_lan_answer(lan_question(), from));
    CHECK(!parse_lan_answer("VETTE2026 LAN/1\nport=26989\n\n", from));  // no secret
    CHECK(!parse_lan_answer("VETTE2026 LAN/1\nport=26989\nsecret=5\n", from));  // no end to the header
    CHECK(!parse_lan_answer("hello", from));
}

TEST(net_lan_search_finds_a_host) {
    constexpr std::uint16_t kPort = 36990;  // not the game's, so a running game doesn't answer
    LanHost host;
    std::string error;
    CHECK(host.start(error, kPort, true));
    LanRace r;
    r.host.port = 40000;
    r.secret = 77;
    r.name = "HOST";
    r.app_version = "0.1.8";
    r.game_build = "DOS 1.1";
    host.set_race(r);
    LanSearch search;
    CHECK(search.start(error, kPort));
    std::optional<LanRace> found;
    for (int i = 0; i < 400 && !found; ++i) {
        host.poll();
        search.poll();
        for (const LanRace& x : search.races()) {
            if (x.name == "HOST")
                found = x;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(found.has_value());
    if (found) {
        CHECK(found->secret == 77 && found->host.port == 40000);
        CHECK_EQ(found->host.ipv4() >> 24, 127u);  // it answered from this computer
    }
}
