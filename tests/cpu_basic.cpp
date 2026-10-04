// Handwritten CPU tests: host interface (callback opcode, code hooks, IRQs, HLT, request_stop) and
// behaviour the game depends on that the SingleStepTests suite does not exercise.

#include <chrono>
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "host/cpu.h"
#include "test.h"

using namespace vette::host;

namespace {

struct TestIo final : IoBus {
    std::vector<std::pair<uint16_t, uint8_t>> writes;
    Cpu* stop_cpu = nullptr;  // request_stop() on any OUT, like the machine does when the PIT is reprogrammed
    uint8_t in8(uint16_t) override { return 0xFF; }
    void out8(uint16_t port, uint8_t value) override {
        writes.emplace_back(port, value);
        if (stop_cpu) stop_cpu->request_stop();
    }
};

struct FakeVideo final : VideoMemory {
    std::vector<uint8_t> vram = std::vector<uint8_t>(Memory::kVideoSize, 0);
    int reads = 0;
    int writes = 0;
    uint8_t vram_read(uint32_t offset) override {
        ++reads;
        return vram[offset];
    }
    void vram_write(uint32_t offset, uint8_t value) override {
        ++writes;
        vram[offset] = value;
    }
};

struct FakePic final : InterruptController {
    bool pending = false;
    uint8_t vector = 0x08;
    int acks = 0;
    bool irq_pending() const override { return pending; }
    uint8_t irq_acknowledge() override {
        pending = false;
        ++acks;
        return vector;
    }
};

struct Rig {
    Memory mem;
    TestIo io;
    Cpu cpu{mem, io};

    Rig() {
        cpu.regs.s[CS] = 0x1000;
        cpu.regs.ip = 0x0100;
        cpu.regs.s[SS] = 0x2000;
        cpu.regs.r[SP] = 0x1000;
        cpu.regs.s[DS] = 0x3000;
        cpu.regs.s[ES] = 0x4000;
    }
    void poke(uint16_t seg, uint16_t off, std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) {
            mem.write8(Cpu::linear(seg, off), b);
            off = static_cast<uint16_t>(off + 1);
        }
    }
    void code(std::initializer_list<uint8_t> bytes) { poke(cpu.regs.s[CS], cpu.regs.ip, bytes); }
    void code(const std::vector<uint8_t>& bytes) {
        uint16_t off = cpu.regs.ip;
        for (uint8_t b : bytes) mem.write8(Cpu::linear(cpu.regs.s[CS], off++), b);
    }
    void vector(uint8_t v, uint16_t seg, uint16_t off) {
        mem.write16(v * 4u, off);
        mem.write16(v * 4u + 2, seg);
    }
    uint16_t stack_word(int index) {  // index 0 = top of stack
        return mem.read16(Cpu::linear(cpu.regs.s[SS], static_cast<uint16_t>(cpu.regs.r[SP] + 2 * index)));
    }
};

}  // namespace

TEST(cpu_callback_opcode) {
    Rig t;
    int calls = 0;
    uint8_t seen = 0;
    uint16_t ip_seen = 0;
    t.cpu.set_callback([&](Cpu& cpu, uint8_t id) {
        ++calls;
        seen = id;
        ip_seen = cpu.regs.ip;
        cpu.regs.r[AX] = 0x1234;
    });
    t.code({0x0F, 0xFF, 0x21, 0x40, 0xF4});  // callback 21h; inc ax; hlt
    t.cpu.run(1000);
    CHECK_EQ(calls, 1);
    CHECK_EQ(seen, 0x21);
    CHECK_EQ(ip_seen, 0x0103);  // the callback sees the continuation address
    CHECK_EQ(t.cpu.regs.r[AX], 0x1235);
    CHECK(t.cpu.halted());
    CHECK_EQ(t.cpu.regs.ip, 0x0105);
}

TEST(cpu_callback_null_is_int6) {
    Rig t;
    t.vector(6, 0x5000, 0x0000);
    t.poke(0x5000, 0x0000, {0xF4});
    t.code({0x2E, 0x0F, 0xFF, 0x21});  // with a prefix: the fault IP is the prefix
    t.cpu.run(1000);
    CHECK_EQ(t.cpu.regs.s[CS], 0x5000);
    CHECK_EQ(t.cpu.regs.ip, 0x0001);
    CHECK_EQ(t.stack_word(0), 0x0100);
    CHECK_EQ(t.stack_word(1), 0x1000);
}

