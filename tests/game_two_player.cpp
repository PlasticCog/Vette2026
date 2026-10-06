// The original's two-player race (game/two_player.h, re/notes/12-two-player.md): the setup text, the
// cable's packets, and two hosted games linked by an in-memory cable, driven into the race through their
// own menus: each sees the other's car, with and without network-like latency; a finish always reaches
// the other game; with Improved Driving the other game shows a car's jump in the air. The real-game
// tests are skipped when the game files are missing (Game/VETTE.EXE, or the folder in VETTE_GAME_DIR).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "game/driving.h"
#include "game/options.h"
#include "game/smooth.h"
#include "game/two_player.h"
#include "game/x86.h"
#include "host/loopback_link.h"
#include "host/machine.h"
#include "test.h"

namespace fs = std::filesystem;
using vette::game::Driving;
using vette::game::LinkFrame;
using vette::game::LinkFrameParser;
using vette::game::LinkPacer;
using vette::game::TwoPlayerSetup;
using vette::game::TwoPlayerStart;
using vette::host::Cpu;
using vette::host::Machine;

TEST(two_player_setup_text) {
    TwoPlayerSetup s;
    s.course = 3;
    s.improved_driving = true;
    CHECK(s.encode() == "vette2p/1 course=3 improved=1");
    const auto back = TwoPlayerSetup::decode(s.encode());
    CHECK(back && back->course == 3 && back->improved_driving);
    const auto later = TwoPlayerSetup::decode("vette2p/1 improved=0 course=2 weather=7");  // unknown keys: ignored
    CHECK(later && later->course == 2 && !later->improved_driving);
    CHECK(!TwoPlayerSetup::decode("vette2p/2 course=1"));
    CHECK(!TwoPlayerSetup::decode("vette2p/1 course=9"));
    CHECK(!TwoPlayerSetup::decode("vette2p/1 improved=1"));
    CHECK(!TwoPlayerSetup::decode("hello"));
}

namespace {

// A packet as serial_send_packet (422F:01C8) puts it on the line.
std::vector<uint8_t> line_packet(std::vector<uint8_t> body, bool corrupt = false) {
    const auto len = static_cast<uint16_t>(body.size() + 2);
    body.insert(body.begin(), {static_cast<uint8_t>(len), static_cast<uint8_t>(len >> 8)});
    uint8_t sum = 0;
    for (const uint8_t b : body) sum = static_cast<uint8_t>(sum + b);
    std::vector<uint8_t> out = {'I', 'D', 'N', static_cast<uint8_t>(sum + (corrupt ? 1 : 0))};
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

} // namespace

TEST(two_player_frame_parser) {
    std::vector<uint8_t> stream = {'x', 'I', 'D', 'y'};  // noise, and a false start
    std::vector<uint8_t> city(0x30, 0);
    city[0] = 4;  // status 4: finished
    city[2 + 4] = 0x34;  // z = 1234h
    city[2 + 5] = 0x12;
    const auto a = line_packet(city);
    const auto b = line_packet({1, 0, 9, 9, 9, 9}, true);  // a status packet (len 8), bad checksum
    stream.insert(stream.end(), a.begin(), a.end());
    stream.push_back('z');
    stream.insert(stream.end(), b.begin(), b.end());
    LinkFrameParser parser;
    std::vector<LinkFrame> frames;
    std::vector<uint8_t> other;
    // In two pieces, split inside the first packet.
    const auto feed = [&](size_t from, size_t to) {
        parser.feed(std::span<const uint8_t>(stream.data() + from, to - from),
                    [&](const LinkFrame& f) { frames.push_back(f); },
                    [&](std::span<const uint8_t> v) { other.insert(other.end(), v.begin(), v.end()); });
    };
    feed(0, 20);
    feed(20, stream.size());
    CHECK_EQ(frames.size(), 2u);
    if (frames.size() == 2) {
        CHECK(frames[0].kind() == LinkFrame::Kind::City);
        CHECK(frames[0].checksum_ok);
        CHECK_EQ(frames[0].status(), 4);
        CHECK_EQ(frames[0].car(LinkFrame::kZ), 0x1234);
        CHECK(frames[1].kind() == LinkFrame::Kind::Status);
        CHECK(!frames[1].checksum_ok);
    }
    CHECK(other == std::vector<uint8_t>({'x', 'I', 'D', 'y', 'z'}));
    CHECK_EQ(parser.skipped(), 5u);
}

namespace {

fs::path game_dir() {
#ifdef _MSC_VER
    char* value = nullptr;
    size_t len = 0;
    std::string env;
    if (_dupenv_s(&value, &len, "VETTE_GAME_DIR") == 0 && value) env = value;
    std::free(value);
#else
    const char* value = std::getenv("VETTE_GAME_DIR");
    const std::string env = value ? value : "";
#endif
    if (!env.empty()) return env;
    return fs::path(__FILE__).parent_path().parent_path() / "Game";
}

bool have_game(const fs::path& dir) {
    if (!fs::exists(dir / "VETTE.EXE")) {
        std::printf("  SKIPPED: no VETTE.EXE in %s\n", dir.string().c_str());
        return false;
    }
    return true;
}

constexpr uint64_t kMs = 1'000'000;
constexpr uint16_t kCode = vette::game::emu_seg(0x3009);
const uint16_t kData = vette::game::kDataSeg;

struct Input {
    double at;  // seconds after this side's race start
    uint8_t scancode = 0;
    uint16_t poke = 0, value = 0;
};

// Course 1: first gear, full throttle, right onto the Great Highway, then placed in lane `y` heading north.
std::vector<Input> north(uint16_t y) {
    return {{7, 0x02}, {7.1, 0x82}, {7.2, 0x48}, {8.2, 0x4D}, {9.3, 0xCD}, {10.7, 0, 0x2D37, y}, {10.7, 0, 0x2D3B, 0}};
}

struct Side {
    std::unique_ptr<Machine> m;
    std::unique_ptr<vette::host::DelayedLink> delayed;
    std::unique_ptr<LinkPacer> pacer;
    std::unique_ptr<TwoPlayerStart> start;
    std::unique_ptr<Driving> driving;
    std::vector<Input> in;
    size_t next = 0;
    uint64_t race_ns = 0;
    double t() const { return race_ns ? static_cast<double>(m->emulated_ns() - race_ns) / 1e9 : -1; }
    int16_t w(uint16_t off) const { return static_cast<int16_t>(vette::game::rd16(m->memory(), kData, off)); }
    long abs_x(uint16_t car) const { return static_cast<long>(w(car + 0x22)) * 0x8000 + static_cast<uint16_t>(w(car)); }
    long abs_y(uint16_t car) const { return static_cast<long>(w(car + 0x24)) * 0x8000 + static_cast<uint16_t>(w(car + 2)); }
};

// Two games on one cable, side A the host. `lag`: each way's latency (default none).
struct Pair {
    vette::host::LoopbackCable cable;
    Side side[2];

