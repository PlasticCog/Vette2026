#pragma once
// Real-mode x86 interpreter (8086 + 80186/80286 real-mode instruction set) for hosting VETTE.EXE.
//
// Contract used by the rest of the host:
//  - Hardware interrupts come from an InterruptController, sampled at instruction boundaries when IF=1
//    (respecting the one-instruction delay after STI and after MOV SS / POP SS).
//  - Opcode 0F FF ib is a host callback: the CPU calls the Callback with ib, then continues after the
//    3-byte instruction. The host BIOS/DOS stubs in ROM are built from it (e.g. `0F FF 21 / IRET`).
//  - Code hooks (for the incremental native port) run instead of the instruction at a linear address.
//    A hook must leave CS:IP at the continuation (typically by emulating RET/RETF itself).
//  - Cycle counts approximate an 80286; they set the emulated machine speed, which drives the game's
//    own frame-rate measurement, so they matter for 1:1 timing.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>

#include "host/memory.h"

namespace vette::host {

enum Reg16 : uint8_t { AX, CX, DX, BX, SP, BP, SI, DI };  // x86 encoding order
enum SegReg : uint8_t { ES, CS, SS, DS };                  // x86 encoding order

namespace flag {
constexpr uint16_t CF = 0x0001, PF = 0x0004, AF = 0x0010, ZF = 0x0040, SF = 0x0080;
constexpr uint16_t TF = 0x0100, IF = 0x0200, DF = 0x0400, OF = 0x0800;
} // namespace flag

struct Registers {
    std::array<uint16_t, 8> r{};  // indexed by Reg16
    std::array<uint16_t, 4> s{};  // indexed by SegReg
    uint16_t ip = 0;
    uint16_t flags = 0x0002;

    uint8_t lo(Reg16 i) const { return static_cast<uint8_t>(r[i]); }
    uint8_t hi(Reg16 i) const { return static_cast<uint8_t>(r[i] >> 8); }
    void set_lo(Reg16 i, uint8_t v) { r[i] = static_cast<uint16_t>((r[i] & 0xFF00) | v); }
    void set_hi(Reg16 i, uint8_t v) { r[i] = static_cast<uint16_t>((r[i] & 0x00FF) | (v << 8)); }
    bool get_flag(uint16_t f) const { return (flags & f) != 0; }
    void set_flag(uint16_t f, bool on) { flags = static_cast<uint16_t>(on ? (flags | f) : (flags & ~f)); }
};

class InterruptController {
public:
    virtual ~InterruptController() = default;
    virtual bool irq_pending() const = 0;
    virtual uint8_t irq_acknowledge() = 0;  // returns the interrupt vector
};

class Cpu {
public:
    using Callback = std::function<void(Cpu&, uint8_t id)>;
    using CodeHook = std::function<void(Cpu&)>;

    Cpu(Memory& mem, IoBus& io);
    ~Cpu();
    Cpu(const Cpu&) = delete;
    Cpu& operator=(const Cpu&) = delete;

    Registers regs;

    void reset();
    void set_interrupt_controller(InterruptController* pic) { pic_ = pic; }
    void set_callback(Callback cb) { callback_ = std::move(cb); }
    void set_code_hook(uint32_t linear, CodeHook hook);
    void clear_code_hook(uint32_t linear);

    // Executes whole instructions until at least `cycles` have elapsed and returns the cycles executed.
    // While halted with no deliverable IRQ, idle time passes: the rest of the slice is consumed (the
    // result is then the full `cycles`), and a later run() wakes the CPU when an IRQ is pending and
    // IF=1. The only early return is request_stop() (e.g. from an IoBus handler): run() returns after
    // the current instruction, its cycles included. REP string instructions are interruptible between
    // iterations; they yield at the end of the slice, on request_stop() or for a pending IRQ, and
    // resume from their first prefix.
    int64_t run(int64_t cycles);
    void request_stop() { stop_ = true; }

    uint64_t total_cycles() const { return total_cycles_; }
    bool halted() const { return halted_; }

    // Push FLAGS/CS/IP, clear IF and TF, load CS:IP from the IVT. Used for IRQs and by HLE code.
    void interrupt(uint8_t vector);

    // Helpers for HLE services.
    static uint32_t linear(uint16_t seg, uint16_t off) { return ((static_cast<uint32_t>(seg) << 4) + off) & Memory::kMask; }
    void push16(uint16_t v);
    uint16_t pop16();
    Memory& memory() { return mem_; }
    IoBus& io() { return io_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Memory& mem_;
    IoBus& io_;
    InterruptController* pic_ = nullptr;
    Callback callback_;
    uint64_t total_cycles_ = 0;
    bool halted_ = false;
    bool stop_ = false;
};

} // namespace vette::host
