// Real-mode 80286 interpreter. See cpu.h for the contract and cpu_timing.h for the cycle model.
//
// Behaviour follows the 80286 in real mode, validated against the SingleStepTests 80286 suite
// (tests/cpu_singlestep.cpp), with one deliberate difference required by the host: word accesses at
// offset FFFFh wrap within the segment like an 8086, where a real 286 raises #GP (INT 0Dh).
// 286 specifics: PUSH SP pushes the value before the push; FLAGS bits 12-15 read as 0; shift counts
// are masked to 5 bits; faults (#DE, #UD, BOUND) push the address of the faulting instruction's first
// prefix; IDIV accepts quotients of -80h / -8000h; undefined opcodes raise INT 6.

#include "host/cpu.h"

#include <bit>
#include <unordered_map>
#include <vector>

#include "host/cpu_timing.h"

namespace vette::host {

namespace {

static_assert(std::endian::native == std::endian::little, "8-bit register access assumes little endian");

using namespace flag;

constexpr uint16_t kArith = CF | PF | AF | ZF | SF | OF;
constexpr uint16_t kLoadable = CF | PF | AF | ZF | SF | TF | IF | DF | OF;  // 0FD5h: what POPF/IRET keep
constexpr uint16_t kFixed = 0x0002;                                       // bit 1 always reads 1
constexpr uint8_t kNoSeg = 0xFF;
constexpr int kMaxPrefixes = 10;  // the 286 limits instructions to 10 bytes (INT 0Dh beyond that)

constexpr uint16_t u16(unsigned v) { return static_cast<uint16_t>(v); }
constexpr uint8_t u8(unsigned v) { return static_cast<uint8_t>(v); }

// SF, ZF and PF for every byte value.
struct SzpTable {
    std::array<uint8_t, 256> v{};
    constexpr SzpTable() {
        for (unsigned i = 0; i < 256; ++i) {
            unsigned f = (std::popcount(i) & 1) ? 0u : PF;
            if (i == 0) f |= ZF;
            if (i & 0x80) f |= SF;
            v[i] = u8(f);
        }
    }
};
constexpr SzpTable kSzp;

template <bool W>
struct Width;
template <>
struct Width<false> {
    static constexpr unsigned kMask = 0xFF, kMsb = 0x80, kBits = 8, kBytes = 1;
};
template <>
struct Width<true> {
    static constexpr unsigned kMask = 0xFFFF, kMsb = 0x8000, kBits = 16, kBytes = 2;
};

enum StrOp : uint8_t { kMovs, kCmps, kScas, kLods, kStos, kIns, kOuts };

}  // namespace

struct Cpu::Impl {
    explicit Impl(Cpu& cpu) : c(cpu), R(cpu.regs), M(cpu.mem_), hook_bits_(Memory::kSize / 64, 0) {}

    Cpu& c;
    Registers& R;
    Memory& M;

    // Decode state of the current instruction.
    uint16_t ip0_ = 0;       // IP of its first byte (first prefix): faults and REP restarts return here
    uint8_t seg_ = kNoSeg;   // segment override
    uint8_t rep_ = 0;        // 0, F2h (REPNE) or F3h (REP/REPE)
    uint8_t modrm_ = 0;
    bool mem_ = false;       // the modrm operand is in memory
    uint8_t eseg_ = 0;       // effective address segment (override applied)
    uint16_t eoff_ = 0;      // effective address offset
    bool no_trap_ = false;   // the instruction suppresses the single-step trap (INT, faults, HLT)

    // State across instructions.
    bool inhibit_ = false;   // interrupt shadow (STI, MOV SS, POP SS) for the next instruction
    uint64_t end_ = 0;       // end of the current run() slice, for REP string yields

    // Code hooks and watches: a bitmap over the 1 MB linear space makes the per-instruction check
    // one bit test. A bit is set while either map has an entry at that address.
    std::vector<uint64_t> hook_bits_;
    std::unordered_map<uint32_t, std::shared_ptr<CodeHook>> hooks_;
    std::unordered_map<uint32_t, std::shared_ptr<Watch>> watches_;

    void update_hook_bit(uint32_t lin) {
        const uint64_t bit = uint64_t{1} << (lin & 63);
        if (hooks_.count(lin) || watches_.count(lin)) {
            hook_bits_[lin >> 6] |= bit;
        } else {
            hook_bits_[lin >> 6] &= ~bit;
        }
    }

    // 286 system state (real mode only).
    uint16_t msw_ = 0xFFF0;
    uint32_t gdt_base_ = 0, idt_base_ = 0;
    uint16_t gdt_limit_ = 0, idt_limit_ = 0x3FF;

    void reset() {
        ip0_ = 0;
        inhibit_ = false;
        msw_ = 0xFFF0;
        gdt_base_ = idt_base_ = 0;
        gdt_limit_ = 0;
        idt_limit_ = 0x3FF;
    }

    void add(int cycles) { c.total_cycles_ += static_cast<uint64_t>(cycles); }

    // --- Memory --------------------------------------------------------------------------------
    uint32_t base(unsigned s) const { return static_cast<uint32_t>(R.s[s]) << 4; }

    uint8_t fetch8() {
        const uint8_t v = M.read8(base(CS) + R.ip);
        R.ip = u16(R.ip + 1);
        return v;
    }
    uint16_t fetch16() {
        const unsigned lo = fetch8();
        return u16(lo | (static_cast<unsigned>(fetch8()) << 8));
    }

    uint8_t rd8(unsigned s, uint16_t off) { return M.read8(base(s) + off); }
    uint16_t rd16(unsigned s, uint16_t off) {
        const uint32_t b = base(s);
        if (off != 0xFFFF) return M.read16(b + off);
        return u16(M.read8(b + 0xFFFF) | (M.read8(b) << 8));  // wraps within the segment
    }
    void wr8(unsigned s, uint16_t off, unsigned v) { M.write8(base(s) + off, u8(v)); }
    void wr16(unsigned s, uint16_t off, unsigned v) {
        const uint32_t b = base(s);
        if (off != 0xFFFF) {
            M.write16(b + off, u16(v));
        } else {
            M.write8(b + 0xFFFF, u8(v));
            M.write8(b, u8(v >> 8));
        }
    }

    void push(unsigned v) {
        R.r[SP] = u16(R.r[SP] - 2);
        wr16(SS, R.r[SP], v);
    }
    uint16_t pop() {
        const uint16_t v = rd16(SS, R.r[SP]);
        R.r[SP] = u16(R.r[SP] + 2);
        return v;
    }

