#include "game/projection.h"

#include "game/x86.h"

namespace vette::game {

using host::AX;
using host::BP;
using host::BX;
using host::CS;
using host::CX;
using host::DI;
using host::DX;
using host::ES;
using host::SI;

namespace {

constexpr uint16_t kCentreX = 0x3169, kCentreY = 0x316B;  // viewport centre (set_viewport)
constexpr uint16_t kRecords = 0x1A86;  // {sx, sy, flags, vertex ptr} per vertex; 32-bit copies at +400h
constexpr uint16_t kCounter = 0xE026;  // vertices left
constexpr uint16_t kClipFlag = 0x9C7A;  // CS byte: FFh if any vertex needs clipping (poly_gather_clip)
constexpr uint16_t kDivErrorAx = 0x7FFF;  // AX after the game's INT 0 handler

uint16_t u(int16_t v) { return static_cast<uint16_t>(v); }
int16_t s(uint16_t v) { return static_cast<int16_t>(v); }
int32_t join(uint16_t hi, uint16_t lo) { return static_cast<int32_t>(uint32_t{hi} << 16 | lo); }

// NOT AX; NOT DX; ADD AX,1; ADC DX,0: the two's complement of DX:AX.
void neg32(uint16_t& dx, uint16_t& ax) {
    const uint32_t v = 0u - (uint32_t{dx} << 16 | ax);
    dx = static_cast<uint16_t>(v >> 16);
    ax = static_cast<uint16_t>(v);
}

// The three stages of one coordinate, as plain values. They also return the registers the original
// leaves behind and how many divide errors (INT 0) it takes, for the adapter.

// Stage 1 (x: A6A8-A6B1, y: A72E-A737): CWD; MOV DL,AH; MOV AH,AL; XOR AL,AL forms DX:AX = v*256
// sign-extended, kept in BP (low word); IDIV z. A divide error goes to the game's INT 0 handler
// (int00_div_overflow 3009:2565), which returns AX = 7FFFh past the 2-byte IDIV with DX unchanged.
struct Divided {
    uint16_t ax, dx;  // quotient and remainder; or 7FFFh and the dividend's high word
    uint16_t bp;      // the dividend's low word
    bool error;
};
Divided divide_by_depth(uint16_t v, uint16_t z) {
    const auto lo = static_cast<uint16_t>(v << 8);
    const auto hi = static_cast<uint16_t>((cwd(v) & 0xFF00) | (v >> 8));
    const DivResult q = idiv32(hi, lo, z);
    if (!q.ok) {
        return {kDivErrorAx, hi, lo, true};
    }
    return {q.quot, q.rem, lo, false};
}

// Stage 2a, any quotient but 7FFFh (x: A716-A71F, y: A79B-A7A4): CWD; ADD AX,centre; STOSW. On signed
// overflow (JO) the record is flagged 1 and ADC DX,0 widens DX:AX with the carry; otherwise CWD.
struct Narrow {
    uint16_t ax, dx;
    bool overflow;
};
Narrow add_centre(uint16_t q, uint16_t centre) {
    const uint32_t sum = uint32_t{q} + centre;
    const auto ax = static_cast<uint16_t>(sum);
    if ((q ^ ax) & (centre ^ ax) & 0x8000) {
        return {ax, static_cast<uint16_t>(cwd(q) + (sum >> 16)), true};
    }
    return {ax, cwd(ax), false};
}

// Stage 2b, quotient 7FFFh (x: A6B8-A707, y: A73E-A78D), whether from a divide error or a real 7FFFh
// (then DX is the remainder, not the dividend's high word, and the result is meaningless; reproduced
// as is). |dividend| is divided by three unsigned DIVs: low word, high word, then the high word's
// remainder; the sign is taken from the vertex coordinate, re-read; the centre is added in 32 bits.
// Any of the DIVs can overflow too and goes through the same INT 0 handler.
struct Wide {
    uint16_t ax, dx, bp;
    int div_errors;
};
Wide recompute_wide(const Divided& d, uint16_t z, ScreenAxis axis, uint16_t v_again, uint16_t centre) {
    int errors = 0;
    const auto div = [&](uint16_t& dx, uint16_t& ax) {  // DIV CX
        const DivResult q = div32(dx, ax, z);
        if (q.ok) {
            ax = q.quot;
            dx = q.rem;
        } else {
            ax = kDivErrorAx;
            ++errors;
        }
    };
    uint16_t ax = d.bp, dx = d.dx;  // MOV AX,BP
    if (dx & 0x8000) {              // OR DX,DX; JGE
        neg32(dx, ax);              // (x uses ADC AX,1, after OR cleared CF: the same)
    }
    uint16_t bp = dx;
    // x: XOR DX,DX. y: CWD, so the DIV overflows whenever bit 15 of the low word is set.
    dx = axis == ScreenAxis::X ? 0 : cwd(ax);
    div(dx, ax);
    const uint16_t si = ax;
    ax = bp;
    dx = cwd(ax);
    div(dx, ax);
    bp = ax;
    ax = 0;
    div(dx, ax);
    const uint32_t low = uint32_t{ax} + si;  // XOR DX,DX; ADD AX,SI; ADC DX,BP
    ax = static_cast<uint16_t>(low);
    dx = static_cast<uint16_t>(bp + (low >> 16));
    if (v_again & 0x8000) {  // XOR SI,SI; CMP [BX+coord],SI; JGE
        neg32(dx, ax);
    }
    const uint32_t sum = uint32_t{ax} + centre;  // ADD AX,centre; ADC DX,SI (0)
    ax = static_cast<uint16_t>(sum);
    dx = static_cast<uint16_t>(dx + (sum >> 16));
    return {ax, dx, bp, errors};
}

// Where each coordinate's code reads and writes. DI points at the coordinate's 16-bit field when its
// code starts (record +0 for x, +2 for y).
struct AxisLayout {
    ScreenAxis axis;
    uint16_t coord;   // offset of the coordinate in the vertex
    uint16_t centre;  // viewport centre (ES)
    int flags_at;     // the record's flags word, relative to DI after the STOSW / ADD DI,2
    int wide_at;      // the 32-bit copy, relative to DI after the STOSW / ADD DI,2
};
constexpr AxisLayout kAxisX{ScreenAxis::X, 0, kCentreX, 2, 0x3FE};
constexpr AxisLayout kAxisY{ScreenAxis::Y, 2, kCentreY, 0, 0x400};

} // namespace

ScreenCoord project_coord(int16_t v, int16_t z, int16_t centre, ScreenAxis axis) {
    const Divided d = divide_by_depth(u(v), u(z));
    if (d.ax != kDivErrorAx) {
        const Narrow n = add_centre(d.ax, u(centre));
        return {join(n.dx, n.ax), s(n.ax), true, n.overflow};
    }
    const Wide w = recompute_wide(d, u(z), axis, u(v), u(centre));
    return {join(w.dx, w.ax), 0, false, true};
}

void project_vertices(Cpu& cpu) {
    Registers& r = cpu.regs;
    Memory& m = cpu.memory();
    const uint16_t es = r.s[ES];
    const uint16_t cs = r.s[CS];
    // The INT 0 handler also acknowledges the PIC (MOV AL,20h; OUT 20h,AL) each time it runs.
    const auto int0_eoi = [&](int times) {
        for (int i = 0; i < times; ++i) {
            cpu.io().out8(0x20, 0x20);
        }
    };

    // One coordinate, with the memory accesses in the original's order. CX = z.
    const auto project_axis = [&](const AxisLayout& a, uint16_t bx, uint16_t v) {
        const uint16_t z = r.r[CX];
        const Divided d = divide_by_depth(v, z);
        int0_eoi(d.error ? 1 : 0);
        r.r[BP] = d.bp;
        if (d.ax != kDivErrorAx) {
            const Narrow n = add_centre(d.ax, rd16(m, es, a.centre));
            stosw(r, m, n.ax);
            if (n.overflow) {
                wr16(m, es, off16(r.r[DI], a.flags_at), 1);
            }
            r.r[AX] = n.ax;
            r.r[DX] = n.dx;
        } else {
            wr8(m, cs, kClipFlag, 0xFF);
            wr16(m, es, off16(r.r[DI], a.flags_at + 2), 1);
            const uint16_t v_again = rd16(m, es, off16(bx, a.coord));
            const Wide w = recompute_wide(d, z, a.axis, v_again, rd16(m, es, a.centre));
            int0_eoi(w.div_errors);
            r.r[AX] = w.ax;
            r.r[DX] = w.dx;
            r.r[BP] = w.bp;
            r.r[SI] = 0;
            r.r[DI] = off16(r.r[DI], 2);
        }
        wr16(m, es, off16(r.r[DI], a.wide_at), r.r[AX]);
        wr16(m, es, off16(r.r[DI], a.wide_at + 2), r.r[DX]);
    };

    wr16(m, es, kCounter, r.r[DX]);
    wr8(m, cs, kClipFlag, r.hi(DX));
    r.r[DI] = kRecords;
    for (;;) {  // A692
        const uint16_t di = r.r[DI], bx = r.r[BX];
        wr16(m, es, off16(di, 4), 0);
        wr16(m, es, off16(di, 6), bx);
        r.r[AX] = rd16(m, es, bx);
        r.r[CX] = rd16(m, es, off16(bx, 4));
        if (s(r.r[CX]) < 1) {  // behind the near plane (A66B): flag 2, no projection
            wr8(m, cs, kClipFlag, 0xFF);
            wr16(m, es, off16(di, 4), 2);
            r.r[DI] = off16(di, 8);
        } else {
            project_axis(kAxisX, bx, r.r[AX]);
            project_axis(kAxisY, bx, rd16(m, es, off16(bx, 2)));
            r.r[DI] = off16(r.r[DI], 4);
        }
        // ADD BX,6; DEC word ES:[E026]; JNZ: the last two flag operations.
        flags::add16(r, bx, 6);
        r.r[BX] = off16(bx, 6);
        const uint16_t count = rd16(m, es, kCounter);
        flags::dec16(r, count);
        wr16(m, es, kCounter, off16(count, -1));
        if (count == 1) {
            break;
        }
    }
}

} // namespace vette::game
