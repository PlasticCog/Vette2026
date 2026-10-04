#include "game/options.h"

#include <algorithm>

#include "game/x86.h"
#include "host/machine.h"

namespace vette::game {
namespace {

// manual_quiz (4160:0A40): PUSH AX BX CX DX DS DI ES SI BP; CALLF random (CX = random number); then
// the question screen and input loop; then the epilogue at 0B22. The hook takes over right after the
// random call returns, so the RNG runs exactly when (and as) it does in the original.
constexpr uint16_t kQuizSeg = emu_seg(0x4160), kAfterRandom = 0x0A4E;
constexpr uint16_t kTextSeg = emu_seg(0x0ACB);        // the quiz's data segment
constexpr uint16_t kCodeSeg = emu_seg(0x3009);        // main code segment
constexpr uint16_t kQuestion = 0x6EC0, kAttempts = 0x6EC2;
constexpr uint16_t kCheckState = 0x2AEC;              // DS: FFh = not asked yet, 1 = passed

} // namespace

void install_skip_manual_check(Cpu& cpu) {
    cpu.set_code_hook(Cpu::linear(kQuizSeg, kAfterRandom), [](Cpu& c) {
        Registers& r = c.regs;
        Memory& m = c.memory();
        using namespace host;
        // 0A53-0A66: the question is the high word of random * 48 (MUL), at most 47.
        const auto question = static_cast<uint16_t>((uint32_t{r.r[CX]} * 0x30) >> 16);
        wr16(m, kTextSeg, kQuestion, question);
        wr16(m, kTextSeg, kAttempts, 0);
        // 0B22-0B30: restore the registers, mark the check passed (with the caller's DS), RETF.
        r.r[BP] = c.pop16();
        r.r[SI] = c.pop16();
        r.s[ES] = c.pop16();
        r.r[DI] = c.pop16();
        r.s[DS] = c.pop16();
        r.r[DX] = c.pop16();
        r.r[CX] = c.pop16();
        r.r[BX] = c.pop16();
        r.r[AX] = c.pop16();
        wr8(m, r.s[DS], kCheckState, 1);
        r.ip = c.pop16();
        r.s[CS] = c.pop16();
    });
}

void install_idle_skip(host::Machine& machine) {
    // wait_vretrace (3009:24F0):
    //   24F3  IN AL,DX / AND AL,8 / JNZ 24F3    wait while a retrace is in progress
    //   24F8  IN AL,DX / AND AL,8 / JZ 24F8     wait until the next retrace starts
    constexpr uint16_t kWhileRetrace = 0x24F3, kUntilRetrace = 0x24F8;
    const auto skip_to = [&machine](uint64_t edge_ns) {
        Cpu& c = machine.cpu();
        const uint64_t target = std::min(machine.cycle_at_ns(edge_ns), machine.next_event_cycle());
        if (target > c.total_cycles()) {
            c.add_cycles(target - c.total_cycles());
        }
    };
    Cpu& cpu = machine.cpu();
    cpu.add_watch(Cpu::linear(kCodeSeg, kWhileRetrace), [&machine, skip_to](Cpu&) {
        if (machine.ega().in_vertical_retrace()) {
            skip_to(machine.ega().next_vertical_retrace_ns(false));
        }
    });
    cpu.add_watch(Cpu::linear(kCodeSeg, kUntilRetrace), [&machine, skip_to](Cpu&) {
        if (!machine.ega().in_vertical_retrace()) {
            skip_to(machine.ega().next_vertical_retrace_ns(true));
        }
    });
}

} // namespace vette::game
