// Two hosted games side by side, linked by an in-memory serial cable (host/loopback_link.h): the
// original's two-player race without a network. Both games are driven into the race through their own
// menus (game/two_player.h, TwoPlayerStart), then scripted keys and pokes play it; screenshots are taken
// of both screens, and the packets on the cable and both cars can be logged.
//
//   vette_link --game Game --seconds 60 --course 1 --shot 10 --log-frames --out shots/
//
// Times (keys, holds, pokes, shots) are seconds after that side's race start ("racing": the frame loop
// running); shots are taken on both sides at side A's time. Side A is the host.
//
// --game DIR        the DOS game folder (default Game)
// --seconds N       stop N emulated seconds after the race start (default 30)
// --out DIR         screenshots (shot_a_T.bmp, shot_b_T.bmp) and the games' saves
// --cpu-hz N        emulated CPU clock (default 12000000, a 286; 140000000: the app's fast PC)
// --course N        1-3, or 4 (the three in a row); the host's choice, sent to the guest
// --driving M       original (default) or improved: the host's driving physics, for both games
// --car S:N --level S:N --opponent S:N   side S's own choices (S = a or b; defaults 0)
// --key S:T:SC      press scan code SC (hex) on side S at second T for 100 ms
// --hold S:A:B:SC   hold SC on side S from second A to B
// --poke S:T:O:V    write word V (hex) to DS:O (hex) on side S at second T
// --shot T          screenshot both sides at second T
// --finish S:T      side S crosses the finish line at second T (as the finish boxes do, 3009:1AFF:
//                   cs:3 = FFh ends the race, DS:2 = 4 goes out in the frame's packet)
// --delay MS --jitter MS   one-way latency of the cable, each way: MS plus a random 0..jitter per chunk,
//                   in order (so late chunks bunch the ones behind them into bursts)
// --stall MS --stalls N    also stop the line for up to MS, N times a second on average
// --offset MS       start side B this much later (default 7: the two PCs aren't in lockstep)
// --raw             no LinkPacer (game/two_player.h): the bytes go to the UART as they arrive, and the
//                   games drop packets as the original does
// --manual-check    don't skip the manual-lookup question (TwoPlayerStart answers it)
// --log-frames      print every packet on the cable, as it is sent
// --log-cars        print both cars on both sides every race frame
// --summary         at the end: packets each way and how many the games took, how far the other car is
//                   shown behind where it is

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "game/driving.h"
#include "game/options.h"
#include "game/two_player.h"
#include "game/x86.h"
#include "host/loopback_link.h"
#include "host/machine.h"

namespace {

using vette::game::TwoPlayerStart;
using vette::host::Cpu;
using vette::host::Ega;
using vette::host::Machine;

constexpr uint64_t kMs = 1'000'000;

void write_u16(std::ofstream& f, uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); }
void write_u32(std::ofstream& f, uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); }

bool save_bmp(const std::filesystem::path& path, const Ega::Frame& frame) {
    if (frame.width == 0) {
        return false;
    }
    const auto w = static_cast<uint32_t>(frame.width), h = static_cast<uint32_t>(frame.height);
    const uint32_t stride = (w + 3) & ~3u;
    const uint32_t data_off = 14 + 40 + 256 * 4;
    std::ofstream f(path, std::ios::binary);
    f.write("BM", 2);
    write_u32(f, data_off + stride * h);
    write_u32(f, 0);
    write_u32(f, data_off);
    write_u32(f, 40);
    write_u32(f, w);
    write_u32(f, h);
    write_u16(f, 1);
    write_u16(f, 8);
    write_u32(f, 0);
    write_u32(f, stride * h);
    write_u32(f, 2835);
    write_u32(f, 2835);
    write_u32(f, 256);
    write_u32(f, 0);
    for (uint32_t i = 0; i < 256; ++i) {
        write_u32(f, (i < 16 ? frame.palette[i] : 0) & 0xFFFFFF);
    }
    std::vector<char> row(stride, 0);
    for (uint32_t y = h; y-- > 0;) {
        std::copy_n(frame.pixels.begin() + static_cast<std::ptrdiff_t>(y * w), w, row.begin());
        f.write(row.data(), stride);
    }
    return static_cast<bool>(f);
}

struct Event {
    uint64_t at_ms;
    uint8_t scancode = 0;
    uint16_t off = 0, value = 0;
    bool poke = false;
    bool finish = false;
};

