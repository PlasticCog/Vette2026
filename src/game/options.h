#pragma once
// Player options applied to the hosted original as code hooks. The player's game files are never
// modified.

namespace vette::host {
class Cpu;
}

namespace vette::game {

// Skips the manual-lookup question before the first race (the copy-protection quiz,
// `manual_quiz` 4160:0A40; re/notes/06-copy-protection.md).
//
// v1.1 already accepts any answer: a JMP at 4160:0B06 bypasses the comparison. Skipping therefore
// only removes the screen. The routine's own RNG call still runs, at its original moment, so the
// random sequence afterwards is exactly the original's. The hook then stores the question index and
// attempt counter and runs the routine's epilogue, which sets the "check passed" flag DS:2AEC = 1.
void install_skip_manual_check(host::Cpu& cpu);

} // namespace vette::game
