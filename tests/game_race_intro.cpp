// The two-player games' intros (game/race_intro.h): the players' names and the host's map, sent down the
// serial cable ahead of the original's packets and taken off before the game sees them.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "game/race_intro.h"
#include "host/loopback_link.h"
#include "test.h"

using vette::game::CityMap;
using vette::game::IntroLink;
using vette::game::RaceIntro;

namespace {

CityMap edited_map() {
    CityMap m;
    m.name = "Hill climb";
    for (int bt = 0; bt < CityMap::kBigTiles; ++bt) {
        m.layout[static_cast<size_t>(bt)] = static_cast<uint8_t>(bt % CityMap::kDesigns);
        m.ground[static_cast<size_t>(bt)] = bt % 3 ? 7 : 9;
    }
    for (int d = 0; d < CityMap::kDesigns; ++d) {
        for (int i = 0; i < 256; ++i)
            m.designs[static_cast<size_t>(d)].cells[static_cast<size_t>(i)] = {static_cast<uint8_t>((d * 7 + i) & 0xFF),
                                                                               static_cast<uint8_t>(i % 3)};
    }
    return m;
}

std::vector<uint8_t> bytes(std::string_view s) { return {s.begin(), s.end()}; }

std::vector<uint8_t> drain(vette::host::SerialLink& link) {
    std::vector<uint8_t> out;
    uint8_t buf[64];
    for (size_t n; (n = link.receive(buf)) > 0;) out.insert(out.end(), buf, buf + n);
    return out;
}

}  // namespace

TEST(race_intro_round_trip) {
    RaceIntro intro;
    intro.name = "Alice";
    intro.map = edited_map();
    std::string error;
    const auto back = RaceIntro::decode(intro.encode(), error);
    CHECK(back.has_value());
    CHECK_EQ(back->name, std::string("Alice"));
    CHECK(back->map.has_value());
    CHECK(*back->map == *intro.map);
    CHECK_EQ(back->map->name, std::string("Hill climb"));

    const auto plain = RaceIntro::decode(RaceIntro{"Bob", std::nullopt}.encode(), error);
    CHECK(plain && plain->name == "Bob" && !plain->map);
    CHECK(!RaceIntro::decode("map\nVETTE2026 MAP 1\n", error));  // no name (and not a map either)
    CHECK(RaceIntro::decode("name=Ann\nlater=1\n", error).has_value());  // a later version's lines are skipped
}

// Both ends of a cable: each game's intro arrives first and is taken off; the game's bytes after it pass
// through unchanged, in order. Bytes the game sends before the cable is connected are lost.
TEST(race_intro_link_both_ways) {
    vette::host::LoopbackCable cable;
    cable.set_plugged(false);
    RaceIntro host_intro{"Alice", edited_map()};
    IntroLink host(cable.end(0), host_intro);
    IntroLink guest(cable.end(1), RaceIntro{"Bob", std::nullopt});
    const auto early = bytes("lost");
    host.send(early);
    host.poll();
    guest.poll();
    CHECK(!host.received() && !guest.received());

    cable.set_plugged(true);
    const auto from_host = bytes("IDN\x01 host's packet");
    const auto from_guest = bytes("IDN\x02 guest's packet");
    host.send(from_host);    // (its intro goes first)
    guest.send(from_guest);
    CHECK(drain(guest) == from_host);  // the intro taken off, the packet left
    CHECK(drain(host) == from_guest);
    CHECK(host.received() && guest.received());
    CHECK(!host.failed() && !guest.failed());
    CHECK_EQ(guest.theirs()->name, std::string("Alice"));
    CHECK(guest.theirs()->map && *guest.theirs()->map == *host_intro.map);
    CHECK_EQ(host.theirs()->name, std::string("Bob"));
    CHECK(!host.theirs()->map);
}

// Arriving a byte at a time: nothing reaches the game until the intro is complete.
TEST(race_intro_link_in_pieces) {
    vette::host::LoopbackCable cable;
    IntroLink guest(cable.end(1), RaceIntro{"Bob", std::nullopt});
    const std::string payload = RaceIntro{"Alice", std::nullopt}.encode();
    const std::string sent = "VETTE2026 INTRO 1\n" + std::to_string(payload.size()) + "\n" + payload + "IDN";
    uint8_t buf[16];
    for (size_t i = 0; i + 3 < sent.size(); ++i) {
        cable.end(0).send(std::span(reinterpret_cast<const uint8_t*>(sent.data()) + i, 1));
        CHECK_EQ(guest.receive(buf), size_t{0});
    }
    cable.end(0).send(std::span(reinterpret_cast<const uint8_t*>(sent.data()) + sent.size() - 3, 3));
    CHECK(drain(guest) == bytes("IDN"));
    CHECK(guest.received());
    CHECK_EQ(guest.theirs()->name, std::string("Alice"));
}

// The other end sends no intro (not this program): its bytes go to the game as they came.
TEST(race_intro_link_without_one) {
    vette::host::LoopbackCable cable;
    IntroLink guest(cable.end(1), RaceIntro{"Bob", std::nullopt});
    const auto packet = bytes("IDN\x05packet");
    cable.end(0).send(packet);
    CHECK(drain(guest) == packet);
    CHECK(guest.failed());
    CHECK(!guest.received());
    // A damaged length: failed too, and what came is passed on.
    vette::host::LoopbackCable cable2;
    IntroLink guest2(cable2.end(1), RaceIntro{"Bob", std::nullopt});
    const auto damaged = bytes("VETTE2026 INTRO 1\nabc\n");
    cable2.end(0).send(damaged);
    CHECK(drain(guest2) == damaged);
    CHECK(guest2.failed());
}
