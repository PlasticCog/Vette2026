#pragma once
// Host BIOS: ROM stubs at F000h, the interrupt vector table, the BIOS data area, and high-level
// services for INT 10h (EGA), 11h, 12h, 13h, 14h, 15h, 16h (keyboard), 17h, 1Ah (time) and
// 33h (mouse driver). Hardware IRQ stubs (INT 08h/09h) are real x86 code that calls back into the
// host for bookkeeping, so programs that chain to the "old" vector behave as on a real PC.

#include <cstdint>
#include <functional>
#include <set>
#include <string>

#include "host/cpu.h"
#include "host/ega.h"

namespace vette::host {

struct RealTime {  // local wall-clock time for DOS/RTC services
    int year, month, day, hour, minute, second, hundredths;
};

class Bios {
public:
    // Callback ids (operand of the 0F FF ib host-callback opcode in the ROM stubs).
    enum : uint8_t {
        kDivideError = 0x00, kInvalidOpcode = 0x06, kTimerTick = 0x08, kKeyboardIrq = 0x09,
        kVideo = 0x10, kEquipment = 0x11, kMemorySize = 0x12, kDisk = 0x13, kSerial = 0x14,
        kSystem = 0x15, kKeyboard = 0x16, kPrinter = 0x17, kTime = 0x1A, kDosTerminate = 0x20,
        kDos = 0x21, kMultiplex = 0x2F, kMouse = 0x33,
    };

    struct Options {
        bool mouse_installed = true;
        bool game_port = false;  // joystick present (equipment word bit 12)
    };

    Bios(Memory& mem, Ega& ega, Options options);

    // Writes the ROM stubs, IVT and BDA, then write-protects the ROM.
    void install();

    // Services every callback id except kDos/kDosTerminate (owned by Dos).
    void handle(Cpu& cpu, uint8_t id);

    void set_clock(std::function<RealTime()> clock) { clock_ = std::move(clock); }
    void set_log(std::function<void(const std::string&)> log) { log_ = std::move(log); }
    // Called when a program faults (divide error / invalid opcode with no handler of its own).
    void set_fault_handler(std::function<void(const std::string&)> fault) { fault_ = std::move(fault); }

    // Mouse input from the host, in device units (mickeys) and a button mask (bit0 L, bit1 R).
    void mouse_motion(int dx, int dy);
    void mouse_buttons(uint8_t mask);

    // Pops a key (scan << 8 | ascii) from the BIOS keyboard buffer, or -1 if empty (used by DOS).
    int read_key(bool remove);

    // The INT 33h driver's pointer, for the frontend to draw (the host driver doesn't draw into
    // video memory). Coordinates are the driver's virtual screen: 640x200 in both 0Dh and 0Eh, so
    // x is twice the pixel column in 320-wide modes.
    struct Cursor {
        bool visible;
        int x, y;
    };
    Cursor mouse_cursor() const { return {opt_.mouse_installed && mouse_.show_count >= 0, mouse_.x, mouse_.y}; }

private:
    struct Mouse {
        int x = 320, y = 100;
        int x_min = 0, x_max = 639, y_min = 0, y_max = 199;
        int mickey_x = 0, mickey_y = 0;     // counters for INT 33h 0Bh
        int frac_x = 0, frac_y = 0;         // sub-pixel remainders
        int ratio_x = 8, ratio_y = 16;      // mickeys per 8 pixels
        int show_count = -1;
        uint8_t buttons = 0;
        uint16_t presses[2] = {0, 0}, releases[2] = {0, 0};
        int16_t press_x[2] = {0, 0}, press_y[2] = {0, 0}, release_x[2] = {0, 0}, release_y[2] = {0, 0};
    };

    void video(Cpu& cpu);
    void keyboard_irq(Cpu& cpu);
    void keyboard_service(Cpu& cpu);
    void time_service(Cpu& cpu);
    void mouse_service(Cpu& cpu);
    void push_key(uint16_t key);
    void log_once(const std::string& what);

    uint8_t bda8(uint16_t off) { return mem_.read8(0x400u + off); }
    uint16_t bda16(uint16_t off) { return mem_.read16(0x400u + off); }
    void set_bda8(uint16_t off, uint8_t v) { mem_.write8(0x400u + off, v); }
    void set_bda16(uint16_t off, uint16_t v) { mem_.write16(0x400u + off, v); }

    Memory& mem_;
    Ega& ega_;
    Options opt_;
    Mouse mouse_;
    bool e0_prefix_ = false;
    std::function<RealTime()> clock_;
    std::function<void(const std::string&)> log_;
    std::function<void(const std::string&)> fault_;
    std::set<std::string> logged_;  // messages already reported by log_once
};

} // namespace vette::host