TEST(cpu_code_hooks) {
    Rig t;
    // call 0200h; inc dx; hlt     with 0200h: mov ax,1; ret (the hook replaces it)
    t.code({0xE8, 0xFD, 0x00, 0x42, 0xF4});
    t.poke(0x1000, 0x0200, {0xB8, 0x01, 0x00, 0xC3});
    int calls = 0;
    const uint32_t at = Cpu::linear(0x1000, 0x0200);
    t.cpu.set_code_hook(at, [&](Cpu& cpu) {
        ++calls;
        cpu.regs.r[AX] = 0x7777;
        cpu.regs.ip = cpu.pop16();  // emulate RET
    });
    t.cpu.run(1000);
    CHECK_EQ(calls, 1);
    CHECK_EQ(t.cpu.regs.r[AX], 0x7777);
    CHECK_EQ(t.cpu.regs.r[DX], 1);
    CHECK(t.cpu.halted());

    // Cleared: the original code runs again.
    t.cpu.clear_code_hook(at);
    t.cpu.reset();
    t.cpu.regs.s[CS] = 0x1000;
    t.cpu.regs.ip = 0x0100;
    t.cpu.regs.s[SS] = 0x2000;
    t.cpu.regs.r[SP] = 0x1000;
    t.cpu.run(1000);
    CHECK_EQ(calls, 1);
    CHECK_EQ(t.cpu.regs.r[AX], 1);

    // A hook may remove itself while running; the same linear address reached via another seg:off fires.
    t.cpu.set_code_hook(at, [&](Cpu& cpu) {
        ++calls;
        cpu.clear_code_hook(at);
        cpu.regs.ip = cpu.pop16();
    });
    t.cpu.reset();
    t.cpu.regs.s[CS] = 0x1010;  // 1010:0000 is 1000:0100
    t.cpu.regs.ip = 0x0000;
    t.cpu.regs.s[SS] = 0x2000;
    t.cpu.regs.r[SP] = 0x1000;
    t.cpu.run(1000);
    CHECK_EQ(calls, 2);
}

TEST(cpu_irq_sti_shadow) {
    Rig t;
    FakePic pic;
    t.cpu.set_interrupt_controller(&pic);
    t.vector(0x08, 0x5000, 0x0000);
    t.poke(0x5000, 0x0000, {0xF4});  // handler: hlt
    // cli; inc bx; sti; inc cx; inc dx; hlt
    t.code({0xFA, 0x43, 0xFB, 0x41, 0x42, 0xF4});
    pic.pending = true;
    t.cpu.run(1000);
    // IF=0 until STI, then one more instruction (INC CX) before the IRQ.
    CHECK_EQ(pic.acks, 1);
    CHECK_EQ(t.cpu.regs.r[BX], 1);
    CHECK_EQ(t.cpu.regs.r[CX], 1);
    CHECK_EQ(t.cpu.regs.r[DX], 0);
    CHECK_EQ(t.cpu.regs.s[CS], 0x5000);
    CHECK_EQ(t.stack_word(0), 0x0104);  // return address: INC DX
    CHECK_EQ(t.stack_word(1), 0x1000);
    CHECK(t.stack_word(2) & flag::IF);
    CHECK(!(t.cpu.regs.flags & flag::IF));
}

TEST(cpu_irq_mov_ss_shadow) {
    Rig t;
    FakePic pic;
    t.cpu.set_interrupt_controller(&pic);
    t.vector(0x08, 0x5000, 0x0000);
    t.poke(0x5000, 0x0000, {0xF4});
    t.cpu.regs.flags |= flag::IF;
    t.cpu.regs.r[AX] = 0x2000;
    // mov ss,ax; mov sp,0800h; inc dx; hlt   -- the IRQ must not land between the two MOVs
    t.code({0x8E, 0xD0, 0xBC, 0x00, 0x08, 0x42, 0xF4});
    t.cpu.run(2);  // executes MOV SS only (2 cycles)
    CHECK_EQ(t.cpu.regs.ip, 0x0102);
    pic.pending = true;
    t.cpu.run(1000);
    CHECK_EQ(pic.acks, 1);
    CHECK_EQ(t.cpu.regs.r[SP], 0x0800 - 6);
    CHECK_EQ(t.stack_word(0), 0x0105);  // taken after MOV SP, before INC DX
}

