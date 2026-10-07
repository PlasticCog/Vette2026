#include "net/nat.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

namespace vette::net {

namespace {

constexpr std::uint32_t kStunCookie = 0x2112A442;

std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }
std::uint32_t be32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}
void put16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}
void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    put16(out, static_cast<std::uint16_t>(v >> 16));
    put16(out, static_cast<std::uint16_t>(v));
}
void put_mapped_v4(std::vector<std::uint8_t>& out, std::uint32_t v4) {
    for (int i = 0; i < 10; ++i) {
        out.push_back(0);
    }
    out.push_back(0xFF);
    out.push_back(0xFF);
    put32(out, v4);
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

const char* kAnotherWay = "Ask your friend to host instead, or use a room code.";

}  // namespace

std::vector<std::uint8_t> stun_binding_request(const StunTransaction& id) {
    std::vector<std::uint8_t> m;
    put16(m, 0x0001);  // Binding request
    put16(m, 0);       // no attributes
    put32(m, kStunCookie);
    m.insert(m.end(), id.begin(), id.end());
    return m;
}

std::optional<SocketAddress> stun_mapped_address(std::span<const std::uint8_t> m, const StunTransaction& id) {
    if (m.size() < 20 || be16(&m[0]) != 0x0101 || be32(&m[4]) != kStunCookie ||
        std::memcmp(&m[8], id.data(), 12) != 0) {
        return std::nullopt;
    }
    const std::size_t end = std::min<std::size_t>(m.size(), 20u + be16(&m[2]));
    std::optional<SocketAddress> plain;
    for (std::size_t pos = 20; pos + 4 <= end;) {
        const std::uint16_t type = be16(&m[pos]), len = be16(&m[pos + 2]);
        const std::size_t v = pos + 4;
        if (v + len > end) {
            break;
        }
        if ((type == 0x0020 || type == 0x0001) && len >= 8 && m[v + 1] == 0x01) {  // IPv4
            std::uint16_t port = be16(&m[v + 2]);
            std::uint32_t ip = be32(&m[v + 4]);
            if (type == 0x0020) {  // XOR-MAPPED-ADDRESS
                port ^= static_cast<std::uint16_t>(kStunCookie >> 16);
                ip ^= kStunCookie;
                return SocketAddress::ipv4(ip, port);
            }
            plain = SocketAddress::ipv4(ip, port);
        }
        pos = v + ((len + 3u) & ~3u);
    }
    return plain;
}

std::optional<std::uint32_t> stun_public_ipv4(const std::vector<std::string>& servers, int timeout_ms,
                                              const std::atomic<bool>* abort) {
    std::random_device rd;
    for (const auto& server : servers) {
        const std::size_t colon = server.rfind(':');
        const std::string host = server.substr(0, colon);
        const auto port = static_cast<std::uint16_t>(colon == std::string::npos ? 3478 : std::stoi(server.substr(colon + 1)));
        const auto addresses = resolve(host, port);
        if (addresses.empty()) {
            continue;
        }
        std::string error;
        Socket s = Socket::udp(false, error);
        if (!s.valid()) {
            return std::nullopt;
        }
        StunTransaction id;
        for (auto& b : id) {
            b = static_cast<std::uint8_t>(rd());
        }
        const auto request = stun_binding_request(id);
        const std::int64_t deadline = now_ms() + timeout_ms;
        std::int64_t next_send = 0, interval = 250;
        while (now_ms() < deadline && !(abort && abort->load())) {
            if (now_ms() >= next_send) {
                s.send_to(addresses.front(), request);
                next_send = now_ms() + interval;
                interval *= 2;
            }
            PollItem item{&s};
            wait_sockets({&item, 1}, nullptr, 50);
            std::uint8_t buf[512];
            SocketAddress from;
            for (std::size_t n; (n = s.receive_from(buf, from)) > 0;) {
                if (const auto mapped = stun_mapped_address({buf, n}, id)) {
                    return mapped->ipv4();
                }
            }
        }
    }
    return std::nullopt;
}

