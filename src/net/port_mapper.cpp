#include "net/port_mapper.h"

#if defined(_WIN32)
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
#include <netinet/in.h>
#endif

#if defined(VETTE_HAVE_MINIUPNPC)
#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>
#endif
#if defined(VETTE_HAVE_NATPMP)
#include <natpmp.h>
extern "C" int getdefaultgateway(in_addr_t* addr);  // libnatpmp's getgateway.c
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <random>
#include <vector>

namespace vette::net {

namespace {

constexpr std::uint32_t kLifetimeS = 3600;  // the router forgets the mapping after an hour if we die
#if defined(VETTE_HAVE_MINIUPNPC)
const char* kDescription = "VETTE! 2026 online race";
#endif

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

struct PortMapper::State {
#if defined(VETTE_HAVE_MINIUPNPC)
    UPNPUrls urls{};
    IGDdatas data{};
    bool have_urls = false;
#endif
    std::uint32_t gateway = 0;  // host order
    std::uint32_t client = 0;
    PcpNonce nonce{};
    std::uint16_t external_port = 0;
    RouterMapping::Method method = RouterMapping::Method::None;
    std::int64_t renew_at_ms = 0;
};

bool PortMapper::available() {
#if defined(VETTE_HAVE_MINIUPNPC) || defined(VETTE_HAVE_NATPMP)
    return true;
#else
    return false;
#endif
}

PortMapper::PortMapper(std::uint16_t port, bool manual_forward, bool use_stun)
    : port_(port), manual_forward_(manual_forward), use_stun_(use_stun), state_(new State) {
    thread_ = std::thread([this] { run(); });
}

PortMapper::~PortMapper() {
    remove();
    delete state_;
}

std::optional<PortMapper::Result> PortMapper::result() const {
    std::lock_guard lock(mutex_);
    return result_;
}

void PortMapper::remove() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void PortMapper::run() {
    init_sockets();
    // STUN alongside the router: they take about as long.
    std::future<std::optional<std::uint32_t>> stun;
    if (use_stun_)
        stun = std::async(std::launch::async, [this] { return stun_public_ipv4(kStunServers, 1500, &stop_); });
    RouterMapping m;
    m.external_port = port_;
    // (A router whose own internet address isn't public can't make us reachable by any of them.)
    const bool mapped =
        map_upnp(m) || ((!m.router_ip || ipv4_public(m.router_ip)) && (map_natpmp(m) || map_pcp(m)));
    if (!mapped) {
        m.method = RouterMapping::Method::None;
    }
    state_->method = m.method;
    state_->external_port = m.external_port;
    state_->renew_at_ms = now_ms() + kLifetimeS * 1000 / 2;
    Result r;
    if (stun.valid())
        r.stun_ip = stun.get();
    r.mapping = m;
    r.reachability = judge_reachability(m, r.stun_ip, port_, manual_forward_, use_stun_);
    {
        std::lock_guard lock(mutex_);
        result_ = r;
    }
    // Keep the mapping alive until we're done, then remove it.
    std::unique_lock lock(mutex_);
    while (!stop_) {
        cv_.wait_for(lock, std::chrono::seconds(30), [this] { return stop_.load(); });
        if (!stop_ && state_->method != RouterMapping::Method::None && now_ms() >= state_->renew_at_ms) {
            lock.unlock();
            renew();
            lock.lock();
        }
    }
    lock.unlock();
    unmap();
}

bool PortMapper::map_upnp([[maybe_unused]] RouterMapping& m) {
#if defined(VETTE_HAVE_MINIUPNPC)
    if (stop_) {
        return false;
    }
    // Asked on the network toward the internet (with several networks, VPNs or virtual machines', the
    // system's choice for multicast may be another one), for every kind of gateway at once: 2 s at most.
    static const char* const kTypes[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                                         "urn:schemas-upnp-org:service:WANIPConnection:1",
                                         "urn:schemas-upnp-org:service:WANPPPConnection:1", nullptr};
    int error = 0;
    const std::uint32_t outward_ip = outward_ipv4();
    const std::string outward = outward_ip ? format_ipv4(outward_ip) : std::string();
    UPNPDev* devices = upnpDiscoverDevices(kTypes, 2000, outward.empty() ? nullptr : outward.c_str(), nullptr,
                                           UPNP_LOCAL_PORT_ANY, 0, 2, &error, 1);
    if (!devices) {
        m.log += "UPnP: no router answered.\n";
        return false;
    }
    char lan[64] = {}, wan[64] = {};
    State& st = *state_;
    const int igd = UPNP_GetValidIGD(devices, &st.urls, &st.data, lan, sizeof lan, wan, sizeof wan);
    freeUPNPDevlist(devices);
    if (igd == UPNP_NO_IGD) {
        m.log += "UPnP: devices answered, but none is an internet gateway.\n";
        return false;
    }
    st.have_urls = true;
    m.lan_ip = lan;
    if (const auto w = parse_ipv4(wan)) {
        m.router_ip = *w;
    }
    char ext[40] = {};
    if (UPNP_GetExternalIPAddress(st.urls.controlURL, st.data.first.servicetype, ext) == UPNPCOMMAND_SUCCESS) {
        if (const auto e = parse_ipv4(ext)) {
            m.router_ip = *e;
        }
    }
    m.log += std::string("UPnP: router at ") + (st.urls.rootdescURL ? st.urls.rootdescURL : "?") +
             ", its internet address " + (m.router_ip ? format_ipv4(m.router_ip) : std::string("unknown")) + ".\n";
    if (igd != UPNP_CONNECTED_IGD) {
        m.log += igd == UPNP_PRIVATEIP_IGD ? "UPnP: the router's internet address isn't a public one.\n"
                                           : "UPnP: the router says it isn't connected.\n";
        return false;
    }
    const std::string internal = std::to_string(port_);
    for (int i = 0; i < 10; ++i) {
        const std::string external = std::to_string(port_ + i);
        const std::string lease = std::to_string(kLifetimeS);
        int r = UPNP_AddPortMapping(st.urls.controlURL, st.data.first.servicetype, external.c_str(), internal.c_str(),
                                    lan, kDescription, "TCP", nullptr, lease.c_str());
        if (r == 725) {  // OnlyPermanentLeasesSupported: removed when we're done, as always
            r = UPNP_AddPortMapping(st.urls.controlURL, st.data.first.servicetype, external.c_str(), internal.c_str(),
                                    lan, kDescription, "TCP", nullptr, "0");
        }
        if (r == UPNPCOMMAND_SUCCESS) {
            m.method = RouterMapping::Method::Upnp;
            m.external_port = static_cast<std::uint16_t>(port_ + i);
            m.log += "UPnP: forwarding TCP port " + external + " to " + lan + ":" + internal + ".\n";
            return true;
        }
        m.log += "UPnP: forwarding TCP port " + external + " refused (" + std::to_string(r) + " " +
                 strupnperror(r) + ").\n";
        if (r != 718) {  // ConflictInMappingEntry: another computer has it; try the next
            break;
        }
    }
    return false;
#else
    return false;
#endif
}

bool PortMapper::map_natpmp([[maybe_unused]] RouterMapping& m) {
#if defined(VETTE_HAVE_NATPMP)
    if (stop_) {
        return false;
    }
    natpmp_t n;
    if (initnatpmp(&n, 0, 0) < 0) {
        m.log += "NAT-PMP: no router address.\n";
        return false;
    }
    auto ask = [&](natpmpresp_t& response) {
        const std::int64_t deadline = now_ms() + 1500;
        int r = NATPMP_TRYAGAIN;
        while (r == NATPMP_TRYAGAIN && now_ms() < deadline && !stop_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            r = readnatpmpresponseorretry(&n, &response);
        }
        return r;
    };
    natpmpresp_t response{};
    bool ok = false;
    if (sendpublicaddressrequest(&n) >= 0 && ask(response) >= 0) {
        m.router_ip = ntohl(response.pnu.publicaddress.addr.s_addr);
        m.log += "NAT-PMP: the router's internet address is " + format_ipv4(m.router_ip) + ".\n";
        if (sendnewportmappingrequest(&n, NATPMP_PROTOCOL_TCP, port_, port_, kLifetimeS) >= 0 && ask(response) >= 0) {
            m.method = RouterMapping::Method::NatPmp;
            m.external_port = response.pnu.newportmapping.mappedpublicport;
            m.log += "NAT-PMP: forwarding TCP port " + std::to_string(m.external_port) + ".\n";
            ok = true;
        } else {
            m.log += "NAT-PMP: the router refused to forward the port.\n";
        }
    } else {
        m.log += "NAT-PMP: no answer from the router.\n";
    }
    closenatpmp(&n);
    return ok;
#else
    return false;
#endif
}

bool PortMapper::map_pcp([[maybe_unused]] RouterMapping& m) {
#if defined(VETTE_HAVE_NATPMP)
    if (stop_) {
        return false;
    }
    in_addr_t gw = 0;
    if (getdefaultgateway(&gw) < 0) {
        return false;
    }
    State& st = *state_;
    st.gateway = ntohl(gw);
    std::string error;
    Socket s = Socket::udp(false, error);
    const SocketAddress router = SocketAddress::ipv4(st.gateway, 5351);
    if (!s.valid() || !s.connect_udp(router)) {
        return false;
    }
    const auto local = s.local_address();
    if (!local) {
        return false;
    }
    st.client = local->ipv4();
    std::random_device rd;
    for (auto& b : st.nonce) {
        b = static_cast<std::uint8_t>(rd());
    }
    const auto request = pcp_map_request(st.client, st.nonce, port_, port_, kLifetimeS);
    const std::int64_t deadline = now_ms() + 1500;
    std::int64_t next_send = 0, interval = 250;
    while (now_ms() < deadline && !stop_) {
        if (now_ms() >= next_send) {
            s.send_to(router, request);
            next_send = now_ms() + interval;
            interval *= 2;
        }
        PollItem item{&s};
        wait_sockets({&item, 1}, nullptr, 50);
        std::uint8_t buf[1100];
        SocketAddress from;
        for (std::size_t n; (n = s.receive_from(buf, from)) > 0;) {
            const auto r = pcp_parse_map_response({buf, n}, st.nonce);
            if (!r) {
                continue;
            }
            if (r->old_version || r->result != 0) {
                m.log += r->old_version ? "PCP: the router only speaks NAT-PMP.\n"
                                        : "PCP: the router refused (result " + std::to_string(r->result) + ").\n";
                return false;
            }
            m.method = RouterMapping::Method::Pcp;
            m.external_port = r->external_port;
            if (r->external_ip) {
                m.router_ip = r->external_ip;
            }
            m.lan_ip = format_ipv4(st.client);
            m.log += "PCP: forwarding TCP port " + std::to_string(m.external_port) + ".\n";
            return true;
        }
    }
    m.log += "PCP: no answer from the router.\n";
    return false;
#else
    return false;
#endif
}

void PortMapper::renew() {
    // The same request again extends the lease.
    State& st = *state_;
    RouterMapping m;
    m.external_port = st.external_port;
    switch (st.method) {
    case RouterMapping::Method::Upnp: {
#if defined(VETTE_HAVE_MINIUPNPC)
        const std::string external = std::to_string(st.external_port), internal = std::to_string(port_);
        const std::string lease = std::to_string(kLifetimeS);
        char lan[64] = {};
        if (const auto r = result(); r) {
            std::snprintf(lan, sizeof lan, "%s", r->mapping.lan_ip.c_str());
        }
        UPNP_AddPortMapping(st.urls.controlURL, st.data.first.servicetype, external.c_str(), internal.c_str(), lan,
                            kDescription, "TCP", nullptr, lease.c_str());
#endif
        break;
    }
    case RouterMapping::Method::NatPmp: map_natpmp(m); break;
    case RouterMapping::Method::Pcp: map_pcp(m); break;
    case RouterMapping::Method::None: break;
    }
    st.renew_at_ms = now_ms() + kLifetimeS * 1000 / 2;
}

void PortMapper::unmap() {
    State& st = *state_;
    if (removed_) {
        return;
    }
    removed_ = true;
    switch (st.method) {
    case RouterMapping::Method::Upnp: {
#if defined(VETTE_HAVE_MINIUPNPC)
        const std::string external = std::to_string(st.external_port);
        UPNP_DeletePortMapping(st.urls.controlURL, st.data.first.servicetype, external.c_str(), "TCP", nullptr);
#endif
        break;
    }
    case RouterMapping::Method::NatPmp: {
#if defined(VETTE_HAVE_NATPMP)
        natpmp_t n;
        if (initnatpmp(&n, 0, 0) >= 0) {
            natpmpresp_t response{};
            if (sendnewportmappingrequest(&n, NATPMP_PROTOCOL_TCP, port_, 0, 0) >= 0) {
                const std::int64_t deadline = now_ms() + 1000;
                while (readnatpmpresponseorretry(&n, &response) == NATPMP_TRYAGAIN && now_ms() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
            }
            closenatpmp(&n);
        }
#endif
        break;
    }
    case RouterMapping::Method::Pcp: {
        std::string error;
        Socket s = Socket::udp(false, error);
        if (s.valid()) {
            s.send_to(SocketAddress::ipv4(st.gateway, 5351), pcp_map_request(st.client, st.nonce, port_, 0, 0));
        }
        break;
    }
    case RouterMapping::Method::None: break;
    }
#if defined(VETTE_HAVE_MINIUPNPC)
    if (st.have_urls) {
        FreeUPNPUrls(&st.urls);
        st.have_urls = false;
    }
#endif
}

}  // namespace vette::net