    // --- Operands ------------------------------------------------------------------------------
    uint8_t& r8(unsigned i) { return reinterpret_cast<uint8_t*>(R.r.data())[((i & 3) << 1) | (i >> 2)]; }

    template <bool W>
    unsigned reg(unsigned i) {
        if constexpr (W) return R.r[i];
        else return r8(i);
    }
    template <bool W>
    void set_reg(unsigned i, unsigned v) {
        if constexpr (W) R.r[i] = u16(v);
        else r8(i) = u8(v);
    }
    template <bool W>
    unsigned rd(unsigned s, uint16_t off) {
        if constexpr (W) return rd16(s, off);
        else return rd8(s, off);
    }
    template <bool W>
    void wr(unsigned s, uint16_t off, unsigned v) {
        if constexpr (W) wr16(s, off, v);
        else wr8(s, off, v);
    }
    template <bool W>
    unsigned imm() {
        if constexpr (W) return fetch16();
        else return fetch8();
    }

    unsigned regf() const { return (modrm_ >> 3) & 7; }
    unsigned data_seg() const { return seg_ != kNoSeg ? seg_ : unsigned{DS}; }

    void decode_modrm() {
        modrm_ = fetch8();
        const unsigned mod = modrm_ >> 6;
        if (mod == 3) {
            mem_ = false;
            return;
        }
        mem_ = true;
        const unsigned rm = modrm_ & 7;
        unsigned off;
        unsigned s = DS;
        switch (rm) {
        case 0: off = R.r[BX] + R.r[SI]; break;
        case 1: off = R.r[BX] + R.r[DI]; break;
        case 2: off = R.r[BP] + R.r[SI]; s = SS; break;
        case 3: off = R.r[BP] + R.r[DI]; s = SS; break;
        case 4: off = R.r[SI]; break;
        case 5: off = R.r[DI]; break;
        case 6:
            if (mod == 0) {
                eoff_ = fetch16();
                eseg_ = u8(data_seg());
                return;
            }
            off = R.r[BP];
            s = SS;
            break;
        default: off = R.r[BX]; break;
        }
        if (mod == 1) {
            off += static_cast<unsigned>(static_cast<int8_t>(fetch8()));
            if (rm < 4) add(timing::kEaBaseIndexDisp);
        } else if (mod == 2) {
            off += fetch16();
            if (rm < 4) add(timing::kEaBaseIndexDisp);
        }
        eoff_ = u16(off);
        eseg_ = u8(seg_ != kNoSeg ? seg_ : s);
    }

    template <bool W>
    unsigned get_rm() {
        return mem_ ? rd<W>(eseg_, eoff_) : reg<W>(modrm_ & 7);
    }
    template <bool W>
    void set_rm(unsigned v) {
        if (mem_) wr<W>(eseg_, eoff_, v);
        else set_reg<W>(modrm_ & 7, v);
    }

    // --- Flags and ALU -------------------------------------------------------------------------
    template <bool W>
    static unsigned szp(unsigned r) {
        if constexpr (W) {
            return (kSzp.v[r & 0xFF] & PF) | ((r & 0xFFFF) == 0 ? ZF : 0u) | ((r >> 8) & SF);
        } else {
            return kSzp.v[r & 0xFF];
        }
    }
    void set_flags(unsigned mask, unsigned v) { R.flags = u16((R.flags & ~mask) | v); }
    bool cf() const { return (R.flags & CF) != 0; }

    template <bool W>
    unsigned op_add(unsigned a, unsigned b, unsigned carry) {
        const unsigned r = a + b + carry;
        unsigned f = szp<W>(r) | ((r >> Width<W>::kBits) & 1) | ((a ^ b ^ r) & AF);
        if ((~(a ^ b) & (a ^ r)) & Width<W>::kMsb) f |= OF;
        set_flags(kArith, f);
        return r & Width<W>::kMask;
    }
    template <bool W>
    unsigned op_sub(unsigned a, unsigned b, unsigned borrow) {
        const unsigned r = a - b - borrow;
        unsigned f = szp<W>(r) | ((r >> Width<W>::kBits) & 1) | ((a ^ b ^ r) & AF);
        if (((a ^ b) & (a ^ r)) & Width<W>::kMsb) f |= OF;
        set_flags(kArith, f);
        return r & Width<W>::kMask;
    }
    template <bool W>
    unsigned op_logic(unsigned r) {
        r &= Width<W>::kMask;
        set_flags(kArith, szp<W>(r));
        return r;
    }
    template <bool W>
    unsigned alu(unsigned op, unsigned a, unsigned b) {
        switch (op) {
        case 0: return op_add<W>(a, b, 0);
        case 1: return op_logic<W>(a | b);
        case 2: return op_add<W>(a, b, R.flags & CF);
        case 3: return op_sub<W>(a, b, R.flags & CF);
        case 4: return op_logic<W>(a & b);
        case 5: return op_sub<W>(a, b, 0);
        case 6: return op_logic<W>(a ^ b);
        default: op_sub<W>(a, b, 0); return a;  // CMP
        }
    }
    template <bool W>
    unsigned op_inc(unsigned v) {
        const unsigned keep = R.flags & CF;
        v = op_add<W>(v, 1, 0);
        set_flags(CF, keep);
        return v;
    }
    template <bool W>
    unsigned op_dec(unsigned v) {
        const unsigned keep = R.flags & CF;
        v = op_sub<W>(v, 1, 0);
        set_flags(CF, keep);
        return v;
    }