TEST(cpu_hlt_wake_and_budget) {
    Rig t;
    FakePic pic;
    t.cpu.set_interrupt_controller(&pic);
    t.vector(0x08, 0x5000, 0x0000);
    t.poke(0x5000, 0x0000, {0x43, 0xCF});  // handler: inc bx; iret
    t.code({0xFB, 0xF4, 0x41, 0xF4});       // sti; hlt; inc cx; hlt
    const uint64_t t0 = t.cpu.total_cycles();
    // Halted time passes: run() consumes the whole slice.
    CHECK_EQ(t.cpu.run(1000), 1000);
    CHECK_EQ(t.cpu.total_cycles() - t0, 1000u);
    CHECK(t.cpu.halted());
    CHECK_EQ(t.cpu.regs.ip, 0x0102);
    CHECK_EQ(t.cpu.run(500), 500);  // still halted, nothing pending
    CHECK(t.cpu.halted());
    CHECK_EQ(t.cpu.regs.r[CX], 0);
    // The IRQ wakes it; the handler returns to the instruction after HLT.
    pic.pending = true;
    t.cpu.run(1000);
    CHECK_EQ(pic.acks, 1);
    CHECK_EQ(t.cpu.regs.r[BX], 1);
    CHECK_EQ(t.cpu.regs.r[CX], 1);
    CHECK(t.cpu.halted());
    CHECK_EQ(t.cpu.regs.ip, 0x0104);

    // HLT with IF=0 stays halted even with an IRQ pending, and still consumes the slice.
    Rig u;
    FakePic pic2;
    u.cpu.set_interrupt_controller(&pic2);
    u.code({0xFA, 0xF4});
    pic2.pending = true;
    CHECK_EQ(u.cpu.run(300), 300);
    CHECK(u.cpu.halted());
    CHECK_EQ(pic2.acks, 0);
}

TEST(cpu_request_stop_from_io) {
    Rig t;
    t.io.stop_cpu = &t.cpu;
    // mov al,34h; out 43h,al; inc cx; inc cx; hlt
    t.code({0xB0, 0x34, 0xE6, 0x43, 0x41, 0x41, 0xF4});
    const uint64_t t0 = t.cpu.total_cycles();
    const int64_t ran = t.cpu.run(100000);
    CHECK_EQ(t.cpu.regs.ip, 0x0104);  // returned right after the OUT
    CHECK_EQ(t.cpu.regs.r[CX], 0);
    CHECK_EQ(t.io.writes.size(), 1u);
    CHECK_EQ(ran, 2 + 3);  // MOV r8,imm (2) + OUT imm8 (3), included in the count
    CHECK_EQ(t.cpu.total_cycles() - t0, 5u);
    t.io.stop_cpu = nullptr;
    t.cpu.run(100000);  // the next run continues normally
    CHECK_EQ(t.cpu.regs.r[CX], 2);
    CHECK(t.cpu.halted());

    // request_stop inside a REP OUTSB stops after the current iteration; resuming finishes the rest.
    Rig u;
    u.io.stop_cpu = &u.cpu;
    u.poke(0x3000, 0x0000, {1, 2, 3, 4});
    u.cpu.regs.r[SI] = 0;
    u.cpu.regs.r[CX] = 4;
    u.cpu.regs.r[DX] = 0x3C4;
    u.code({0xF3, 0x6E, 0xF4});  // rep outsb; hlt
    u.cpu.run(100000);
    CHECK_EQ(u.io.writes.size(), 1u);
    CHECK_EQ(u.cpu.regs.r[CX], 3);
    CHECK_EQ(u.cpu.regs.ip, 0x0100);  // back on the REP prefix
    u.io.stop_cpu = nullptr;
    u.cpu.run(100000);
    CHECK_EQ(u.io.writes.size(), 4u);
    CHECK_EQ(u.io.writes[3].second, 4);
    CHECK_EQ(u.cpu.regs.r[CX], 0);
    CHECK(u.cpu.halted());
}

