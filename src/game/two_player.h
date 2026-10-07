#pragma once
// The original's two-player game over the serial link (re/notes/12-two-player.md), and getting two
// hosted games into a race together.
//
//  - LinkFrame / LinkFrameParser: the original's packets as they go over the cable (for logs, tests and
//    tools; the games themselves exchange them unchanged).
//  - TwoPlayerSetup: what both games must agree on before a race (the course, and the host's driving
//    physics), as a short text the transport carries from the host to the guest.
//  - TwoPlayerStart: drives the original's own menus with keys, as a player would (title, car, level,
//    opponent, course, then Communications > Two players > Direct, 57.6k, DONE? Yes, which waits for the
//    other game), and starts the race.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "host/machine.h"

namespace vette::game {

// --- The cable's packets -------------------------------------------------------------------------------
// On the line: 'I' 'D' 'N', a checksum, then `len` bytes starting with the little-endian word `len`
// itself. The checksum is the 8-bit sum of those `len` bytes (serial_send_packet 422F:01C8).
struct LinkFrame {
    enum class Kind { Handshake, City, Highway, Status, Other };
    uint16_t len = 0;
    uint8_t checksum = 0;
    bool checksum_ok = false;
    std::vector<uint8_t> body;  // the `len` bytes, from the length word on

    uint16_t word(size_t off) const {
        return off + 1 < body.size() ? static_cast<uint16_t>(body[off] | body[off + 1] << 8) : 0;
    }
    uint16_t status() const { return word(2); }  // lo: the sender's DS:2 (0 racing, 4 finished, ...)
    Kind kind() const;

    // City (len 32h): the sender's car struct DS:2D35 +0..+2Dh from body offset 4 (notes 04 section 2).
    int16_t car(size_t field) const { return static_cast<int16_t>(word(4 + field)); }
    static constexpr size_t kX = 0x00, kY = 0x02, kZ = 0x04, kHeading = 0x06, kPitch = 0x08, kFlight = 0x0C,
                            kSpeed = 0x0E, kGear = 0x10, kGround = 0x1E, kRow = 0x22, kCol = 0x24;
    // Absolute position of a city packet's car (big tile * 8000h + local).
    int32_t abs_x() const { return car(kRow) * 0x8000 + static_cast<uint16_t>(car(kX)); }
    int32_t abs_y() const { return car(kCol) * 0x8000 + static_cast<uint16_t>(car(kY)); }

    std::string describe() const;  // one line for logs
};

// Splits a byte stream into frames as the original's receive interrupt does (422F:025F): a frame starts
// at "IDN" and is `len + 4` bytes long; lengths over 100h restart the search.
class LinkFrameParser {
public:
    // `on_other` (optional) gets the bytes that turn out not to be part of a frame, in order.
    void feed(std::span<const uint8_t> bytes, const std::function<void(const LinkFrame&)>& on_frame,
              const std::function<void(std::span<const uint8_t>)>& on_other = {});
    uint64_t skipped() const { return skipped_; }  // bytes outside frames

private:
    std::vector<uint8_t> buf_;  // the frame being collected, from 'I'
    uint64_t skipped_ = 0;
};

// Makes sure the other game's packets that change something reach this game. The original's receiver
// keeps the first packet that arrives and drops the ones after it until the game has taken it (once a
// frame, opponent_step 3009:0F41 -> 422F:0176). Two packets within one frame (two PCs never run at the
// same frame rate, and a network delivers in bursts) lose the newer one: about 1 in 20 at equal speeds.
// For a position that hardly matters, the next one follows; but the packet saying the other player
// finished is sent once (the race ends there), and if it is lost this game never learns it, and both
// players see themselves win. The pacer sits between the link and the UART: packets go through as they
// arrive (the newest data, as the original), except one whose status differs from the last one passed
// (finished, paused, wrecked, on or off the freeway, a handshake): that one waits until no packet is on
// its way in and the game's receive slot is empty, so the game takes it; newer packets replace it while
// it waits. Bytes outside packets pass straight through. The game itself is unchanged.
class LinkPacer final : public host::SerialLink {
public:
    LinkPacer(host::Machine& machine, host::SerialLink& inner);
    ~LinkPacer() override;
    LinkPacer(const LinkPacer&) = delete;
    LinkPacer& operator=(const LinkPacer&) = delete;

    void send(std::span<const uint8_t> bytes) override { inner_.send(bytes); }
    size_t receive(std::span<uint8_t> out) override;
    bool connected() const override { return inner_.connected(); }

