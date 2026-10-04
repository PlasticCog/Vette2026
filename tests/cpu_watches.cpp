// Watches: several observers per address (verification harness, smooth renderer, idle skip), the
// single-owner set_watch shorthand, and add_cycles / IRQ inhibit used with them.

#include <vector>

#include "host/cpu.h"
#include "test.h"

using namespace vette::host;

namespace {

struct NullIo final : IoBus {
    uint8_t in8(uint16_t) override { return 0xFF; }
    void out8(uint16_t, uint8_t) override {}
};

struct PendingPic final : InterruptController {
    bool pending = true;
    bool irq_pending() const override { return pending; }
    uint8_t irq_acknowledge() override {
        pending = false;
        return 0x08;
    }
};

// NOP NOP NOP HLT at 1000:0100, stack at 2000:0100.
struct Rig {
    Memory mem;
    NullIo io;
    Cpu cpu{mem, io};
    uint32_t at(uint16_t ip) const { return Cpu::linear(0x1000, ip); }

    Rig() {
        for (uint16_t i = 0; i < 3; ++i) {
            mem.write8(at(static_cast<uint16_t>(0x100 + i)), 0x90);
        }
        mem.write8(at(0x103), 0xF4);
        cpu.regs.s[CS] = 0x1000;
        cpu.regs.ip = 0x0100;
        cpu.regs.s[SS] = 0x2000;
        cpu.regs.r[SP] = 0x0100;
    }
};

} // namespace

TEST(cpu_watches_share_an_address) {
    Rig rig;
    std::vector<int> order;
    const auto a = rig.cpu.add_watch(rig.at(0x101), [&](Cpu&) { order.push_back(1); });
    rig.cpu.add_watch(rig.at(0x101), [&](Cpu&) { order.push_back(2); });
    rig.cpu.set_watch(rig.at(0x101), [&](Cpu&) { order.push_back(3); });
    rig.cpu.run(20);
    CHECK_EQ(order.size(), size_t{3});  // all three ran, once, and the NOP still executed
    CHECK(rig.cpu.halted());
    (void)a;
}

TEST(cpu_remove_and_clear_watch_are_selective) {
    Rig rig;
    std::vector<int> order;
    const auto a = rig.cpu.add_watch(rig.at(0x101), [&](Cpu&) { order.push_back(1); });
    rig.cpu.add_watch(rig.at(0x101), [&](Cpu&) { order.push_back(2); });
    rig.cpu.set_watch(rig.at(0x101), [&](Cpu&) { order.push_back(3); });
    rig.cpu.remove_watch(a);              // drops only that watch
    rig.cpu.clear_watch(rig.at(0x101));   // drops only the set_watch one
    rig.cpu.run(20);
    CHECK(order == std::vector<int>{2});
}

TEST(cpu_set_watch_replaces_only_its_own) {
    Rig rig;
    int added = 0, owned_first = 0, owned_second = 0;
    rig.cpu.add_watch(rig.at(0x100), [&](Cpu&) { ++added; });
    rig.cpu.set_watch(rig.at(0x100), [&](Cpu&) { ++owned_first; });
    rig.cpu.set_watch(rig.at(0x100), [&](Cpu&) { ++owned_second; });
    rig.cpu.run(20);
    CHECK_EQ(added, 1);
    CHECK_EQ(owned_first, 0);
    CHECK_EQ(owned_second, 1);
}

TEST(cpu_watch_redirect_skips_later_watches_and_instruction) {
    Rig rig;
    int later = 0;
    rig.cpu.add_watch(rig.at(0x100), [](Cpu& c) { c.regs.ip = 0x0103; });  // straight to the HLT
    rig.cpu.add_watch(rig.at(0x100), [&](Cpu&) { ++later; });
    rig.cpu.run(20);
    CHECK_EQ(later, 0);
    CHECK(rig.cpu.halted());
}

TEST(cpu_add_cycles_and_irq_inhibit) {
    Rig rig;
    PendingPic pic;
    rig.cpu.set_interrupt_controller(&pic);
    rig.cpu.regs.flags = static_cast<uint16_t>(rig.cpu.regs.flags | flag::IF);
    rig.mem.write16(0x08 * 4, 0x0103);  // IRQ vector -> the HLT
    rig.mem.write16(0x08 * 4 + 2, 0x1000);

    rig.cpu.set_irq_inhibit(true);
    rig.cpu.run(6);  // the NOPs run; the IRQ stays pending
    CHECK(pic.pending);
    CHECK(rig.cpu.regs.ip > 0x0100);

    rig.cpu.set_irq_inhibit(false);
    const uint64_t before = rig.cpu.total_cycles();
    rig.cpu.add_cycles(1000);
    CHECK_EQ(rig.cpu.total_cycles(), before + 1000);
    rig.cpu.run(20);
    CHECK(!pic.pending);  // delivered once allowed
}
