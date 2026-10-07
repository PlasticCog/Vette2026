// Online play: addresses, STUN, PCP, the direct offer, and whether a friend can reach the host.

#include "net/direct.h"
#include "net/nat.h"
#include "net/socket.h"
#include "test.h"

using namespace vette::net;

TEST(net_socket_address) {
    auto a = SocketAddress::parse("203.0.113.5:26989");
    CHECK(a && !a->v6 && a->ipv4() == 0xCB007105u && a->port == 26989);
    CHECK(a && a->to_string() == "203.0.113.5:26989");
    a = SocketAddress::parse("[2001:db8::1]:26989");
    CHECK(a && a->v6 && a->port == 26989 && a->ip[0] == 0x20 && a->ip[15] == 1);
    CHECK(a && a->to_string() == "[2001:db8::1]:26989");
    a = SocketAddress::parse("10.0.0.2", 7);
    CHECK(a && a->port == 7);
    CHECK(!SocketAddress::parse("300.1.1.1:5"));
    CHECK(!SocketAddress::parse("1.2.3:5"));
    CHECK(!SocketAddress::parse("1.2.3.4:0"));
    CHECK(!SocketAddress::parse("host.example:5"));

    CHECK(ipv4_private(*parse_ipv4("192.168.1.20")) && ipv4_private(*parse_ipv4("172.20.0.1")) &&
          ipv4_private(*parse_ipv4("10.9.8.7")));
    CHECK(!ipv4_private(*parse_ipv4("172.32.0.1")));
    CHECK(ipv4_shared(*parse_ipv4("100.64.0.1")) && ipv4_shared(*parse_ipv4("100.127.255.254")));
    CHECK(!ipv4_shared(*parse_ipv4("100.128.0.1")));
    CHECK(ipv4_public(*parse_ipv4("153.66.209.28")) && ipv4_public(*parse_ipv4("8.8.8.8")));
    CHECK(!ipv4_public(*parse_ipv4("127.0.0.1")) && !ipv4_public(*parse_ipv4("169.254.3.4")) &&
          !ipv4_public(*parse_ipv4("100.70.1.1")) && !ipv4_public(*parse_ipv4("192.168.0.1")));
    CHECK_EQ(format_ipv4(0xC0000201), std::string("192.0.2.1"));
}

TEST(net_stun) {
    // RFC 5769, 2.2: a response with XOR-MAPPED-ADDRESS 192.0.2.1:32853.
    const std::vector<std::uint8_t> response = {
        0x01, 0x01, 0x00, 0x3c, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87,
        0xdf, 0xae, 0x80, 0x22, 0x00, 0x0b, 0x74, 0x65, 0x73, 0x74, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x20,
        0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43, 0x00, 0x08, 0x00, 0x14, 0x2b, 0x91,
        0xf5, 0x99, 0xfd, 0x9e, 0x90, 0xc3, 0x8c, 0x74, 0x89, 0xf9, 0x2a, 0xf9, 0xba, 0x53, 0xf0, 0x6b, 0xe7, 0xd7,
        0x80, 0x28, 0x00, 0x04, 0xc0, 0x7d, 0x4c, 0x96};
    const StunTransaction id = {0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae};
    const auto mapped = stun_mapped_address(response, id);
    CHECK(mapped && mapped->ipv4() == 0xC0000201u && mapped->port == 32853);
    StunTransaction other = id;
    other[0] ^= 1;
    CHECK(!stun_mapped_address(response, other));  // not our request's answer
    const auto request = stun_binding_request(id);
    CHECK_EQ(request.size(), std::size_t{20});
    CHECK(request[0] == 0 && request[1] == 1 && request[4] == 0x21 && request[19] == 0xae);
}