    // Shift/rotate group (C0/C1/D0-D3). The 286 masks the count to 5 bits and iterates; the flags are
    // those of the last single-bit step. A zero count changes nothing.
    template <bool W>
    unsigned shift(unsigned op, unsigned v, unsigned n) {
        constexpr unsigned kTop = Width<W>::kBits - 1;
        constexpr unsigned kMask = Width<W>::kMask;
        unsigned cy = R.flags & CF;
        unsigned o;
        switch (op) {
        case 0:  // ROL
            for (unsigned i = 0; i < n; ++i) {
                cy = (v >> kTop) & 1;
                v = ((v << 1) | cy) & kMask;
            }
            o = ((v >> kTop) ^ cy) & 1;
            set_flags(CF | OF, cy | (o ? OF : 0u));
            return v;
        case 1:  // ROR
            for (unsigned i = 0; i < n; ++i) {
                cy = v & 1;
                v = (v >> 1) | (cy << kTop);
            }
            o = ((v >> kTop) ^ (v >> (kTop - 1))) & 1;
            set_flags(CF | OF, cy | (o ? OF : 0u));
            return v;
        case 2:  // RCL
            for (unsigned i = 0; i < n; ++i) {
                const unsigned out = (v >> kTop) & 1;
                v = ((v << 1) | cy) & kMask;
                cy = out;
            }
            o = ((v >> kTop) ^ cy) & 1;
            set_flags(CF | OF, cy | (o ? OF : 0u));
            return v;
        case 3:  // RCR
            for (unsigned i = 0; i < n; ++i) {
                const unsigned out = v & 1;
                v = (v >> 1) | (cy << kTop);
                cy = out;
            }
            o = ((v >> kTop) ^ (v >> (kTop - 1))) & 1;
            set_flags(CF | OF, cy | (o ? OF : 0u));
            return v;
        case 4:  // SHL
        case 6:  // SAL (undocumented alias)
            for (unsigned i = 0; i < n; ++i) {
                cy = (v >> kTop) & 1;
                v = (v << 1) & kMask;
            }
            o = ((v >> kTop) ^ cy) & 1;
            break;
        case 5:  // SHR
            o = 0;
            for (unsigned i = 0; i < n; ++i) {
                o = (v >> kTop) & 1;
                cy = v & 1;
                v >>= 1;
            }
            break;
        default:  // SAR
            for (unsigned i = 0; i < n; ++i) {
                cy = v & 1;
                v = (v >> 1) | (v & Width<W>::kMsb);
            }
            o = 0;
            break;
        }
        set_flags(kArith, szp<W>(v) | cy | (o ? OF : 0u));
        return v;
    }

    template <bool W>
    void grp2(unsigned count, int base_cost) {
        const unsigned n = count & 0x1F;
        add(base_cost + static_cast<int>(n) * timing::kShiftPerBit);
        const unsigned v = get_rm<W>();
        if (n == 0) return;
        set_rm<W>(shift<W>(regf(), v, n));
    }

    // --- Faults and interrupts -----------------------------------------------------------------
    void fault(uint8_t vector) {
        R.ip = ip0_;
        c.interrupt(vector);
        add(timing::kException);
        no_trap_ = true;
    }
    void soft_int(uint8_t vector) {
        c.interrupt(vector);
        no_trap_ = true;
    }
    void trap() {
        if (no_trap_ || inhibit_) return;
        c.interrupt(1);
        add(timing::kException);
    }

    bool should_yield() const {
        return c.total_cycles_ >= end_ || c.stop_ ||
               (!c.irq_inhibit_ && (R.flags & IF) && c.pic_ && c.pic_->irq_pending());
    }

    // Runs the watch and/or hook at CS:IP. Returns true if execution must not continue with the
    // instruction at the original CS:IP (a hook replaced it, or a watch redirected execution).
    bool run_hook() {
        const uint32_t lin = (base(CS) + R.ip) & Memory::kMask;
        if (((hook_bits_[lin >> 6] >> (lin & 63)) & 1) == 0) return false;
        if (const auto w = watches_.find(lin); w != watches_.end()) {
            const std::shared_ptr<Watch> watch = w->second;  // the watch may remove itself
            const uint16_t cs = R.s[CS], ip = R.ip;
            (*watch)(c);
            if (R.s[CS] != cs || R.ip != ip) return true;
        }
        const auto it = hooks_.find(lin);
        if (it == hooks_.end()) return false;
        const std::shared_ptr<CodeHook> hook = it->second;  // the hook may remove itself
        add(timing::kCodeHook);
        (*hook)(c);
        return true;
    }

    // --- Instruction groups --------------------------------------------------------------------
    template <bool W>
    void alu_rm_r(unsigned op) {
        decode_modrm();
        const unsigned r = alu<W>(op, get_rm<W>(), reg<W>(regf()));
        if (op != 7) set_rm<W>(r);
    }
    template <bool W>
    void alu_r_rm(unsigned op) {
        decode_modrm();
        const unsigned r = alu<W>(op, reg<W>(regf()), get_rm<W>());
        if (op != 7) set_reg<W>(regf(), r);
    }
    template <bool W>
    void alu_acc(unsigned op) {
        const unsigned b = imm<W>();
        const unsigned r = alu<W>(op, reg<W>(AX), b);
        if (op != 7) set_reg<W>(AX, r);
    }

    // 80-83. `sx` sign-extends an 8-bit immediate to 16 bits (83).
    template <bool W>
    void grp1(bool sx) {
        decode_modrm();
        const unsigned op = regf();
        add(mem_ ? timing::kGrp1[op].mem : timing::kGrp1[op].reg);
        const unsigned a = get_rm<W>();
        unsigned b;
        if (sx) b = u16(static_cast<int8_t>(fetch8()));
        else b = imm<W>();
        const unsigned r = alu<W>(op, a, b);
        if (op != 7) set_rm<W>(r);
    }