// The cable's end as one game sees it, with the frames it sends logged.
class TapLink final : public vette::host::SerialLink {
public:
    TapLink(vette::host::SerialLink& inner, std::string name, const Machine& machine, const uint64_t& race_ns, bool log)
        : inner_(inner), name_(std::move(name)), machine_(machine), race_ns_(race_ns), log_(log) {}
    void send(std::span<const uint8_t> bytes) override {
        sent_bytes += bytes.size();
        parser_.feed(bytes, [&](const vette::game::LinkFrame& f) {
            ++frames;
            if (f.kind() == vette::game::LinkFrame::Kind::City) ++city;
            if (log_) {
                // Seconds after the sender's race start (before it: negative).
                const double t = (static_cast<double>(machine_.emulated_ns()) - static_cast<double>(race_ns_)) / 1e9;
                std::printf("frame t=%8.3fs %s %s\n", race_ns_ ? t : -1.0, name_.c_str(), f.describe().c_str());
            }
        });
        inner_.send(bytes);
    }
    size_t receive(std::span<uint8_t> out) override {
        const size_t n = inner_.receive(out);
        received_bytes += n;
        return n;
    }
    bool connected() const override { return inner_.connected(); }

    uint64_t sent_bytes = 0, received_bytes = 0, frames = 0, city = 0;

private:
    vette::host::SerialLink& inner_;
    std::string name_;
    const Machine& machine_;
    const uint64_t& race_ns_;
    bool log_;
    vette::game::LinkFrameParser parser_;
};

struct Side {
    std::string name;
    std::unique_ptr<Machine> machine;
    std::unique_ptr<vette::host::DelayedLink> delayed;
    std::unique_ptr<TapLink> tap;
    std::unique_ptr<vette::game::LinkPacer> pacer;
    std::unique_ptr<TwoPlayerStart> start;
    std::unique_ptr<vette::game::Driving> driving;
    TwoPlayerStart::Own own;
    std::vector<Event> events;
    size_t next_event = 0;
    uint64_t race_ns = 0;  // emulated time the race started (0: not yet)
    uint64_t packets_seen = 0;  // race frames with a packet from the other side
    uint64_t taken = 0;         // packets the game took (serial_recv_packet)
    double lag_sum = 0;   // the remote car's distance behind the other side's own car, summed per frame
    uint64_t lag_n = 0;
    bool finish_armed = false;
};

int usage() {
    std::fprintf(stderr, "usage: vette_link --game <dir> [--seconds N] [--out dir] [--cpu-hz N] [--course N]\n"
                         "       [--driving original|improved] [--car S:N] [--level S:N] [--opponent S:N]\n"
                         "       [--key S:T:SC]... [--hold S:A:B:SC]... [--poke S:T:O:V]... [--shot T]...\n"
                         "       [--delay MS] [--jitter MS] [--stall MS] [--stalls N] [--offset MS]\n"
                         "       [--raw] [--manual-check] [--log-frames] [--log-cars] [--summary]\n");
    return 2;
}

int16_t s16(uint16_t v) { return static_cast<int16_t>(v); }

} // namespace

