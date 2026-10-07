#include "net/socket.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <charconv>
#include <cstring>
#include <mutex>

namespace vette::net {

namespace {

#if defined(_WIN32)
using Fd = SOCKET;
constexpr Fd kBad = INVALID_SOCKET;
int last_error() { return WSAGetLastError(); }
bool would_block(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
void close_fd(Fd fd) { ::closesocket(fd); }
bool set_nonblocking(Fd fd) {
    u_long on = 1;
    return ::ioctlsocket(fd, FIONBIO, &on) == 0;
}
using SockLen = int;
#else
using Fd = int;
constexpr Fd kBad = -1;
int last_error() { return errno; }
bool would_block(int e) { return e == EWOULDBLOCK || e == EAGAIN || e == EINPROGRESS; }
void close_fd(Fd fd) { ::close(fd); }
bool set_nonblocking(Fd fd) {
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    return ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK) == 0;
}
using SockLen = socklen_t;
#endif

Fd fd_of(std::uintptr_t h) { return static_cast<Fd>(h); }

void init() {
#if defined(_WIN32)
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
#endif
}

std::string error_text(int e) {
#if defined(_WIN32)
    char buf[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(e), 0,
                   buf, sizeof buf, nullptr);
    std::string s = buf;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.')) {
        s.pop_back();
    }
    return s.empty() ? "error " + std::to_string(e) : s;
#else
    return std::strerror(e);
#endif
}

// sockaddr storage for a SocketAddress.
SockLen to_sockaddr(const SocketAddress& a, sockaddr_storage& ss) {
    std::memset(&ss, 0, sizeof ss);
    if (a.v6) {
        auto* s6 = reinterpret_cast<sockaddr_in6*>(&ss);
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons(a.port);
        std::memcpy(&s6->sin6_addr, a.ip.data(), 16);
        s6->sin6_scope_id = a.scope;
        return sizeof(sockaddr_in6);
    }
    auto* s4 = reinterpret_cast<sockaddr_in*>(&ss);
    s4->sin_family = AF_INET;
    s4->sin_port = htons(a.port);
    std::memcpy(&s4->sin_addr, a.ip.data(), 4);
    return sizeof(sockaddr_in);
}

std::optional<SocketAddress> from_sockaddr(const sockaddr* sa) {
    SocketAddress a;
    if (sa->sa_family == AF_INET) {
        const auto* s4 = reinterpret_cast<const sockaddr_in*>(sa);
        std::memcpy(a.ip.data(), &s4->sin_addr, 4);
        a.port = ntohs(s4->sin_port);
        return a;
    }
    if (sa->sa_family == AF_INET6) {
        const auto* s6 = reinterpret_cast<const sockaddr_in6*>(sa);
        a.v6 = true;
        std::memcpy(a.ip.data(), &s6->sin6_addr, 16);
        a.port = ntohs(s6->sin6_port);
        a.scope = s6->sin6_scope_id;
        // An IPv4 address seen through an IPv6 socket (::ffff:a.b.c.d) is an IPv4 address.
        static constexpr std::uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        if (std::memcmp(a.ip.data(), kMapped, 12) == 0) {
            a.v6 = false;
            std::memmove(a.ip.data(), a.ip.data() + 12, 4);
            std::fill(a.ip.begin() + 4, a.ip.end(), std::uint8_t{0});
            a.scope = 0;
        }
        return a;
    }
    return std::nullopt;
}

}  // namespace

// --- Addresses ------------------------------------------------------------------------------------------

SocketAddress SocketAddress::ipv4(std::uint32_t address, std::uint16_t port) {
    SocketAddress a;
    a.ip[0] = static_cast<std::uint8_t>(address >> 24);
    a.ip[1] = static_cast<std::uint8_t>(address >> 16);
    a.ip[2] = static_cast<std::uint8_t>(address >> 8);
    a.ip[3] = static_cast<std::uint8_t>(address);
    a.port = port;
    return a;
}

std::uint32_t SocketAddress::ipv4() const {
    return static_cast<std::uint32_t>(ip[0]) << 24 | static_cast<std::uint32_t>(ip[1]) << 16 |
           static_cast<std::uint32_t>(ip[2]) << 8 | ip[3];
}