    // F6/F7: TEST NOT NEG MUL IMUL DIV IDIV
    template <bool W>
    void grp3() {
        decode_modrm();
        const unsigned op = regf();
        const auto& costs = W ? timing::kGrp3w : timing::kGrp3b;
        add(mem_ ? costs[op].mem : costs[op].reg);
        switch (op) {
        case 0:
        case 1: {  // TEST (/1 is an alias)
            const unsigned a = get_rm<W>();
            op_logic<W>(a & imm<W>());
            break;
        }
        case 2: set_rm<W>(~get_rm<W>()); break;
        case 3: set_rm<W>(op_sub<W>(0, get_rm<W>(), 0)); break;
        case 4: {  // MUL
            const unsigned src = get_rm<W>();
            bool hi;
            if constexpr (W) {
                const uint32_t r = static_cast<uint32_t>(R.r[AX]) * src;
                R.r[AX] = u16(r);
                R.r[DX] = u16(r >> 16);
                hi = (r >> 16) != 0;
                set_flags(kArith, szp<true>(r) | (hi ? CF | OF : 0u));
            } else {
                const unsigned r = (R.r[AX] & 0xFF) * src;
                R.r[AX] = u16(r);
                hi = (r >> 8) != 0;
                set_flags(kArith, szp<false>(r) | (hi ? CF | OF : 0u));
            }
            break;
        }
        case 5: {  // IMUL
            const unsigned src = get_rm<W>();
            if constexpr (W) {
                const int32_t r = static_cast<int16_t>(R.r[AX]) * static_cast<int16_t>(src);
                R.r[AX] = u16(static_cast<uint32_t>(r));
                R.r[DX] = u16(static_cast<uint32_t>(r) >> 16);
                const bool hi = r != static_cast<int16_t>(r);
                set_flags(kArith, szp<true>(static_cast<uint32_t>(r)) | (hi ? CF | OF : 0u));
            } else {
                const int r = static_cast<int8_t>(R.r[AX]) * static_cast<int8_t>(src);
                R.r[AX] = u16(static_cast<unsigned>(r));
                const bool hi = r != static_cast<int8_t>(r);
                set_flags(kArith, szp<false>(static_cast<unsigned>(r)) | (hi ? CF | OF : 0u));
            }
            break;
        }
        case 6: {  // DIV
            const unsigned d = get_rm<W>();
            if constexpr (W) {
                const uint32_t n = (static_cast<uint32_t>(R.r[DX]) << 16) | R.r[AX];
                if (d == 0 || n / d > 0xFFFF) return fault(0);
                R.r[AX] = u16(n / d);
                R.r[DX] = u16(n % d);
            } else {
                const unsigned n = R.r[AX];
                if (d == 0 || n / d > 0xFF) return fault(0);
                R.r[AX] = u16(((n % d) << 8) | (n / d));
            }
            break;
        }
        default: {  // IDIV
            const unsigned d = get_rm<W>();
            if constexpr (W) {
                const int64_t n = static_cast<int32_t>((static_cast<uint32_t>(R.r[DX]) << 16) | R.r[AX]);
                const int64_t dv = static_cast<int16_t>(d);
                if (dv == 0) return fault(0);
                int64_t q = n / dv;
                int64_t r = n % dv;
                if (q > 0x7FFF || q < -0x8000) {
                    if (!idiv_quirk(n, dv, 15, q, r)) return fault(0);
                }
                R.r[AX] = u16(static_cast<uint32_t>(q));
                R.r[DX] = u16(static_cast<uint32_t>(r));
            } else {
                const int64_t n = static_cast<int16_t>(R.r[AX]);
                const int64_t dv = static_cast<int8_t>(d);
                if (dv == 0) return fault(0);
                int64_t q = n / dv;
                int64_t r = n % dv;
                if (q > 0x7F || q < -0x80) {
                    if (!idiv_quirk(n, dv, 7, q, r)) return fault(0);
                }
                R.r[AX] = u16(((static_cast<unsigned>(r) & 0xFF) << 8) | (static_cast<unsigned>(q) & 0xFF));
            }
            break;
        }
        }
    }

    // 286 IDIV quirk, measured on hardware (SingleStepTests F6.7): the 286 accepts a quotient of -80h
    // (-8000h), but its check for that case compares only the low 7 (15) bits of |dividend| >> 7 (>> 15)
    // against |divisor|. So an overflowing division with a negative result, |n| >> h == |d| + 2^h and
    // (|n| mod 2^h) < |d|, returns quotient -2^h and remainder +-(|n| mod 2^h) instead of raising #DE.
    // The byte form matches all 5000 hardware tests; the word form is the same rule by analogy
    // (no hardware test reaches it).
    static bool idiv_quirk(int64_t n, int64_t d, int h, int64_t& q, int64_t& r) {
        const int64_t an = n < 0 ? -n : n;
        const int64_t ad = d < 0 ? -d : d;
        const int64_t lim = int64_t{1} << h;
        if ((n < 0) == (d < 0) || (an >> h) - ad != lim || (an & (lim - 1)) >= ad) return false;
        q = -lim;
        r = n < 0 ? -(an & (lim - 1)) : (an & (lim - 1));
        return true;
    }

    void imul3(unsigned a, unsigned b) {
        const int32_t r = static_cast<int16_t>(a) * static_cast<int16_t>(b);
        R.r[regf()] = u16(static_cast<uint32_t>(r));
        const bool hi = r != static_cast<int16_t>(r);
        set_flags(kArith, szp<true>(static_cast<uint32_t>(r)) | (hi ? CF | OF : 0u));
    }

    // --- String instructions -------------------------------------------------------------------
    template <bool W>
    void string_once(StrOp op) {
        const uint16_t d = (R.flags & DF) ? u16(0u - Width<W>::kBytes) : u16(Width<W>::kBytes);
        switch (op) {
        case kMovs:
            wr<W>(ES, R.r[DI], rd<W>(data_seg(), R.r[SI]));
            R.r[SI] = u16(R.r[SI] + d);
            R.r[DI] = u16(R.r[DI] + d);
            break;
        case kCmps: {
            const unsigned a = rd<W>(data_seg(), R.r[SI]);
            op_sub<W>(a, rd<W>(ES, R.r[DI]), 0);
            R.r[SI] = u16(R.r[SI] + d);
            R.r[DI] = u16(R.r[DI] + d);
            break;
        }
        case kScas:
            op_sub<W>(reg<W>(AX), rd<W>(ES, R.r[DI]), 0);
            R.r[DI] = u16(R.r[DI] + d);
            break;
        case kLods:
            set_reg<W>(AX, rd<W>(data_seg(), R.r[SI]));
            R.r[SI] = u16(R.r[SI] + d);
            break;
        case kStos:
            wr<W>(ES, R.r[DI], reg<W>(AX));
            R.r[DI] = u16(R.r[DI] + d);
            break;
        case kIns: {
            unsigned v;
            if constexpr (W) v = c.io_.in16(R.r[DX]);
            else v = c.io_.in8(R.r[DX]);
            wr<W>(ES, R.r[DI], v);
            R.r[DI] = u16(R.r[DI] + d);
            break;
        }
        case kOuts: {
            const unsigned v = rd<W>(data_seg(), R.r[SI]);
            if constexpr (W) c.io_.out16(R.r[DX], u16(v));
            else c.io_.out8(R.r[DX], u8(v));
            R.r[SI] = u16(R.r[SI] + d);
            break;
        }
        }
    }

    // Returns true if it charged its own cycles (REP form).
    template <bool W>
    bool string_op(StrOp op, timing::RepCost cost) {
        if (rep_ == 0) {
            string_once<W>(op);
            return false;
        }
        add(cost.base);
        const bool conditional = op == kCmps || op == kScas;
        while (R.r[CX] != 0) {
            string_once<W>(op);
            R.r[CX] = u16(R.r[CX] - 1);
            add(cost.per);
            if (conditional && (rep_ == 0xF3) != ((R.flags & ZF) != 0)) break;
            if (R.r[CX] == 0) break;
            if (should_yield()) {
                R.ip = ip0_;  // resume (with all prefixes) after the interrupt / next slice
                break;
            }
        }
        return true;
    }