std::vector<std::uint8_t> pcp_map_request(std::uint32_t client, const PcpNonce& nonce, std::uint16_t internal_port,
                                          std::uint16_t suggested_port, std::uint32_t lifetime_s) {
    std::vector<std::uint8_t> m;
    m.push_back(2);  // version
    m.push_back(1);  // request, MAP
    put16(m, 0);
    put32(m, lifetime_s);
    put_mapped_v4(m, client);
    m.insert(m.end(), nonce.begin(), nonce.end());
    m.push_back(6);  // TCP
    m.push_back(0);
    m.push_back(0);
    m.push_back(0);
    put16(m, internal_port);
    put16(m, suggested_port);
    put_mapped_v4(m, 0);  // any external address
    return m;
}

std::optional<PcpMapResult> pcp_parse_map_response(std::span<const std::uint8_t> m, const PcpNonce& nonce) {
    PcpMapResult r;
    if (m.size() >= 4 && m[0] == 0) {  // NAT-PMP's version: "unsupported version"
        r.old_version = true;
        return r;
    }
    if (m.size() < 60 || m[0] != 2 || m[1] != (0x80 | 1) || std::memcmp(&m[24], nonce.data(), 12) != 0) {
        return std::nullopt;
    }
    r.result = m[3];
    r.lifetime_s = be32(&m[4]);
    r.external_port = be16(&m[42]);
    static constexpr std::uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (std::memcmp(&m[44], kMapped, 12) == 0) {
        r.external_ip = be32(&m[56]);
    }
    return r;
}

const char* to_string(RouterMapping::Method method) {
    switch (method) {
    case RouterMapping::Method::None: return "none";
    case RouterMapping::Method::Upnp: return "UPnP";
    case RouterMapping::Method::NatPmp: return "NAT-PMP";
    case RouterMapping::Method::Pcp: return "PCP";
    }
    return "?";
}

Reachability judge_reachability(const RouterMapping& mapping, std::optional<std::uint32_t> stun_ip,
                                std::uint16_t internal_port, bool manual_forward) {
    Reachability r;
    const std::uint32_t router = mapping.router_ip;
    auto shared = [&](std::uint32_t seen_by_router, std::uint32_t seen_outside) {
        r.problem = Reachability::Problem::SharedAddress;
        r.reason = "Your internet provider shares one internet address between many customers (carrier-grade NAT";
        if (seen_by_router && seen_outside && seen_by_router != seen_outside) {
            r.reason += ": your router has " + format_ipv4(seen_by_router) + ", the internet sees " +
                        format_ipv4(seen_outside);
        }
        r.reason += "), so your friend can't connect to you directly.";
        r.suggestion = kAnotherWay;
    };
    if (router && ipv4_shared(router)) {
        shared(router, stun_ip.value_or(0));
        return r;
    }
    if (router && !ipv4_public(router)) {
        r.problem = Reachability::Problem::DoubleNat;
        r.reason = "Your router is behind another router (its internet address is " + format_ipv4(router) +
                   "), so your friend can't connect to you directly.";
        r.suggestion = kAnotherWay;
        return r;
    }
    if (stun_ip && ipv4_shared(*stun_ip)) {
        shared(router, *stun_ip);
        return r;
    }
    if (router && stun_ip && *stun_ip != router) {
        shared(router, *stun_ip);
        return r;
    }
    const std::uint32_t ip = router ? router : stun_ip.value_or(0);
    if (!ip || !ipv4_public(ip)) {
        r.problem = Reachability::Problem::NoInternet;
        r.reason = "Couldn't find your internet address: there's no internet connection, or the network blocks it.";
        r.suggestion = "Check your connection, or use a room code.";
        return r;
    }
    if (mapping.method == RouterMapping::Method::None && !manual_forward) {
        r.problem = Reachability::Problem::NoMapping;
        r.reason = "Your router didn't open a port for the game: it doesn't do UPnP or NAT-PMP, or they're "
                   "switched off in its settings.";
        r.suggestion = "Switch UPnP on in your router, or forward TCP port " + std::to_string(internal_port) +
                       " to this computer yourself. " + kAnotherWay;
        r.public_ip = ip;
        r.public_port = internal_port;
        return r;
    }
    r.public_ip = ip;
    r.public_port = mapping.method == RouterMapping::Method::None ? internal_port : mapping.external_port;
    return r;
}

}  // namespace vette::net