std::optional<std::uint32_t> parse_ipv4(std::string_view text) {
    std::uint32_t a = 0;
    for (int i = 0; i < 4; ++i) {
        unsigned part = 0;
        const auto r = std::from_chars(text.data(), text.data() + text.size(), part);
        if (r.ec != std::errc() || part > 255 || r.ptr == text.data() || r.ptr - text.data() > 3) {
            return std::nullopt;
        }
        a = a << 8 | part;
        text.remove_prefix(static_cast<std::size_t>(r.ptr - text.data()));
        if (i < 3) {
            if (text.empty() || text.front() != '.') {
                return std::nullopt;
            }
            text.remove_prefix(1);
        }
    }
    return text.empty() ? std::optional<std::uint32_t>(a) : std::nullopt;
}

std::string format_ipv4(std::uint32_t a) {
    return std::to_string(a >> 24) + "." + std::to_string((a >> 16) & 255) + "." + std::to_string((a >> 8) & 255) +
           "." + std::to_string(a & 255);
}

bool ipv4_private(std::uint32_t a) {
    return (a >> 24) == 10 || (a >> 20) == (172u << 4 | 1) || (a >> 16) == (192u << 8 | 168);
}

bool ipv4_shared(std::uint32_t a) { return (a >> 22) == (100u << 2 | 1); }

bool ipv4_public(std::uint32_t a) {
    const std::uint32_t top = a >> 24;
    return !(top == 0 || top == 127 || top >= 224 || ipv4_private(a) || ipv4_shared(a) ||
             (a >> 16) == (169u << 8 | 254) ||   // link-local
             (a >> 8) == (192u << 16 | 0 << 8 | 0) ||  // 192.0.0/24
             (a >> 8) == (192u << 16 | 0 << 8 | 2) ||  // TEST-NET-1
             (a >> 17) == (198u << 7 | 9) ||           // 198.18/15 benchmarking
             (a >> 8) == (198u << 16 | 51 << 8 | 100) ||  // TEST-NET-2
             (a >> 8) == (203u << 16 | 0 << 8 | 113));     // TEST-NET-3
}

bool ipv6_global(const std::array<std::uint8_t, 16>& ip) { return (ip[0] & 0xE0) == 0x20; }

std::optional<SocketAddress> SocketAddress::parse(std::string_view text, std::uint16_t default_port) {
    SocketAddress a;
    a.port = default_port;
    std::string_view host = text;
    std::string_view port;
    if (text.starts_with('[')) {
        const std::size_t close = text.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        host = text.substr(1, close - 1);
        if (close + 1 < text.size()) {
            if (text[close + 1] != ':') {
                return std::nullopt;
            }
            port = text.substr(close + 2);
        }
        a.v6 = true;
    } else if (std::count(text.begin(), text.end(), ':') == 1) {
        const std::size_t colon = text.find(':');
        host = text.substr(0, colon);
        port = text.substr(colon + 1);
    } else if (text.find(':') != std::string_view::npos) {
        a.v6 = true;  // a bare IPv6 address
    }
    if (!port.empty()) {
        unsigned p = 0;
        const auto r = std::from_chars(port.data(), port.data() + port.size(), p);
        if (r.ec != std::errc() || r.ptr != port.data() + port.size() || p == 0 || p > 65535) {
            return std::nullopt;
        }
        a.port = static_cast<std::uint16_t>(p);
    }
    if (a.v6) {
        init();
        std::string h(host);
        const std::size_t pct = h.find('%');
        if (pct != std::string::npos) {
            h.resize(pct);  // a zone ("%eth0") isn't supported in codes and offers
        }
        in6_addr addr{};
        if (inet_pton(AF_INET6, h.c_str(), &addr) != 1) {
            return std::nullopt;
        }
        std::memcpy(a.ip.data(), &addr, 16);
        return a;
    }
    const auto v4 = parse_ipv4(host);
    if (!v4) {
        return std::nullopt;
    }
    return SocketAddress::ipv4(*v4, a.port);
}

std::string SocketAddress::ip_string() const {
    if (!v6) {
        return format_ipv4(ipv4());
    }
    init();
    char buf[64] = {};
    in6_addr addr{};
    std::memcpy(&addr, ip.data(), 16);
    inet_ntop(AF_INET6, &addr, buf, sizeof buf);
    return buf;
}