    // --- BCD -----------------------------------------------------------------------------------
    void daa() {
        const unsigned old_al = R.r[AX] & 0xFF;
        const bool old_cf = cf();
        unsigned al = old_al;
        unsigned f = 0;
        if ((al & 0x0F) > 9 || (R.flags & AF)) {
            al += 6;
            f |= AF;
        }
        if (old_al > 0x99 || old_cf) {
            al += 0x60;
            f |= CF;
        }
        al &= 0xFF;
        set_reg<false>(AX, al);
        set_flags(kArith, f | szp<false>(al));
    }
    void das() {
        const unsigned old_al = R.r[AX] & 0xFF;
        const bool old_cf = cf();
        unsigned al = old_al;
        unsigned f = 0;
        if ((al & 0x0F) > 9 || (R.flags & AF)) {
            if (al < 6 || old_cf) f |= CF;
            al -= 6;
            f |= AF;
        }
        if (old_al > 0x99 || old_cf) {
            al -= 0x60;
            f |= CF;
        }
        al &= 0xFF;
        set_reg<false>(AX, al);
        set_flags(kArith, f | szp<false>(al));
    }
    void aaa() {
        if ((R.r[AX] & 0x0F) > 9 || (R.flags & AF)) {
            R.r[AX] = u16(R.r[AX] + 0x106);
            set_flags(AF | CF, AF | CF);
        } else {
            set_flags(AF | CF, 0);
        }
        R.r[AX] &= 0xFF0F;
    }
    void aas() {
        if ((R.r[AX] & 0x0F) > 9 || (R.flags & AF)) {
            R.r[AX] = u16(R.r[AX] - 6);
            R.r[AX] = u16(R.r[AX] - 0x100);
            set_flags(AF | CF, AF | CF);
        } else {
            set_flags(AF | CF, 0);
        }
        R.r[AX] &= 0xFF0F;
    }

    // --- Main decoder --------------------------------------------------------------------------
    void step();
    void op_0f();
    void enter();
};

void Cpu::Impl::enter() {
    const uint16_t size = fetch16();
    const unsigned level = fetch8() & 0x1F;
    push(R.r[BP]);
    const uint16_t frame = R.r[SP];
    if (level > 0) {
        for (unsigned i = 1; i < level; ++i) {
            R.r[BP] = u16(R.r[BP] - 2);
            push(rd16(SS, R.r[BP]));
        }
        push(frame);
    }
    R.r[BP] = frame;
    R.r[SP] = u16(R.r[SP] - size);
    if (level == 1) add(timing::kEnterLevel1 - timing::kOp[0xC8].reg);
    else if (level > 1) add(timing::kEnterBase + timing::kEnterPerLevel * static_cast<int>(level - 1) - timing::kOp[0xC8].reg);
}

void Cpu::Impl::op_0f() {
    const uint8_t op = fetch8();
    switch (op) {
    case 0x01: {
        decode_modrm();
        const unsigned r = regf();
        add(mem_ ? timing::kGrp0F01[r].mem : timing::kGrp0F01[r].reg);
        switch (r) {
        case 0:    // SGDT
        case 1: {  // SIDT
            if (!mem_) return fault(6);
            const uint32_t b = r == 0 ? gdt_base_ : idt_base_;
            wr16(eseg_, eoff_, r == 0 ? gdt_limit_ : idt_limit_);
            wr16(eseg_, u16(eoff_ + 2), u16(b));
            wr8(eseg_, u16(eoff_ + 4), u8(b >> 16));
            wr8(eseg_, u16(eoff_ + 5), 0xFF);  // the 286 stores FFh in the unused byte
            return;
        }
        case 2:    // LGDT
        case 3: {  // LIDT
            if (!mem_) return fault(6);
            const uint16_t limit = rd16(eseg_, eoff_);
            const uint32_t b = rd16(eseg_, u16(eoff_ + 2)) | (static_cast<uint32_t>(rd8(eseg_, u16(eoff_ + 4))) << 16);
            if (r == 2) {
                gdt_base_ = b;
                gdt_limit_ = limit;
            } else {
                if (b != 0) return fault(6);  // a relocated real-mode IVT is not supported
                idt_base_ = b;
                idt_limit_ = limit;
            }
            return;
        }
        case 4: set_rm<true>(msw_); return;  // SMSW
        case 6: {                            // LMSW: protected mode is not supported
            const unsigned v = get_rm<true>();
            if (v & 1) return fault(6);
            msw_ = u16((msw_ & ~0x000Eu) | (v & 0x000E));
            return;
        }
        default: return fault(6);
        }
    }
    case 0x06:  // CLTS
        msw_ = u16(msw_ & ~0x0008u);
        add(timing::kClts);
        return;
    case 0xFF: {  // host callback
        const uint8_t id = fetch8();
        if (!c.callback_) return fault(6);
        add(timing::kCallback);
        c.callback_(c, id);
        return;
    }
    default:  // 0F 00 (SLDT...VERW), LAR, LSL, LOADALL and everything else: #UD in real mode
        return fault(6);
    }
}

void Cpu::Impl::step() {
    ip0_ = R.ip;
    seg_ = kNoSeg;
    rep_ = 0;
    mem_ = false;
    no_trap_ = false;

    unsigned op;
    for (int prefixes = 0;; ++prefixes) {
        if (prefixes == kMaxPrefixes) return fault(13);
        op = fetch8();
        switch (op) {
        case 0x26: seg_ = ES; continue;
        case 0x2E: seg_ = CS; continue;
        case 0x36: seg_ = SS; continue;
        case 0x3E: seg_ = DS; continue;
        case 0xF0:  // LOCK
        case 0xF1:  // LOCK alias
            continue;
        case 0xF2:
        case 0xF3: rep_ = u8(op); continue;
        default: break;
        }
        break;
    }

    switch (op) {
#define VETTE_ALU_OPS(b)                         \
    case (b) + 0: alu_rm_r<false>((b) >> 3); break; \
    case (b) + 1: alu_rm_r<true>((b) >> 3); break;  \
    case (b) + 2: alu_r_rm<false>((b) >> 3); break; \
    case (b) + 3: alu_r_rm<true>((b) >> 3); break;  \
    case (b) + 4: alu_acc<false>((b) >> 3); break;  \
    case (b) + 5: alu_acc<true>((b) >> 3); break;
        VETTE_ALU_OPS(0x00)
        VETTE_ALU_OPS(0x08)
        VETTE_ALU_OPS(0x10)
        VETTE_ALU_OPS(0x18)
        VETTE_ALU_OPS(0x20)
        VETTE_ALU_OPS(0x28)
        VETTE_ALU_OPS(0x30)
        VETTE_ALU_OPS(0x38)
#undef VETTE_ALU_OPS

    case 0x06: push(R.s[ES]); break;
    case 0x0E: push(R.s[CS]); break;
    case 0x16: push(R.s[SS]); break;
    case 0x1E: push(R.s[DS]); break;
    case 0x07: R.s[ES] = pop(); break;
    case 0x17:
        R.s[SS] = pop();
        inhibit_ = true;
        break;
    case 0x1F: R.s[DS] = pop(); break;
    case 0x0F: op_0f(); break;
    case 0x27: daa(); break;
    case 0x2F: das(); break;
    case 0x37: aaa(); break;
    case 0x3F: aas(); break;

    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        R.r[op & 7] = u16(op_inc<true>(R.r[op & 7]));
        break;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        R.r[op & 7] = u16(op_dec<true>(R.r[op & 7]));
        break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        push(R.r[op & 7]);  // PUSH SP pushes the value before the push (286)
        break;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        const uint16_t v = pop();
        R.r[op & 7] = v;
        break;
    }

    case 0x60: {  // PUSHA
        const uint16_t sp = R.r[SP];
        push(R.r[AX]);
        push(R.r[CX]);
        push(R.r[DX]);
        push(R.r[BX]);
        push(sp);
        push(R.r[BP]);
        push(R.r[SI]);
        push(R.r[DI]);
        break;
    }
    case 0x61:  // POPA
        R.r[DI] = pop();
        R.r[SI] = pop();
        R.r[BP] = pop();
        pop();
        R.r[BX] = pop();
        R.r[DX] = pop();
        R.r[CX] = pop();
        R.r[AX] = pop();
        break;
    case 0x62: {  // BOUND
        decode_modrm();
        if (!mem_) return fault(6);
        const int idx = static_cast<int16_t>(R.r[regf()]);
        const int lo = static_cast<int16_t>(rd16(eseg_, eoff_));
        const int hi = static_cast<int16_t>(rd16(eseg_, u16(eoff_ + 2)));
        if (idx < lo || idx > hi) return fault(5);
        break;
    }
    case 0x68: push(fetch16()); break;
    case 0x69: {
        decode_modrm();
        const unsigned a = get_rm<true>();
        imul3(a, fetch16());
        break;
    }
    case 0x6A: push(u16(static_cast<int8_t>(fetch8()))); break;
    case 0x6B: {
        decode_modrm();
        const unsigned a = get_rm<true>();
        imul3(a, u16(static_cast<int8_t>(fetch8())));
        break;
    }
    case 0x6C: if (string_op<false>(kIns, timing::kRepIns)) return; break;
    case 0x6D: if (string_op<true>(kIns, timing::kRepIns)) return; break;
    case 0x6E: if (string_op<false>(kOuts, timing::kRepOuts)) return; break;
    case 0x6F: if (string_op<true>(kOuts, timing::kRepOuts)) return; break;

    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        const int8_t d = static_cast<int8_t>(fetch8());
        const unsigned f = R.flags;
        bool t;
        switch ((op >> 1) & 7) {
        case 0: t = (f & OF) != 0; break;
        case 1: t = (f & CF) != 0; break;
        case 2: t = (f & ZF) != 0; break;
        case 3: t = (f & (CF | ZF)) != 0; break;
        case 4: t = (f & SF) != 0; break;
        case 5: t = (f & PF) != 0; break;
        case 6: t = ((f & SF) != 0) != ((f & OF) != 0); break;
        default: t = (f & ZF) != 0 || (((f & SF) != 0) != ((f & OF) != 0)); break;
        }
        if (t != ((op & 1) != 0)) {
            R.ip = u16(R.ip + d);
            add(timing::kJccTaken);
            return;
        }
        break;
    }

