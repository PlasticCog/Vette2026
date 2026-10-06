// Online play: the WebSocket handshake and frames (RFC 6455), without a network.

#include <cstring>

#include "net/websocket.h"
#include "test.h"

using namespace vette::net::ws;

namespace {

std::vector<std::uint8_t> bytes(std::string_view s) { return {s.begin(), s.end()}; }

std::string hex(std::span<const std::uint8_t> data) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (const auto b : data) {
        out += kHex[b >> 4];
        out += kHex[b & 15];
    }
    return out;
}

}  // namespace

TEST(net_sha1_base64) {
    CHECK_EQ(hex(sha1(bytes("abc"))), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    CHECK_EQ(hex(sha1(bytes(""))), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    CHECK_EQ(hex(sha1(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
             std::string("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
    CHECK_EQ(base64(bytes("")), std::string(""));
    CHECK_EQ(base64(bytes("f")), std::string("Zg=="));
    CHECK_EQ(base64(bytes("fo")), std::string("Zm8="));
    CHECK_EQ(base64(bytes("foo")), std::string("Zm9v"));
    CHECK_EQ(base64(bytes("foobar")), std::string("Zm9vYmFy"));
    // RFC 6455, section 1.3.
    CHECK_EQ(accept_key("dGhlIHNhbXBsZSBub25jZQ=="), std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

TEST(net_handshake) {
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string request = handshake_request("relay.example:8787", "/v1/create?proto=1", key, "VETTE2026/0.1.6");
    CHECK(request.starts_with("GET /v1/create?proto=1 HTTP/1.1\r\nHost: relay.example:8787\r\n"));
    CHECK(request.find("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") != std::string::npos);
    CHECK(request.ends_with("\r\n\r\n"));

    const std::string ok = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    auto r = parse_handshake_response(ok + "\x81\x02hi", key);
    CHECK(r.state == HandshakeResponse::State::Accepted);
    CHECK_EQ(r.length, ok.size());  // the frame after it isn't part of the response
    r = parse_handshake_response(ok.substr(0, 40), key);
    CHECK(r.state == HandshakeResponse::State::Incomplete);
    r = parse_handshake_response("HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\n\r\n", key);
    CHECK(r.state == HandshakeResponse::State::Refused && r.status == 429);
    CHECK(r.error.find("free allowance") != std::string::npos);
    r = parse_handshake_response("HTTP/1.1 404 Not Found\r\n\r\n", key);
    CHECK(r.state == HandshakeResponse::State::Refused && r.status == 404);
    std::string wrong = ok;
    wrong.replace(wrong.find("s3pP"), 4, "XXXX");
    r = parse_handshake_response(wrong, key);
    CHECK(r.state == HandshakeResponse::State::Refused);
}

TEST(net_frame_encode) {
    // RFC 6455, section 5.7: a masked "Hello".
    std::vector<std::uint8_t> out;
    append_frame(out, kText, bytes("Hello"), {0x37, 0xfa, 0x21, 0x3d});
    CHECK_EQ(hex(out), std::string("818537fa213d7f9f4d5158"));
    // 256 bytes: the 16-bit length.
    out.clear();
    append_frame(out, kBinary, std::vector<std::uint8_t>(256, 0), {0, 0, 0, 0});
    CHECK_EQ(hex(std::span(out).first(4)), std::string("82fe0100"));
    CHECK_EQ(out.size(), std::size_t{4 + 4 + 256});
    // 65536 bytes: the 64-bit length.
    out.clear();
    append_frame(out, kBinary, std::vector<std::uint8_t>(65536, 0), {0, 0, 0, 0});
    CHECK_EQ(hex(std::span(out).first(10)), std::string("82ff0000000000010000"));
}

TEST(net_frame_decode) {
    FrameDecoder d;
    std::vector<FrameDecoder::Message> out;
    // Unmasked "Hello", fed a byte at a time.
    const std::vector<std::uint8_t> hello = {0x81, 0x05, 'H', 'e', 'l', 'l', 'o'};
    for (const auto b : hello) {
        CHECK(d.feed(std::span(&b, 1), out));
    }
    CHECK_EQ(out.size(), std::size_t{1});
    CHECK(out.size() == 1 && out[0].op == kText && out[0].data == bytes("Hello"));
    // Fragmented "Hel" + "lo" with a ping between the fragments.
    out.clear();
    const std::vector<std::uint8_t> frag = {0x01, 0x03, 'H', 'e', 'l', 0x89, 0x01, 'p', 0x80, 0x02, 'l', 'o'};
    CHECK(d.feed(frag, out));
    CHECK_EQ(out.size(), std::size_t{2});
    CHECK(out.size() == 2 && out[0].op == kPing && out[0].data == bytes("p"));
    CHECK(out.size() == 2 && out[1].op == kText && out[1].data == bytes("Hello"));
    // A 300-byte binary message (16-bit length) and a close frame in one feed.
    out.clear();
    std::vector<std::uint8_t> two = {0x82, 0x7E, 0x01, 0x2C};
    for (int i = 0; i < 300; ++i) {
        two.push_back(static_cast<std::uint8_t>(i));
    }
    two.insert(two.end(), {0x88, 0x02, 0x03, 0xE8});
    CHECK(d.feed(two, out));
    CHECK(out.size() == 2 && out[0].op == kBinary && out[0].data.size() == 300 && out[0].data[299] == 43);
    CHECK(out.size() == 2 && out[1].op == kClose && out[1].data.size() == 2);
    // A server must not mask its frames.
    FrameDecoder masked;
    out.clear();
    CHECK(!masked.feed(std::vector<std::uint8_t>{0x81, 0x85, 1, 2, 3, 4, 0, 0, 0, 0, 0}, out));
    CHECK(!masked.error().empty());
    // Nor send messages beyond the limit.
    FrameDecoder small(100);
    CHECK(!small.feed(std::vector<std::uint8_t>{0x82, 0x7E, 0x01, 0x00}, out));
}