TEST(cpu_rep_movsb_override_into_video) {
    Rig t;
    FakeVideo video;
    t.mem.set_video(&video);
    // Source in the code segment via CS: override; destination ES:DI = A000:0010.
    t.poke(0x1000, 0x0400, {10, 20, 30, 40, 50, 60, 70, 80});
    t.cpu.regs.s[ES] = 0xA000;
    t.cpu.regs.r[DI] = 0x0010;
    t.cpu.regs.r[SI] = 0x0400;
    t.cpu.regs.r[CX] = 8;
    t.code({0xF3, 0x2E, 0xA4, 0xF4});  // rep cs: movsb; hlt
    t.cpu.run(10000);
    CHECK(t.cpu.halted());
    CHECK_EQ(video.writes, 8);
    CHECK_EQ(video.vram[0x10], 10);
    CHECK_EQ(video.vram[0x17], 80);
    CHECK_EQ(t.cpu.regs.r[CX], 0);
    CHECK_EQ(t.cpu.regs.r[SI], 0x0408);
    CHECK_EQ(t.cpu.regs.r[DI], 0x0018);

    // Interrupted mid-way (slice ends), it resumes with the override intact (prefix order swapped).
    Rig u;
    FakeVideo v2;
    u.mem.set_video(&v2);
    u.poke(0x1000, 0x0400, {1, 2, 3, 4, 5, 6, 7, 8});
    u.poke(0x3000, 0x0400, {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE});  // DS data: must not be used
    u.cpu.regs.s[ES] = 0xA000;
    u.cpu.regs.r[DI] = 0;
    u.cpu.regs.r[SI] = 0x0400;
    u.cpu.regs.r[CX] = 8;
    u.code({0x2E, 0xF3, 0xA4, 0xF4});  // cs: rep movsb; hlt
    u.cpu.run(10);                     // 5 + 4n: yields after two iterations
    CHECK_EQ(u.cpu.regs.ip, 0x0100);
    CHECK(u.cpu.regs.r[CX] > 0 && u.cpu.regs.r[CX] < 8);
    u.cpu.run(10000);
    CHECK(u.cpu.halted());
    for (int i = 0; i < 8; ++i) CHECK_EQ(v2.vram[i], i + 1);
    CHECK_EQ(v2.writes, 8);

    // REP STOSW with a pending IRQ yields to it, and the IRET resumes the string instruction.
    Rig w;
    FakePic pic;
    w.cpu.set_interrupt_controller(&pic);
    w.vector(0x08, 0x5000, 0x0000);
    w.poke(0x5000, 0x0000, {0x43, 0xCF});  // inc bx; iret
    w.cpu.regs.flags |= flag::IF;
    w.cpu.regs.r[AX] = 0xBEEF;
    w.cpu.regs.r[DI] = 0;
    w.cpu.regs.r[CX] = 100;
    w.code({0xF3, 0xAB, 0xF4});  // rep stosw; hlt
    w.cpu.run(50);
    pic.pending = true;
    w.cpu.run(100000);
    CHECK_EQ(pic.acks, 1);
    CHECK_EQ(w.cpu.regs.r[BX], 1);
    CHECK_EQ(w.cpu.regs.r[CX], 0);
    CHECK_EQ(w.cpu.regs.r[DI], 200);
    CHECK_EQ(w.mem.read16(Cpu::linear(0x4000, 198)), 0xBEEF);
    CHECK(w.cpu.halted());
}

TEST(cpu_divide_error) {
    Rig t;
    t.vector(0, 0x5000, 0x0000);
    t.poke(0x5000, 0x0000, {0xF4});
    t.cpu.regs.r[AX] = 0x1234;
    t.cpu.regs.r[BX] = 0;
    t.code({0x26, 0xF7, 0xF3});  // es: div bx (prefix included in the fault address)
    t.cpu.run(1000);
    CHECK_EQ(t.cpu.regs.s[CS], 0x5000);
    CHECK_EQ(t.stack_word(0), 0x0100);
    CHECK_EQ(t.stack_word(1), 0x1000);
    CHECK_EQ(t.cpu.regs.r[AX], 0x1234);

    // Quotient overflow also faults; AAM 0 faults.
    Rig u;
    u.vector(0, 0x5000, 0x0000);
    u.poke(0x5000, 0x0000, {0xF4});
    u.cpu.regs.r[AX] = 0x4000;
    u.cpu.regs.r[CX] = 0x0010;
    u.code({0xF6, 0xF1});  // div cl -> 400h > FFh
    u.cpu.run(1000);
    CHECK_EQ(u.stack_word(0), 0x0100);

    Rig v;
    v.vector(0, 0x5000, 0x0000);
    v.poke(0x5000, 0x0000, {0xF4});
    v.code({0x90, 0xD4, 0x00});  // nop; aam 0
    v.cpu.run(1000);
    CHECK_EQ(v.stack_word(0), 0x0101);
}

