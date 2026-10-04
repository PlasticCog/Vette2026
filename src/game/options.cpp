#include "game/options.h"

#include "game/x86.h"

namespace vette::game {
namespace {

// manual_quiz (4160:0A40): PUSH AX BX CX DX DS DI ES SI BP; CALLF random (CX = random number); then
// the question screen and input loop; then the epilogue at 0B22. The hook takes over right after the
// random call returns, so the RNG runs exactly when (and as) it does in the original.
constexpr uint16_t kQuizSeg = emu_seg(0x4160), kAfterRandom = 0x0A4E;
constexpr uint16_t kTextSeg = emu_seg(0x0ACB);        // the quiz's data segment
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

} // namespace vette::game