    struct Stats {
        uint64_t packets = 0;     // complete packets from the link
        uint64_t held = 0;        // status changes held for a free slot
        uint64_t superseded = 0;  // held, then replaced by a newer packet
        uint64_t released = 0;    // held packets passed on
        uint64_t taken = 0;       // packets the game took (serial_recv_packet 422F:01A0)
    };
    const Stats& stats() const { return stats_; }

private:
    host::Machine& machine_;
    host::SerialLink& inner_;
    std::vector<host::Cpu::WatchId> watches_;
    LinkFrameParser parser_;
    std::vector<uint8_t> held_;  // a status change waiting for the slot, as on the line
    std::vector<uint8_t> out_;   // passed bytes the UART hasn't taken yet
    size_t out_pos_ = 0;
    uint16_t status_ = 0;        // the status word of the last packet passed
    uint64_t held_since_ = 0;
    Stats stats_;
};

// --- What the two games agree on -----------------------------------------------------------------------
// The original exchanges nothing before a race: each player picks a car, a level, the opponent's look and
// a course, and the manual says to agree on the course beforehand. Here the host's choices are sent to
// the guest. Each player's car, level and the other car's look stay their own (TwoPlayerStart::Own).
struct TwoPlayerSetup {
    int course = 1;                 // 1-3, or 4: the three in a row
    bool improved_driving = false;  // both games drive with the host's physics (game/driving.h)

    // "vette2p/1 course=1 improved=0": short, printable, versioned. decode() refuses other versions.
    std::string encode() const;
    static std::optional<TwoPlayerSetup> decode(std::string_view text);
};

// --- Into a race ---------------------------------------------------------------------------------------
// The setup screen's saved choices (CONFIG.BIN, notes 12: ten words, each row's item offset, then the dial
// text) with the link this program makes already chosen: Direct, COM1, 57.6k (the copy DOS 1.1 shipped
// with says a modem's COM2 and 9600). The other rows as in `config`; empty if `config` is too short.
std::vector<uint8_t> link_config(std::span<const uint8_t> config);

class TwoPlayerStart {
public:
    enum class Role { Host, Guest };  // who chose the setup (the original itself has no such roles)
    struct Own {
        int car = 0;       // 0 Stock, 1 ZR1, 2 Twin Turbo, 3 Sledgehammer
        int level = 0;     // 0 Trainee, 1 Rookie, 2 Pro
        int opponent = 0;  // how the other player's car looks here: 0 Porsche, 1 Lamborghini, 2 Testarossa, 3 F40
    };
    enum class Phase {
        Title,      // waiting for the title screen
        Garage,     // choosing the car
        Level,
        Opponent,
        Course,     // choosing the course, then the Communications menu
        Setup,      // the two-player setup screen
        Linking,    // DONE? Yes: the game waits for the other one (no timeout in the original)
        Starting,   // back on the course map with two players on: Enter
        Racing,     // the race has begun
        Failed,
    };

    // Attach the link (Machine::attach_serial) before the game reaches DONE?. The machine must have
    // booted; the game may be anywhere from its start to the course map (not in a race). Watches are
    // installed on the original's menu loops; keys are injected from poll(). With the manual check not
    // skipped (game/options.h), it is answered (any answer is accepted).
    TwoPlayerStart(host::Machine& machine, Role role, TwoPlayerSetup setup, Own own);
    ~TwoPlayerStart();
    TwoPlayerStart(const TwoPlayerStart&) = delete;
    TwoPlayerStart& operator=(const TwoPlayerStart&) = delete;

    // Call between emulation slices (after Machine::run_for). Does nothing once racing or failed.
    void poll();

    Phase phase() const { return phase_; }
    bool racing() const { return phase_ == Phase::Racing; }
    bool failed() const { return phase_ == Phase::Failed; }
    const std::string& error() const { return error_; }
    static const char* phase_name(Phase p);
    // The original's role, once linked (cs:1): true for the game that takes the second start position
    // (the higher random number in the direct-connection handshake, 422F:0424).
    bool second_slot() const;

    // A line per step for logs (optional).
    std::function<void(const std::string&)> on_log;

private:
    enum Screen : int { kTitle, kGarage, kLevel, kOpponent, kCourse, kSetup, kHandshake, kRace, kScreens };
    void tap(uint8_t scancode);
    void fail(const std::string& why);
    void enter(Phase p);
    void log(const std::string& line);
    bool waiting_on(Screen s) const;  // the game's key loop for `s` ran since the last key
    uint64_t now() const { return machine_.emulated_ns(); }

    host::Machine& machine_;
    Role role_;
    TwoPlayerSetup setup_;
    Own own_;
    Phase phase_ = Phase::Title;
    std::string error_;
    std::vector<host::Cpu::WatchId> watches_;
    uint64_t seen_[kScreens] = {};  // emulated ns each screen's key loop last ran (0: not yet)
    uint64_t key_at_ = 0;           // when the last key went in
    uint64_t phase_at_ = 0;
    int setup_row_ = -1;            // setup screen: the row being set
    int presses_ = 0;               // keys pressed in this phase
    int menu_keys_ = 0;             // keys left to press in the options menu
    bool quiz_ = false;             // the manual check came up
    std::vector<uint8_t> releases_;  // keys to release on the next poll
    uint64_t release_at_ = 0;
};

} // namespace vette::game
