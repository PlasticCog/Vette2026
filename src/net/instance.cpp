#include "net/instance.h"

#include <array>
#include <chrono>
#include <cstddef>

namespace vette::net {
namespace {

// A new copy says "VETTE2026 INVITE <link>\n"; the running game answers "OK\n".
constexpr std::string_view kHello = "VETTE2026 INVITE ";
constexpr std::string_view kScheme = "vette2026://";
constexpr std::size_t kMaxLine = 512;

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

SocketAddress loopback() { return SocketAddress::ipv4(0x7F000001, kInstancePort); }

}  // namespace

InstanceChannel& InstanceChannel::get() {
    static InstanceChannel channel;
    return channel;
}

bool InstanceChannel::forward(std::string_view link, int timeout_ms) {
    if (link.size() + kHello.size() + 1 > kMaxLine)
        return false;
    init_sockets();
    std::string error;
    Socket s = Socket::connect_tcp(loopback(), error);
    if (!s.valid())
        return false;
    const std::int64_t deadline = now_ms() + timeout_ms;
    const std::string message = std::string(kHello) + std::string(link) + "\n";
    std::size_t sent = 0;
    std::string answer;
    bool connected = false;
    while (now_ms() < deadline) {
        PollItem item{&s, !connected || sent == message.size(), !connected || sent < message.size()};
        wait_sockets({&item, 1}, nullptr, 50);
        if (item.failed)
            return false;
        if (!connected) {
            const auto state = s.finish_connect(error);
            if (state == Socket::ConnectState::Failed)
                return false;  // nobody listening: no game running
            if (state == Socket::ConnectState::Pending)
                continue;
            connected = true;
        }
        if (sent < message.size()) {
            const long n = s.send({reinterpret_cast<const std::uint8_t*>(message.data()) + sent, message.size() - sent},
                                  error);
            if (n < 0)
                return false;
            sent += static_cast<std::size_t>(n);
            continue;
        }
        std::array<std::uint8_t, 16> buf{};
        const long n = s.receive(buf, error);
        if (n < 0)
            return false;
        answer.append(reinterpret_cast<const char*>(buf.data()), static_cast<std::size_t>(n));
        if (answer.find('\n') != std::string::npos)
            return answer.rfind("OK", 0) == 0;
    }
    return false;  // something else on the port, or a game that didn't answer
}

bool InstanceChannel::listen() {
    if (listener_.valid())
        return true;
    init_sockets();
    std::string error;
    listener_ = Socket::listen_tcp(false, kInstancePort, true, error);
    return listener_.valid();
}

void InstanceChannel::close() {
    clients_.clear();
    listener_.close();
}

std::optional<std::string> InstanceChannel::take() {
    if (!listener_.valid())
        return std::nullopt;
    for (;;) {
        Socket s = listener_.accept();
        if (!s.valid())
            break;
        if (clients_.size() < 4)
            clients_.push_back({std::move(s), {}});
    }
    std::optional<std::string> link;
    for (std::size_t i = 0; i < clients_.size();) {
        Client& c = clients_[i];
        std::string error;
        std::array<std::uint8_t, 256> buf{};
        bool drop = false;
        for (;;) {
            const long n = c.socket.receive(buf, error);
            if (n < 0) {
                drop = true;
                break;
            }
            if (n == 0)
                break;
            c.line.append(reinterpret_cast<const char*>(buf.data()), static_cast<std::size_t>(n));
            if (c.line.size() > kMaxLine) {
                drop = true;
                break;
            }
        }
        const std::size_t end = c.line.find('\n');
        if (!drop && end != std::string::npos) {
            const std::string_view line = std::string_view(c.line).substr(0, end);
            if (!link && line.rfind(kHello, 0) == 0 && line.substr(kHello.size()).rfind(kScheme, 0) == 0) {
                link = std::string(line.substr(kHello.size()));
                static constexpr std::uint8_t kOk[] = {'O', 'K', '\n'};
                c.socket.send(kOk, error);  // (a short reply into an empty buffer: it fits)
            }
            drop = true;
        }
        if (drop)
            clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(i));
        else
            ++i;
    }
    return link;
}

}  // namespace vette::net