TEST(cpu_segment_wrap) {
    Rig t;
    // Word read at DS:FFFF takes its high byte from DS:0000, not DS:10000.
    t.poke(0x3000, 0xFFFF, {0x34});
    t.poke(0x3000, 0x0000, {0x12});
    t.poke(0x4000, 0x0000, {0x99});  // 3000:10000 linear, must not be read
    t.code({0xA1, 0xFF, 0xFF,        // mov ax,[FFFF]
            0xC7, 0x06, 0xFF, 0xFF, 0xCD, 0xAB,  // mov word [FFFF],ABCDh
            0xF4});
    t.cpu.run(1000);
    CHECK_EQ(t.cpu.regs.r[AX], 0x1234);
    CHECK_EQ(t.mem.read8(Cpu::linear(0x3000, 0xFFFF)), 0xCD);
    CHECK_EQ(t.mem.read8(Cpu::linear(0x3000, 0x0000)), 0xAB);
    CHECK_EQ(t.mem.read8(Cpu::linear(0x4000, 0x0000)), 0x99);

    // Stack wraps within SS: push with SP=0 writes at SS:FFFE; with SP=1 the word straddles FFFF/0000.
    Rig s;
    s.cpu.regs.r[SP] = 0x0001;
    s.cpu.regs.r[AX] = 0x5678;
    s.code({0x50, 0xF4});
    s.cpu.run(1000);
    CHECK_EQ(s.cpu.regs.r[SP], 0xFFFF);
    CHECK_EQ(s.mem.read8(Cpu::linear(0x2000, 0xFFFF)), 0x78);
    CHECK_EQ(s.mem.read8(Cpu::linear(0x2000, 0x0000)), 0x56);

    // Instruction fetch wraps within CS: B8 at CS:FFFF, imm16 at CS:0000.
    Rig f;
    f.cpu.regs.ip = 0xFFFF;
    f.poke(0x1000, 0xFFFF, {0xB8});
    f.poke(0x1000, 0x0000, {0x22, 0x11, 0xF4});
    f.cpu.run(1000);
    CHECK_EQ(f.cpu.regs.r[AX], 0x1122);
    CHECK_EQ(f.cpu.regs.ip, 0x0003);

    // Linear addresses wrap at 1 MB: FFFF:0010 is 00000h.
    Rig l;
    l.mem.write8(0x00000, 0x5A);
    l.cpu.regs.s[DS] = 0xFFFF;
    l.code({0xA0, 0x10, 0x00, 0xF4});  // mov al,[0010]
    l.cpu.run(1000);
    CHECK_EQ(l.cpu.regs.r[AX] & 0xFF, 0x5A);
}

TEST(cpu_push_sp_is_286) {
    Rig t;
    // The game's 8088-vs-286 test: push sp; pop ax; cmp ax,sp; (8088: AX = SP-2, 286: AX = SP)
    t.code({0x54, 0x58, 0x3B, 0xC4, 0xF4});
    t.cpu.run(1000);
    CHECK_EQ(t.cpu.regs.r[AX], 0x1000);
    CHECK(t.cpu.regs.flags & flag::ZF);

    // PUSHA stores the SP from before the instruction; FF /6 (push sp via modrm) also pushes the old SP.
    Rig u;
    u.code({0x60, 0xFF, 0xF4, 0xF4});
    u.cpu.run(1000);
    CHECK_EQ(u.stack_word(0), 0x1000 - 16);
    CHECK_EQ(u.stack_word(4), 0x1000);  // PUSHA's SP slot (below DI, SI, BP)

    // FLAGS bits 12-15 read as 0 in real mode (the 286-vs-386 test pushes F000h and reads it back).
    Rig v;
    v.code({0x68, 0x00, 0xF0, 0x9D, 0x9C, 0x58, 0xF4});  // push F000h; popf; pushf; pop ax
    v.cpu.run(1000);
    CHECK_EQ(v.cpu.regs.r[AX], 0x0002);

    // SMSW reports a 286 in real mode.
    Rig w;
    w.code({0x0F, 0x01, 0xE0, 0xF4});  // smsw ax
    w.cpu.run(1000);
    CHECK_EQ(w.cpu.regs.r[AX], 0xFFF0);
}

