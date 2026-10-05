#include "sound/pc98_sound.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

#include "assets/pc98_disk.h"
#include "game/sound_events.h"
#include "host/cpu.h"
#include "host/memory.h"
#include "sound/fm_chip.h"

namespace vette::sound {
namespace {

using host::Cpu;
using namespace std::string_view_literals;  // signatures hold 00h bytes

// The driver module of VETTE.EXE 1.02J: image segment 0FFDh, data from 0000h, code from 11B1h,
// self-contained (it sets DS = ES = CS and touches nothing outside but the OPN, the interrupt
// controller and its own interrupt vector). Offsets below are within it (re/notes/09-pc98.md).
constexpr uint32_t kImageOffset = 0x0FFD * 16;
constexpr uint32_t kSize = 0x1CA5;
constexpr uint16_t kInit = 0x11B1;      // far: detect the board, mute, hook the IRQ
constexpr uint16_t kEntry = 0x1292;     // far: function AH, argument AL (DX)
constexpr uint16_t kIsr = 0x1671;       // the OPN timer interrupt handler
constexpr uint16_t kTrackEnd = 0x1890;  // a track's end byte was read: DI = track, [DI+0Ch] = loop
constexpr uint16_t kSongs = 0x02B4;     // song list: a length word, then the song
constexpr uint16_t kCode = 0x11B1;      // the song list ends where the code begins
constexpr uint16_t kMusicTracks = 0x006A, kTrackSize = 0x2E;
constexpr uint16_t kIdleTrack = 0x0067;  // where stopped tracks point
constexpr uint16_t kMusicActive = 0x02AF;
constexpr uint16_t kPresent = 0x02B1;

// Driver functions (AH).
constexpr uint16_t kFnMusic = 0x0100;  // AL = song (0 stops)
constexpr uint16_t kFnStopMusic = 0x0300;

struct Signature {
    uint16_t offset;
    std::string_view bytes;
};
// Code the host depends on: the entry points and the track-end instruction it watches.
constexpr Signature kSignatures[] = {
    {kInit, "\x1E\x0E\x1F\xBA\x88\x01\xEC\xFE\xC0"sv},
    {kEntry, "\x1E\x06\x60\x8C\xCB\x8E\xDB\x8E\xC3\x80\x3E\xB1\x02\x00"sv},
    {kIsr, "\xFB\x60\x1E\x06\xFC\x0E\x1F\x0E\x07\xBA\x88\x01\xEC\xA8\x02"sv},
    {kTrackEnd, "\x8B\x75\x0C\x23\xF6\x75\xF4\xBE\x67\x00"sv},
    {0x1C9D, "\x50\xEC\xD0\xE0\x72\xFB\x58\xC3"sv},  // the busy-flag wait
};

bool matches(std::span<const uint8_t> image, size_t base, const Signature& s) {
    if (base + s.offset + s.bytes.size() > image.size()) {
        return false;
    }
    return std::equal(s.bytes.begin(), s.bytes.end(), image.begin() + static_cast<std::ptrdiff_t>(base + s.offset),
                      [](char a, uint8_t b) { return static_cast<uint8_t>(a) == b; });
}

uint16_t le16(std::span<const uint8_t> b, size_t at) { return static_cast<uint16_t>(b[at] | b[at + 1] << 8); }

// The driver module from VETTE.EXE: at its 1.02J place, else wherever its interrupt handler is.
bool extract_driver(std::span<const uint8_t> exe, std::vector<uint8_t>& out, std::string& error) {
    if (exe.size() < 0x20 || exe[0] != 'M' || exe[1] != 'Z') {
        error = "VETTE.EXE isn't an EXE file";
        return false;
    }
    const size_t header = size_t{le16(exe, 8)} * 16;
    if (header >= exe.size()) {
        error = "VETTE.EXE is truncated";
        return false;
    }
    const std::span<const uint8_t> image = exe.subspan(header);
    const auto all_match = [&](size_t base) {
        return base + kSize <= image.size() &&
               std::all_of(std::begin(kSignatures), std::end(kSignatures), [&](const Signature& s) { return matches(image, base, s); });
    };
    std::optional<size_t> base;
    if (all_match(kImageOffset)) {
        base = kImageOffset;
    } else {
        const Signature& isr = kSignatures[2];
        const auto it = std::search(image.begin(), image.end(), isr.bytes.begin(), isr.bytes.end(),
                                    [](uint8_t a, char b) { return a == static_cast<uint8_t>(b); });
        const auto at = static_cast<size_t>(it - image.begin());
        if (it != image.end() && at >= kIsr && (at - kIsr) % 16 == 0 && all_match(at - kIsr)) {
            base = at - kIsr;
        }
    }
    if (!base) {
        error = "VETTE.EXE has no PC-98 FM driver (the PC-98 version 1.02J is needed)";
        return false;
    }
    out.assign(image.begin() + static_cast<std::ptrdiff_t>(*base), image.begin() + static_cast<std::ptrdiff_t>(*base + kSize));

    // The song list must be intact: the driver walks it by length words without checks.
    size_t pos = kSongs;
    int songs = 0;
    while (pos < kCode) {
        const size_t len = le16(out, pos);
        if (len < 8 + 6 * 4 || pos + len > kCode || le16(out, pos + 2) >= len || le16(out, pos + 4) >= len ||
            le16(out, pos + 6) + size_t{6 * 4} > len) {
            break;
        }
        pos += len;
        ++songs;
    }
    if (pos != kCode || songs != kPc98SongCount) {
        error = "VETTE.EXE's FM song list is damaged";
        return false;
    }
    return true;
}

// The chip's own sample rate (ymfm's YM2203 runs at clock/4 for an exact SSG).
uint32_t chip_rate() { return FmChip(FmChip::Type::Ym2203, 48000).chip_rate(); }

}  // namespace

// The scratch PC-98: memory with the driver, a 286 to run it, and the I/O it uses.
struct Pc98Sound::Machine final : host::IoBus {
    static constexpr uint16_t kSeg = 0x1000;         // where the driver is loaded
    static constexpr uint16_t kStubSeg = 0x0050;     // returns land on a host callback here
    static constexpr uint16_t kStackSeg = 0x2000;
    static constexpr uint8_t kReturned = 1;
    static constexpr int kChunk = 256;  // chip samples between interrupt checks (about 0.26 ms)