    bool boot(const fs::path& dir, TwoPlayerSetup setup, vette::host::DelayedLink::Options lag = {},
              bool pacer = true) {
        for (int k = 0; k < 2; ++k) {
            Side& s = side[k];
            vette::host::MachineConfig config;
            config.game_dir = dir;
            config.save_dir = fs::temp_directory_path() / (k ? "vette2026_2p_test_b" : "vette2026_2p_test_a");
            config.start_time = vette::host::RealTime{1989, 10, 23, 12, 0, 0, 0};
            s.m = std::make_unique<Machine>(config);
            std::string error;
            if (!s.m->boot(error)) {
                std::printf("  boot failed: %s\n", error.c_str());
                return false;
            }
            vette::game::install_skip_manual_check(s.m->cpu());
            lag.seed = 7u + static_cast<uint32_t>(k);
            s.delayed = std::make_unique<vette::host::DelayedLink>(cable.end(k), [this] { return side[0].m->emulated_ns(); }, lag);
            if (pacer) {
                s.pacer = std::make_unique<LinkPacer>(*s.m, *s.delayed);
                s.m->attach_serial(s.pacer.get());
            } else {
                s.m->attach_serial(s.delayed.get());
            }
            s.start = std::make_unique<TwoPlayerStart>(*s.m, k ? TwoPlayerStart::Role::Guest : TwoPlayerStart::Role::Host,
                                                       setup, TwoPlayerStart::Own{});
        }
        for (int ms = 0; ms < 7; ++ms) {
            side[1].m->run_for(kMs);  // the two PCs aren't in lockstep
        }
        return true;
    }