std::string SocketAddress::to_string() const {
    return v6 ? "[" + ip_string() + "]:" + std::to_string(port) : ip_string() + ":" + std::to_string(port);
}

// --- Waker ----------------------------------------------------------------------------------------------

#if defined(_WIN32)

// A UDP socket on the loopback interface, sending to itself: WSAPoll can't wait on anything but sockets.
Waker::Waker() {
    init();
    const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof addr;
    u_long nonblocking = 1;
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0 ||
        ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        ::ioctlsocket(s, FIONBIO, &nonblocking) != 0) {
        ::closesocket(s);
        return;
    }
    read_ = write_ = static_cast<std::uintptr_t>(s);
    ok_ = true;
}

Waker::~Waker() {
    if (ok_) {
        ::closesocket(static_cast<SOCKET>(read_));
    }
}

void Waker::wake() {
    if (ok_) {
        ::send(static_cast<SOCKET>(write_), "w", 1, 0);
    }
}

void Waker::drain() {
    char buf[64];
    while (ok_ && ::recv(static_cast<SOCKET>(read_), buf, sizeof buf, 0) > 0) {
    }
}

#else

Waker::Waker() {
    int fds[2];
    if (::pipe(fds) != 0) {
        return;
    }
    for (const int fd : fds) {
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    read_ = static_cast<std::uintptr_t>(fds[0]);
    write_ = static_cast<std::uintptr_t>(fds[1]);
    ok_ = true;
}

Waker::~Waker() {
    if (ok_) {
        ::close(static_cast<int>(read_));
        ::close(static_cast<int>(write_));
    }
}

void Waker::wake() {
    if (ok_) {
        [[maybe_unused]] const auto n = ::write(static_cast<int>(write_), "w", 1);
    }
}

void Waker::drain() {
    char buf[64];
    while (ok_ && ::read(static_cast<int>(read_), buf, sizeof buf) > 0) {
    }
}

#endif

// --- Socket ---------------------------------------------------------------------------------------------

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = kInvalid;
    }
    return *this;
}

void Socket::close() {
    if (fd_ != kInvalid) {
        close_fd(fd_of(fd_));
        fd_ = kInvalid;
    }
}

Socket Socket::listen_tcp(bool v6, std::uint16_t port, bool loopback_only, std::string& error) {
    init();
    const Fd fd = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == kBad) {
        error = error_text(last_error());
        return {};
    }
    Socket s(static_cast<std::uintptr_t>(fd));
    int on = 1;
#if defined(_WIN32)
    // Nobody else may bind the port while we listen (Windows' SO_REUSEADDR would allow that).
    ::setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof on);
#else
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof on);
#endif
    if (v6) {
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&on), sizeof on);
    }
    SocketAddress any;
    any.v6 = v6;
    any.port = port;
    if (loopback_only) {
        if (v6) {
            any.ip[15] = 1;
        } else {
            any = SocketAddress::ipv4(0x7F000001, port);
        }
    }
    sockaddr_storage ss;
    const SockLen len = to_sockaddr(any, ss);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&ss), len) != 0 || ::listen(fd, 8) != 0 || !set_nonblocking(fd)) {
        error = error_text(last_error());
        return {};
    }
    return s;
}

Socket Socket::accept(SocketAddress* from) {
    sockaddr_storage ss{};
    SockLen len = sizeof ss;
    const Fd fd = ::accept(fd_of(fd_), reinterpret_cast<sockaddr*>(&ss), &len);
    if (fd == kBad) {
        return {};
    }
    Socket s(static_cast<std::uintptr_t>(fd));
    set_nonblocking(fd);
    int on = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
#if defined(__APPLE__)
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    if (from) {
        if (auto a = from_sockaddr(reinterpret_cast<sockaddr*>(&ss))) {
            *from = *a;
        }
    }
    return s;
}

Socket Socket::connect_tcp(const SocketAddress& to, std::string& error) {
    init();
    const Fd fd = ::socket(to.v6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == kBad) {
        error = error_text(last_error());
        return {};
    }
    Socket s(static_cast<std::uintptr_t>(fd));
    set_nonblocking(fd);
    int on = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
#if defined(__APPLE__)
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    sockaddr_storage ss;
    const SockLen len = to_sockaddr(to, ss);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&ss), len) != 0 && !would_block(last_error())) {
        error = error_text(last_error());
        return {};
    }
    return s;
}