TEST(cpu_undefined_opcode_and_trap) {
    Rig t;
    t.vector(6, 0x5000, 0x0000);
    t.poke(0x5000, 0x0000, {0xF4});
    t.code({0x90, 0x0F, 0x0B});  // nop; ud2-style 0F 0B
    t.cpu.run(1000);
    CHECK_EQ(t.cpu.regs.s[CS], 0x5000);
    CHECK_EQ(t.stack_word(0), 0x0101);

    // Single step: TF set by POPF traps after the following instruction.
    Rig s;
    s.vector(1, 0x5000, 0x0000);
    s.poke(0x5000, 0x0000, {0xF4});
    s.code({0x68, 0x02, 0x01, 0x9D, 0x40, 0x40, 0xF4});  // push 0102h; popf; inc ax; inc ax
    s.cpu.run(1000);
    CHECK_EQ(s.cpu.regs.r[AX], 1);
    CHECK_EQ(s.cpu.regs.s[CS], 0x5000);
    CHECK_EQ(s.stack_word(0), 0x0105);
    CHECK(s.stack_word(2) & flag::TF);
    CHECK(!(s.cpu.regs.flags & flag::TF));

    // ESC opcodes decode their operand and touch nothing (no coprocessor); WAIT is a NOP.
    Rig e;
    e.poke(0x3000, 0x0010, {0x11, 0x22});
    e.code({0xDD, 0x3E, 0x10, 0x00, 0x9B, 0xF4});  // fnstsw [0010]; wait
    e.cpu.run(1000);
    CHECK_EQ(e.mem.read16(Cpu::linear(0x3000, 0x0010)), 0x2211);
    CHECK_EQ(e.cpu.regs.ip, 0x0106);
}

TEST(cpu_benchmark) {
    // Tight loops; prints emulated cycles per host second. Target: >= 50M in Release.
    struct Prog {
        const char* name;
        std::vector<uint8_t> code;
    };
    const Prog progs[] = {
        // L: inc ax; dec bx; jnz L; jmp L
        {"register loop", {0x40, 0x4B, 0x75, 0xFC, 0xEB, 0xFA}},
        // L: add ax,[si]; mov [di+2],ax; inc si; shl ax,1; xor dx,ax; loop L; jmp to start
        {"memory loop", {0x03, 0x04, 0x89, 0x45, 0x02, 0x46, 0xD1, 0xE0, 0x31, 0xC2, 0xE2, 0xF4, 0xEB, 0xF2}},
        // L: mov cx,64; rep movsw; sub si,128; sub di,128; jmp L
        {"rep movsw", {0xB9, 0x40, 0x00, 0xF3, 0xA5, 0x81, 0xEE, 0x80, 0x00, 0x81, 0xEF, 0x80, 0x00, 0xEB, 0xF1}},
    };
    for (const auto& p : progs) {
        Rig t;
        t.code(p.code);
        t.cpu.regs.r[SI] = 0x1000;
        t.cpu.regs.r[DI] = 0x2000;
        // About 0.4 s of host time per program (at least 10M cycles), so Debug runs stay short.
        const auto t0 = std::chrono::steady_clock::now();
        int64_t done = 0;
        double secs = 0;
        while (done < 10'000'000 || secs < 0.4) {
            done += t.cpu.run(1'000'000);
            secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }
        std::printf("  bench %-14s %7.1f M emulated cycles/s\n", p.name, static_cast<double>(done) / secs / 1e6);
        CHECK(!t.cpu.halted());
    }
}
