// Online play: room codes, race settings, the message header, the server's JSON, server addresses.

#include "net/protocol.h"
#include "test.h"

using namespace vette::net;

TEST(net_room_code_normalize) {
    CHECK(normalize_room_code("VETTE-4KQ7") == std::optional<std::string>("4KQ7"));
    CHECK(normalize_room_code("vette-4kq7") == std::optional<std::string>("4KQ7"));
    CHECK(normalize_room_code(" 4kq7 ") == std::optional<std::string>("4KQ7"));
    CHECK(normalize_room_code("VETTE 4KQ7") == std::optional<std::string>("4KQ7"));
    CHECK(normalize_room_code("vette4kq7") == std::optional<std::string>("4KQ7"));
    CHECK(!normalize_room_code("4KQ"));        // too short
    CHECK(!normalize_room_code("4KQ7X"));      // too long
    CHECK(!normalize_room_code("40Q7"));       // 0 isn't in the alphabet
    CHECK(!normalize_room_code("4OQ7"));       // nor O
    CHECK(!normalize_room_code("41Q7"));       // nor 1
    CHECK(!normalize_room_code("4IQ7"));       // nor I
    CHECK(!normalize_room_code("4K/Q7"));
    CHECK(!normalize_room_code(""));
    CHECK_EQ(format_room_code("4KQ7"), std::string("VETTE-4KQ7"));
    static_assert(kCodeAlphabet.size() == 32);
}

TEST(net_race_settings) {
    RaceSettings s;
    CHECK(s.set("improved_driving", "1"));
    CHECK(s.set("laps", "3"));
    CHECK(s.set("improved_driving", "0"));  // replaces, keeps the order
    CHECK_EQ(s.serialize(), std::string("improved_driving=0\nlaps=3"));
    CHECK(!s.get_bool("improved_driving", true));
    CHECK(s.get_bool("missing", true));
    CHECK(s.get("laps") == std::optional<std::string>("3"));
    CHECK(!s.set("Bad", "1"));           // keys are lower case
    CHECK(!s.set("", "1"));
    CHECK(!s.set("a=b", "1"));
    CHECK(!s.set("k", "line\nbreak"));  // values are one line
    CHECK(!s.set("k", std::string(65, 'x')));
    const auto parsed = RaceSettings::parse(s.serialize());
    CHECK(parsed && *parsed == s);
    CHECK(RaceSettings::parse("") && RaceSettings::parse("")->empty());
    CHECK(!RaceSettings::parse("novalue"));
    CHECK(!RaceSettings::parse("UPPER=1"));
    // The whole text is limited.
    RaceSettings big;
    bool all = true;
    for (int i = 0; i < 20; ++i) {
        all = all && big.set("key" + std::to_string(i), std::string(40, 'v'));
    }
    CHECK(!all);
    CHECK(big.serialize().size() <= RaceSettings::kMaxBytes);
}

TEST(net_message_header) {
    MessageHeader h;
    h.kind = MessageHeader::kAck;
    h.flags = MessageHeader::kResend;
    h.ts_us = 0x01020304;
    h.echo_ts_us = 0xA0B0C0D0;
    h.echo_hold_us = 1234;
    h.offset = 0xFFFFFFF0;
    h.ack = 77;
    std::vector<std::uint8_t> bytes;
    h.append_to(bytes);
    CHECK_EQ(bytes.size(), MessageHeader::kSize);
    CHECK_EQ(bytes[4], std::uint8_t{0x04});  // little-endian
    const auto back = MessageHeader::decode(bytes);
    CHECK(back && *back == h);
    CHECK(!MessageHeader::decode(std::span(bytes).first(10)));
    bytes[0] = 9;
    CHECK(!MessageHeader::decode(bytes));
}

TEST(net_json) {
    const auto m = parse_json_object(
        R"( {"t":"welcome","role":"guest","code":"4KQ7","resumed":false,"grace":30,"settings":"a=1\nb=2",)"
        R"("reason":"Café \"quoted\" \\ /"} )");
    CHECK(m.has_value());
    if (m) {
        CHECK_EQ(m->at("t"), std::string("welcome"));
        CHECK_EQ(m->at("resumed"), std::string("false"));
        CHECK_EQ(m->at("grace"), std::string("30"));
        CHECK_EQ(m->at("settings"), std::string("a=1\nb=2"));
        CHECK_EQ(m->at("reason"), std::string("Caf\xC3\xA9 \"quoted\" \\ /"));
    }
    CHECK(parse_json_object("{}").has_value());
    CHECK(!parse_json_object(R"({"a":{"b":1}})"));  // nesting isn't part of the protocol
    CHECK(!parse_json_object(R"({"a":1)"));
    CHECK(!parse_json_object(R"({"a":1} x)"));
    CHECK(!parse_json_object("pong"));
    const std::string tricky = "line\nquote\" back\\slash\t\x01";
    const auto round = parse_json_object("{\"k\":" + json_quote(tricky) + "}");
    CHECK(round && round->at("k") == tricky);
}

TEST(net_server_url) {
    auto u = parse_server_url("wss://vette2026-relay.someone.workers.dev");
    CHECK(u && u->tls && u->port == 443 && u->host == "vette2026-relay.someone.workers.dev" && u->path.empty());
    CHECK(u && u->host_header() == "vette2026-relay.someone.workers.dev");
    CHECK(u && u->curl_url() == "https://vette2026-relay.someone.workers.dev:443/");
    u = parse_server_url("ws://127.0.0.1:8787/");
    CHECK(u && !u->tls && u->port == 8787 && u->host == "127.0.0.1" && u->host_header() == "127.0.0.1:8787");
    u = parse_server_url("HTTPS://example.com:8443/relay/");
    CHECK(u && u->tls && u->port == 8443 && u->path == "/relay");
    u = parse_server_url("ws://[::1]:8787");
    CHECK(u && u->host == "::1" && u->host_header() == "[::1]:8787");
    CHECK(!parse_server_url("ftp://example.com"));
    CHECK(!parse_server_url("wss://"));
    CHECK(!parse_server_url("wss://example.com:99999"));
    CHECK(!parse_server_url("example.com"));
}

TEST(net_percent_encode) {
    CHECK_EQ(percent_encode("DOS 1.1"), std::string("DOS%201.1"));
    CHECK_EQ(percent_encode("a=1\nb=2&c"), std::string("a%3D1%0Ab%3D2%26c"));
    CHECK_EQ(percent_encode("0.1.6-abc_~"), std::string("0.1.6-abc_~"));
}