int main(int argc, char* argv[]) {
    vette::host::MachineConfig config;
    config.game_dir = "Game";
    config.start_time = vette::host::RealTime{1989, 10, 23, 12, 0, 0, 0};
    double seconds = 30;
    std::filesystem::path out_dir = ".";
    vette::game::TwoPlayerSetup setup;
    std::vector<double> shots;
    vette::host::DelayedLink::Options lag;
    uint64_t offset_ms = 7;
    bool log_frames = false, log_cars = false, summary = false, raw = false, manual_check = false;
    Side side[2];
    side[0].name = "A";
    side[1].name = "B";

    const auto side_of = [&](const std::string& v) -> Side* {
        if (v.size() < 2 || v[1] != ':' || (v[0] != 'a' && v[0] != 'b')) return nullptr;
        return &side[v[0] == 'a' ? 0 : 1];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        if (a == "--game" && has_value) {
            config.game_dir = argv[++i];
        } else if (a == "--seconds" && has_value) {
            seconds = std::atof(argv[++i]);
        } else if (a == "--out" && has_value) {
            out_dir = argv[++i];
        } else if (a == "--cpu-hz" && has_value) {
            config.cpu_hz = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--course" && has_value) {
            setup.course = std::atoi(argv[++i]);
        } else if (a == "--driving" && has_value) {
            setup.improved_driving = std::string(argv[++i]) == "improved";
        } else if ((a == "--car" || a == "--level" || a == "--opponent") && has_value) {
            const std::string v = argv[++i];
            Side* s = side_of(v);
            if (!s) return usage();
            const int n = std::atoi(v.c_str() + 2);
            (a == "--car" ? s->own.car : a == "--level" ? s->own.level : s->own.opponent) = n;
        } else if ((a == "--key" || a == "--hold" || a == "--poke") && has_value) {
            const std::string v = argv[++i];
            Side* s = side_of(v);
            if (!s) return usage();
            std::vector<std::string> f;
            for (size_t p = 2, q; p <= v.size(); p = q + 1) {
                q = std::min(v.find(':', p), v.size());
                f.push_back(v.substr(p, q - p));
            }
            const auto ms = [](const std::string& t) { return static_cast<uint64_t>(std::atof(t.c_str()) * 1000); };
            const auto hex = [](const std::string& t) { return static_cast<uint16_t>(std::strtoul(t.c_str(), nullptr, 16)); };
            if (a == "--key" && f.size() == 2) {
                s->events.push_back({ms(f[0]), static_cast<uint8_t>(hex(f[1]))});
                s->events.push_back({ms(f[0]) + 100, static_cast<uint8_t>(hex(f[1]) | 0x80)});
            } else if (a == "--hold" && f.size() == 3) {
                s->events.push_back({ms(f[0]), static_cast<uint8_t>(hex(f[2]))});
                s->events.push_back({ms(f[1]), static_cast<uint8_t>(hex(f[2]) | 0x80)});
            } else if (a == "--poke" && f.size() == 3) {
                s->events.push_back({ms(f[0]), 0, hex(f[1]), hex(f[2]), true});
            } else {
                return usage();
            }
        } else if (a == "--finish" && has_value) {
            const std::string v = argv[++i];
            Side* s = side_of(v);
            if (!s) return usage();
            s->events.push_back({static_cast<uint64_t>(std::atof(v.c_str() + 2) * 1000), 0, 0, 0, false, true});
        } else if (a == "--shot" && has_value) {
            shots.push_back(std::atof(argv[++i]));
        } else if (a == "--delay" && has_value) {
            lag.delay_ns = static_cast<uint64_t>(std::atof(argv[++i]) * 1e6);
        } else if (a == "--jitter" && has_value) {
            lag.jitter_ns = static_cast<uint64_t>(std::atof(argv[++i]) * 1e6);
        } else if (a == "--stall" && has_value) {
            lag.stall_ns = static_cast<uint64_t>(std::atof(argv[++i]) * 1e6);
        } else if (a == "--stalls" && has_value) {
            lag.stalls = std::atof(argv[++i]);
        } else if (a == "--offset" && has_value) {
            offset_ms = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--log-frames") {
            log_frames = true;
        } else if (a == "--log-cars") {
            log_cars = true;
        } else if (a == "--summary") {
            summary = true;
        } else if (a == "--raw") {
            raw = true;
        } else if (a == "--manual-check") {
            manual_check = true;
        } else {
            return usage();
        }
    }
    std::sort(shots.begin(), shots.end());
    std::filesystem::create_directories(out_dir);

    vette::host::LoopbackCable cable;
    const Machine* clock = nullptr;  // the cable's time: side A's emulated time
    for (int k = 0; k < 2; ++k) {
        Side& s = side[k];
        vette::host::MachineConfig c = config;
        c.save_dir = out_dir / ("save_" + std::string(k ? "b" : "a"));
        s.machine = std::make_unique<Machine>(c);
        s.machine->set_log([&s](const std::string& msg) {
            if (msg.find("unhandled") != std::string::npos) std::printf("[%s %s]\n", s.name.c_str(), msg.c_str());
        });
        std::string error;
        if (!s.machine->boot(error)) {
            std::fprintf(stderr, "boot failed: %s\n", error.c_str());
            return 1;
        }
        if (!manual_check) {
            vette::game::install_skip_manual_check(s.machine->cpu());
        }
        lag.seed = 1234u + static_cast<uint32_t>(k);
        s.delayed = std::make_unique<vette::host::DelayedLink>(
            cable.end(k), [&clock] { return clock->emulated_ns(); }, lag);
        s.tap = std::make_unique<TapLink>(*s.delayed, k ? "B>A" : "A>B", *s.machine, s.race_ns, log_frames);
        if (raw) {
            s.machine->attach_serial(s.tap.get());
        } else {
            s.pacer = std::make_unique<vette::game::LinkPacer>(*s.machine, *s.tap);
            s.machine->attach_serial(s.pacer.get());
        }
        s.machine->cpu().add_watch(Cpu::linear(vette::game::emu_seg(0x422F), 0x01A0), [&s](Cpu&) { ++s.taken; });
        s.start = std::make_unique<TwoPlayerStart>(*s.machine, k ? TwoPlayerStart::Role::Guest : TwoPlayerStart::Role::Host,
                                                   setup, s.own);
        s.start->on_log = [&s](const std::string& line) { std::printf("[%s] %s\n", s.name.c_str(), line.c_str()); };
        if (setup.improved_driving) {
            s.driving = std::make_unique<vette::game::Driving>(*s.machine, vette::game::Driving::Options{true, false});
        }
        std::stable_sort(s.events.begin(), s.events.end(), [](const Event& x, const Event& y) { return x.at_ms < y.at_ms; });
        Machine& m = *s.machine;
        // --finish: right after the player step (3009:020D), where a finish box would have set them, so the
        // frame's packet (023F) says so.
        m.cpu().add_watch(Cpu::linear(vette::game::emu_seg(0x3009), 0x020D), [&s](Cpu&) {
            if (s.finish_armed) {
                s.finish_armed = false;
                vette::game::wr8(s.machine->memory(), vette::game::emu_seg(0x3009), 0x0003, 0xFF);
                vette::game::wr8(s.machine->memory(), vette::game::kDataSeg, 0x0002, 4);
            }
        });
        // After opponent_step (3009:0267 -> 026A): this frame's cars.
        m.cpu().add_watch(Cpu::linear(vette::game::emu_seg(0x3009), 0x026A), [&s, &side, k, log_cars](Cpu&) {
            if (!s.race_ns) return;
            vette::host::Memory& mem = s.machine->memory();
            const auto w = [&mem](uint16_t off) { return s16(vette::game::rd16(mem, vette::game::kDataSeg, off)); };
            const auto ax = [&](uint16_t base) { return static_cast<long>(w(base + 0x22)) * 0x8000 + static_cast<uint16_t>(w(base)); };
            const auto ay = [&](uint16_t base) { return static_cast<long>(w(base + 0x24)) * 0x8000 + static_cast<uint16_t>(w(base + 2)); };
            const int age = vette::game::rd8(mem, vette::game::kDataSeg, 0x2B01);
            // How far the other car, as shown here, is behind where it really is on its own side.
            Side& o = side[1 - k];
            if (o.race_ns) {
                if (age == 0) ++s.packets_seen;
                vette::host::Memory& om = o.machine->memory();
                const auto ow = [&om](uint16_t off) { return s16(vette::game::rd16(om, vette::game::kDataSeg, off)); };
                const double dx = static_cast<double>(static_cast<long>(ow(0x2D57)) * 0x8000 + static_cast<uint16_t>(ow(0x2D35)) - ax(0x2F09));
                const double dy = static_cast<double>(static_cast<long>(ow(0x2D59)) * 0x8000 + static_cast<uint16_t>(ow(0x2D37)) - ay(0x2F09));
                s.lag_sum += std::hypot(dx, dy);
                ++s.lag_n;
            }
            if (log_cars) {
                std::printf("cars t=%8.3fs %s fr %2d me x %6ld y %6ld z %4d hd %3d sp %4d | other x %6ld y %6ld z %4d hd %3d pitch %+3d "
                            "sp %4d flight %04X age %3d status %d\n",
                            static_cast<double>(s.machine->emulated_ns() - s.race_ns) / 1e9, s.name.c_str(), w(0x2CD3),
                            ax(0x2D35), ay(0x2D35), w(0x2D39), w(0x2D3B), w(0x2D43), ax(0x2F09), ay(0x2F09), w(0x2F0D),
                            w(0x2F0F), w(0x2F11), w(0x2F17), static_cast<uint16_t>(w(0x2F15)), age,
                            vette::game::rd8(mem, vette::game::kDataSeg, 0x0003));
            }
        });
    }
    clock = side[0].machine.get();

    // Side B starts a little later: two PCs are never in lockstep (and the original's handshake would
    // draw the same random numbers forever on two identical machines).
    for (uint64_t ms = 0; ms < offset_ms; ++ms) {
        side[1].machine->run_for(kMs);
    }
    size_t next_shot = 0;
    const uint64_t limit_ms = static_cast<uint64_t>(seconds * 1000) + 240'000;  // the menus take a while
    for (uint64_t ms = 0; ms < limit_ms; ++ms) {
        bool done = false;
        for (Side& s : side) {
            Machine& m = *s.machine;
            m.run_for(kMs);
            s.start->poll();
            if (s.start->failed()) {
                std::fprintf(stderr, "side %s: %s\n", s.name.c_str(), s.start->error().c_str());
                return 1;
            }
            if (!s.race_ns && s.start->racing()) {
                s.race_ns = m.emulated_ns();
                std::printf("[%s] race start at %.3f s (second start position: %s)\n", s.name.c_str(),
                            static_cast<double>(s.race_ns) / 1e9, s.start->second_slot() ? "yes" : "no");
            }
            if (s.race_ns) {
                const uint64_t t_ms = (m.emulated_ns() - s.race_ns) / kMs;
                for (; s.next_event < s.events.size() && s.events[s.next_event].at_ms <= t_ms; ++s.next_event) {
                    const Event& e = s.events[s.next_event];
                    if (e.finish) {
                        s.finish_armed = true;  // at the next player step (below)
                    } else if (e.poke) {
                        vette::game::wr16(m.memory(), vette::game::kDataSeg, e.off, e.value);
                    } else {
                        m.key(e.scancode);
                    }
                }
            }
            if (m.stopped()) {
                std::fprintf(stderr, "side %s stopped: %s\n", s.name.c_str(), m.fault().c_str());
                return 3;
            }
        }
        Side& a = side[0];
        if (a.race_ns) {
            const double t = static_cast<double>(a.machine->emulated_ns() - a.race_ns) / 1e9;
            while (next_shot < shots.size() && shots[next_shot] <= t) {
                for (Side& s : side) {
                    Ega::Frame frame;
                    s.machine->render(frame);
                    char name[64];
                    std::snprintf(name, sizeof name, "shot_%c_%06.2f.bmp", s.name[0] == 'A' ? 'a' : 'b', shots[next_shot]);
                    save_bmp(out_dir / name, frame);
                }
                std::printf("t=%.2fs shots saved\n", shots[next_shot]);
                ++next_shot;
            }
            done = t >= seconds;
        }
        if (done) break;
    }
    for (Side& s : side) {
        vette::host::Memory& mem = s.machine->memory();
        std::printf("side %s: knows the other player finished: %s (DS:FA45 %02X, other's status DS:3 %d)\n",
                    s.name.c_str(), vette::game::rd8(mem, vette::game::kDataSeg, 0xFA45) == 0xFF ? "yes" : "no",
                    vette::game::rd8(mem, vette::game::kDataSeg, 0xFA45), vette::game::rd8(mem, vette::game::kDataSeg, 0x0003));
        const auto& u0 = s.machine->uart(0);
        const auto& u1 = s.machine->uart(1);
        std::printf("side %s: %s; sent %llu bytes (%llu frames, %llu city), received %llu; UART COM1 %llu/%llu COM2 %llu/%llu\n",
                    s.name.c_str(), TwoPlayerStart::phase_name(s.start->phase()),
                    static_cast<unsigned long long>(s.tap->sent_bytes), static_cast<unsigned long long>(s.tap->frames),
                    static_cast<unsigned long long>(s.tap->city), static_cast<unsigned long long>(s.tap->received_bytes),
                    static_cast<unsigned long long>(u0.stats().sent), static_cast<unsigned long long>(u0.stats().received),
                    static_cast<unsigned long long>(u1.stats().sent), static_cast<unsigned long long>(u1.stats().received));
        if (summary) {
            const Side& o = side[&s == &side[0] ? 1 : 0];
            std::printf("side %s: took %llu of the %llu packets the other side sent; race frames with a new packet %llu of %llu; "
                        "the other car shown %.0f units behind where it is, on average\n",
                        s.name.c_str(), static_cast<unsigned long long>(s.taken), static_cast<unsigned long long>(o.tap->frames),
                        static_cast<unsigned long long>(s.packets_seen), static_cast<unsigned long long>(s.lag_n),
                        s.lag_n ? s.lag_sum / static_cast<double>(s.lag_n) : 0.0);
            if (s.pacer) {
                const auto& ps = s.pacer->stats();
                std::printf("side %s: pacer: %llu packets in, %llu status changes held for a free slot (%llu replaced by newer)\n",
                            s.name.c_str(), static_cast<unsigned long long>(ps.packets),
                            static_cast<unsigned long long>(ps.held), static_cast<unsigned long long>(ps.superseded));
            }
        }
    }
    return side[0].race_ns && side[1].race_ns ? 0 : 1;
}
