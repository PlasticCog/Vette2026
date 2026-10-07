#include "game/two_player.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <utility>

#include "game/x86.h"

namespace vette::game {
namespace {

constexpr uint16_t kCode = emu_seg(0x3009);
constexpr uint16_t kSerialCode = emu_seg(0x422F);
constexpr uint16_t kText = emu_seg(0x0ACB);  // the menus' data segment
constexpr uint16_t kQuizSeg = emu_seg(0x4160);

// The original's key loops, one per screen (each runs while the screen waits for a key).
constexpr uint16_t kTitleLoop = 0xC57E;     // title_screen: the animation's key check
constexpr uint16_t kGarageLoop = 0xF74B;    // garage_screen (8A22) -> F742: the car-select key wait
constexpr uint16_t kLevelLoop = 0xDF3B;     // the TRAINEE / ROOKIE / PRO plates
constexpr uint16_t kOpponentLoop = 0xE2A6;  // the opponents (E074 -> E2A0)
constexpr uint16_t kCourseLoop = 0xF7F8;    // the course map (FBCC -> FC09)
constexpr uint16_t kSetupLoop = 0xFA9F;     // the two-player setup screen (ED5C -> EDE1 -> FA96)
constexpr uint16_t kHandshake1 = 0x0440, kHandshake2 = 0x0465;  // 422F:0424: waiting for the other game
constexpr uint16_t kFrameLoop = 0x0135;     // the race's frame loop
constexpr uint16_t kQuiz = 0x0A40;          // manual_quiz (4160), when the manual check isn't skipped

// State the steps read.
constexpr uint16_t kCar = 0x8DAE;           // cs: the car, 0-3
constexpr uint16_t kLevelHighlight = 0xF4DA;  // cs: b, the highlighted plate 0-2
constexpr uint16_t kOpponentSel = 0xFA6C;   // DS: opponent * 2
constexpr uint16_t kCourseSel = 0xFD0E;     // DS: course 1-4
constexpr uint16_t kTwoPlayers = 0x0002;    // cs: b, FFh with two players on
constexpr uint16_t kSlot = 0x0001;          // cs: b, FFh: this game takes the second start position
constexpr uint16_t kSetupRow = 0x7709;      // 0ACB: w, the setup screen's row * 2
constexpr uint16_t kSetupItems = 0x770B;    // 0ACB: w[10], each row's item offset (4 = the first item)

// The setup screen's rows (re/notes/12-two-player.md): Connection, Port, Baud rate, Mode, Procedure,
// Dial, Line type, Phone, Save options, DONE?. -1: left as it is (saving writes the same link to the save
// folder's CONFIG.BIN: link_config's, the screen's default here).
constexpr int kSetupRows = 10;
constexpr int kItemCount[kSetupRows] = {2, 2, 7, 2, 2, 2, 2, 2, 2, 2};
constexpr int kWanted[kSetupRows] = {0 /*Direct*/, 0 /*COM1*/, 6 /*57.6k*/, -1, -1, -1, -1, -1, -1, 0 /*Yes*/};

// Scan codes (set 1).
constexpr uint8_t kEsc = 0x01, kEnter = 0x1C, kSpace = 0x39, kDown = 0x50, kLeft = 0x4B, kRight = 0x4D;

constexpr uint64_t kMs = 1'000'000;
constexpr uint64_t kKeyHold = 100 * kMs;    // a key is released this long after it's pressed
constexpr uint64_t kSettle = 150 * kMs;     // after a key, the screen's loop must run again this much later
constexpr uint64_t kMenuStep = 400 * kMs;   // the options menu has no loop watched: keys this far apart
constexpr uint64_t kPhaseTimeout = 30'000 * kMs;

const char* kind_name(LinkFrame::Kind k) {
    switch (k) {
    case LinkFrame::Kind::Handshake: return "handshake";
    case LinkFrame::Kind::City: return "city";
    case LinkFrame::Kind::Highway: return "highway";
    case LinkFrame::Kind::Status: return "status";
    default: return "other";
    }
}

} // namespace

// --- Frames --------------------------------------------------------------------------------------------

LinkFrame::Kind LinkFrame::kind() const {
    switch (len) {
    case 3:
    case 4: return Kind::Handshake;
    case 0x32: return Kind::City;
    case 0x16: return Kind::Highway;
    case 8: return Kind::Status;
    default: return Kind::Other;
    }
}

std::string LinkFrame::describe() const {
    char line[200];
    const Kind k = kind();
    if (k == Kind::City) {
        std::snprintf(line, sizeof line, "city   status %04X x %6d y %6d z %4d heading %3d pitch %+3d speed %4d flight %04X%s",
                      status(), abs_x(), abs_y(), car(kZ), car(kHeading), car(kPitch), car(kSpeed),
                      static_cast<uint16_t>(car(kFlight)), checksum_ok ? "" : " BAD CHECKSUM");
    } else if (k == Kind::Highway) {
        std::snprintf(line, sizeof line, "highway road %d segment %d pos %d heading %3d speed %4d%s", word(4), word(6),
                      word(8), static_cast<int16_t>(word(16)), static_cast<int16_t>(word(18)),
                      checksum_ok ? "" : " BAD CHECKSUM");
    } else if (k == Kind::Handshake) {
        std::snprintf(line, sizeof line, "handshake len %d%s%s", len, len == 4 ? " random " : "",
                      len == 4 ? std::to_string(word(2)).c_str() : "");
    } else {
        std::snprintf(line, sizeof line, "%s len %d status %04X%s", kind_name(k), len, status(),
                      checksum_ok ? "" : " BAD CHECKSUM");
    }
    return line;
}

void LinkFrameParser::feed(std::span<const uint8_t> bytes, const std::function<void(const LinkFrame&)>& on_frame,
                           const std::function<void(std::span<const uint8_t>)>& on_other) {
    static constexpr uint8_t kSync[3] = {'I', 'D', 'N'};
    const auto other = [&](std::span<const uint8_t> v) {
        skipped_ += v.size();
        if (on_other && !v.empty()) {
            on_other(v);
        }
    };
    for (const uint8_t b : bytes) {
        if (buf_.size() < 3) {
            if (b == kSync[buf_.size()]) {
                buf_.push_back(b);
            } else {
                other(buf_);
                buf_.clear();
                if (b == 'I') {
                    buf_.push_back(b);
                } else {
                    other(std::span<const uint8_t>(&b, 1));
                }
            }
            continue;
        }
        buf_.push_back(b);
        if (buf_.size() < 6) {
            continue;
        }
        const uint16_t len = static_cast<uint16_t>(buf_[4] | buf_[5] << 8);
        if (len < 2 || len > 0x100) {
            other(buf_);
            buf_.clear();
            continue;
        }
        if (buf_.size() == static_cast<size_t>(len) + 4) {
            LinkFrame f;
            f.len = len;
            f.checksum = buf_[3];
            f.body.assign(buf_.begin() + 4, buf_.end());
            uint8_t sum = 0;
            for (const uint8_t v : f.body) {
                sum = static_cast<uint8_t>(sum + v);
            }
            f.checksum_ok = sum == f.checksum;
            buf_.clear();
            on_frame(f);
        }
    }
}

// --- Pacing --------------------------------------------------------------------------------------------

namespace {
constexpr uint16_t kSerialData = emu_seg(0x2F98);  // the serial library's data
constexpr uint16_t kRecvSlot = 0x000A;     // w: the received packet waiting for the game (0: none)
constexpr uint16_t kRecvIndex = 0x0012;    // w: bytes of the packet being received (0-2: looking for IDN)
constexpr uint16_t kRecvTaken = 0x01A0;    // serial_recv_packet: `mov word [0Ah],0`, a packet taken
constexpr uint64_t kStuckNs = 2'000'000'000;  // a held packet still waiting: let it go anyway
} // namespace

LinkPacer::LinkPacer(host::Machine& machine, host::SerialLink& inner) : machine_(machine), inner_(inner) {
    watches_.push_back(machine_.cpu().add_watch(host::Cpu::linear(kSerialCode, kRecvTaken),
                                                [this](host::Cpu&) { ++stats_.taken; }));
}

LinkPacer::~LinkPacer() {
    for (const host::Cpu::WatchId id : watches_) {
        machine_.cpu().remove_watch(id);
    }
}

size_t LinkPacer::receive(std::span<uint8_t> out) {
    const uint64_t now = machine_.emulated_ns();
    std::array<uint8_t, 512> buf;
    for (size_t n; (n = inner_.receive(buf)) > 0;) {
        parser_.feed(
            std::span<const uint8_t>(buf.data(), n),
            [&](const LinkFrame& f) {
                ++stats_.packets;
                std::vector<uint8_t> line = {'I', 'D', 'N', f.checksum};
                line.insert(line.end(), f.body.begin(), f.body.end());
                if (!held_.empty()) {
                    ++stats_.superseded;  // the newest state goes instead
                    held_ = std::move(line);
                } else if (f.status() != status_) {
                    ++stats_.held;
                    held_ = std::move(line);
                    held_since_ = now;
                } else {
                    out_.insert(out_.end(), line.begin(), line.end());
                }
            },
            [this](std::span<const uint8_t> other) { out_.insert(out_.end(), other.begin(), other.end()); });
    }
    // A held packet once nothing is on its way to the game's receiver and its slot is empty. The UART
    // asks for bytes only when it has none left and RBR has been read, so with everything passed handed
    // over, all that can be in flight is a packet the receive interrupt is still collecting (index 3 or
    // more: past "IDN") or one waiting in the slot.
    if (!held_.empty() && out_pos_ == out_.size()) {
        host::Memory& m = machine_.memory();
        const uint16_t index = rd16(m, kSerialData, kRecvIndex);
        const bool idle = (index < 3 || index > 0x100) && rd16(m, kSerialData, kRecvSlot) == 0;
        if (idle || now - held_since_ > kStuckNs) {
            status_ = held_.size() >= 8 ? static_cast<uint16_t>(held_[6] | held_[7] << 8) : 0;  // (as LinkFrame::status)
            out_.swap(held_);
            out_pos_ = 0;
            held_.clear();
            ++stats_.released;
        }
    }
    const size_t n = std::min(out.size(), out_.size() - out_pos_);
    std::copy_n(out_.begin() + static_cast<std::ptrdiff_t>(out_pos_), n, out.begin());
    out_pos_ += n;
    if (out_pos_ == out_.size()) {
        out_.clear();
        out_pos_ = 0;
    }
    return n;
}

// --- Setup ---------------------------------------------------------------------------------------------

std::string TwoPlayerSetup::encode() const {
    return "vette2p/1 course=" + std::to_string(course) + " improved=" + (improved_driving ? "1" : "0");
}

std::optional<TwoPlayerSetup> TwoPlayerSetup::decode(std::string_view text) {
    constexpr std::string_view kMagic = "vette2p/1";
    if (text.substr(0, kMagic.size()) != kMagic) {
        return std::nullopt;
    }
    TwoPlayerSetup s;
    bool course = false;
    size_t pos = kMagic.size();
    while (pos < text.size()) {
        while (pos < text.size() && text[pos] == ' ') ++pos;
        const size_t end = std::min(text.find(' ', pos), text.size());
        const std::string_view item = text.substr(pos, end - pos);
        pos = end;
        const size_t eq = item.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = item.substr(0, eq), value = item.substr(eq + 1);
        int v = 0;
        if (std::from_chars(value.data(), value.data() + value.size(), v).ec != std::errc{}) {
            return std::nullopt;
        }
        if (key == "course") {
            if (v < 1 || v > 4) return std::nullopt;
            s.course = v;
            course = true;
        } else if (key == "improved") {
            s.improved_driving = v != 0;
        }  // other keys: from a later version, ignored
    }
    if (!course) {
        return std::nullopt;
    }
    return s;
}

// --- Into a race ---------------------------------------------------------------------------------------

const char* TwoPlayerStart::phase_name(Phase p) {
    switch (p) {
    case Phase::Title: return "title";
    case Phase::Garage: return "car";
    case Phase::Level: return "level";
    case Phase::Opponent: return "opponent";
    case Phase::Course: return "course";
    case Phase::Setup: return "two-player setup";
    case Phase::Linking: return "waiting for the other game";
    case Phase::Starting: return "starting";
    case Phase::Racing: return "racing";
    default: return "failed";
    }
}

std::vector<uint8_t> link_config(std::span<const uint8_t> config) {
    if (config.size() < 2 * kSetupRows) {
        return {};
    }
    std::vector<uint8_t> out(config.begin(), config.end());
    for (int row = 0; row < kSetupRows; ++row) {
        if (kWanted[row] >= 0) {
            const int offset = 4 * (kWanted[row] + 1);  // the item's offset in its row record
            out[static_cast<size_t>(2 * row)] = static_cast<uint8_t>(offset);
            out[static_cast<size_t>(2 * row + 1)] = static_cast<uint8_t>(offset >> 8);
        }
    }
    return out;
}

TwoPlayerStart::TwoPlayerStart(host::Machine& machine, Role role, TwoPlayerSetup setup, Own own)
    : machine_(machine), role_(role), setup_(setup), own_(own) {
    using host::Cpu;
    Cpu& cpu = machine_.cpu();
    const auto watch = [&](uint16_t seg, uint16_t off, Screen s) {
        watches_.push_back(cpu.add_watch(Cpu::linear(seg, off), [this, s](Cpu&) { seen_[s] = now(); }));
    };
    watch(kCode, kTitleLoop, kTitle);
    watch(kCode, kGarageLoop, kGarage);
    watch(kCode, kLevelLoop, kLevel);
    watch(kCode, kOpponentLoop, kOpponent);
    watch(kCode, kCourseLoop, kCourse);
    watch(kCode, kSetupLoop, kSetup);
    watch(kSerialCode, kHandshake1, kHandshake);
    watch(kSerialCode, kHandshake2, kHandshake);
    watch(kCode, kFrameLoop, kRace);
    // The manual check (unless skipped): any answer is accepted; Enter gives an empty one.
    watches_.push_back(cpu.add_watch(Cpu::linear(kQuizSeg, kQuiz), [this](Cpu&) { quiz_ = true; }));
    setup_.course = std::clamp(setup_.course, 1, 4);
    own_.car = std::clamp(own_.car, 0, 3);
    own_.level = std::clamp(own_.level, 0, 2);
    own_.opponent = std::clamp(own_.opponent, 0, 3);
    phase_at_ = now();
    log(std::string(role_ == Role::Host ? "host" : "guest") + ": " + setup_.encode() + " car " +
        std::to_string(own_.car) + " level " + std::to_string(own_.level) + " opponent " + std::to_string(own_.opponent));
}

TwoPlayerStart::~TwoPlayerStart() {
    for (const host::Cpu::WatchId id : watches_) {
        machine_.cpu().remove_watch(id);
    }
}

bool TwoPlayerStart::second_slot() const {
    return rd8(machine_.memory(), kCode, kSlot) != 0;
}

void TwoPlayerStart::log(const std::string& line) {
    if (on_log) {
        char t[32];
        std::snprintf(t, sizeof t, "%.2fs ", static_cast<double>(now()) / 1e9);
        on_log(t + line);
    }
}

void TwoPlayerStart::tap(uint8_t scancode) {
    machine_.key(scancode);
    releases_.push_back(static_cast<uint8_t>(scancode | 0x80));
    release_at_ = now() + kKeyHold;
    key_at_ = now();
    ++presses_;
}

void TwoPlayerStart::fail(const std::string& why) {
    error_ = std::string("two-player start: ") + phase_name(phase_) + ": " + why;
    phase_ = Phase::Failed;
    log(error_);
}

void TwoPlayerStart::enter(Phase p) {
    phase_ = p;
    phase_at_ = now();
    presses_ = 0;
    setup_row_ = -1;
    log(std::string("-> ") + phase_name(p));
}

bool TwoPlayerStart::waiting_on(Screen s) const {
    return seen_[s] != 0 && seen_[s] >= key_at_ + kSettle && now() >= key_at_ + kSettle;
}

void TwoPlayerStart::poll() {
    if (!releases_.empty() && now() >= release_at_) {
        for (const uint8_t b : releases_) {
            machine_.key(b);
        }
        releases_.clear();
    }
    if (phase_ == Phase::Racing || phase_ == Phase::Failed || !releases_.empty()) {
        return;
    }
    host::Memory& m = machine_.memory();
    if (phase_ != Phase::Linking && now() - phase_at_ > kPhaseTimeout) {
        fail("the game didn't get there");
        return;
    }
    switch (phase_) {
    case Phase::Title:
        // (Started later on, the game may already be on one of the menus: take it from there.)
        if (waiting_on(kGarage)) {
            enter(Phase::Garage);
        } else if (waiting_on(kLevel)) {
            enter(Phase::Level);
        } else if (waiting_on(kOpponent)) {
            enter(Phase::Opponent);
        } else if (waiting_on(kCourse)) {
            enter(Phase::Course);
        } else if (seen_[kRace] > phase_at_) {
            fail("the race has begun: two players must be switched on before it (the start positions)");
        } else if (waiting_on(kTitle)) {
            tap(kSpace);
        }
        break;
    case Phase::Garage:
        if (waiting_on(kLevel)) {
            enter(Phase::Level);
        } else if (waiting_on(kGarage)) {
            const int car = rd16(m, kCode, kCar);
            tap(car < own_.car ? kRight : car > own_.car ? kLeft : kEnter);
        }
        break;
    case Phase::Level:
        if (waiting_on(kOpponent)) {
            enter(Phase::Opponent);
        } else if (waiting_on(kLevel)) {
            tap(rd8(m, kCode, kLevelHighlight) != own_.level ? kDown : kEnter);
        }
        break;
    case Phase::Opponent:
        if (waiting_on(kCourse)) {
            enter(Phase::Course);
        } else if (waiting_on(kOpponent)) {
            tap(rd16(m, kDataSeg, kOpponentSel) != 2 * own_.opponent ? kRight : kEnter);
        }
        break;
    case Phase::Course:
        // Choose the course, then Esc > Communications > Two players: the setup screen.
        if (waiting_on(kSetup)) {
            enter(Phase::Setup);
        } else if (menu_keys_ > 0) {
            if (now() >= key_at_ + kMenuStep) {
                --menu_keys_;
                tap(menu_keys_ == 0 ? kEnter : kRight);  // Right, Right (Communications), Enter
            }
        } else if (waiting_on(kCourse)) {
            if (rd16(m, kDataSeg, kCourseSel) != setup_.course) {
                tap(kDown);
            } else if (presses_ > 12) {
                fail("the Communications menu didn't open the setup screen");
            } else {
                tap(kEsc);
                menu_keys_ = 3;
            }
        }
        break;
    case Phase::Setup: {
        if (!waiting_on(kSetup)) {
            break;
        }
        const int row = rd16(m, kText, kSetupRow) / 2;
        if (row < 0 || row >= kSetupRows) {
            fail("unexpected setup screen row " + std::to_string(row));
            break;
        }
        const int item = rd16(m, kText, static_cast<uint16_t>(kSetupItems + 2 * row)) / 4 - 1;
        if (kWanted[row] >= 0 && item != kWanted[row]) {
            if (presses_ > 60) {
                fail("the setup screen doesn't take the settings");
                break;
            }
            const int n = kItemCount[row], ahead = (kWanted[row] - item + n) % n;
            tap(ahead <= n - ahead ? kRight : kLeft);  // the shorter way round (the items wrap)
        } else if (row + 1 < kSetupRows) {
            tap(kDown);  // (arriving at a row runs its item's handler: every row is visited)
        } else {
            tap(kEnter);  // DONE? Yes
            enter(Phase::Linking);
        }
        break;
    }
    case Phase::Linking:
        // The handshake is done once two players are on and the map waits for a key again: start the race
        // at once (the other game does the same as its handshake completes, so the races start together).
        if (rd8(m, kCode, kTwoPlayers) != 0 && seen_[kCourse] > phase_at_) {
            log(std::string("linked; this game starts in the ") + (second_slot() ? "second" : "first") +
                " position");
            enter(Phase::Starting);
            tap(kEnter);
        }
        break;
    case Phase::Starting:
        if (seen_[kRace] > phase_at_) {
            enter(Phase::Racing);
        } else if (quiz_ && now() >= key_at_ + kMenuStep) {
            tap(kEnter);  // the manual check: any answer
        }
        break;
    default:
        break;
    }
}

} // namespace vette::game
