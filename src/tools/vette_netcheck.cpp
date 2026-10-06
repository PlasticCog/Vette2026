// Online play check: creates or joins a room on the relay server, then sends test packets as a game
// would and checks that the friend's arrive intact, printing round-trip times.
//
//   vette_netcheck [options] create            the host: prints the room code
//   vette_netcheck [options] join VETTE-4KQ7   the guest
//
// --server URL      the relay server (default ws://127.0.0.1:8787, `wrangler dev`)
// --app V           VETTE! 2026's version in the hello (default 0.1.6); both sides must match
// --game B          the original game's build in the hello (default "DOS 1.1"); both sides must match
// --settings T      host: the race settings, key=value pairs separated by ';' (default improved_driving=1)
// --set-at S:T      host: change the race settings S seconds after the friend joined
// --seconds N       how long to send once connected (default 10)
// --rate HZ         packets per second (default 30)
// --size N          bytes per packet (default 50)
// --drop-at S[:O]   drop the connection S seconds after connecting, as a network failure would, with
//                   the network gone for O more seconds (repeatable); the link reconnects and the
//                   stream must stay intact
// --wait N          host: how long to wait for the friend (default 120 s)
// --ca FILE         extra trusted CA certificates (PEM), for a server with a self-made certificate
// --proto N         the protocol version to claim (to see the server refuse it)
// --code-file F     host: also write the room code to F
//
// Each side's stream is a known sequence (byte i of the host's is a function of i, and the guest's
// another), so the receiver checks every byte. Exit code: 0 intact, 1 couldn't connect or refused,
// 2 the stream arrived damaged, 3 bad arguments.

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

#include "net/room_link.h"

namespace {

using Clock = std::chrono::steady_clock;
using vette::net::LinkState;
using vette::net::LinkStatus;
using vette::net::RaceSettings;
using vette::net::RoomLink;
using vette::net::RoomOptions;

std::uint8_t stream_byte(bool host_stream, std::uint64_t i) {
    const std::uint64_t x = i * 2654435761u + (host_stream ? 0x5A : 0xA5) + (i >> 9);
    return static_cast<std::uint8_t>(x ^ (x >> 13));
}

std::optional<RaceSettings> parse_settings(std::string text) {
    std::replace(text.begin(), text.end(), ';', '\n');
    return RaceSettings::parse(text);
}

int usage() {
    std::fprintf(stderr, "usage: vette_netcheck [options] create | join CODE  (see the source for options)\n");
    return 3;
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) {
        return -1;
    }
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5))];
}

}  // namespace

