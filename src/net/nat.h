#pragma once
// Getting through the home router for a direct connection (net/direct.h): this computer's address on the
// internet (STUN, RFC 8489), the PCP port-mapping messages (RFC 6887; UPnP and NAT-PMP are in
// net/port_mapper.h, through miniupnpc and libnatpmp), and the verdict: can a friend connect to us,
// and if not, why not, in words for the player.

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "net/socket.h"

namespace vette::net {

// --- STUN ----------------------------------------------------------------------------------------------
using StunTransaction = std::array<std::uint8_t, 12>;
std::vector<std::uint8_t> stun_binding_request(const StunTransaction& id);
// The address the server saw us at (XOR-MAPPED-ADDRESS, else MAPPED-ADDRESS), if `message` is the
// success response to `id`.
std::optional<SocketAddress> stun_mapped_address(std::span<const std::uint8_t> message, const StunTransaction& id);

inline const std::vector<std::string> kStunServers = {"stun.cloudflare.com:3478", "stun.l.google.com:19302"};
// Asks the servers in turn (blocking, at most `timeout_ms` each): this computer's public IPv4 address.
std::optional<std::uint32_t> stun_public_ipv4(const std::vector<std::string>& servers = kStunServers,
                                              int timeout_ms = 1500, const std::atomic<bool>* abort = nullptr);

// --- PCP -----------------------------------------------------------------------------------------------
using PcpNonce = std::array<std::uint8_t, 12>;
// A MAP request for TCP `internal_port` from `client` (our address toward the router); lifetime 0
// removes the mapping.
std::vector<std::uint8_t> pcp_map_request(std::uint32_t client, const PcpNonce& nonce, std::uint16_t internal_port,
                                          std::uint16_t suggested_port, std::uint32_t lifetime_s);
struct PcpMapResult {
    int result = -1;  // 0: success (RFC 6887 result codes)
    std::uint32_t lifetime_s = 0;
    std::uint16_t external_port = 0;
    std::uint32_t external_ip = 0;  // IPv4, host order (0 if the router gave an IPv6 one)
    bool old_version = false;       // the router speaks NAT-PMP, not PCP
};
std::optional<PcpMapResult> pcp_parse_map_response(std::span<const std::uint8_t> message, const PcpNonce& nonce);

// --- Can a friend connect to us? -----------------------------------------------------------------------
struct RouterMapping {
    enum class Method { None, Upnp, NatPmp, Pcp };
    Method method = Method::None;
    std::uint16_t external_port = 0;
    std::uint32_t router_ip = 0;  // the router's own internet address, as it says (0: unknown)
    std::string lan_ip;           // this computer's address toward the router
    std::string log;              // what was tried, for logs and vette_netcheck
};
const char* to_string(RouterMapping::Method method);

struct Reachability {
    enum class Problem {
        None,
        NoInternet,  // no public address found at all
        NoMapping,   // the router didn't open the port (no UPnP / NAT-PMP / PCP)
        SharedAddress,  // carrier-grade NAT: the provider shares the address (100.64/10, or STUN differs)
        DoubleNat,   // the router is behind another one
    };
    Problem problem = Problem::None;
    std::uint32_t public_ip = 0;  // where a friend connects (host order)
    std::uint16_t public_port = 0;
    std::string reason;      // for the player, when there's a problem
    std::string suggestion;  // what to do instead
    bool ok() const { return problem == Problem::None; }
};
// From what the router did and what STUN saw. `manual_forward`: the player has forwarded the port in the
// router themselves, so no mapping is needed.
Reachability judge_reachability(const RouterMapping& mapping, std::optional<std::uint32_t> stun_ip,
                                std::uint16_t internal_port, bool manual_forward = false);

}  // namespace vette::net
