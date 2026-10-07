// Online play check: hosts a race or joins one (net/online.h), then sends test packets as a game would
// and checks that the friend's arrive intact, printing the route and round-trip times.
//
//   vette_netcheck [options] host               a room code (with --server) and a direct code
//   vette_netcheck [options] join CODE          a room code, a direct code or an invite link
//   vette_netcheck stun                         this computer's internet address, by STUN
//   vette_netcheck map [--seconds N]            opens the port on the router, shows what it did, removes it
//
// --server URL      the relay server (default ws://127.0.0.1:8787, `wrangler dev`; "" for none)
// --app V / --game B  versions in the hello (default 0.1.6 / "DOS 1.1"); both sides must match
// --settings T      host: the race settings, key=value pairs separated by ';' (default improved_driving=1)
// --set-at S:T      host: change the race settings S seconds after the friend joined
// --seconds N       how long to send once connected (default 10)
// --rate HZ / --size N  test packets (default 30 a second, 50 bytes)
// --drop-at S[:O]   drop the connection S seconds after connecting, offline O more seconds (repeatable)
// --wait N          host: how long to wait for the friend (default 120 s)
// --no-room         host: no room code          --no-direct   host: no direct code
// --no-upgrade      don't try going direct through a room (host or guest)
// --port N          host: the port to listen on (default 26989)
// --code-address A  host: make the direct code for address A (a LAN address, 127.0.0.1) instead of the
//                   internet address: no router involved
// --loopback        host: listen on loopback only
// --offer A:P       host: offer this address for going direct through a room, instead of its own (repeatable;
//                   e.g. an unreachable one, to see the fallback to the server)
// --manual-forward  host: the port is forwarded on the router by hand
// --ca FILE         extra trusted CA certificates for the server (PEM)
// --code-file F     host: write the room code (else the direct code) to F;  --direct-code-file F: the direct code
//
// Each side's stream is a known sequence, so the receiver checks every byte. Exit code: 0 intact, 1 couldn't
// connect or was refused, 2 the stream arrived damaged, 3 bad arguments.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "net/nat.h"
#include "net/online.h"
#include "net/port_mapper.h"

namespace {

using Clock = std::chrono::steady_clock;
using vette::net::LinkState;
using vette::net::OnlineLink;
using vette::net::OnlineOptions;
using vette::net::OnlineStatus;
using vette::net::RaceSettings;
using vette::net::RouteStatus;

std::uint8_t stream_byte(bool host_stream, std::uint64_t i) {
    const std::uint64_t x = i * 2654435761u + (host_stream ? 0x5A : 0xA5) + (i >> 9);
    return static_cast<std::uint8_t>(x ^ (x >> 13));
}

std::optional<RaceSettings> parse_settings(std::string text) {
    std::replace(text.begin(), text.end(), ';', '\n');
    return RaceSettings::parse(text);
}

int usage() {
    std::fprintf(stderr, "usage: vette_netcheck [options] host | join CODE | stun | map  (options: see the source)\n");
    return 3;
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) {
        return -1;
    }
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5))];
}

const char* state_name(RouteStatus::State s) {
    switch (s) {
    case RouteStatus::State::Off: return "off";
    case RouteStatus::State::Starting: return "starting";
    case RouteStatus::State::Ready: return "ready";
    case RouteStatus::State::Failed: return "failed";
    }
    return "?";
}

std::string route_text(const char* name, const RouteStatus& r) {
    std::string t = std::string(name) + " " + state_name(r.state);
    if (!r.code.empty()) {
        t += " " + r.code;
    }
    if (r.state == RouteStatus::State::Failed) {
        t += ": " + r.reason + (r.suggestion.empty() ? "" : " [" + r.suggestion + "]");
    }
    return t;
}

}  // namespace