    case 0x80:
    case 0x82: grp1<false>(false); break;
    case 0x81: grp1<true>(false); break;
    case 0x83: grp1<true>(true); break;
    case 0x84: {
        decode_modrm();
        op_logic<false>(get_rm<false>() & reg<false>(regf()));
        break;
    }
    case 0x85: {
        decode_modrm();
        op_logic<true>(get_rm<true>() & reg<true>(regf()));
        break;
    }
    case 0x86: {
        decode_modrm();
        const unsigned a = get_rm<false>();
        set_rm<false>(reg<false>(regf()));
        set_reg<false>(regf(), a);
        break;
    }
    case 0x87: {
        decode_modrm();
        const unsigned a = get_rm<true>();
        set_rm<true>(reg<true>(regf()));
        set_reg<true>(regf(), a);
        break;
    }
    case 0x88: decode_modrm(); set_rm<false>(reg<false>(regf())); break;
    case 0x89: decode_modrm(); set_rm<true>(reg<true>(regf())); break;
    case 0x8A: decode_modrm(); set_reg<false>(regf(), get_rm<false>()); break;
    case 0x8B: decode_modrm(); set_reg<true>(regf(), get_rm<true>()); break;
    case 0x8C:  // MOV r/m16,sreg
        decode_modrm();
        if (regf() > 3) return fault(6);
        set_rm<true>(R.s[regf()]);
        break;
    case 0x8D:  // LEA
        decode_modrm();
        if (!mem_) return fault(6);
        R.r[regf()] = eoff_;
        break;
    case 0x8E: {  // MOV sreg,r/m16
        decode_modrm();
        const unsigned s = regf();
        if (s == CS || s > 3) return fault(6);
        R.s[s] = u16(get_rm<true>());
        if (s == SS) inhibit_ = true;
        break;
    }
    case 0x8F:  // POP r/m16
        decode_modrm();
        if (regf() != 0) return fault(6);
        set_rm<true>(pop());
        break;

