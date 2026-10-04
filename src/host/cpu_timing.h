#pragma once
// Approximate 80286 real-mode instruction timings, used to advance emulated time.
//
// Source: Intel 80286 Programmer's Reference Manual, appendix B, and the iAPX 286 data sheet
// (real-address-mode column, zero wait states). Everything that sets the emulated machine's speed is
// in this file, so tuning a reference machine profile (wait states, an 8 vs 12 MHz part, ...) means
// editing numbers here only.
//
// Model:
//  * Each opcode has a base cost for its register form and its memory form (Cost{reg, mem}). Group
//    opcodes (80-83, C0/C1, D0-D3, F6/F7, FE/FF, 0F 01) have a table per reg field.
//  * Memory-form costs already include the effective-address calculation. The 286 adds one clock for
//    base+index+displacement addressing (kEaBaseIndexDisp).
//  * Control transfers cost "n + m" in Intel's tables, where m is the size of the next instruction
//    (prefetch refill). That is approximated by the constant kM.
//  * Variable parts (taken branches, shift counts, REP iterations, ENTER nesting) are listed below.
//  * Memory wait states and bus contention are not modelled.

#include <array>
#include <cstdint>

namespace vette::host::timing {

struct Cost {
    uint8_t reg;  // register operand (mod == 3), or the only form
    uint8_t mem;  // memory operand
};

// --- Scalar tunables ------------------------------------------------------------------------------
inline constexpr int kM = 2;                 // "m" after a control transfer
inline constexpr int kEaBaseIndexDisp = 1;   // [base+index+disp] addressing
inline constexpr int kJccTaken = 7 + kM;     // Jcc not-taken cost is the base table entry (3)
inline constexpr int kLoopTaken = 8 + kM;    // LOOP/LOOPZ/LOOPNZ/JCXZ, not taken: base entry (4)
inline constexpr int kIntoTaken = 24 + kM;   // INTO when OF=1 (base 3 otherwise)
inline constexpr int kException = 23 + kM;   // CPU exceptions (#DE, #UD, BOUND, single-step)
inline constexpr int kIrq = 23 + kM + 3;     // hardware interrupt: INT sequence + INTA cycles
inline constexpr int kShiftPerBit = 1;       // shift/rotate by CL or imm: base + count
inline constexpr int kEnterLevel1 = 15;      // ENTER with nesting level 1 (level 0 is the base, 11)
inline constexpr int kEnterBase = 12;        // ENTER level L>1: kEnterBase + kEnterPerLevel * (L - 1)
inline constexpr int kEnterPerLevel = 4;
inline constexpr int kCallback = 20;         // host callback (0F FF ib); HLE work itself is free
inline constexpr int kCodeHook = 1;          // a code hook that replaces an instruction
inline constexpr int kHalt = 2;              // HLT itself; halted time is the rest of the slice

// REP string instructions: base + per_iteration * count. Without REP the plain base table applies.
struct RepCost {
    uint8_t base;
    uint8_t per;
};
inline constexpr RepCost kRepMovs{5, 4};
inline constexpr RepCost kRepCmps{5, 9};
inline constexpr RepCost kRepScas{5, 8};
inline constexpr RepCost kRepLods{5, 4};
inline constexpr RepCost kRepStos{4, 3};
inline constexpr RepCost kRepIns{5, 4};
inline constexpr RepCost kRepOuts{5, 4};

// --- One-byte opcodes ---------------------------------------------------------------------------
// Entries for prefixes and group opcodes are 0 (groups use the tables further down).
constexpr std::array<Cost, 256> make_op_table() {
    std::array<Cost, 256> t{};
    auto set = [&t](int op, int reg, int mem) { t[op] = Cost{static_cast<uint8_t>(reg), static_cast<uint8_t>(mem)}; };
    auto range = [&set](int first, int last, int reg, int mem) {
        for (int op = first; op <= last; ++op) set(op, reg, mem);
    };

    // ALU: ADD OR ADC SBB AND SUB XOR CMP. r/m,r and r,r/m: 2 / 7; acc,imm: 3.
    for (int base = 0x00; base <= 0x38; base += 8) {
        range(base, base + 3, 2, 7);
        range(base + 4, base + 5, 3, 3);
    }
    set(0x3A, 2, 6);  // CMP r,m reads only
    set(0x3B, 2, 6);
    set(0x06, 3, 3);          // PUSH ES
    set(0x0E, 3, 3);          // PUSH CS
    set(0x16, 3, 3);          // PUSH SS
    set(0x1E, 3, 3);          // PUSH DS
    set(0x07, 5, 5);          // POP ES
    set(0x17, 5, 5);          // POP SS
    set(0x1F, 5, 5);          // POP DS
    set(0x27, 3, 3);          // DAA
    set(0x2F, 3, 3);          // DAS
    set(0x37, 3, 3);          // AAA
    set(0x3F, 3, 3);          // AAS
    range(0x40, 0x4F, 2, 2);  // INC/DEC r16
    range(0x50, 0x57, 3, 3);  // PUSH r16
    range(0x58, 0x5F, 5, 5);  // POP r16
    set(0x60, 17, 17);        // PUSHA
    set(0x61, 19, 19);        // POPA
    set(0x62, 13, 13);        // BOUND (no exception)
    set(0x68, 3, 3);          // PUSH imm16
    set(0x69, 21, 24);        // IMUL r,r/m,imm16
    set(0x6A, 3, 3);          // PUSH imm8
    set(0x6B, 21, 24);        // IMUL r,r/m,imm8
    range(0x6C, 0x6F, 5, 5);  // INS/OUTS (single)
    range(0x70, 0x7F, 3, 3);  // Jcc not taken
    set(0x84, 2, 6);          // TEST r/m,r
    set(0x85, 2, 6);
    set(0x86, 3, 5);          // XCHG r/m,r
    set(0x87, 3, 5);
    range(0x88, 0x89, 2, 3);  // MOV r/m,r
    range(0x8A, 0x8B, 2, 5);  // MOV r,r/m
    set(0x8C, 2, 3);          // MOV r/m,sreg
    set(0x8D, 3, 3);          // LEA
    set(0x8E, 2, 5);          // MOV sreg,r/m
    set(0x8F, 5, 5);          // POP r/m
    set(0x90, 3, 3);          // NOP
    range(0x91, 0x97, 3, 3);  // XCHG AX,r16
    set(0x98, 2, 2);          // CBW
    set(0x99, 2, 2);          // CWD
    set(0x9A, 13 + kM, 13 + kM);  // CALL far
    set(0x9B, 3, 3);          // WAIT
    set(0x9C, 3, 3);          // PUSHF
    set(0x9D, 5, 5);          // POPF
    set(0x9E, 2, 2);          // SAHF
    set(0x9F, 2, 2);          // LAHF
    range(0xA0, 0xA1, 5, 5);  // MOV acc,moffs
    range(0xA2, 0xA3, 3, 3);  // MOV moffs,acc
    range(0xA4, 0xA5, 5, 5);  // MOVS
    range(0xA6, 0xA7, 8, 8);  // CMPS
    range(0xA8, 0xA9, 3, 3);  // TEST acc,imm
    range(0xAA, 0xAB, 3, 3);  // STOS
    range(0xAC, 0xAD, 5, 5);  // LODS
    range(0xAE, 0xAF, 7, 7);  // SCAS
    range(0xB0, 0xBF, 2, 2);  // MOV r,imm
    set(0xC2, 11 + kM, 11 + kM);  // RET imm16
    set(0xC3, 11 + kM, 11 + kM);  // RET
    set(0xC4, 7, 7);          // LES
    set(0xC5, 7, 7);          // LDS
    set(0xC6, 2, 3);          // MOV r/m,imm
    set(0xC7, 2, 3);
    set(0xC8, 11, 11);        // ENTER, level 0
    set(0xC9, 5, 5);          // LEAVE
    set(0xCA, 15 + kM, 15 + kM);  // RETF imm16
    set(0xCB, 15 + kM, 15 + kM);  // RETF
    set(0xCC, 23 + kM, 23 + kM);  // INT3
    set(0xCD, 23 + kM, 23 + kM);  // INT imm8
    set(0xCE, 3, 3);              // INTO not taken
    set(0xCF, 17 + kM, 17 + kM);  // IRET
    set(0xD4, 16, 16);        // AAM
    set(0xD5, 14, 14);        // AAD
    set(0xD6, 2, 2);          // SALC (undocumented)
    set(0xD7, 5, 5);          // XLAT
    range(0xD8, 0xDF, 9, 9);  // ESC (no coprocessor: decode only)
    range(0xE0, 0xE3, 4, 4);  // LOOPNZ/LOOPZ/LOOP/JCXZ not taken
    range(0xE4, 0xE5, 5, 5);  // IN acc,imm8
    range(0xE6, 0xE7, 3, 3);  // OUT imm8,acc
    set(0xE8, 7 + kM, 7 + kM);    // CALL near
    set(0xE9, 7 + kM, 7 + kM);    // JMP near
    set(0xEA, 11 + kM, 11 + kM);  // JMP far
    set(0xEB, 7 + kM, 7 + kM);    // JMP short
    range(0xEC, 0xED, 5, 5);  // IN acc,DX
    range(0xEE, 0xEF, 3, 3);  // OUT DX,acc
    set(0xF4, kHalt, kHalt);  // HLT
    set(0xF5, 2, 2);          // CMC
    range(0xF8, 0xFD, 2, 2);  // CLC STC CLI STI CLD STD
    return t;
}
inline constexpr std::array<Cost, 256> kOp = make_op_table();

// --- Group opcodes, indexed by the modrm reg field ----------------------------------------------
// 80-83: ADD OR ADC SBB AND SUB XOR CMP r/m,imm
inline constexpr std::array<Cost, 8> kGrp1{{{3, 7}, {3, 7}, {3, 7}, {3, 7}, {3, 7}, {3, 7}, {3, 7}, {3, 6}}};
// D0/D1: shift/rotate by 1
inline constexpr Cost kShift1{2, 7};
// D2/D3 (by CL) and C0/C1 (by imm8): base + kShiftPerBit * count
inline constexpr Cost kShiftN{5, 8};
// F6 (byte) / F7 (word): TEST TEST NOT NEG MUL IMUL DIV IDIV
inline constexpr std::array<Cost, 8> kGrp3b{{{3, 6}, {3, 6}, {2, 7}, {2, 7}, {13, 16}, {13, 16}, {14, 17}, {17, 20}}};
inline constexpr std::array<Cost, 8> kGrp3w{{{3, 6}, {3, 6}, {2, 7}, {2, 7}, {21, 24}, {21, 24}, {22, 25}, {25, 28}}};
// FE/FF: INC DEC CALL CALLF JMP JMPF PUSH (undefined)
inline constexpr std::array<Cost, 8> kGrp45{{{2, 7},
                                              {2, 7},
                                              {7 + kM, 11 + kM},
                                              {16 + kM, 16 + kM},
                                              {7 + kM, 11 + kM},
                                              {15 + kM, 15 + kM},
                                              {3, 5},
                                              {0, 0}}};
// 0F 01: SGDT SIDT LGDT LIDT SMSW (undef) LMSW (undef)
inline constexpr std::array<Cost, 8> kGrp0F01{{{11, 11}, {12, 12}, {11, 11}, {12, 12}, {2, 3}, {0, 0}, {3, 6}, {0, 0}}};
inline constexpr int kClts = 2;  // 0F 06

}  // namespace vette::host::timing
