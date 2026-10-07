#pragma once
// A WebSocket client connection to the relay server: libcurl makes the TCP connection and the TLS
// session (Schannel on Windows, Secure Transport on macOS, OpenSSL on Linux, each with the system's
// trusted certificates), and net/websocket.h does the WebSocket protocol on top. After connect(),
// everything is non-blocking: the network thread waits in wait() for the socket or a Waker.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "net/protocol.h"
#include "net/socket.h"
#include "net/websocket.h"

namespace vette::net {

struct ConnectOptions {
    std::string ca_file;  // extra trusted certificates (PEM): a test server's own CA. Normally empty.
    int connect_timeout_ms = 8000;
    int handshake_timeout_ms = 8000;
    std::string user_agent = "VETTE2026";
    const std::atomic<bool>* abort = nullptr;  // set: give up connecting at once
};

class WebSocketConnection {
public:
    WebSocketConnection();
    ~WebSocketConnection();
    WebSocketConnection(const WebSocketConnection&) = delete;
    WebSocketConnection& operator=(const WebSocketConnection&) = delete;

    // Connects and completes the WebSocket handshake for `target` (path and query). Blocks for at most
    // the timeouts. False with `error` (a sentence for the player) on failure.
    bool connect(const ServerUrl& server, const std::string& target, const ConnectOptions& options,
                 std::string& error);
    bool is_open() const { return curl_ != nullptr; }

    // Queue messages; flush() writes what the socket takes now.
    void send_text(std::string_view text);
    void send_binary(std::span<const std::uint8_t> data);
    bool flush(std::string& error);
    bool wants_write() const { return out_pos_ < out_.size(); }

    // Reads what has arrived and appends the complete messages (text and binary; pings are answered
    // here). False when the connection has ended, with `error` saying why.
    bool read(std::vector<ws::FrameDecoder::Message>& out, std::string& error);

    // Waits until the connection has something to read (or room to write, if wants_write()), `waker`
    // is woken, or `timeout_ms` passes. Without a connection, waits for the waker or the timeout.
    void wait(Waker& waker, int timeout_ms);

    // Closes politely: a close frame, a moment to send it. close() just drops the connection.
    void shutdown(int timeout_ms);
    void close();

private:
    void queue(ws::Opcode op, std::span<const std::uint8_t> payload);
    std::uintptr_t socket() const;

    void* curl_ = nullptr;
    std::vector<std::uint8_t> out_;
    std::size_t out_pos_ = 0;
    ws::FrameDecoder decoder_;
    std::vector<ws::FrameDecoder::Message> pending_;  // arrived with the handshake's response
    std::mt19937 rng_;
};

// For Linux: the CA bundle and directory to trust, found on this system (distributions keep them in
// different places). Empty elsewhere, or if none is found.
struct CaPaths {
    std::string file, dir;
};
CaPaths find_system_ca();

}  // namespace vette::net
