#pragma once
// The emulated PC that hosts VETTE.EXE: 286-class CPU, EGA, 8259 PIC, 8253 PIT, keyboard
// controller, PC speaker, game port, and the host BIOS/DOS. Emulated time advances only inside
// run_for(), in whole CPU cycles, so a run is deterministic for a given input sequence.

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "host/bios.h"
#include "host/cpu.h"
#include "host/dos.h"
#include "host/ega.h"
#include "host/memory.h"
#include "host/pic.h"
#include "host/pit.h"
#include "host/speaker.h"

namespace vette::host {

struct MachineConfig {
    std::filesystem::path game_dir;
    std::filesystem::path save_dir;
    // Emulated CPU clock. With the game's variable timestep this sets the frame cadence, i.e. the
    // "reference machine" for Classic mode (re/notes/01-startup-and-timing.md).
    uint64_t cpu_hz = 12'000'000;
    int audio_rate = 48000;
    bool mouse = true;
    bool joystick = false;
    std::optional<RealTime> start_time;  // fixed wall clock for deterministic runs; default: host clock
};

class Machine final : public IoBus {
public:
    // Load segment for the program image. With PSP at 0FF0h this equals Ghidra's MZ base, so
    // emulator addresses match the Ghidra project (image-relative SEG + 1000h).
    static constexpr uint16_t kLoadSegment = 0x1000;

    explicit Machine(MachineConfig config);
    ~Machine() override;
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    bool boot(std::string& error);  // loads VETTE.EXE from the game folder
    void run_for(uint64_t ns);

    bool stopped() const { return dos_.terminated() || !fault_.empty(); }
    int exit_code() const { return dos_.exit_code(); }
    const std::string& fault() const { return fault_; }

    // Input. Keys are scan-code set 1 bytes (an E0 prefix is a separate byte).
    void key(uint8_t scancode);
    void mouse_motion(int dx, int dy) { bios_.mouse_motion(dx, dy); }
    void mouse_buttons(uint8_t mask) { bios_.mouse_buttons(mask); }
    void joystick_axes(float x, float y);  // -1..1
    void joystick_buttons(uint8_t mask) { joy_buttons_ = mask; }

    // Output.
    void render(Ega::Frame& out) const { ega_.render(out); }
    Bios::Cursor mouse_cursor() const { return bios_.mouse_cursor(); }
    void take_audio(std::vector<int16_t>& out);

    uint64_t emulated_ns() const;
    Cpu& cpu() { return cpu_; }
    Memory& memory() { return mem_; }
    Ega& ega() { return ega_; }
    void set_log(std::function<void(const std::string&)> log);

    // IoBus
    uint8_t in8(uint16_t port) override;
    void out8(uint16_t port, uint8_t value) override;

private:
    uint64_t pit_now() const;
    uint64_t cycle_of_pit(uint64_t pit) const;
    void deliver_key();
    RealTime wall_clock() const;
    void log_once(const std::string& what);

    MachineConfig config_;
    Memory mem_;
    Ega ega_;
    Pic pic_;
    Pit pit_;
    Speaker speaker_;
    Cpu cpu_;
    Bios bios_;
    Dos dos_;

    uint64_t target_cycles_ = 0;
    uint64_t cycle_remainder_ = 0;  // sub-cycle remainder of ns * cpu_hz
    uint64_t next_irq0_ = 0;
    std::vector<int16_t> audio_;

    // Keyboard controller
    std::deque<uint8_t> kbd_queue_;
    uint8_t kbd_data_ = 0;
    bool kbd_full_ = false;
    uint64_t kbd_ready_at_ = 0;  // PIT time the next byte may be delivered

    uint8_t port61_ = 0;
    uint64_t joy_fired_ = 0;
    float joy_x_ = 0, joy_y_ = 0;
    uint8_t joy_buttons_ = 0;

    int64_t start_epoch_ = 0;  // wall clock at boot, seconds since 1970 (local time)
    std::string fault_;
    std::function<void(const std::string&)> log_;
    std::vector<std::string> logged_;
    std::string console_line_;
};

} // namespace vette::host