int main(int argc, char* argv[]) {
    OnlineOptions options;
    options.server_url = "ws://127.0.0.1:8787";
    options.app_version = "0.1.6";
    options.game_build = "DOS 1.1";
    std::string settings_text = "improved_driving=1";
    std::vector<std::pair<double, std::string>> set_at;
    std::vector<std::pair<double, double>> drops;
    double seconds = 10, rate = 30, wait_s = 120;
    int size = 50;
    std::string mode, code, code_file, direct_code_file;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(3);
            }
            return argv[++i];
        };
        if (a == "--server") {
            options.server_url = value();
        } else if (a == "--app") {
            options.app_version = value();
        } else if (a == "--game") {
            options.game_build = value();
        } else if (a == "--settings") {
            settings_text = value();
        } else if (a == "--set-at") {
            const std::string v = value();
            const auto colon = v.find(':');
            if (colon == std::string::npos) {
                return usage();
            }
            set_at.emplace_back(std::atof(v.substr(0, colon).c_str()), v.substr(colon + 1));
        } else if (a == "--seconds") {
            seconds = std::atof(value().c_str());
        } else if (a == "--rate") {
            rate = std::max(0.1, std::atof(value().c_str()));
        } else if (a == "--size") {
            size = std::max(1, std::atoi(value().c_str()));
        } else if (a == "--drop-at") {
            const std::string v = value();
            const auto colon = v.find(':');
            drops.emplace_back(std::atof(v.substr(0, colon).c_str()),
                               colon == std::string::npos ? 0.0 : std::atof(v.substr(colon + 1).c_str()));
        } else if (a == "--wait") {
            wait_s = std::atof(value().c_str());
        } else if (a == "--no-room") {
            options.room = false;
        } else if (a == "--no-direct") {
            options.direct_code = false;
        } else if (a == "--no-upgrade") {
            options.go_direct = false;
        } else if (a == "--port") {
            options.port = static_cast<std::uint16_t>(std::atoi(value().c_str()));
        } else if (a == "--code-address") {
            options.code_address = value();
        } else if (a == "--offer") {
            options.offer_endpoints.push_back(value());
        } else if (a == "--loopback") {
            options.loopback_only = true;
        } else if (a == "--manual-forward") {
            options.manual_port_forward = true;
        } else if (a == "--ca") {
            options.ca_file = value();
        } else if (a == "--code-file") {
            code_file = value();
        } else if (a == "--direct-code-file") {
            direct_code_file = value();
        } else if ((a == "host" || a == "create" || a == "stun" || a == "map") && mode.empty()) {
            mode = a == "create" ? "host" : a;
        } else if (a == "join" && mode.empty()) {
            mode = a;
            code = value();
        } else {
            return usage();
        }
    }
    if (mode.empty()) {
        return usage();
    }

    if (mode == "stun") {
        const auto ip = vette::net::stun_public_ipv4();
        std::printf("STUN: %s\n", ip ? vette::net::format_ipv4(*ip).c_str() : "no answer");
        return ip ? 0 : 1;
    }
    if (mode == "map") {
        std::printf("port mapping %s in this build\n", vette::net::PortMapper::available() ? "available" : "NOT available");
        auto mapper = std::make_unique<vette::net::PortMapper>(options.port, options.manual_port_forward);
        std::optional<vette::net::PortMapper::Result> r;
        while (!(r = mapper->result())) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::printf("%s", r->mapping.log.c_str());
        std::printf("method: %s, router's address: %s, LAN address: %s, external port %u\n",
                    vette::net::to_string(r->mapping.method),
                    r->mapping.router_ip ? vette::net::format_ipv4(r->mapping.router_ip).c_str() : "unknown",
                    r->mapping.lan_ip.c_str(), r->mapping.external_port);
        std::printf("STUN: %s\n", r->stun_ip ? vette::net::format_ipv4(*r->stun_ip).c_str() : "no answer");
        if (r->reachability.ok()) {
            std::printf("reachable at %s:%u\n", vette::net::format_ipv4(r->reachability.public_ip).c_str(),
                        r->reachability.public_port);
        } else {
            std::printf("not reachable: %s\n  suggestion: %s\n", r->reachability.reason.c_str(),
                        r->reachability.suggestion.c_str());
        }
        std::this_thread::sleep_for(std::chrono::duration<double>(std::min(seconds, 60.0)));
        mapper.reset();
        std::printf("mapping removed\n");
        return 0;
    }

    const auto initial = parse_settings(settings_text);
    if (!initial) {
        std::fprintf(stderr, "bad --settings: %s\n", settings_text.c_str());
        return 3;
    }
    options.race_settings = *initial;

    const auto t0 = Clock::now();
    auto since = [&](Clock::time_point t) { return std::chrono::duration<double>(t - t0).count(); };
    auto log = [&](const std::string& text) {
        std::printf("[%7.3f] %s\n", since(Clock::now()), text.c_str());
        std::fflush(stdout);
    };

    const bool host = mode == "host";
    auto link = host ? OnlineLink::host(options) : OnlineLink::join(options, code);

    std::string last_line, last_room, last_direct, last_invite;
    std::uint32_t last_gen = 0;
    std::uint64_t last_samples = 0;
    std::vector<double> rtts;
    std::uint64_t sent = 0, received = 0, damaged = 0;
    std::optional<Clock::time_point> connected_at;
    Clock::time_point next_packet = Clock::now(), next_report = Clock::now();
    std::size_t next_drop = 0, next_set = 0;
    std::sort(drops.begin(), drops.end());
    std::vector<std::uint8_t> packet(static_cast<std::size_t>(size)), in(4096);
    int exit_code = 0;
    OnlineStatus st;

    for (;;) {
        const auto now = Clock::now();
        st = link->status();
        const std::string line = std::string(vette::net::to_string(st.state)) +
                                 (st.route != vette::net::Route::None ? std::string(" (") + to_string(st.route) + ")" : "") +
                                 (st.activity.empty() ? "" : ": " + st.activity) +
                                 (st.reason.empty() ? "" : " | " + st.reason) +
                                 (st.suggestion.empty() ? "" : " [" + st.suggestion + "]");
        if (line != last_line) {
            log(line);
            last_line = line;
        }
        const std::string room = route_text("room", st.room), direct = route_text("direct", st.direct);
        if (room != last_room && st.room.state != RouteStatus::State::Off) {
            log(room);
            if (!code_file.empty() && st.room.state == RouteStatus::State::Ready) {
                std::ofstream(code_file) << st.room.code << "\n";
            }
        }
        if (direct != last_direct && st.direct.state != RouteStatus::State::Off) {
            log(direct + (st.direct.detail.empty() ? "" : " (" + st.direct.detail + ")"));
            if (!st.direct.log.empty() && st.direct.state != RouteStatus::State::Starting) {
                std::printf("%s", st.direct.log.c_str());
            }
            if (st.direct.state == RouteStatus::State::Ready) {
                if (!direct_code_file.empty()) {
                    std::ofstream(direct_code_file) << st.direct.code << "\n";
                }
                if (!code_file.empty() && st.room.state == RouteStatus::State::Off) {
                    std::ofstream(code_file) << st.direct.code << "\n";
                }
            }
        }
        last_room = room;
        last_direct = direct;
        if (host) {
            const std::string invite = link->invite_url();
            if (invite != last_invite && !invite.empty()) {
                log("invite: " + invite);
                last_invite = invite;
            }
        }
        if (st.link.settings_gen != last_gen && st.state == LinkState::Connected) {
            const auto s = link->race_settings();
            std::string text = s ? s->serialize() : "";
            std::replace(text.begin(), text.end(), '\n', ';');
            log("race settings (generation " + std::to_string(st.link.settings_gen) + "): " + text);
            last_gen = st.link.settings_gen;
        }
        if (st.link.rtt_samples > last_samples) {
            last_samples = st.link.rtt_samples;
            rtts.push_back(st.link.rtt_last_ms);
        }
        if (st.state == LinkState::Failed || st.state == LinkState::Closed ||
            (st.state == LinkState::PeerLeft && (!host || connected_at))) {
            exit_code = connected_at ? 0 : 1;
            break;
        }
        if (!connected_at && since(now) > wait_s) {
            log("nobody joined in time");
            exit_code = 1;
            break;
        }
        if (st.state == LinkState::Connected && !connected_at) {
            connected_at = now;
            next_packet = next_report = now;
            if (!host) {
                const auto s = link->race_settings();
                log(std::string("race settings before the first serial byte: ") +
                    (s ? "yes (" + s->serialize() + ")" : "NO"));
            }
        }

        for (std::size_t n; (n = link->receive(in)) > 0;) {
            for (std::size_t i = 0; i < n; ++i, ++received) {
                if (in[i] != stream_byte(!host, received)) {
                    ++damaged;
                }
            }
        }

        if (connected_at) {
            const double t = std::chrono::duration<double>(now - *connected_at).count();
            if (t >= seconds) {
                break;
            }
            if (next_drop < drops.size() && t >= drops[next_drop].first) {
                log("dropping the connection (test), offline for " + std::to_string(drops[next_drop].second) + " s");
                link->simulate_drop(static_cast<int>(drops[next_drop].second * 1000));
                ++next_drop;
            }
            if (host && next_set < set_at.size() && t >= set_at[next_set].first) {
                if (auto s = parse_settings(set_at[next_set].second)) {
                    log("host changes the race settings to " + set_at[next_set].second);
                    link->set_race_settings(*s);
                }
                ++next_set;
            }
            if (link->connected() && now >= next_packet) {
                for (auto& b : packet) {
                    b = stream_byte(host, sent++);
                }
                link->send(packet);
                next_packet += std::chrono::microseconds(static_cast<std::int64_t>(1e6 / rate));
                if (next_packet < now) {
                    next_packet = now;
                }
            }
            if (now >= next_report) {
                char buf[256];
                std::snprintf(buf, sizeof buf, "sent %llu B, received %llu B%s, rtt %.2f ms (last %.2f), messages %llu/%llu",
                              static_cast<unsigned long long>(st.link.bytes_sent),
                              static_cast<unsigned long long>(received), damaged ? " DAMAGED" : " intact", st.rtt_ms,
                              st.link.rtt_last_ms, static_cast<unsigned long long>(st.link.messages_sent),
                              static_cast<unsigned long long>(st.link.messages_received));
                log(buf);
                next_report += std::chrono::seconds(1);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (connected_at && exit_code == 0) {
        const auto until = Clock::now() + std::chrono::milliseconds(1500);
        while (Clock::now() < until) {
            for (std::size_t n; (n = link->receive(in)) > 0;) {
                for (std::size_t i = 0; i < n; ++i, ++received) {
                    if (in[i] != stream_byte(!host, received)) {
                        ++damaged;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    st = link->status();
    link->close();
    link.reset();

    std::printf("\nrole %s, route %s, final state: %s%s%s\n", host ? "host" : "guest",
                st.route == vette::net::Route::None ? "none" : to_string(st.route), vette::net::to_string(st.state),
                st.reason.empty() ? "" : ": ", st.reason.c_str());
    std::printf("stream out: %llu bytes in %llu messages (%llu resent); in: %llu bytes, %s\n",
                static_cast<unsigned long long>(st.link.bytes_sent),
                static_cast<unsigned long long>(st.link.messages_sent),
                static_cast<unsigned long long>(st.link.bytes_resent), static_cast<unsigned long long>(received),
                damaged ? "DAMAGED" : "every byte intact and in order");
    std::printf("reconnects: %d; race settings generation %u\n", st.link.reconnects, st.link.settings_gen);
    if (!rtts.empty()) {
        std::printf("round trip to the friend's game (%zu samples): min %.2f, median %.2f, p95 %.2f, max %.2f ms\n",
                    rtts.size(), *std::min_element(rtts.begin(), rtts.end()), percentile(rtts, 0.5),
                    percentile(rtts, 0.95), *std::max_element(rtts.begin(), rtts.end()));
    }
    if (damaged) {
        return 2;
    }
    return exit_code;
}
