# Porting original routines to native C++ (Phase 2)

Each routine of `VETTE.EXE` is replaced by native C++, one at a time, and proven identical to the
original while the game runs. The harness is `NativeRunner` (`src/host/native.h`); the ports live in
`src/game/` and are listed in `src/game/natives.cpp`.

## How verification works
In **Verify** mode, every call to a ported routine:
1. runs the **original** first, journaling every RAM write (and snapshotting the EGA for drawing routines);
2. rolls the machine back and runs the **native** version from the same state;
3. compares all registers, FLAGS (subject to `flags_mask`), every byte either version wrote, and the
   EGA state;
4. reports a mismatch and keeps the original's result, so the game continues exactly as the original.

Emulated time is the original's. Hardware interrupts wait until the call returns, and a verified
routine called from inside another one runs its original code.

Limitation: **port I/O is not rolled back**, so in Verify mode an `OUT` happens twice (original, then
native). It's harmless for what's ported so far: the only case is the INT 0 handler's EOI, sent while no
interrupt is in service. A routine that programs hardware (EGA registers, PIT, speaker) must be verified
with that in mind. Its EGA register writes are compared through the EGA snapshot when
`touches_vram` is set.

## Steps
1. **Read the routine.** Use the Ghidra export (`re/out/VETTE_unpacked.exe/funcs/`), `re/tools/vdis.py`,
   `re/symbols.csv` and `re/notes/`. Write down its inputs, outputs, every memory location it writes,
   and the registers and FLAGS it leaves changed.
2. **Write two layers:**
   - a **pure core**: typed C++ with no emulator types, which later native code and the
     Enhanced renderer will call directly;
   - an **adapter** `void name(host::Cpu&)`: it reads inputs from registers and emulated memory,
     calls the core, writes the results, and reproduces the original's register and FLAGS side
     effects. `src/game/x86.h` has segment:offset access with 64 KB wrap and FLAGS helpers.
3. **Register it** in `natives.cpp`:
   - name, image-relative address;
   - `far` / `ret n`, matching how callers return;
   - `touches_vram`;
   - `flags_mask`, which stays `0xFFFF` unless you document why some flags are dead.
4. **Verify** with the race script from the README:
   ```
   vette_run ... --verify NAME
   ```
   Widen coverage with steering (`--hold A:B:4B`, `4D`), views (F1–F3: `3B`–`3D`), the mirror (F6: `40`),
   long runs and other courses. The exit code is 4 on any mismatch. **Zero mismatches, with
   thousands of calls, before a port counts.**
   Then add the routine to `src/tools/vette_fuzz.cpp` and run `vette_fuzz --trials 5000`. It calls
   each port with random inputs, reaching cases play doesn't. It found an out-of-range-angle bug
   in `sincos_deg` that 48,000 in-game calls missed.
5. **Record it** in the status table below.

## Exactness rules
- Reproduce 16-bit wraparound, signed vs unsigned comparisons, truncating `idiv`, `sar` rounding,
  and saturation exactly as the instructions do. When in doubt, follow the disassembly literally.
- Read tables and data **through emulated memory** with the original's address arithmetic, so that
  out-of-range indexes behave the same.
- Every register is compared: preserve what the original preserves and leave clobbered registers
  with the values the original leaves in them.
- Writes below the returned SP (the routine's own stack scratch) are not compared.
- Verified routines must not do host I/O (DOS file calls). Their effects outside emulated memory
  can't be rolled back.

## Status
| Routine | Address | Status | Verified calls |
|---|---|---|---|
| `sincos_deg` | 3009:4E3C | verified (stores in the original's order) | 1,220,790, 0 mismatches |
| `camera_matrix_from_angles` | 3009:3F2D | verified | 11,120, 0 mismatches |
| `vec_mul_mat3` | 3009:3D51 | verified | 229,136, 0 mismatches |
| `points_rel_camera` | 3009:3D8C | verified | 200,834, 0 mismatches |
| `xform_points_to_camera` | 3009:3D2F | verified | 200,834, 0 mismatches |
| `world_to_camera_point` | 3009:3917 | verified | 3,248 (long drive only), 0 mismatches |
| `build_axis_table` | 3009:39B9 | verified | 11,116, 0 mismatches |
| `project_vertices` | 3009:A685 | verified, incl. INT 0 paths (4,324 IDIV and 141 DIV overflows) | 204,082, 0 mismatches |

Calls are summed over four sessions, each routine verified on its own: the README race (90 s; it
leaves the car in neutral), then, with `A` (automatic, `1E`) and `1` (`02`) pressed after the start,
a long drive cycling the F1–F3 views and the F6 mirror (480 s) and left and right steering pulses
(150 s and 300 s).