    host::Memory mem;
    Cpu cpu{mem, *this};
    // The chip renders at its own rate (FmChip doesn't resample then); render() averages each output
    // sample's span of chip samples, a box filter that keeps the SSG's square waves from aliasing.
    FmChip chip;
    uint8_t address = 0;
    std::array<uint8_t, 256> regs{};  // last values written: the data port reads them back
    bool returned = false;

    int output_rate;
    double ratio;      // chip samples per output sample
    double need;       // chip time still missing from the output sample being built
    double acc = 0;
    std::vector<float> chunk = std::vector<float>(kChunk);
    std::vector<float> ready;  // output samples made but not yet taken
    size_t taken = 0;
    float dc_in = 0, dc_out = 0, dc_r;  // the board's AC coupling: a 10 Hz high-pass

    Machine(uint32_t rate, int out_rate)
        : chip(FmChip::Type::Ym2203, static_cast<int>(rate)),
          output_rate(out_rate),
          ratio(static_cast<double>(rate) / out_rate),
          need(ratio),
          dc_r(1.0f - static_cast<float>(2 * 3.14159265358979 * 10.0 / out_rate)) {}

    // Turns a chunk of chip samples into output samples.
    void decimate() {
        constexpr float kGain = 0.5f;  // the loudest song peaks near 0.55 (the PC speaker is 0.25)
        for (const float s : chunk) {
            double left = 1.0;
            while (left > 0) {
                const double take = std::min(left, need);
                acc += s * take;
                need -= take;
                left -= take;
                if (need <= 1e-9) {
                    const float x = static_cast<float>(acc / ratio) * kGain;
                    dc_out = x - dc_in + dc_r * dc_out;
                    dc_in = x;
                    ready.push_back(dc_out);
                    acc = 0;
                    need += ratio;
                }
            }
        }
    }