    case 0x90: break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        const uint16_t t = R.r[op & 7];
        R.r[op & 7] = R.r[AX];
        R.r[AX] = t;
        break;
    }
    case 0x98: R.r[AX] = u16(static_cast<int8_t>(R.r[AX])); break;      // CBW
    case 0x99: R.r[DX] = (R.r[AX] & 0x8000) ? 0xFFFF : 0x0000; break;  // CWD
    case 0x9A: {  // CALL far
        const uint16_t off = fetch16();
        const uint16_t sg = fetch16();
        push(R.s[CS]);
        push(R.ip);
        R.s[CS] = sg;
        R.ip = off;
        break;
    }
    case 0x9B: break;  // WAIT: no coprocessor
    case 0x9C: push((R.flags & kLoadable) | kFixed); break;
    case 0x9D: R.flags = u16((pop() & kLoadable) | kFixed); break;
    case 0x9E: set_flags(SF | ZF | AF | PF | CF, (R.r[AX] >> 8) & (SF | ZF | AF | PF | CF)); break;
    case 0x9F: set_reg<false>(4, (R.flags & (SF | ZF | AF | PF | CF)) | kFixed); break;

    case 0xA0: set_reg<false>(AX, rd8(data_seg(), fetch16())); break;
    case 0xA1: R.r[AX] = rd16(data_seg(), fetch16()); break;
    case 0xA2: wr8(data_seg(), fetch16(), R.r[AX]); break;
    case 0xA3: wr16(data_seg(), fetch16(), R.r[AX]); break;
    case 0xA4: if (string_op<false>(kMovs, timing::kRepMovs)) return; break;
    case 0xA5: if (string_op<true>(kMovs, timing::kRepMovs)) return; break;
    case 0xA6: if (string_op<false>(kCmps, timing::kRepCmps)) return; break;
    case 0xA7: if (string_op<true>(kCmps, timing::kRepCmps)) return; break;
    case 0xA8: op_logic<false>(R.r[AX] & fetch8()); break;
    case 0xA9: op_logic<true>(R.r[AX] & fetch16()); break;
    case 0xAA: if (string_op<false>(kStos, timing::kRepStos)) return; break;
    case 0xAB: if (string_op<true>(kStos, timing::kRepStos)) return; break;
    case 0xAC: if (string_op<false>(kLods, timing::kRepLods)) return; break;
    case 0xAD: if (string_op<true>(kLods, timing::kRepLods)) return; break;
    case 0xAE: if (string_op<false>(kScas, timing::kRepScas)) return; break;
    case 0xAF: if (string_op<true>(kScas, timing::kRepScas)) return; break;

    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        r8(op & 7) = fetch8();
        break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        R.r[op & 7] = fetch16();
        break;

    case 0xC0: {
        decode_modrm();
        grp2<false>(fetch8(), mem_ ? timing::kShiftN.mem : timing::kShiftN.reg);
        break;
    }
    case 0xC1: {
        decode_modrm();
        grp2<true>(fetch8(), mem_ ? timing::kShiftN.mem : timing::kShiftN.reg);
        break;
    }
    case 0xC2: {  // RET imm16
        const uint16_t n = fetch16();
        R.ip = pop();
        R.r[SP] = u16(R.r[SP] + n);
        break;
    }
    case 0xC3: R.ip = pop(); break;
    case 0xC4:    // LES
    case 0xC5: {  // LDS
        decode_modrm();
        if (!mem_) return fault(6);
        const uint16_t off = rd16(eseg_, eoff_);
        const uint16_t sg = rd16(eseg_, u16(eoff_ + 2));
        R.r[regf()] = off;
        R.s[op == 0xC4 ? ES : DS] = sg;
        break;
    }
    case 0xC6:
        decode_modrm();
        if (regf() != 0) return fault(6);
        set_rm<false>(fetch8());
        break;
    case 0xC7:
        decode_modrm();
        if (regf() != 0) return fault(6);
        set_rm<true>(fetch16());
        break;
    case 0xC8: enter(); break;
    case 0xC9:  // LEAVE
        R.r[SP] = R.r[BP];
        R.r[BP] = pop();
        break;
    case 0xCA: {  // RETF imm16
        const uint16_t n = fetch16();
        R.ip = pop();
        R.s[CS] = pop();
        R.r[SP] = u16(R.r[SP] + n);
        break;
    }
    case 0xCB:
        R.ip = pop();
        R.s[CS] = pop();
        break;
    case 0xCC: soft_int(3); break;
    case 0xCD: soft_int(fetch8()); break;
    case 0xCE:  // INTO
        if (R.flags & OF) {
            soft_int(4);
            add(timing::kIntoTaken);
            return;
        }
        break;
    case 0xCF:  // IRET
        R.ip = pop();
        R.s[CS] = pop();
        R.flags = u16((pop() & kLoadable) | kFixed);
        break;

    case 0xD0: decode_modrm(); grp2<false>(1, mem_ ? timing::kShift1.mem : timing::kShift1.reg); break;
    case 0xD1: decode_modrm(); grp2<true>(1, mem_ ? timing::kShift1.mem : timing::kShift1.reg); break;
    case 0xD2: decode_modrm(); grp2<false>(R.r[CX] & 0xFF, mem_ ? timing::kShiftN.mem : timing::kShiftN.reg); break;
    case 0xD3: decode_modrm(); grp2<true>(R.r[CX] & 0xFF, mem_ ? timing::kShiftN.mem : timing::kShiftN.reg); break;
    case 0xD4: {  // AAM
        const unsigned d = fetch8();
        if (d == 0) {
            // The 286 leaves SZP of AL>>1 (and clears OF/AF/CF) before the fault; measured on hardware.
            set_flags(kArith, szp<false>((R.r[AX] & 0xFF) >> 1));
            return fault(0);
        }
        const unsigned al = R.r[AX] & 0xFF;
        R.r[AX] = u16(((al / d) << 8) | (al % d));
        set_flags(kArith, szp<false>(al % d));
        break;
    }
    case 0xD5: {  // AAD
        const unsigned d = fetch8();
        const unsigned al = ((R.r[AX] & 0xFF) + (R.r[AX] >> 8) * d) & 0xFF;
        R.r[AX] = u16(al);
        set_flags(kArith, szp<false>(al));
        break;
    }
    case 0xD6: set_reg<false>(AX, cf() ? 0xFF : 0x00); break;  // SALC (undocumented)
    case 0xD7: set_reg<false>(AX, rd8(data_seg(), u16(R.r[BX] + (R.r[AX] & 0xFF)))); break;  // XLAT
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        decode_modrm();  // ESC: no coprocessor, so nothing else happens
        break;

    case 0xE0:    // LOOPNZ
    case 0xE1:    // LOOPZ
    case 0xE2: {  // LOOP
        const int8_t d = static_cast<int8_t>(fetch8());
        R.r[CX] = u16(R.r[CX] - 1);
        bool t = R.r[CX] != 0;
        if (op == 0xE0) t = t && !(R.flags & ZF);
        else if (op == 0xE1) t = t && (R.flags & ZF);
        if (t) {
            R.ip = u16(R.ip + d);
            add(timing::kLoopTaken);
            return;
        }
        break;
    }
    case 0xE3: {  // JCXZ
        const int8_t d = static_cast<int8_t>(fetch8());
        if (R.r[CX] == 0) {
            R.ip = u16(R.ip + d);
            add(timing::kLoopTaken);
            return;
        }
        break;
    }
    case 0xE4: set_reg<false>(AX, c.io_.in8(fetch8())); break;
    case 0xE5: R.r[AX] = c.io_.in16(fetch8()); break;
    case 0xE6: c.io_.out8(fetch8(), u8(R.r[AX])); break;
    case 0xE7: c.io_.out16(fetch8(), R.r[AX]); break;
    case 0xE8: {  // CALL near
        const uint16_t d = fetch16();
        push(R.ip);
        R.ip = u16(R.ip + d);
        break;
    }
    case 0xE9: {
        const uint16_t d = fetch16();
        R.ip = u16(R.ip + d);
        break;
    }
    case 0xEA: {  // JMP far
        const uint16_t off = fetch16();
        const uint16_t sg = fetch16();
        R.ip = off;
        R.s[CS] = sg;
        break;
    }
    case 0xEB: {
        const int8_t d = static_cast<int8_t>(fetch8());
        R.ip = u16(R.ip + d);
        break;
    }
    case 0xEC: set_reg<false>(AX, c.io_.in8(R.r[DX])); break;
    case 0xED: R.r[AX] = c.io_.in16(R.r[DX]); break;
    case 0xEE: c.io_.out8(R.r[DX], u8(R.r[AX])); break;
    case 0xEF: c.io_.out16(R.r[DX], R.r[AX]); break;

    case 0xF4:  // HLT
        c.halted_ = true;
        no_trap_ = true;
        break;
    case 0xF5: R.flags ^= CF; break;
    case 0xF6: grp3<false>(); break;
    case 0xF7: grp3<true>(); break;
    case 0xF8: set_flags(CF, 0); break;
    case 0xF9: set_flags(CF, CF); break;
    case 0xFA: set_flags(IF, 0); break;
    case 0xFB:
        set_flags(IF, IF);
        inhibit_ = true;
        break;
    case 0xFC: set_flags(DF, 0); break;
    case 0xFD: set_flags(DF, DF); break;
    case 0xFE: {
        decode_modrm();
        const unsigned r = regf();
        if (r > 1) return fault(6);
        add(mem_ ? timing::kGrp45[r].mem : timing::kGrp45[r].reg);
        const unsigned v = get_rm<false>();
        set_rm<false>(r == 0 ? op_inc<false>(v) : op_dec<false>(v));
        break;
    }
    case 0xFF: {
        decode_modrm();
        const unsigned r = regf();
        if (r == 7 || (!mem_ && (r == 3 || r == 5))) return fault(6);
        add(mem_ ? timing::kGrp45[r].mem : timing::kGrp45[r].reg);
        switch (r) {
        case 0: set_rm<true>(op_inc<true>(get_rm<true>())); break;
        case 1: set_rm<true>(op_dec<true>(get_rm<true>())); break;
        case 2: {  // CALL near r/m
            const uint16_t t = u16(get_rm<true>());
            push(R.ip);
            R.ip = t;
            break;
        }
        case 3: {  // CALL far m16:16
            const uint16_t off = rd16(eseg_, eoff_);
            const uint16_t sg = rd16(eseg_, u16(eoff_ + 2));
            push(R.s[CS]);
            push(R.ip);
            R.s[CS] = sg;
            R.ip = off;
            break;
        }
        case 4: R.ip = u16(get_rm<true>()); break;
        case 5: {  // JMP far m16:16
            const uint16_t off = rd16(eseg_, eoff_);
            R.s[CS] = rd16(eseg_, u16(eoff_ + 2));
            R.ip = off;
            break;
        }
        default: push(get_rm<true>()); break;  // PUSH r/m (PUSH SP pushes the old value)
        }
        break;
    }

    default:  // 63 (ARPL), 64-67: undefined in real mode
        return fault(6);
    }

    add(mem_ ? timing::kOp[op].mem : timing::kOp[op].reg);
}

