#pragma once
// Helpers for native ports while they still run against the emulated machine (Phase 2):
// segment:offset memory access with the 8086's 64 KB wrap, string-instruction stepping, 286 division,
// and the FLAGS results of the arithmetic the original code performed last, so register and flag side
// effects match exactly. Instruction semantics follow the emulator (src/host/cpu.cpp, validated against
// a real 80286), including its choices for flags the 286 leaves undefined.

#include <bit>
#include <cstdint>

#include "host/cpu.h"

namespace vette::game {

using host::Cpu;
using host::Memory;
using host::Registers;

// Emulator segment of an image-relative segment (VETTE.EXE is loaded at 1000h).
constexpr uint16_t emu_seg(uint16_t image_seg) { return static_cast<uint16_t>(image_seg + 0x1000); }
constexpr uint16_t kDataSeg = emu_seg(0x124A);

inline uint8_t rd8(Memory& m, uint16_t seg, uint16_t off) { return m.read8(Cpu::linear(seg, off)); }
inline uint16_t rd16(Memory& m, uint16_t seg, uint16_t off) {
    return static_cast<uint16_t>(rd8(m, seg, off) | rd8(m, seg, static_cast<uint16_t>(off + 1)) << 8);
}
inline void wr8(Memory& m, uint16_t seg, uint16_t off, uint8_t v) { m.write8(Cpu::linear(seg, off), v); }
inline void wr16(Memory& m, uint16_t seg, uint16_t off, uint16_t v) {
    wr8(m, seg, off, static_cast<uint8_t>(v));
    wr8(m, seg, static_cast<uint16_t>(off + 1), static_cast<uint8_t>(v >> 8));
}

// 16-bit offset arithmetic (wraps within the segment).
constexpr uint16_t off16(uint16_t base, int delta) { return static_cast<uint16_t>(base + delta); }

// LODSW / STOSW without a segment override: LODSW reads DS:SI, STOSW writes ES:DI, and each steps its
// index register by 2 in the direction DF selects.
inline uint16_t string_step(const Registers& r) { return (r.flags & host::flag::DF) ? 0xFFFE : 0x0002; }
inline uint16_t lodsw(Registers& r, Memory& m) {
    const uint16_t v = rd16(m, r.s[host::DS], r.r[host::SI]);
    r.r[host::SI] = static_cast<uint16_t>(r.r[host::SI] + string_step(r));
    return v;
}
inline void stosw(Registers& r, Memory& m, uint16_t v) {
    wr16(m, r.s[host::ES], r.r[host::DI], v);
    r.r[host::DI] = static_cast<uint16_t>(r.r[host::DI] + string_step(r));
}

// CWD: the sign of a word, as the high word of its 32-bit extension.
constexpr uint16_t cwd(uint16_t v) { return (v & 0x8000) ? 0xFFFF : 0x0000; }

// DIV / IDIV r/m16 of DX:AX. `ok` is false where the CPU raises INT 0 (divide error) instead; the
// instruction then leaves AX and DX unchanged.
struct DivResult {
    bool ok;
    uint16_t quot, rem;
};

// DIV: unsigned; faults on a zero divisor or a quotient above FFFFh.
constexpr DivResult div32(uint16_t dx, uint16_t ax, uint16_t d) {
    const uint32_t n = uint32_t{dx} << 16 | ax;
    if (d == 0 || n / d > 0xFFFF) {
        return {false, 0, 0};
    }
    return {true, static_cast<uint16_t>(n / d), static_cast<uint16_t>(n % d)};
}

// IDIV: signed, truncating toward zero; the remainder takes the dividend's sign. Faults on a zero
// divisor or a quotient outside -8000h..7FFFh, except for the 286 quirk measured on hardware
// (cpu.cpp idiv_quirk): when the true quotient is negative, |n| >> 15 == |d| + 8000h and
// (|n| mod 8000h) < |d|, the 286 returns quotient -8000h and remainder +-(|n| mod 8000h).
constexpr DivResult idiv32(uint16_t dx, uint16_t ax, uint16_t d) {
    const int64_t n = static_cast<int32_t>(uint32_t{dx} << 16 | ax);
    const int64_t dv = static_cast<int16_t>(d);
    if (dv == 0) {
        return {false, 0, 0};
    }
    int64_t q = n / dv;
    int64_t r = n % dv;
    if (q > 0x7FFF || q < -0x8000) {
        constexpr int64_t kLim = 0x8000;
        const int64_t an = n < 0 ? -n : n;
        const int64_t ad = dv < 0 ? -dv : dv;
        if ((n < 0) == (dv < 0) || (an >> 15) - ad != kLim || (an & (kLim - 1)) >= ad) {
            return {false, 0, 0};
        }
        q = -kLim;
        r = n < 0 ? -(an & (kLim - 1)) : (an & (kLim - 1));
    }
    return {true, static_cast<uint16_t>(q), static_cast<uint16_t>(r)};
}

namespace flags {

using namespace host::flag;
constexpr uint16_t kArith = CF | PF | AF | ZF | SF | OF;

inline uint16_t szp16(uint16_t r) {
    uint16_t f = 0;
    if (r == 0) f |= ZF;
    if (r & 0x8000) f |= SF;
    if ((std::popcount(static_cast<uint8_t>(r)) & 1) == 0) f |= PF;  // parity of the low byte
    return f;
}

inline void set(Registers& r, uint16_t f) { r.flags = static_cast<uint16_t>((r.flags & ~kArith) | f); }

// FLAGS after ADD a, b.
inline void add16(Registers& r, uint16_t a, uint16_t b) {
    const uint32_t sum = uint32_t{a} + b;
    const auto res = static_cast<uint16_t>(sum);
    uint16_t f = szp16(res);
    if (sum > 0xFFFF) f |= CF;
    if ((a ^ b ^ res) & 0x10) f |= AF;
    if ((a ^ res) & (b ^ res) & 0x8000) f |= OF;
    set(r, f);
}

// FLAGS after SUB a, b (and CMP a, b).
inline void sub16(Registers& r, uint16_t a, uint16_t b) {
    const auto res = static_cast<uint16_t>(a - b);
    uint16_t f = szp16(res);
    if (a < b) f |= CF;
    if ((a ^ b ^ res) & 0x10) f |= AF;
    if ((a ^ b) & (a ^ res) & 0x8000) f |= OF;
    set(r, f);
}

// FLAGS after NEG x (which is SUB 0, x).
inline void neg16(Registers& r, uint16_t x) { sub16(r, 0, x); }

// FLAGS after DEC x: those of SUB x, 1, except that CF is kept.
inline void dec16(Registers& r, uint16_t x) {
    const auto cf = static_cast<uint16_t>(r.flags & CF);
    sub16(r, x, 1);
    r.flags = static_cast<uint16_t>((r.flags & ~CF) | cf);
}

// FLAGS after SHL x, 1: SF/ZF/PF of the result, CF = the bit shifted out, OF = CF xor the result's
// sign bit. AF is undefined on the 286; the emulator clears it.
inline void shl1(Registers& r, uint16_t x) {
    const auto res = static_cast<uint16_t>(x << 1);
    uint16_t f = szp16(res);
    if (x & 0x8000) f |= CF;
    if ((x ^ res) & 0x8000) f |= OF;
    set(r, f);
}

// FLAGS after RCL x, 1: CF = the bit rotated out, OF = CF xor the result's sign bit (bit 14 of x, so
// neither depends on the incoming CF). SF, ZF, PF and AF are kept.
inline void rcl1(Registers& r, uint16_t x) {
    auto f = static_cast<uint16_t>(r.flags & ~(CF | OF));
    if (x & 0x8000) f |= CF;
    if ((x ^ (x << 1)) & 0x8000) f |= OF;
    r.flags = static_cast<uint16_t>(f);
}

} // namespace flags
} // namespace vette::game