    uint8_t in8(uint16_t port) override {
        switch (port) {
        case 0x188:
            return chip.status();
        case 0x18A:
            // SSG port A (0Eh) reads the board's jumpers: bits 6-7 pick the IRQ (11 = INT5; any works,
            // the host calls the handler itself), bits 0-5 are joystick inputs (1 = released).
            return address == 0x0E ? 0xFF : regs[address];
        default:
            return 0x00;  // interrupt controller mask registers: all enabled
        }
    }
    void out8(uint16_t port, uint8_t value) override {
        if (port == 0x188) {
            address = value;
        } else if (port == 0x18A) {
            regs[address] = value;
            chip.write(address, value);
        }
        // Interrupt controller (00h, 02h, 08h, 0Ah): end-of-interrupt and mask writes, not needed.
    }

    uint8_t byte(uint16_t off) { return mem.read8(Cpu::linear(kSeg, off)); }
    uint16_t word(uint16_t off) { return mem.read16(Cpu::linear(kSeg, off)); }
};

std::string_view pc98_song_name(Pc98Song song) {
    switch (song) {
    case Pc98Song::Title: return "title";
    case Pc98Song::Winner: return "winner";
    case Pc98Song::Loser: return "loser";
    case Pc98Song::Menu: return "menu";
    }
    return "?";
}

std::unique_ptr<Pc98Sound> Pc98Sound::create(const assets::Pc98Files& files, int output_rate, std::string& error) {
    const auto exe = files.read("VETTE.EXE");
    if (!exe) {
        error = "no VETTE.EXE in the PC-98 files";
        return nullptr;
    }
    return create(*exe, output_rate, error);
}

std::unique_ptr<Pc98Sound> Pc98Sound::create(std::span<const uint8_t> vette_exe, int output_rate, std::string& error) {
    std::vector<uint8_t> driver;
    if (output_rate < 8000 || output_rate > 384000) {
        error = "unsupported output rate";
        return nullptr;
    }
    if (!extract_driver(vette_exe, driver, error)) {
        return nullptr;
    }
    auto m = std::make_unique<Machine>(chip_rate(), output_rate);
    std::copy(driver.begin(), driver.end(), m->mem.ram() + Cpu::linear(Machine::kSeg, 0));
    const uint8_t stub[] = {0x0F, 0xFF, Machine::kReturned, 0xF4};  // host callback, HLT
    std::copy(std::begin(stub), std::end(stub), m->mem.ram() + Cpu::linear(Machine::kStubSeg, 0));

    std::unique_ptr<Pc98Sound> s(new Pc98Sound(std::move(m)));
    if (!s->run(kInit, false) || s->m_->byte(kPresent) != 1) {
        error = "the PC-98 FM driver didn't start" + (s->failed_ ? ": " + s->failure_ : std::string());
        return nullptr;
    }
    return s;
}

Pc98Sound::Pc98Sound(std::unique_ptr<Machine> m) : m_(std::move(m)) {
    Machine* mp = m_.get();
    m_->cpu.set_callback([mp](Cpu& c, uint8_t id) {
        if (id == Machine::kReturned) {
            mp->returned = true;
            c.request_stop();
        }
    });
    m_->cpu.add_watch(Cpu::linear(Machine::kSeg, kTrackEnd), [this](Cpu&) { track_end(); });
}

Pc98Sound::~Pc98Sound() = default;

// Runs the driver from `entry` until it returns: a far call, or an interrupt (FLAGS pushed too).
bool Pc98Sound::run(uint16_t entry, bool interrupt) {
    if (failed_) {
        return false;
    }
    constexpr int64_t kBudget = 4'000'000;  // cycles; a call needs a few thousand
    Cpu& cpu = m_->cpu;
    host::Registers& r = cpu.regs;
    r.s[host::SS] = Machine::kStackSeg;
    r.r[host::SP] = 0;
    r.flags = 0x0202;
    if (interrupt) {
        cpu.push16(r.flags);
    }
    cpu.push16(Machine::kStubSeg);
    cpu.push16(0);
    r.s[host::CS] = Machine::kSeg;
    r.ip = entry;
    m_->returned = false;
    for (int64_t left = kBudget; !m_->returned && left > 0;) {
        left -= cpu.run(left);
    }
    if (!m_->returned) {
        failed_ = true;
        failure_ = "the PC-98 FM driver stopped responding";
        m_->chip.reset();  // silence whatever it left sounding
    }
    return !failed_;
}

bool Pc98Sound::call(uint16_t ax, uint16_t dx) {
    m_->cpu.regs.r[host::AX] = ax;
    m_->cpu.regs.r[host::DX] = dx;
    return run(kEntry, false);
}

void Pc98Sound::track_end() {
    const uint16_t di = m_->cpu.regs.r[host::DI];
    if (di < kMusicTracks || di >= kMusicTracks + 6 * kTrackSize || (di - kMusicTracks) % kTrackSize != 0) {
        return;  // the driver's sound-effect tracks (unused by the game)
    }
    const uint16_t loop = m_->word(static_cast<uint16_t>(di + 0x0C));
    if (loop == 0 || loop == kIdleTrack) {
        return;
    }
    ++track_loops_[static_cast<size_t>((di - kMusicTracks) / kTrackSize)];
    // The song has looped once every looping track has.
    int least = 0x7FFFFFFF;
    for (size_t k = 0; k < track_loops_.size(); ++k) {
        const uint16_t t = static_cast<uint16_t>(kMusicTracks + k * kTrackSize);
        const uint16_t l = m_->word(static_cast<uint16_t>(t + 0x0C));
        if (l != 0 && l != kIdleTrack) {
            least = std::min(least, track_loops_[k]);
        }
    }
    loops_ = least == 0x7FFFFFFF ? 0 : least;
}

void Pc98Sound::play(Pc98Song song) {
    const int n = static_cast<int>(song);
    if (n < 1 || n > kPc98SongCount) {
        return;
    }
    loops_ = 0;
    track_loops_ = {};
    fade_gain_ = 1;
    fade_step_ = 0;
    current_ = song;
    call(static_cast<uint16_t>(kFnMusic | n));
}

void Pc98Sound::stop() {
    call(kFnStopMusic);
    current_.reset();
    loops_ = 0;
}

std::optional<Pc98Song> Pc98Sound::song_for(game::Sfx sfx) {
    switch (sfx) {
    case game::Sfx::TitleTune: return Pc98Song::Title;  // DOS sequence 3009:9304, from 3009:C560
    case game::Sfx::WinTune: return Pc98Song::Winner;   // DOS sequence 3009:92C4, from 3009:CEBD
    default: return std::nullopt;
    }
}

bool Pc98Sound::start(game::Sfx sfx) {
    const auto song = song_for(sfx);
    if (song) {
        play(*song);
    }
    return song.has_value();
}

void Pc98Sound::stop(game::Sfx sfx) {
    if (current_ && song_for(sfx) == current_) {
        stop();
    }
}

void Pc98Sound::fade_out(double seconds) {
    if (playing() && fade_step_ == 0) {
        fade_step_ = static_cast<float>(1.0 / std::max(1.0, seconds * m_->output_rate));
    }
}

bool Pc98Sound::playing() const { return !failed_ && m_->byte(kMusicActive) != 0; }

void Pc98Sound::render(float* out, int frames) {
    // The chip runs a chunk at a time; between chunks a raised IRQ (a timer expired) runs the driver's
    // handler, which services the timer and acknowledges it.
    Machine& m = *m_;
    for (int i = 0; i < frames; ++i) {
        if (m.taken == m.ready.size()) {
            m.ready.clear();
            m.taken = 0;
            while (m.ready.empty()) {
                if (!failed_ && m.chip.irq()) {
                    run(kIsr, true);
                }
                m.chip.render(m.chunk.data(), Machine::kChunk);
                m.decimate();
            }
        }
        out[i] = m.ready[m.taken++] * fade_gain_;
        if (fade_step_ > 0) {
            fade_gain_ = std::max(0.0f, fade_gain_ - fade_step_);
            if (fade_gain_ == 0) {
                fade_step_ = 0;
                stop();
            }
        }
    }
}

}  // namespace vette::sound
