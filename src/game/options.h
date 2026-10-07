#pragma once
// Player options applied to the hosted original as code hooks. The player's game files are never
// modified.

namespace vette::host {
class Cpu;
class Machine;
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

// Skips the emulated time the race spends polling for vertical retrace (`wait_vretrace` 3009:24F0,
// called twice per frame by the page flip). On a fast emulated PC most cycles are spent there.
// Watches on the loop's two IN instructions move time forward to the moment the poll's answer
// changes, but never past the next timer or keyboard interrupt; the original instructions then run
// as usual. Only polls that would have read the same value are dropped, so the game sees the same
// sequence of events. It isn't cycle-identical (the loop exits at the edge instead of up to one poll
// later), but it is deterministic.
void install_idle_skip(host::Machine& machine);

// Freeway traffic, smooth (Settings::smooth_traffic). `hw_spawn_car` (4021:1007) puts a new highway car
// 5-12 road slices ahead of the player (above 364 speed units; below, 3-6 behind), in plain view of the
// long-distance 3D view, and the original takes cars off more than 22 slices ahead or 7 behind (4021:11C4).
// Watches after its random draws move a new car to 14-21 slices ahead (the far end of the road the
// original shows) or 6-7 behind, so the random sequence stays the original's. The Enhanced view fades
// them in and out (SceneOptions::smooth_traffic).
void install_far_freeway_spawns(host::Cpu& cpu);

} // namespace vette::game