int main(int argc, char* argv[]) {
    RoomOptions options;
    options.server_url = "ws://127.0.0.1:8787";
    options.app_version = "0.1.6";
    options.game_build = "DOS 1.1";
    std::string settings_text = "improved_driving=1";
    std::vector<std::pair<double, std::string>> set_at;
    std::vector<std::pair<double, double>> drops;  // when, and how long offline
    double seconds = 10, rate = 30, wait_s = 120;
    int size = 50;
    std::string mode, code, code_file;
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
        } else if (a == "--ca") {
            options.ca_file = value();
        } else if (a == "--proto") {
            options.protocol = std::atoi(value().c_str());
        } else if (a == "--code-file") {
            code_file = value();
        } else if (a == "create" && mode.empty()) {
            mode = a;
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
    const auto initial = parse_settings(settings_text);
    if (!initial) {
        std::fprintf(stderr, "bad --settings: %s\n", settings_text.c_str());
        return 3;
    }
    options.race_settings = *initial;

    const auto t0 = Clock::now();
    auto since = [&](Clock::time_point t) { return std::chrono::duration<double>(t - t0).count(); };
    auto log = [&](const char* fmt, auto... args) {
        std::printf("[%7.3f] ", since(Clock::now()));
        std::printf(fmt, args...);
        std::printf("\n");
        std::fflush(stdout);
    };

    auto link = mode == "create" ? RoomLink::create_room(options) : RoomLink::join_room(options, code);
    const bool host = mode == "create";

    LinkStatus last;
    last.state = static_cast<LinkState>(-1);
    std::uint32_t last_gen = 0;
    std::uint64_t last_samples = 0;
    std::vector<double> rtts;
    std::uint64_t sent = 0, received = 0, damaged = 0;
    bool settings_before_stream = false;
    std::optional<Clock::time_point> connected_at;
    Clock::time_point next_packet = Clock::now(), next_report = Clock::now();
    std::size_t next_drop = 0, next_set = 0;
    std::sort(drops.begin(), drops.end());
    std::vector<std::uint8_t> packet(static_cast<std::size_t>(size)), in(4096);
    int exit_code = 0;

    for (;;) {
        const auto now = Clock::now();
        const LinkStatus st = link->status();
        if (st.state != last.state || st.reason != last.reason) {
            if (st.state == LinkState::Waiting) {
                log("waiting for the friend: room %s", st.code.c_str());
                if (!code_file.empty()) {
                    std::ofstream(code_file) << st.code << "\n";
                }
            } else {
                log("%s%s%s", vette::net::to_string(st.state), st.reason.empty() ? "" : ": ", st.reason.c_str());
            }
            if (st.state == LinkState::Connected && !connected_at) {
                connected_at = now;
                next_packet = next_report = now;
            }
        }
        if (st.settings_gen != last_gen) {
            const auto s = link->race_settings();
            std::string text = s ? s->serialize() : "";
            std::replace(text.begin(), text.end(), '\n', ';');
            log("race settings (generation %u): %s", st.settings_gen, text.c_str());
            last_gen = st.settings_gen;
        }
        if (st.rtt_samples > last_samples) {  // the latest measurement (polled every millisecond)
            last_samples = st.rtt_samples;
            rtts.push_back(st.rtt_last_ms);
        }
        last = st;
        if (st.state == LinkState::Failed || st.state == LinkState::Closed ||
            (st.state == LinkState::PeerLeft && (!host || connected_at))) {
            exit_code = connected_at ? 0 : 1;
            break;
        }
        if (!connected_at && since(now) > wait_s) {
            log("nobody joined within %.0f s", wait_s);
            exit_code = 1;
            break;
        }

        // Receive and check the friend's stream.
        for (std::size_t n; (n = link->receive(in)) > 0;) {
            if (received == 0 && !host) {
                settings_before_stream = link->race_settings().has_value() && st.settings_gen > 0;
                log("first serial byte; the race settings were there before it: %s",
                    settings_before_stream ? "yes" : "NO");
            }
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
                log("dropping the connection (test), offline for %.1f s", drops[next_drop].second);
                link->simulate_drop(static_cast<int>(drops[next_drop].second * 1000));
                ++next_drop;
            }
            if (host && next_set < set_at.size() && t >= set_at[next_set].first) {
                if (auto s = parse_settings(set_at[next_set].second)) {
                    log("host changes the race settings to %s", set_at[next_set].second.c_str());
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
                log("sent %llu B, received %llu B%s, rtt %.1f ms (last %.1f), server %.1f ms, messages %llu/%llu, "
                    "resent %llu B",
                    static_cast<unsigned long long>(st.bytes_sent), static_cast<unsigned long long>(received),
                    damaged ? " DAMAGED" : " intact", st.rtt_ms, st.rtt_last_ms, st.server_rtt_ms,
                    static_cast<unsigned long long>(st.messages_sent),
                    static_cast<unsigned long long>(st.messages_received),
                    static_cast<unsigned long long>(st.bytes_resent));
                next_report += std::chrono::seconds(1);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Let the last bytes arrive, then leave.
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
    const LinkStatus st = link->status();
    link->close();
    link.reset();

    std::printf("\nrole %s, room %s, final state: %s%s%s\n", host ? "host" : "guest", st.code.c_str(),
                vette::net::to_string(st.state), st.reason.empty() ? "" : ": ", st.reason.c_str());
    std::printf("stream out: %llu bytes in %llu messages (%llu bytes resent); in: %llu bytes, %s\n",
                static_cast<unsigned long long>(st.bytes_sent), static_cast<unsigned long long>(st.messages_sent),
                static_cast<unsigned long long>(st.bytes_resent), static_cast<unsigned long long>(received),
                damaged ? "DAMAGED" : "every byte intact and in order");
    std::printf("reconnects: %d; race settings generation %u\n", st.reconnects, st.settings_gen);
    if (!rtts.empty()) {
        std::printf("round trip to the friend's client and back through the relay (%zu samples): min %.2f, "
                    "median %.2f, p95 %.2f, max %.2f ms\n",
                    rtts.size(), *std::min_element(rtts.begin(), rtts.end()), percentile(rtts, 0.5),
                    percentile(rtts, 0.95), *std::max_element(rtts.begin(), rtts.end()));
    }
    if (st.server_rtt_ms >= 0) {
        std::printf("round trip to the server (ping): %.2f ms\n", st.server_rtt_ms);
    }
    if (damaged) {
        return 2;
    }
    return exit_code;
}
