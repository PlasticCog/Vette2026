#pragma once
// A SerialLink over TCP, for development: two vette2026 processes on one PC (or a LAN) race each other
// without the relay server (vette2026 --link-listen PORT / --link-connect HOST:PORT). Not meant for the
// internet: no NAT traversal, no authentication.
//
// Everything is non-blocking and runs on the caller's thread: poll() accepts, connects, reads and
// writes what it can; send() and receive() call it too. The listening end can send a line of text
// before the serial bytes (the host's settings, game/two_player.h TwoPlayerSetup); the connecting end
// reads it into hello() and passes on only what follows.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "host/serial_link.h"

namespace vette::host {

class TcpLink final : public SerialLink {
public:
    // Waits for one peer on `port` (all interfaces). nullptr and `error` if the port can't be opened.
    static std::unique_ptr<TcpLink> listen(uint16_t port, std::string& error);
    // Connects to host:port, retrying every second until the peer is there (or the link is destroyed).
    static std::unique_ptr<TcpLink> connect(const std::string& host, uint16_t port, std::string& error);
    ~TcpLink() override;
    TcpLink(const TcpLink&) = delete;
    TcpLink& operator=(const TcpLink&) = delete;

    void send(std::span<const uint8_t> bytes) override;
    size_t receive(std::span<uint8_t> out) override;
    bool connected() const override { return state_ == State::Connected; }

    void poll();
    // Listening end: the line sent first to each peer (without the newline).
    void set_hello(std::string line) { hello_out_ = std::move(line); }
    // Connecting end: the listening end's line, once it has arrived.
    const std::optional<std::string>& hello() const { return hello_in_; }
    // Why the connection ended (empty while it hasn't).
    const std::string& closed_reason() const { return closed_; }

private:
    enum class State { Listening, Connecting, Connected, Closed };
    TcpLink() = default;
    void close_peer(const std::string& why);
    bool start_connect();

    State state_ = State::Closed;
    bool listener_ = false;
    std::intptr_t listen_sock_ = -1;
    std::intptr_t sock_ = -1;
    std::string host_;
    uint16_t port_ = 0;
    uint64_t retry_at_ms_ = 0;
    std::string hello_out_;
    std::optional<std::string> hello_in_;
    std::string hello_buf_;
    std::vector<uint8_t> out_;   // waiting to be written
    std::deque<uint8_t> in_;     // received
    std::string closed_;
};

} // namespace vette::host
