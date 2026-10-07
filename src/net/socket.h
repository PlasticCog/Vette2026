#pragma once
// Plain sockets for the direct connection (net/direct.h), STUN and port mapping: addresses, non-blocking
// TCP and UDP sockets, waiting on several at once, and this computer's own addresses. Windows (Winsock)
// and POSIX.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vette::net {

// An IPv4 or IPv6 address with a port.
struct SocketAddress {
    bool v6 = false;
    std::array<std::uint8_t, 16> ip{};  // IPv4: the first 4 bytes
    std::uint16_t port = 0;
    std::uint32_t scope = 0;  // IPv6 link-local interface

    static SocketAddress ipv4(std::uint32_t address, std::uint16_t port);  // address in host order
    std::uint32_t ipv4() const;                                           // host order (IPv4 only)
    // "203.0.113.5:26989", "[2001:db8::1]:26989"; a bare address takes `default_port`.
    static std::optional<SocketAddress> parse(std::string_view text, std::uint16_t default_port = 0);
    std::string to_string() const;   // with the port
    std::string ip_string() const;   // without

    bool operator==(const SocketAddress&) const = default;
};

// What kind of address an IPv4 address is (host order).
bool ipv4_private(std::uint32_t a);  // 10/8, 172.16/12, 192.168/16
bool ipv4_shared(std::uint32_t a);   // 100.64/10: carrier-grade NAT
bool ipv4_public(std::uint32_t a);   // reachable on the internet (none of the reserved ranges)
std::optional<std::uint32_t> parse_ipv4(std::string_view text);
std::string format_ipv4(std::uint32_t a);
bool ipv6_global(const std::array<std::uint8_t, 16>& ip);  // 2000::/3

// Wakes a thread waiting in wait_sockets() (a socket it can poll alongside the others).
class Waker {
public:
    Waker();
    ~Waker();
    Waker(const Waker&) = delete;
    Waker& operator=(const Waker&) = delete;

    void wake();
    void drain();
    std::uintptr_t handle() const { return read_; }
    bool ok() const { return ok_; }

private:
    std::uintptr_t read_ = 0, write_ = 0;
    bool ok_ = false;
};

// A non-blocking socket.
class Socket {
public:
    Socket() = default;
    ~Socket() { close(); }
    Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = kInvalid; }
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool valid() const { return fd_ != kInvalid; }
    std::uintptr_t handle() const { return fd_; }
    void close();

    // TCP: listening on `port` (0: any) on every interface, or only the loopback one.
    static Socket listen_tcp(bool v6, std::uint16_t port, bool loopback_only, std::string& error);
    Socket accept(SocketAddress* from = nullptr);
    // TCP: starts connecting; finish_connect() says when it has (poll for writing).
    static Socket connect_tcp(const SocketAddress& to, std::string& error);
    enum class ConnectState { Pending, Connected, Failed };
    ConnectState finish_connect(std::string& error) const;

    // UDP, bound to any port.
    static Socket udp(bool v6, std::string& error);
    // UDP (IPv4) for finding races on the local network (net/lan.h): allowed to send broadcasts; bound to
    // `local` (0: every interface; an interface's address: its broadcasts leave through it) and `port` (0:
    // any; else shared, so several games on one computer can each have it).
    static Socket udp_lan(std::uint32_t local, std::uint16_t port, std::string& error);
    bool send_to(const SocketAddress& to, std::span<const std::uint8_t> data);
    // Bytes received (0 if nothing waiting); `from` is the sender.
    std::size_t receive_from(std::span<std::uint8_t> buf, SocketAddress& from);
    // UDP: fixes the peer, so local_address() is the address this computer uses toward it.
    bool connect_udp(const SocketAddress& to);

    // >0 bytes; 0 would block; -1 the connection ended or failed (`error` says which).
    long send(std::span<const std::uint8_t> data, std::string& error);
    long receive(std::span<std::uint8_t> buf, std::string& error);

    std::optional<SocketAddress> local_address() const;

private:
    static constexpr std::uintptr_t kInvalid = ~std::uintptr_t{0};
    explicit Socket(std::uintptr_t fd) : fd_(fd) {}
    std::uintptr_t fd_ = kInvalid;
};

// Waits until one of the sockets can be read (or written, where asked), the waker is woken, or the
// timeout passes; then sets each item's flags.
struct PollItem {
    const Socket* socket = nullptr;
    bool want_read = true, want_write = false;
    bool readable = false, writable = false, failed = false;
};
void wait_sockets(std::span<PollItem> items, Waker* waker, int timeout_ms);

// This computer's addresses on its networks: IPv4 (no loopback or self-assigned 169.254) and global
// IPv6, for the direct connection's offer.
std::vector<SocketAddress> local_addresses();

// This computer's IPv4 address on the network that leads to the internet (it may have others: VPNs,
// virtual machines' networks), found by "connecting" a UDP socket, which sends nothing. 0: none.
std::uint32_t outward_ipv4();

// Looks a name up (blocking): its IPv4 addresses (and IPv6 ones if `v6`).
std::vector<SocketAddress> resolve(const std::string& host, std::uint16_t port, bool v6 = false);

std::string computer_name();

// Starts the system's sockets (Winsock) if they aren't yet; other socket libraries need it first.
void init_sockets();

}  // namespace vette::net