// --- Cpu ----------------------------------------------------------------------------------------

Cpu::Cpu(Memory& mem, IoBus& io) : impl_(nullptr), mem_(mem), io_(io) {
    impl_ = std::make_unique<Impl>(*this);
    reset();
}

Cpu::~Cpu() = default;

void Cpu::reset() {
    regs = Registers{};
    regs.s[CS] = 0xF000;
    regs.ip = 0xFFF0;
    halted_ = false;
    stop_ = false;
    impl_->reset();
}

void Cpu::set_code_hook(uint32_t linear, CodeHook hook) {
    linear &= Memory::kMask;
    if (!hook) {
        clear_code_hook(linear);
        return;
    }
    impl_->hooks_[linear] = std::make_shared<CodeHook>(std::move(hook));
    impl_->update_hook_bit(linear);
}

void Cpu::clear_code_hook(uint32_t linear) {
    linear &= Memory::kMask;
    impl_->hooks_.erase(linear);
    impl_->update_hook_bit(linear);
}

void Cpu::set_watch(uint32_t linear, Watch watch) {
    linear &= Memory::kMask;
    if (!watch) {
        clear_watch(linear);
        return;
    }
    impl_->watches_[linear] = std::make_shared<Watch>(std::move(watch));
    impl_->update_hook_bit(linear);
}

void Cpu::clear_watch(uint32_t linear) {
    linear &= Memory::kMask;
    impl_->watches_.erase(linear);
    impl_->update_hook_bit(linear);
}

int64_t Cpu::run(int64_t cycles) {
    stop_ = false;
    Impl& im = *impl_;
    const uint64_t start = total_cycles_;
    const uint64_t end = start + static_cast<uint64_t>(cycles > 0 ? cycles : 0);
    im.end_ = end;
    while (total_cycles_ < end) {
        if (im.inhibit_) {
            im.inhibit_ = false;  // interrupt shadow: no IRQ before this instruction
        } else if (!irq_inhibit_ && (regs.flags & flag::IF) && pic_ && pic_->irq_pending()) {
            interrupt(pic_->irq_acknowledge());
            total_cycles_ += timing::kIrq;
            continue;
        }
        if (halted_) {
            total_cycles_ = end;  // idle until the next slice; an IRQ wakes us there
            break;
        }
        if ((!im.hooks_.empty() || !im.watches_.empty()) && im.run_hook()) {
            if (stop_) break;
            continue;
        }
        const bool trap = (regs.flags & flag::TF) != 0;
        im.step();
        if (trap) im.trap();
        if (stop_) break;
    }
    return static_cast<int64_t>(total_cycles_ - start);
}

void Cpu::interrupt(uint8_t vector) {
    push16(u16((regs.flags & kLoadable) | kFixed));
    regs.flags = u16(regs.flags & ~(flag::IF | flag::TF));
    push16(regs.s[CS]);
    push16(regs.ip);
    const uint32_t a = static_cast<uint32_t>(vector) * 4;
    regs.ip = mem_.read16(a);
    regs.s[CS] = mem_.read16(a + 2);
    halted_ = false;
}

void Cpu::push16(uint16_t v) { impl_->push(v); }

uint16_t Cpu::pop16() { return impl_->pop(); }

}  // namespace vette::host