TEST(net_pcp) {
    const PcpNonce nonce = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const auto req = pcp_map_request(0xC0A80114, nonce, 26989, 26989, 3600);
    CHECK_EQ(req.size(), std::size_t{60});
    CHECK(req[0] == 2 && req[1] == 1);                   // version 2, MAP request
    CHECK(req[18] == 0xFF && req[19] == 0xFF && req[20] == 192 && req[23] == 20);  // ::ffff:192.168.1.20
    CHECK(req[24] == 1 && req[35] == 12 && req[36] == 6);                          // nonce, TCP
    CHECK(req[40] == (26989 >> 8) && req[41] == (26989 & 255));
    // A success response: the same, as a response, with the router's address and port.
    std::vector<std::uint8_t> resp = req;
    resp[1] = 0x81;
    resp[3] = 0;  // success
    resp[42] = 0x69;
    resp[43] = 0x6E;  // port 26990
    resp[56] = 203;
    resp[57] = 0;
    resp[58] = 113;
    resp[59] = 5;
    const auto r = pcp_parse_map_response(resp, nonce);
    CHECK(r && r->result == 0 && r->external_port == 26990 && r->external_ip == 0xCB007105u && r->lifetime_s == 3600);
    PcpNonce other = nonce;
    other[0] = 9;
    CHECK(!pcp_parse_map_response(resp, other));
    const std::vector<std::uint8_t> natpmp_only = {0, 0x81, 0, 1, 0, 0, 0, 0};
    const auto old = pcp_parse_map_response(natpmp_only, nonce);
    CHECK(old && old->old_version);
}

TEST(net_reachability) {
    RouterMapping upnp;
    upnp.method = RouterMapping::Method::Upnp;
    upnp.external_port = 26989;
    upnp.router_ip = *parse_ipv4("153.66.209.28");
    auto r = judge_reachability(upnp, upnp.router_ip, 26989);
    CHECK(r.ok() && r.public_ip == upnp.router_ip && r.public_port == 26989);
    r = judge_reachability(upnp, std::nullopt, 26989);  // STUN blocked: the router's word
    CHECK(r.ok());

    RouterMapping cgnat = upnp;
    cgnat.router_ip = *parse_ipv4("100.72.10.4");
    r = judge_reachability(cgnat, *parse_ipv4("153.66.209.28"), 26989);
    CHECK(r.problem == Reachability::Problem::SharedAddress);
    CHECK(r.reason.find("carrier-grade NAT") != std::string::npos);
    CHECK(r.suggestion.find("host instead") != std::string::npos);

    r = judge_reachability(upnp, *parse_ipv4("153.66.209.99"), 26989);  // the internet sees another address
    CHECK(r.problem == Reachability::Problem::SharedAddress);
    CHECK(r.reason.find("153.66.209.99") != std::string::npos);

    RouterMapping doubled = upnp;
    doubled.router_ip = *parse_ipv4("192.168.0.10");
    r = judge_reachability(doubled, *parse_ipv4("153.66.209.28"), 26989);
    CHECK(r.problem == Reachability::Problem::DoubleNat);

    RouterMapping none;
    r = judge_reachability(none, *parse_ipv4("153.66.209.28"), 26989);
    CHECK(r.problem == Reachability::Problem::NoMapping);
    CHECK(r.reason.find("UPnP") != std::string::npos && r.suggestion.find("room code") != std::string::npos);
    r = judge_reachability(none, *parse_ipv4("153.66.209.28"), 26989, true);  // forwarded by hand
    CHECK(r.ok() && r.public_port == 26989);
    r = judge_reachability(none, *parse_ipv4("100.64.0.9"), 26989);
    CHECK(r.problem == Reachability::Problem::SharedAddress);
    r = judge_reachability(none, std::nullopt, 26989);
    CHECK(r.problem == Reachability::Problem::NoInternet);
}

TEST(net_direct_offer) {
    DirectOffer o;
    o.key = "0123456789abcdef";
    o.endpoints = {*SocketAddress::parse("192.168.1.20:26989"), *SocketAddress::parse("[2001:db8::7]:26989"),
                   *SocketAddress::parse("153.66.209.28:26990")};
    const auto back = DirectOffer::decode(o.encode());
    CHECK(back && back->key == o.key && back->endpoints == o.endpoints);
    CHECK(!DirectOffer::decode("direct 2\nkey=x"));
    CHECK(!DirectOffer::decode("direct 1\nep=1.2.3.4:5"));  // no key
    // Proofs differ by direction and depend on every input.
    const auto p = direct_guest_proof("k", "h", "g");
    CHECK(p.size() == 40 && p != direct_host_proof("k", "h", "g") && p != direct_guest_proof("k2", "h", "g") &&
          p != direct_guest_proof("k", "h2", "g"));
}
