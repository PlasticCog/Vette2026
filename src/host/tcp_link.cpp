#include "host/tcp_link.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace vette::host {
namespace {

#ifdef _WIN32
using socket_t = SOCKET;
const socket_t kNoSocket = INVALID_SOCKET;
int last_error() { return WSAGetLastError(); }
bool would_block(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY; }
void close_socket(socket_t s) { closesocket(s); }
bool set_nonblocking(socket_t s) {
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
}
// Winsock stays initialised for the life of the program once a link is made.
bool sockets_ready() {
    static const bool ok = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
}
#else
using socket_t = int;
constexpr socket_t kNoSocket = -1;
int last_error() { return errno; }
bool would_block(int e) { return e == EWOULDBLOCK || e == EAGAIN || e == EINPROGRESS || e == EALREADY; }
void close_socket(socket_t s) { ::close(s); }
bool set_nonblocking(socket_t s) {
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
bool sockets_ready() { return true; }
#endif

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;  // a closed peer is an error, not SIGPIPE
#else
constexpr int kSendFlags = 0;
#endif

socket_t sock_of(std::intptr_t v) { return static_cast<socket_t>(v); }
std::intptr_t value_of(socket_t s) { return static_cast<std::intptr_t>(s); }

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

void no_delay(socket_t s) {
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&on), sizeof on);
#endif
}

} // namespace

std::unique_ptr<TcpLink> TcpLink::listen(uint16_t port, std::string& error) {
    if (!sockets_ready()) {
        error = "sockets unavailable";
        return nullptr;
    }
    const socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) {
        error = "socket() failed";
        return nullptr;
    }
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof on);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 || ::listen(s, 1) != 0 ||
        !set_nonblocking(s)) {
        error = "can't listen on port " + std::to_string(port);
        close_socket(s);
        return nullptr;
    }
    std::unique_ptr<TcpLink> link(new TcpLink());
    link->listener_ = true;
    link->listen_sock_ = value_of(s);
    link->port_ = port;
    link->state_ = State::Listening;
    return link;
}

std::unique_ptr<TcpLink> TcpLink::connect(const std::string& host, uint16_t port, std::string& error) {
    if (!sockets_ready()) {
        error = "sockets unavailable";
        return nullptr;
    }
    std::unique_ptr<TcpLink> link(new TcpLink());
    link->host_ = host;
    link->port_ = port;
    if (!link->start_connect()) {
        error = "can't resolve " + host;
        return nullptr;
    }
    return link;
}

TcpLink::~TcpLink() {
    if (sock_ != -1) {
        close_socket(sock_of(sock_));
    }
    if (listen_sock_ != -1) {
        close_socket(sock_of(listen_sock_));
    }
}

bool TcpLink::start_connect() {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &found) != 0 || !found) {
        return false;
    }
    const socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket || !set_nonblocking(s)) {
        freeaddrinfo(found);
        if (s != kNoSocket) close_socket(s);
        return false;
    }
    const int r = ::connect(s, found->ai_addr, static_cast<int>(found->ai_addrlen));
    freeaddrinfo(found);
    if (r != 0 && !would_block(last_error())) {
        close_socket(s);
        state_ = State::Connecting;  // refused (nobody listening yet): try again in a second
        retry_at_ms_ = now_ms() + 1000;
        return true;
    }
    sock_ = value_of(s);
    state_ = State::Connecting;
    return true;
}

void TcpLink::close_peer(const std::string& why) {
    if (sock_ != -1) {
        close_socket(sock_of(sock_));
        sock_ = -1;
    }
    closed_ = why;
    out_.clear();
    state_ = listener_ ? State::Listening : State::Closed;  // the listening end waits for another peer
}

void TcpLink::poll() {
    if (state_ == State::Listening) {
        const socket_t s = ::accept(sock_of(listen_sock_), nullptr, nullptr);
        if (s == kNoSocket) {
            return;
        }
        set_nonblocking(s);
        no_delay(s);
        sock_ = value_of(s);
        state_ = State::Connected;
        closed_.clear();
        out_.assign(hello_out_.begin(), hello_out_.end());
        out_.push_back('\n');
        in_.clear();
    }
    if (state_ == State::Connecting) {
        if (sock_ == -1) {
            if (now_ms() >= retry_at_ms_) {
                start_connect();
            }
            return;
        }
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(sock_of(sock_), &writable);
        FD_SET(sock_of(sock_), &failed);
        timeval zero{};
        if (::select(static_cast<int>(sock_ + 1), nullptr, &writable, &failed, &zero) <= 0) {
            return;  // still connecting
        }
        int err = 0;
        socklen_t len = sizeof err;
        getsockopt(sock_of(sock_), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        if (err != 0 || FD_ISSET(sock_of(sock_), &failed)) {
            close_socket(sock_of(sock_));
            sock_ = -1;
            retry_at_ms_ = now_ms() + 1000;
            return;
        }
        no_delay(sock_of(sock_));
        state_ = State::Connected;
        closed_.clear();
    }
    if (state_ != State::Connected) {
        return;
    }
    // Write what can be written.
    while (!out_.empty()) {
        const auto n = static_cast<int>(::send(sock_of(sock_), reinterpret_cast<const char*>(out_.data()),
                                               static_cast<int>(std::min<size_t>(out_.size(), 1 << 16)), kSendFlags));
        if (n > 0) {
            out_.erase(out_.begin(), out_.begin() + n);
        } else {
            if (n < 0 && would_block(last_error())) break;
            close_peer("the connection was lost");
            return;
        }
    }
    // Read what has arrived.
    std::array<char, 4096> buf;
    for (;;) {
        const auto n = static_cast<int>(::recv(sock_of(sock_), buf.data(), static_cast<int>(buf.size()), 0));
        if (n > 0) {
            for (int i = 0; i < n; ++i) {
                const auto b = static_cast<uint8_t>(buf[static_cast<size_t>(i)]);
                if (!listener_ && !hello_in_) {
                    if (b == '\n') {
                        hello_in_ = hello_buf_;
                    } else if (hello_buf_.size() < 1024) {
                        hello_buf_.push_back(static_cast<char>(b));
                    }
                } else {
                    in_.push_back(b);
                }
            }
        } else if (n == 0) {
            close_peer("the other side closed the connection");
            return;
        } else {
            if (!would_block(last_error())) close_peer("the connection was lost");
            return;
        }
    }
}

void TcpLink::send(std::span<const uint8_t> bytes) {
    if (state_ != State::Connected) {
        return;  // nobody at the other end of the cable
    }
    out_.insert(out_.end(), bytes.begin(), bytes.end());
    poll();
}

size_t TcpLink::receive(std::span<uint8_t> out) {
    poll();
    const size_t n = std::min(out.size(), in_.size());
    std::copy_n(in_.begin(), n, out.begin());
    in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(n));
    return n;
}

} // namespace vette::host