Socket::ConnectState Socket::finish_connect(std::string& error) const {
    PollItem item{this, false, true};
    wait_sockets({&item, 1}, nullptr, 0);
    if (!item.writable && !item.failed) {
        return ConnectState::Pending;
    }
    int e = 0;
    SockLen len = sizeof e;
    ::getsockopt(fd_of(fd_), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&e), &len);
    if (e != 0 || item.failed) {
        error = error_text(e != 0 ? e : last_error());
        return ConnectState::Failed;
    }
    return ConnectState::Connected;
}

Socket Socket::udp(bool v6, std::string& error) {
    init();
    const Fd fd = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == kBad) {
        error = error_text(last_error());
        return {};
    }
    Socket s(static_cast<std::uintptr_t>(fd));
    set_nonblocking(fd);
    SocketAddress any;
    any.v6 = v6;
    sockaddr_storage ss;
    const SockLen len = to_sockaddr(any, ss);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&ss), len) != 0) {
        error = error_text(last_error());
        return {};
    }
    return s;
}

bool Socket::send_to(const SocketAddress& to, std::span<const std::uint8_t> data) {
    sockaddr_storage ss;
    const SockLen len = to_sockaddr(to, ss);
    return ::sendto(fd_of(fd_), reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
                    reinterpret_cast<sockaddr*>(&ss), len) == static_cast<long>(data.size());
}

std::size_t Socket::receive_from(std::span<std::uint8_t> buf, SocketAddress& from) {
    sockaddr_storage ss{};
    SockLen len = sizeof ss;
    const auto n = ::recvfrom(fd_of(fd_), reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0,
                              reinterpret_cast<sockaddr*>(&ss), &len);
    if (n <= 0) {
        return 0;
    }
    if (auto a = from_sockaddr(reinterpret_cast<sockaddr*>(&ss))) {
        from = *a;
    }
    return static_cast<std::size_t>(n);
}

bool Socket::connect_udp(const SocketAddress& to) {
    sockaddr_storage ss;
    const SockLen len = to_sockaddr(to, ss);
    return ::connect(fd_of(fd_), reinterpret_cast<sockaddr*>(&ss), len) == 0;
}

long Socket::send(std::span<const std::uint8_t> data, std::string& error) {
#if defined(MSG_NOSIGNAL)
    constexpr int kFlags = MSG_NOSIGNAL;
#else
    constexpr int kFlags = 0;
#endif
    const auto n = ::send(fd_of(fd_), reinterpret_cast<const char*>(data.data()),
                          static_cast<int>(std::min<std::size_t>(data.size(), 1 << 20)), kFlags);
    if (n >= 0) {
        return static_cast<long>(n);
    }
    const int e = last_error();
    if (would_block(e)) {
        return 0;
    }
    error = error_text(e);
    return -1;
}

long Socket::receive(std::span<std::uint8_t> buf, std::string& error) {
    const auto n = ::recv(fd_of(fd_), reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0);
    if (n > 0) {
        return static_cast<long>(n);
    }
    if (n == 0) {
        error = "The other side closed the connection.";
        return -1;
    }
    const int e = last_error();
    if (would_block(e)) {
        return 0;
    }
    error = error_text(e);
    return -1;
}

std::optional<SocketAddress> Socket::local_address() const {
    sockaddr_storage ss{};
    SockLen len = sizeof ss;
    if (::getsockname(fd_of(fd_), reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
        return std::nullopt;
    }
    return from_sockaddr(reinterpret_cast<sockaddr*>(&ss));
}

void wait_sockets(std::span<PollItem> items, Waker* waker, int timeout_ms) {
    init();
#if defined(_WIN32)
    std::vector<WSAPOLLFD> fds;
    for (const auto& it : items) {
        WSAPOLLFD p{};
        p.fd = fd_of(it.socket->handle());
        p.events = static_cast<SHORT>((it.want_read ? POLLRDNORM : 0) | (it.want_write ? POLLWRNORM : 0));
        fds.push_back(p);
    }
    if (waker && waker->ok()) {
        WSAPOLLFD p{};
        p.fd = static_cast<SOCKET>(waker->handle());
        p.events = POLLRDNORM;
        fds.push_back(p);
    }
    if (fds.empty()) {
        ::Sleep(static_cast<DWORD>(timeout_ms));
        return;
    }
    ::WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeout_ms);
    constexpr SHORT kRead = POLLRDNORM | POLLHUP, kWrite = POLLWRNORM, kFail = POLLERR | POLLNVAL;
#else
    std::vector<pollfd> fds;
    for (const auto& it : items) {
        pollfd p{};
        p.fd = fd_of(it.socket->handle());
        p.events = static_cast<short>((it.want_read ? POLLIN : 0) | (it.want_write ? POLLOUT : 0));
        fds.push_back(p);
    }
    if (waker && waker->ok()) {
        pollfd p{};
        p.fd = static_cast<int>(waker->handle());
        p.events = POLLIN;
        fds.push_back(p);
    }
    ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout_ms);
    constexpr short kRead = POLLIN | POLLHUP, kWrite = POLLOUT, kFail = POLLERR | POLLNVAL;