    // Runs both a millisecond at a time while `go` holds (at most `seconds`); false if a side failed.
    template <typename F>
    bool run(double seconds, F go) {
        for (int ms = 0; ms < static_cast<int>(seconds * 1000) && go(); ++ms) {
            for (Side& s : side) {
                s.m->run_for(kMs);
                s.start->poll();
                if (s.start->failed() || s.m->stopped()) {
                    std::printf("  %s\n", s.start->error().c_str());
                    return false;
                }
                if (!s.race_ns && s.start->racing()) s.race_ns = s.m->emulated_ns();
                for (; s.race_ns && s.next < s.in.size() && s.in[s.next].at <= s.t(); ++s.next) {
                    if (s.in[s.next].scancode) {
                        s.m->key(s.in[s.next].scancode);
                    } else {
                        vette::game::wr16(s.m->memory(), kData, s.in[s.next].poke, s.in[s.next].value);
                    }
                }
            }
        }
        return true;
    }
    bool racing() const { return side[0].race_ns && side[1].race_ns; }
};

// Into the race and on the Great Highway together (A in the right lane, B in the next), then how far
// behind its real position each game shows the other car, on average over `from`..`to` seconds.
struct DriveResult {
    bool ok = false;
    double lag[2] = {0, 0};
    double max_lag[2] = {0, 0};
    uint64_t sent[2] = {0, 0};
    double start_skew = 1e9;  // seconds between the two race starts (side B runs 7 ms ahead)
};
DriveResult drive_together(const fs::path& dir, vette::host::DelayedLink::Options lag, double to) {
    DriveResult r;
    Pair p;
    if (!p.boot(dir, TwoPlayerSetup{}, lag)) return r;
    if (!p.run(60, [&] { return !p.racing(); }) || !p.racing()) return r;
    const uint16_t kOther = 0x2F09, kMine = 0x2D35;
    std::printf("  race start: A %.2f s, B %.2f s (emulated); B in the second start position: %s\n",
                static_cast<double>(p.side[0].race_ns) / 1e9, static_cast<double>(p.side[1].race_ns) / 1e9,
                p.side[1].start->second_slot() ? "yes" : "no");
    if (p.side[0].start->second_slot() == p.side[1].start->second_slot()) return r;  // one each
    r.start_skew = std::fabs(static_cast<double>(p.side[0].race_ns) - static_cast<double>(p.side[1].race_ns)) / 1e9;
    p.side[0].in = north(0x10E0);
    p.side[1].in = north(0x10A0);
    double sum[2] = {0, 0};
    int n[2] = {0, 0};
    const bool ok = p.run(to + 1, [&] {
        for (int k = 0; k < 2; ++k) {
            const Side& me = p.side[k];
            const Side& other = p.side[1 - k];
            if (me.t() >= 30 && me.t() <= to) {
                const double d = std::hypot(static_cast<double>(other.abs_x(kMine) - me.abs_x(kOther)),
                                            static_cast<double>(other.abs_y(kMine) - me.abs_y(kOther)));
                sum[k] += d;
                ++n[k];
                r.max_lag[k] = std::max(r.max_lag[k], d);
            }
        }
        return p.side[0].t() < to;
    });
    for (int k = 0; k < 2; ++k) {
        r.lag[k] = n[k] ? sum[k] / n[k] : 1e9;
        r.sent[k] = p.side[k].m->uart(0).stats().sent + p.side[k].m->uart(1).stats().sent;
    }
    r.ok = ok && n[0] > 0 && n[1] > 0;
    return r;
}

} // namespace

// Both games through their menus into the race, then each sees the other's car where it is: at most a
// frame or so behind (both at full speed, 810 units/s, ~10 frames a second on the 12 MHz PC).
TEST(two_player_loopback_race) {
    const fs::path dir = game_dir();
    if (!have_game(dir)) return;
    const DriveResult r = drive_together(dir, {}, 45);
    CHECK(r.ok);
    std::printf("  no latency: the other car shown %.0f / %.0f units behind on average (at most %.0f / %.0f)\n",
                r.lag[0], r.lag[1], r.max_lag[0], r.max_lag[1]);
    CHECK(r.lag[0] < 100 && r.lag[1] < 100);
    CHECK(r.sent[0] > 10'000 && r.sent[1] > 10'000);
    CHECK(r.start_skew < 0.1);  // the handshake lines the two starts up
}

// The same with a network's latency each way: 50-150 ms, in bursts. The race goes on; each car is
// shown about a tenth of a second behind (81 units at full speed).
TEST(two_player_loopback_race_with_latency) {
    const fs::path dir = game_dir();
    if (!have_game(dir)) return;
    vette::host::DelayedLink::Options lag;
    lag.delay_ns = 50 * kMs;
    lag.jitter_ns = 100 * kMs;
    lag.stall_ns = 150 * kMs;
    lag.stalls = 0.5;
    const DriveResult r = drive_together(dir, lag, 45);
    CHECK(r.ok);
    std::printf("  50-150 ms: the other car shown %.0f / %.0f units behind on average (at most %.0f / %.0f)\n",
                r.lag[0], r.lag[1], r.max_lag[0], r.max_lag[1]);
    CHECK(r.lag[0] < 250 && r.lag[1] < 250);
    CHECK(r.start_skew < 0.3);  // a one-way delay or so
}

// A player finishing sends one packet saying so; the other game must get it (with the pacer it always
// does: the original's receiver would drop it if it arrived in the same frame as the packet before).
// The smooth renderer replays the race view on a scratch copy of the machine many times a game frame;
// that copy must never touch the link. The same race with and without it drawing on side A (125 frames a
// second): both games end bit-identical, with the same bytes sent and received.
TEST(two_player_smooth_renderer_leaves_the_link_alone) {
    const fs::path dir = game_dir();
    if (!have_game(dir)) return;
    struct End {
        std::vector<uint8_t> ram[2];
        uint64_t sent[2], received[2];
        uint64_t replays = 0;
    };
    const auto race = [&](bool smooth) {
        End e{};
        Pair p;
        CHECK(p.boot(dir, TwoPlayerSetup{}));
        std::unique_ptr<vette::game::SmoothRenderer> renderer;
        if (smooth) renderer = std::make_unique<vette::game::SmoothRenderer>(*p.side[0].m);
        vette::host::Ega::Frame frame;
        p.side[0].in = north(0x10E0);
        p.side[1].in = north(0x10A0);
        int ms = 0;
        CHECK(p.run(40, [&] {
            if (renderer && ++ms % 8 == 0) renderer->render(p.side[0].m->emulated_ns(), frame);  // 125 a second
            return p.side[0].t() < 16;
        }));
        for (int k = 0; k < 2; ++k) {
            const Machine& m = *p.side[k].m;
            e.ram[k].assign(p.side[k].m->memory().ram(), p.side[k].m->memory().ram() + vette::host::Memory::kSize);
            e.sent[k] = m.uart(0).stats().sent + m.uart(1).stats().sent;
            e.received[k] = m.uart(0).stats().received + m.uart(1).stats().received;
        }
        if (renderer) e.replays = renderer->stats().replays;
        return e;
    };
    const End plain = race(false), smooth = race(true);
    std::printf("  %llu replays; bytes sent %llu / %llu, received %llu / %llu\n",
                static_cast<unsigned long long>(smooth.replays), static_cast<unsigned long long>(smooth.sent[0]),
                static_cast<unsigned long long>(smooth.sent[1]), static_cast<unsigned long long>(smooth.received[0]),
                static_cast<unsigned long long>(smooth.received[1]));
    CHECK(smooth.replays > 500);
    for (int k = 0; k < 2; ++k) {
        CHECK(plain.sent[k] > 5'000);
        CHECK_EQ(plain.sent[k], smooth.sent[k]);
        CHECK_EQ(plain.received[k], smooth.received[k]);
        CHECK(plain.ram[k] == smooth.ram[k]);
    }
}

TEST(two_player_finish_reaches_the_other_game) {
    const fs::path dir = game_dir();
    if (!have_game(dir)) return;
    for (const double finish : {14.0, 14.27, 14.51}) {
        Pair p;
        CHECK(p.boot(dir, TwoPlayerSetup{}));
        CHECK(p.run(60, [&] { return !p.racing(); }) && p.racing());
        p.side[0].in = north(0x10E0);
        p.side[1].in = north(0x10A0);
        bool armed = false, sent = false;
        Machine& a = *p.side[0].m;
        // Where a finish box would (3009:1AFF, in the player step): cs:3 ends the race, DS:2 = 4 goes out
        // in this frame's packet.
        a.cpu().add_watch(Cpu::linear(kCode, 0x020D), [&](Cpu&) {
            if (armed && !sent) {
                vette::game::wr8(a.memory(), kCode, 0x0003, 0xFF);
                vette::game::wr8(a.memory(), kData, 0x0002, 4);
                sent = true;
            }
        });
        CHECK(p.run(finish + 4, [&] {
            armed = p.side[0].t() >= finish;
            return p.side[0].t() < finish + 3;
        }));
        const Side& b = p.side[1];
        std::printf("  finish at %.2f s: the other game knows: %s (its status %d)\n", finish,
                    vette::game::rd8(b.m->memory(), kData, 0xFA45) == 0xFF ? "yes" : "no",
                    vette::game::rd8(b.m->memory(), kData, 0x0003));
        CHECK(sent);
        CHECK_EQ(vette::game::rd8(b.m->memory(), kData, 0xFA45), 0xFF);  // opponent_finished ran
        CHECK_EQ(vette::game::rd8(b.m->memory(), kData, 0x0003), 4);      // "OPPONENT FINISHED"
    }
}

// Improved Driving in a two-player race: the host plays Improved, the guest started with the original
// physics and takes the host's before the race. The guest's car flies over the Great Highway crest; its
// packets carry its height in the air, and the host shows the car in the air (not on the road), also
// with latency and stalls, when frames pass without a packet (then it continues the flight under gravity).
TEST(two_player_improved_jump_seen_by_the_other_game) {
    const fs::path dir = game_dir();
    if (!have_game(dir)) return;
    for (const bool latency : {false, true}) {
        Pair p;
        vette::host::DelayedLink::Options lag;
        if (latency) {  // and stalls: frames without a packet in the flight
            lag.delay_ns = 100 * kMs;
            lag.jitter_ns = 50 * kMs;
            lag.stall_ns = 300 * kMs;
            lag.stalls = 3;
        }
        TwoPlayerSetup setup;
        setup.improved_driving = true;
        CHECK(p.boot(dir, setup, lag));
        p.side[0].driving = std::make_unique<Driving>(*p.side[0].m, Driving::Options{true, false});
        p.side[1].driving = std::make_unique<Driving>(*p.side[1].m, Driving::Options{false, false});
        p.side[1].driving->set_options(Driving::Options{setup.improved_driving, false});  // the host's physics
        CHECK(p.run(60, [&] { return !p.racing(); }) && p.racing());
        p.side[1].in = north(0x10E0);  // the guest jumps; the host waits by the road, past the crest
        p.side[0].in = {{60, 0, 0x2D35, 0x395A}, {60, 0, 0x2D57, 1}, {60, 0, 0x2D37, 0x1004}, {60, 0, 0x2D59, 0},
                        {60, 0, 0x2D3B, 90}};
        // The jumper's own frames (x, z, ground), and where the host shows its car each frame.
        struct Point {
            double x, z, ground;
            bool airborne;
        };
        std::vector<Point> own, shown;
        Driving& jumper = *p.side[1].driving;
        Side& host = p.side[0];
        jumper.on_frame = [&](const Driving::Telemetry& tm) {
            if (p.side[1].t() >= 70) own.push_back({tm.x, tm.z, tm.ground_z, tm.airborne});
        };
        host.m->cpu().add_watch(Cpu::linear(kCode, 0x026A), [&](Cpu&) {  // after opponent_step
            if (p.side[1].t() >= 70) shown.push_back({static_cast<double>(host.abs_x(0x2F09)), static_cast<double>(host.w(0x2F0D)), 0, false});
        });
        CHECK(p.run(95, [&] { return p.side[1].t() < 90; }));
        // The flight, along x (north): where the jumper was in the air, and the ground under each point.
        double lo = 1e9, hi = -1e9, max_own = 0;
        for (const Point& q : own) {
            if (q.airborne) {
                lo = std::min(lo, q.x);
                hi = std::max(hi, q.x);
                max_own = std::max(max_own, q.z - q.ground);
            }
        }
        const auto ground_at = [&](double x) {
            const Point* best = &own.front();
            for (const Point& q : own) {
                if (std::fabs(q.x - x) < std::fabs(best->x - x)) best = &q;
            }
            return best->ground;
        };
        // Wherever the host shows the car over that stretch, it shows it in the air.
        int over = 0, in_air = 0, on_road = 0;
        double max_shown = 0;
        for (const Point& q : shown) {
            if (q.x > lo + 40 && q.x < hi - 40) {
                ++over;
                in_air += q.z > ground_at(q.x) + 4;
                on_road += std::fabs(q.z - ground_at(q.x)) <= 1;
                max_shown = std::max(max_shown, q.z - ground_at(q.x));
            }
        }
        const auto& remote = host.driving->remote();
        std::printf("  %s: %d jump(s), up to %.0f units over the ground (x %.0f-%.0f); the host showed the car over that "
                    "stretch %d times: %d in the air (up to %.0f), %d on the road; %d packets said airborne, %d frames "
                    "continued under gravity\n",
                    latency ? "100-150 ms, stalls" : "no latency", jumper.telemetry().jumps, max_own, lo, hi, over, in_air,
                    max_shown, on_road, remote.packets_airborne, remote.extrapolated_total);
        CHECK_EQ(jumper.telemetry().jumps, 1);
        CHECK(max_own >= 8);
        CHECK(over >= 3);
        CHECK_EQ(in_air, over);
        CHECK_EQ(on_road, 0);
        CHECK(remote.packets_airborne >= 3);
        if (latency) {
            CHECK(remote.extrapolated_total >= 2);
        }
    }
}
