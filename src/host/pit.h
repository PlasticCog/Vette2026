#pragma once
// Intel 8253 programmable interval timer, ports 40h-43h. Time is measured in PIT input clocks
// (1193182 Hz) supplied by the caller. Channel 0 drives IRQ0; channel 2 feeds the PC speaker,
// gated by port 61h bit 0.

#include <array>
#include <cstdint>

namespace vette::host {

constexpr uint64_t kPitHz = 1193182;

class Pit {
public:
    struct Channel {
        uint8_t mode = 3;
        uint8_t access = 3;      // 1 = low byte, 2 = high byte, 3 = low then high
        uint32_t reload = 65536;  // 0 written means 65536
        uint64_t start = 0;      // PIT time the current count began
        bool gate = true;
        bool write_hi = false;   // next write is the high byte (access 3)
        bool read_hi = false;    // next read is the high byte (access 3)
        bool latched = false;
        uint16_t latch = 0;
        uint16_t pending_low = 0;
    };

    Pit();

    uint8_t in8(uint16_t port, uint64_t now);
    void out8(uint16_t port, uint8_t value, uint64_t now);

    // Channel 2 gate (port 61h bit 0). A rising edge restarts the count.
    void set_gate2(bool gate, uint64_t now);

    // First IRQ0 (channel 0 output rising edge) strictly after `now`, or UINT64_MAX if none.
    uint64_t next_irq0_after(uint64_t now) const;

    uint16_t count(int ch, uint64_t now) const;
    bool output(int ch, uint64_t now) const;
    const Channel& channel(int ch) const { return ch_[static_cast<size_t>(ch)]; }

private:
    std::array<Channel, 3> ch_{};
};

} // namespace vette::host
