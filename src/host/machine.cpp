#include "host/machine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <limits>

namespace vette::host {
namespace {

constexpr uint64_t kNever = std::numeric_limits<uint64_t>::max();
constexpr uint64_t kNsPerSecond = 1'000'000'000;
constexpr uint64_t kKeyboardByteGap = 1200;  // PIT clocks (~1 ms) between scan-code bytes

// a * b / c without 64-bit overflow for the magnitudes used here (b, c < 2^32).
uint64_t mul_div(uint64_t a, uint64_t b, uint64_t c) { return (a / c) * b + (a % c) * b / c; }

std::tm local_tm(int64_t seconds) {
    const auto t = static_cast<std::time_t>(seconds);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

} // namespace

Machine::Machine(MachineConfig config)
    : config_(std::move(config)),
      cpu_(mem_, *this),
      bios_(mem_, ega_, Bios::Options{config_.mouse, config_.joystick}),
      dos_(mem_, bios_, Dos::Paths{config_.game_dir, config_.save_dir}) {
    mem_.set_video(&ega_);
    cpu_.set_interrupt_controller(&pic_);
    cpu_.set_callback([this](Cpu& cpu, uint8_t id) {
        if (id == Bios::kDos) {
            dos_.int21(cpu);
        } else if (id == Bios::kDosTerminate) {
            dos_.int20(cpu);
        } else {
            bios_.handle(cpu, id);
        }
    });
    ega_.set_time_source([this] { return emulated_ns(); });
    bios_.set_clock([this] { return wall_clock(); });
    dos_.set_clock([this] { return wall_clock(); });
    bios_.set_fault_handler([this](const std::string& what) {
        fault_ = what;
        cpu_.request_stop();
    });

    if (config_.start_time) {
        const RealTime& t = *config_.start_time;
        std::tm tm{};
        tm.tm_year = t.year - 1900;
        tm.tm_mon = t.month - 1;
        tm.tm_mday = t.day;
        tm.tm_hour = t.hour;
        tm.tm_min = t.minute;
        tm.tm_sec = t.second;
        tm.tm_isdst = -1;
        start_epoch_ = static_cast<int64_t>(std::mktime(&tm));
    } else {
        start_epoch_ = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    }
}

Machine::~Machine() = default;

void Machine::set_log(std::function<void(const std::string&)> log) {
    log_ = log;
    bios_.set_log(log);
    dos_.set_log(log);
    dos_.set_console([this](char c) {  // DOS console output is logged in whole lines
        if (c == '\n') {
            if (log_) {
                log_("console: " + console_line_);
            }
            console_line_.clear();
        } else if (c != '\r') {
            console_line_.push_back(c);
        }
    });
}

void Machine::log_once(const std::string& what) {
    if (log_ && std::find(logged_.begin(), logged_.end(), what) == logged_.end()) {
        logged_.push_back(what);
        log_(what);
    }
}

bool Machine::boot(std::string& error) {
    std::filesystem::path exe_path;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(config_.game_dir, ec)) {
        std::string name = entry.path().filename().string();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        if (name == "VETTE.EXE") {
            exe_path = entry.path();
        }
    }
    if (exe_path.empty()) {
        error = "VETTE.EXE not found in " + config_.game_dir.string();
        return false;
    }
    std::ifstream in(exe_path, std::ios::binary);
    const std::vector<uint8_t> exe((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    bios_.install();
    if (!dos_.load_exe(cpu_, exe, kLoadSegment, "C:\\VETTE.EXE", error)) {
        return false;
    }
    // The BIOS leaves PIT channel 0 at 18.2 Hz (mode 3, count 65536).
    out8(0x43, 0x36);
    out8(0x40, 0x00);
    out8(0x40, 0x00);
    return true;
}

uint64_t Machine::pit_now() const { return mul_div(cpu_.total_cycles(), kPitHz, config_.cpu_hz); }

uint64_t Machine::cycle_of_pit(uint64_t pit) const {
    if (pit == kNever) {
        return kNever;
    }
    return mul_div(pit, config_.cpu_hz, kPitHz) + 1;  // first cycle at or after that PIT clock
}

uint64_t Machine::emulated_ns() const { return mul_div(cpu_.total_cycles(), kNsPerSecond, config_.cpu_hz); }

uint64_t Machine::next_event_cycle() const {
    uint64_t next = cycle_of_pit(next_irq0_);
    if (!kbd_full_ && !kbd_queue_.empty()) {
        next = std::min(next, cycle_of_pit(kbd_ready_at_));
    }
    return next;
}

uint64_t Machine::cycle_at_ns(uint64_t ns) const {
    if (ns == kNever) {
        return kNever;
    }
    return mul_div(ns, config_.cpu_hz, kNsPerSecond) + 1;  // first cycle at or after that time
}

RealTime Machine::wall_clock() const {
    const uint64_t ns = emulated_ns();
    const std::tm tm = local_tm(start_epoch_ + static_cast<int64_t>(ns / kNsPerSecond));
    return {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
            static_cast<int>(ns % kNsPerSecond / 10'000'000)};
}

void Machine::run_for(uint64_t ns) {
    cycle_remainder_ += ns * config_.cpu_hz;
    target_cycles_ += cycle_remainder_ / kNsPerSecond;
    cycle_remainder_ %= kNsPerSecond;

    while (!stopped() && cpu_.total_cycles() < target_cycles_) {
        const uint64_t now = pit_now();
        if (now >= next_irq0_) {
            pic_.raise(0);
            next_irq0_ = pit_.next_irq0_after(now);
        }
        deliver_key();

        uint64_t slice_end = std::min(target_cycles_, cycle_of_pit(next_irq0_));
        if (!kbd_full_ && !kbd_queue_.empty()) {
            slice_end = std::min(slice_end, cycle_of_pit(kbd_ready_at_));
        }
        const uint64_t at = cpu_.total_cycles();
        slice_end = std::max(slice_end, at + 1);
        cpu_.run(static_cast<int64_t>(slice_end - at));
    }
    const uint64_t now = pit_now();
    speaker_.render(now, config_.audio_rate, audio_);
}

void Machine::take_audio(std::vector<int16_t>& out) {
    out.insert(out.end(), audio_.begin(), audio_.end());
    audio_.clear();
}

void Machine::key(uint8_t scancode) { kbd_queue_.push_back(scancode); }

void Machine::deliver_key() {
    if (!kbd_full_ && !kbd_queue_.empty() && pit_now() >= kbd_ready_at_) {
        kbd_data_ = kbd_queue_.front();
        kbd_queue_.pop_front();
        kbd_full_ = true;
        pic_.raise(1);
    }
}

void Machine::joystick_axes(float x, float y) {
    joy_x_ = std::clamp(x, -1.0f, 1.0f);
    joy_y_ = std::clamp(y, -1.0f, 1.0f);
}

uint8_t Machine::in8(uint16_t port) {
    if (Ega::handles(port)) {
        return ega_.in8(port);
    }
    switch (port) {
    case 0x20:
    case 0x21:
        return pic_.in8(port);
    case 0x40:
    case 0x41:
    case 0x42:
    case 0x43:
        return pit_.in8(port, pit_now());
    case 0x60:
        kbd_full_ = false;
        kbd_ready_at_ = pit_now() + kKeyboardByteGap;
        return kbd_data_;
    case 0x61: {
        const uint64_t now = pit_now();
        const auto refresh = static_cast<uint8_t>(((now / 18) & 1) << 4);  // toggles every ~15 us
        const auto out2 = static_cast<uint8_t>(pit_.output(2, now) ? 0x20 : 0);
        return static_cast<uint8_t>((port61_ & 0x0F) | refresh | out2);
    }
    case 0x64:
        return static_cast<uint8_t>(0x14 | (kbd_full_ ? 1 : 0));
    case 0x201: {
        uint8_t v = static_cast<uint8_t>(0xF0 & ~(joy_buttons_ << 4));
        if (!config_.joystick) {
            return 0xFF;  // nothing connected: buttons up, axis one-shots never time out
        }
        // One-shot length ~24.2 us + 0.011 us/ohm across a 0..100 kOhm pot.
        const uint64_t elapsed = pit_now() - joy_fired_;
        auto timing = [&](float pos) {
            const double us = 24.2 + 1100.0 * (pos + 1.0) / 2.0;
            return elapsed < static_cast<uint64_t>(us * kPitHz / 1e6);
        };
        v |= static_cast<uint8_t>((timing(joy_x_) ? 1 : 0) | (timing(joy_y_) ? 2 : 0) | 0x0C);
        return v;
    }
    default:
        if ((port & 0xFFF8) == 0x3F8 || (port & 0xFFF8) == 0x2F8) {
            return 0xFF;  // no UART yet (two-player link)
        }
        log_once("unhandled port read " + std::to_string(port));
        return 0xFF;
    }
}

void Machine::out8(uint16_t port, uint8_t value) {
    if (Ega::handles(port)) {
        ega_.out8(port, value);
        return;
    }
    switch (port) {
    case 0x20:
    case 0x21:
        pic_.out8(port, value);
        break;
    case 0x40:
    case 0x41:
    case 0x42:
    case 0x43: {
        const uint64_t now = pit_now();
        pit_.out8(port, value, now);
        if (port == 0x40 || (port == 0x43 && (value >> 6) == 0)) {
            next_irq0_ = pit_.next_irq0_after(now);
            cpu_.request_stop();  // the current slice was sized for the old rate
        }
        speaker_.update(now, (port61_ & 2) != 0, pit_);
        break;
    }
    case 0x60:
        kbd_queue_.push_front(0xFA);  // keyboard command (e.g. set LEDs): acknowledge
        break;
    case 0x61: {
        const uint64_t now = pit_now();
        port61_ = value;
        pit_.set_gate2((value & 1) != 0, now);
        speaker_.update(now, (value & 2) != 0, pit_);
        break;
    }
    case 0x64:
    case 0x80:
        break;
    case 0x201:
        joy_fired_ = pit_now();
        break;
    default:
        if ((port & 0xFFF8) == 0x3F8 || (port & 0xFFF8) == 0x2F8) {
            break;
        }
        log_once("unhandled port write " + std::to_string(port));
        break;
    }
}

} // namespace vette::host
