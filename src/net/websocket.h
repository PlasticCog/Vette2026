#pragma once
// The WebSocket protocol (RFC 6455), client side, without I/O: the opening handshake and the frames.
// net/connection.h puts it on a TCP or TLS connection.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vette::net::ws {

enum Opcode : std::uint8_t { kContinuation = 0, kText = 1, kBinary = 2, kClose = 8, kPing = 9, kPong = 10 };

std::array<std::uint8_t, 20> sha1(std::span<const std::uint8_t> data);
std::string base64(std::span<const std::uint8_t> data);

// The handshake: the client's GET request, and the Sec-WebSocket-Accept the server must answer with.
// `key` is base64 of 16 random bytes.
std::string handshake_request(std::string_view host_header, std::string_view target, std::string_view key,
                              std::string_view user_agent);
std::string accept_key(std::string_view key);

struct HandshakeResponse {
    enum class State { Incomplete, Accepted, Refused } state = State::Incomplete;
    int status = 0;            // HTTP status
    std::size_t length = 0;    // bytes of the response head (what follows is WebSocket frames)
    std::string error;         // why it was refused, for the player
};
// Reads the server's response to the handshake from what has arrived so far.
HandshakeResponse parse_handshake_response(std::string_view received, std::string_view key);

// Appends a frame the client sends: final, masked with `mask`.
void append_frame(std::vector<std::uint8_t>& out, Opcode op, std::span<const std::uint8_t> payload,
                  std::array<std::uint8_t, 4> mask);

// Reads the server's frames as they arrive: data messages reassembled from their fragments, control
// frames (ping, pong, close) as they come.
class FrameDecoder {
public:
    struct Message {
        Opcode op = kBinary;
        std::vector<std::uint8_t> data;
    };

    explicit FrameDecoder(std::size_t max_message = 1 << 20) : max_message_(max_message) {}

    // Appends the messages completed by `bytes` to `out`. False on a protocol violation (error()); the
    // connection must then be closed.
    bool feed(std::span<const std::uint8_t> bytes, std::vector<Message>& out);
    const std::string& error() const { return error_; }

private:
    bool fail(std::string why) {
        error_ = std::move(why);
        return false;
    }

    std::size_t max_message_;
    std::vector<std::uint8_t> buf_;      // bytes not yet decoded
    std::vector<std::uint8_t> partial_;  // a fragmented message so far
    Opcode partial_op_ = kContinuation;  // kContinuation: none in progress
    std::string error_;
};

}  // namespace vette::net::ws
