// Online play: direct codes, invite links and what players paste.

#include "net/invite.h"
#include "net/protocol.h"
#include "test.h"

using namespace vette::net;

TEST(net_direct_code_round_trip) {
    std::mt19937_64 rng(42);
    for (int i = 0; i < 200; ++i) {
        const auto ip = static_cast<std::uint32_t>(rng());
        const std::uint16_t port = i % 2 ? kDirectPort : static_cast<std::uint16_t>(1 + rng() % 65535);
        const DirectCode c = DirectCode::make(ip, port, rng);
        const std::string text = c.encode();
        CHECK_EQ(text.size(), std::size_t{port == kDirectPort ? 14u : 19u});  // with the dashes
        const auto back = DirectCode::decode(text);
        CHECK(back && *back == c);
        // Without dashes, in lower case, with spaces.
        std::string loose;
        for (const char ch : text) {
            if (ch != '-') {
                loose += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
        }
        CHECK(DirectCode::decode(loose) == back);
        CHECK(DirectCode::decode(" " + text + " ") == back);
    }
    const DirectCode c{0xCB007105, kDirectPort, 0x12345};  // 203.0.113.5
    CHECK_EQ(c.key(), std::string("code:74565"));
}

TEST(net_direct_code_catches_typos) {
    std::mt19937_64 rng(7);
    const std::string text = DirectCode::make(0xC0A80114, kDirectPort, rng).encode();
    int caught = 0, tried = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '-') {
            continue;
        }
        for (const char sub : kCodeAlphabet) {
            if (sub == text[i]) {
                continue;
            }
            std::string typo = text;
            typo[i] = sub;
            ++tried;
            caught += DirectCode::decode(typo) ? 0 : 1;
        }
    }
    CHECK_EQ(caught, tried);  // every single wrong character
    // Characters outside the alphabet, and wrong lengths.
    CHECK(!DirectCode::decode("0000-0000-0000"));
    CHECK(!DirectCode::decode("2345-6789-ABC"));
    CHECK(!DirectCode::decode("VETTE-4KQ7"));
    CHECK(!DirectCode::decode(""));
}

TEST(net_parse_invite) {
    std::mt19937_64 rng(1);
    const std::string direct = DirectCode::make(0x99420001, kDirectPort, rng).encode();
    struct Case {
        std::string pasted;
        std::optional<Invite::Kind> kind;
        std::string code;
    };
    const Case cases[] = {
        {"VETTE-4KQ7", Invite::Kind::Room, "VETTE-4KQ7"},
        {"  vette-4kq7\n", Invite::Kind::Room, "VETTE-4KQ7"},
        {"4KQ7", Invite::Kind::Room, "VETTE-4KQ7"},
        {"https://vette2026-relay.someone.workers.dev/join/VETTE-4KQ7", Invite::Kind::Room, "VETTE-4KQ7"},
        {"https://vette2026-relay.someone.workers.dev/join/4kq7/?utm=x#top", Invite::Kind::Room, "VETTE-4KQ7"},
        {"<https://relay.example/join/VETTE-4KQ7>", Invite::Kind::Room, "VETTE-4KQ7"},
        {"vette2026://join/VETTE-4KQ7", Invite::Kind::Room, "VETTE-4KQ7"},
        {"VETTE2026://JOIN/vette-4kq7", Invite::Kind::Room, "VETTE-4KQ7"},
        {"Race me! https://relay.example/join/VETTE-4KQ7 see you", Invite::Kind::Room, "VETTE-4KQ7"},
        {"my code is VETTE-4KQ7.", Invite::Kind::Room, "VETTE-4KQ7"},
        {direct, Invite::Kind::Direct, direct},
        {"https://relay.example/direct/" + direct, Invite::Kind::Direct, direct},
        {"vette2026://direct/" + direct, Invite::Kind::Direct, direct},
        {"join with " + direct + " please", Invite::Kind::Direct, direct},
        {"Let's race", std::nullopt, ""},          // "RACE" alone in a sentence isn't a code
        {"https://relay.example/join/VETTE-40I7", std::nullopt, ""},
        {"https://example.com/", std::nullopt, ""},
        {"", std::nullopt, ""},
    };
    for (const auto& c : cases) {
        const auto got = parse_invite(c.pasted);
        if (!c.kind) {
            if (got) {
                ::vette::test::fail(__FILE__, __LINE__, "parsed: " + c.pasted);
            }
            continue;
        }
        if (!got || got->kind != *c.kind || got->code != c.code) {
            ::vette::test::fail(__FILE__, __LINE__, "not parsed right: " + c.pasted + " -> " + (got ? got->code : "nothing"));
        }
    }
}

TEST(net_invite_links) {
    const Invite room{Invite::Kind::Room, "VETTE-4KQ7"};
    const Invite direct{Invite::Kind::Direct, "7K3M-QX9P-2HDA"};
    CHECK_EQ(invite_link("wss://vette2026-relay.someone.workers.dev", room),
             std::string("https://vette2026-relay.someone.workers.dev/join/VETTE-4KQ7"));
    CHECK_EQ(invite_link("ws://127.0.0.1:8787/", direct), std::string("http://127.0.0.1:8787/direct/7K3M-QX9P-2HDA"));
    CHECK_EQ(invite_link("", direct), std::string("7K3M-QX9P-2HDA"));
    CHECK_EQ(app_link(room), std::string("vette2026://join/VETTE-4KQ7"));
    CHECK_EQ(app_link(direct), std::string("vette2026://direct/7K3M-QX9P-2HDA"));
    // What a link says, the parser reads back.
    const auto back = parse_invite(invite_link("wss://relay.example", room));
    CHECK(back && back->code == room.code);
}
