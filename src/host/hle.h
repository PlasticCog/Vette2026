#pragma once
// Helpers for high-level BIOS/DOS services. Each service lives in a ROM stub `0F FF id / IRET`, so
// when the callback runs, SS:SP points at the caller's IRET frame (IP, CS, FLAGS).

#include <cstddef>
#include <string>

#include "host/cpu.h"

namespace vette::host::hle {

inline uint32_t frame_addr(const Cpu& cpu, uint16_t offset) {
    return Cpu::linear(cpu.regs.s[SS], static_cast<uint16_t>(cpu.regs.r[SP] + offset));
}

// Sets or clears a flag in the FLAGS image that IRET will restore (e.g. CF for DOS errors).
inline void set_return_flag(Cpu& cpu, uint16_t f, bool on) {
    const uint32_t a = frame_addr(cpu, 4);
    const uint16_t flags = cpu.memory().read16(a);
    cpu.memory().write16(a, static_cast<uint16_t>(on ? (flags | f) : (flags & ~f)));
}

inline void set_carry(Cpu& cpu, bool on) { set_return_flag(cpu, flag::CF, on); }

// Makes the caller's 2-byte `INT nn` run again after IRET. Used for blocking services (e.g. wait
// for a key) so interrupts keep being serviced while the program waits.
inline void retry_int(Cpu& cpu) {
    const uint32_t a = frame_addr(cpu, 0);
    cpu.memory().write16(a, static_cast<uint16_t>(cpu.memory().read16(a) - 2));
}

inline std::string read_asciiz(Memory& mem, uint16_t seg, uint16_t off, size_t max = 128) {
    std::string s;
    for (size_t i = 0; i < max; ++i) {
        const char c = static_cast<char>(mem.read8(Cpu::linear(seg, static_cast<uint16_t>(off + i))));
        if (c == '\0') {
            break;
        }
        s.push_back(c);
    }
    return s;
}

} // namespace vette::host::hle