#endif
    for (std::size_t i = 0; i < items.size(); ++i) {
        items[i].readable = (fds[i].revents & kRead) != 0;
        items[i].writable = (fds[i].revents & kWrite) != 0;
        items[i].failed = (fds[i].revents & kFail) != 0;
    }
    if (waker) {
        waker->drain();
    }
}

std::uint32_t outward_ipv4() {
    std::string error;
    Socket s = Socket::udp(false, error);
    if (!s.valid() || !s.connect_udp(SocketAddress::ipv4(0x08080808, 53))) {
        return 0;
    }
    const auto a = s.local_address();
    return a && !a->v6 ? a->ipv4() : 0;
}

std::vector<SocketAddress> local_addresses() {
    init();
    std::vector<SocketAddress> out;
    auto add = [&](const sockaddr* sa) {
        const auto a = from_sockaddr(sa);
        if (!a) {
            return;
        }
        if (!a->v6) {
            const std::uint32_t v4 = a->ipv4();
            if ((v4 >> 24) == 127 || (v4 >> 16) == (169u << 8 | 254) || v4 == 0) {
                return;
            }
        } else if (!ipv6_global(a->ip)) {
            return;
        }
        SocketAddress clean = *a;
        clean.port = 0;
        clean.scope = 0;
        if (std::find(out.begin(), out.end(), clean) == out.end()) {
            out.push_back(clean);
        }
    };
#if defined(_WIN32)
    ULONG size = 16384;
    std::vector<std::uint8_t> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int i = 0; i < 3 && rc == ERROR_BUFFER_OVERFLOW; ++i) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_UNSPEC,
                                  GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) {
        return out;
    }
    for (auto* ad = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); ad; ad = ad->Next) {
        if (ad->OperStatus != IfOperStatusUp || ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
            ad->IfType == IF_TYPE_TUNNEL) {
            continue;
        }
        for (auto* ua = ad->FirstUnicastAddress; ua; ua = ua->Next) {
            add(ua->Address.lpSockaddr);
        }
    }
#else
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) {
        return out;
    }
    for (ifaddrs* it = list; it; it = it->ifa_next) {
        if (it->ifa_addr && (it->ifa_flags & IFF_UP) && !(it->ifa_flags & IFF_LOOPBACK)) {
            add(it->ifa_addr);
        }
    }
    ::freeifaddrs(list);
#endif
    // IPv4 first: more likely to work across a home network.
    std::stable_partition(out.begin(), out.end(), [](const SocketAddress& a) { return !a.v6; });
    return out;
}

std::vector<SocketAddress> resolve(const std::string& host, std::uint16_t port, bool v6) {
    init();
    std::vector<SocketAddress> out;
    addrinfo hints{};
    hints.ai_family = v6 ? AF_UNSPEC : AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0) {
        return out;
    }
    for (addrinfo* it = res; it; it = it->ai_next) {
        if (auto a = from_sockaddr(it->ai_addr)) {
            a->port = port;
            if (std::find(out.begin(), out.end(), *a) == out.end()) {
                out.push_back(*a);
            }
        }
    }
    ::freeaddrinfo(res);
    return out;
}

void init_sockets() { init(); }

std::string computer_name() {
    init();
    char buf[256] = {};
    if (::gethostname(buf, sizeof buf - 1) != 0) {
        return {};
    }
    return buf;
}

}  // namespace vette::net
