# VETTE! 2026 — Rebuild Plan

Goal: a C++20/SDL3 rebuild of VETTE! (Spectrum HoloByte, 1989) built from a disassembly of the DOS
version. **Classic** mode plays 1:1 with the original. **Enhanced** mode adds high resolution, FM/AdLib
music, digitized sound effects, and other options, drawing on the Mac and PC-98 versions.

## What we have

| Source | Version | Notes |
|---|---|---|
| `Vette_DOS_EN/VETTE.EXE` | 1.1 (EN) | EXEPACK-packed. Unpacked with `re/tools/unexepack.py` → `re/bin/VETTE_unpacked.exe` (272,304-byte image, 368 relocs, entry `3009:0025`, game DS = `124Ah`). EGA: menus in mode 0Eh (640×200), the race view in mode 0Dh (320×200). PC speaker only, mouse, joystick, modem two-player. All world data (map, cell types, models) is static in the EXE (notes 05). |
| `Vette_PC-98_JA/extracted/VETTE.EXE` | 1.02J | Same codebase, **not packed**. Drives the YM2203 OPN FM chip (ports `188h`/`18Ah`). Credits add "Sound by … H.Nagata(J)". Gauge/dash art comes as separate `.PIC` files. |
| `Vette_Mac_EN` resource forks | 1.02 | `VETTE!.Data`: 16 `INST` digitized sounds (8-bit unsigned PCM), plus `MAPS`/`CLST`/`OBJS`/`PERF` world, car and route data. `Color VETTE!`: 192 color `PICT` screens. |

Kept from the Codex pass, as **unverified reference**: the asset decoders in `tools/reverse_engineering/`
(PCX-style RLE, 4-plane EGA images, mask+4-plane sprites), the PC-98 HDI and Mac resource extraction, and
the manual text in `analysis/manual/`.
Discarded: `engine/`, `release/`, and the docs describing them (Three.js reimagining, not based on DOS code).

## Approach: emulator-hosted incremental decompilation

This follows the OpenDUNE and OpenRCT2 model. The game stays playable at every step, and the original
code acts as a built-in test oracle.

1. **Host it.** Our SDL3 app loads the user's original `VETTE.EXE` into a small built-in real-mode x86
   interpreter. DOS/BIOS calls are handled at a high level, and the hardware Vette touches (EGA, PIT,
   keyboard, PC speaker, mouse, joystick) is modelled natively. Result: a playable 1:1 game on day one.
2. **Replace it.** Original functions get swapped one at a time for native C++ hooked at their original
   `seg:off`. Recorded input demos run the original and the hybrid in lockstep, comparing RAM and the
   framebuffer every tick. Any divergence is a bug in the port.
3. **Drop the CPU.** Once a full playthrough runs no original code, the interpreter leaves the default build.
   It stays available as a developer oracle.

This beats decompiling everything first and hoping it matches, because each function gets checked against
the original as soon as it's ported.

### Core rule: Classic and Enhanced are layers, not forks

The simulation is one deterministic, fixed-step, original-integer-math core. Enhanced *video* and *audio*
options only **read** simulation state. Gameplay-changing options are their own explicit category, so
"Enhanced visuals + Classic gameplay" stays exactly 1:1.

## Phases

### Phase 0 — RE foundations
- [x] Unpack EXEPACK.
- [x] Ghidra project tooling: `re/tools/ghidra_build.py` builds the project headlessly from `Game/` and
      applies `re/symbols.csv`, the committed source of truth for names. The database is local only.
- [x] Timer/keyboard ISRs and the PC-speaker driver located (`re/notes/01-startup-and-timing.md`).
- [x] **Timing model**: a variable timestep. Each frame measures elapsed 291 Hz PIT ticks and scales
      the simulation by the resulting frame rate. The race clock is real time. Classic mode therefore
      needs a *reference machine profile*, and high-refresh play is native to the original design.
- [x] 3D pipeline: Q15 rotation, focal-256 projection (64° FOV), near-plane clip, span fill with
      dithering, self-modifying line drawer (notes 03).
- [x] Which simulation quantities scale by `frame_rate`, and which don't: steering, braking and
      several others are per-frame (notes 04). Car struct, traffic and pedestrian records.
- [x] Visibility rules for extended draw distance (notes 03, summarized under Phase 4).
- [x] World data: 5×5 big tiles of 16×16 cells, a 256-entry cell-type table, 125 code-drawn plus 59
      data-driven models. Mac `QUAD`/`OBJS` share formats but are renumbered; the Mac map grid differs
      (notes 05).
- [ ] Remaining: RLE loader, two-player link code, highway renderer details, dash sign and street-name
      tables, opponent and freeway route formats, the D150 path planner, the quiz routine (notes 06).

Exit: annotated map of the main loop and every hardware touchpoint.

### Phase 1 — Host runtime (playable original)
`src/host/` is an SDL-free emulation library; `vette_run` runs it headlessly for testing.
- [x] CMake + C++20 + SDL3 skeleton (MSVC 2022 first, Linux/macOS kept buildable).
- [x] Real-mode 8086/186/286 interpreter (`cpu.*`): passes 100% of the 1.47M applicable
      SingleStepTests 80286 cases and runs at 400–680M emulated cycles/s. The original packed EXE
      is loaded as-is, and its own EXEPACK stub unpacks it.
- [x] EGA (`ega.*`): planar memory, latches, write/read modes, attribute palette with the 200-line
      CGA-style color decode, CRTC page flipping latched at retrace, register-derived retrace timing.
- [x] PIC, PIT (291 Hz IRQ0), PC speaker (band-limited square-wave synthesis), keyboard controller,
      game port, `Machine` run loop. Time is in exact CPU cycles, so runs are deterministic.
- [x] BIOS in ROM stubs (`0F FF id` host callbacks, real x86 for IRQ handlers so chaining works) and
      DOS services with real MCB chains. Files are read from `Game/`; writes go to a save folder.
- [x] Headless boot (`vette_run`). The original runs end to end: intro → garage → skill → opponent →
      course → manual quiz → race → crash cutscene → garage. It measures itself at 12–17 fps on an
      emulated 12 MHz 286. The 3D view runs in EGA mode 0Dh (320×200).
- [ ] Wire into the SDL app (window, keyboard via `platform/keymap`, mouse, audio).

Exit: title → garage → race → results all playable, with frames matching DOSBox reference captures.

### Phase 2 — Incremental native port
Port order: platform leaves (blits, line/polygon fill, sound driver, input) → fixed-point math and 3D
pipeline → game systems (vehicle physics, traffic AI, police and tickets, courses and checkpoints, HUD,
menus, garage quiz, high scores, two-player).

Exit: a full playthrough executes no original code.

### Phase 3 — Classic release
Pure native C++/SDL3, reading data from the user's own copy of the game. EGA output with correct 4:3
aspect, PC-speaker emulation, original timing. The serial/modem link is replaced by TCP/IP with the
same protocol semantics.

### Phase 4 — Enhanced mode (each option toggles independently)
- **Extended draw distance** (headline feature): everything visible at all times where possible, with
  no object pop-in, roads and landmarks drawn to the horizon. It's render-only, so traffic and police
  behave 1:1. The original has **no far clip plane**; visibility comes from these rules (notes 03):
  - **A ~6-cell window**: own cell, 2 ahead, one side row, so 4096–6144 units ahead.
  - **A sort-depth cull** at 0x1400, plus lateral margins.
  - **Per-object LOD distances.**
  - **Detail toggles** for buildings, windows and the mirror. Slow CPUs start with windows and mirror
    off; default them on in Enhanced.

  Design consequences:
  - **The original render pass writes simulation state** (traffic cell indices, collision candidates).
    The port therefore runs an invisible *visibility pass* with the original window every frame,
    plus a separate unrestricted *draw pass*.
  - **Keep each cell's list order**, because roads and markings are coplanar.
  - **Traffic replicas.** Traffic repeats every 4 cells and the original window hides that. Either
    show only the bound cells, or optionally render the replicas.
  - **32-bit camera maths.** The original's 16-bit coordinates wrap beyond 16 cells.
  - **Redesign the horizon.** The painted panorama (HORIZON0-2) scrolls at a different rate from the
    geometry and assumes a short draw distance.
- **Video**: the same polygon scene rendered at native resolution and widescreen FOV, using a
  render-only high-precision projection while the simulation stays untouched. Optional Mac Color VETTE!
  256-color screens, PC-98 art where it's better, and scaling filters (integer, CRT, smooth).
- **Digitized SFX** from the Mac `INST` set: engine loop pitched by RPM, horn, siren, crash, skid, cable
  car bell, helicopter, thud, beeps, and the Mac "Opening song" on the title screen.
- **FM**: PC-98 YM2203 music/SFX through `ymfm` emulation, and AdLib (OPL2/3) arrangements converted
  from that data or newly authored. *DOS never supported AdLib, so this is new content.*
- **QoL** (gameplay category): analog gamepad/wheel, key rebinding, render interpolation for
  high-refresh displays, persistent settings.

## Open questions and risks
- Reference machine profile for Classic: which PC's frame cadence counts as "1:1"? It could be
  user-selectable (e.g. "286-12", "386-33"), defaulting to a typical 1989 machine. The original's
  double vsync wait caps it at refresh/2 (~30 fps). An emulated 12 MHz 286 runs the race at 12–17 fps.
- Native function hooks cost ~0 emulated cycles, so a partly ported build runs "faster" in emulated
  time. Lockstep tests must feed recorded `elapsed` values, or hooks must charge the original's cycles.
- DOS and Mac world data differ: the Mac map grid is 52×47 and its cell types are renumbered. Mixing
  in Mac content needs a mapping layer (notes 05).
- The meaning of the Mac `INST` header: bytes 6–7 are the sample count; 4–5 look like a rate code and
  0–3 like loop points. Mapping DOS's procedural speaker effects onto samples is a design task.
- The PC-98 sound driver format needs its own RE pass before FM playback.
- CPU core: writing our own minimal core is recommended (clean licensing, built-in tracing and hooks)
  over adopting an existing one.
- **Legal**: original game files must not ship in a public repo. The port loads them from a
  user-supplied folder.

## Proposed layout
```
re/          tools (unexepack.py, ...), Ghidra project, symbol maps, notes
re/bin/      derived binaries (gitignored)
src/platform SDL3 window/input/audio/timing
src/host     x86 interpreter + DOS/EGA/PIT/speaker models (scaffold, dev oracle later)
src/game     native ported game code
src/enhanced hi-res renderer, digitized audio, FM
third_party/
```

## Toolchain on this machine
VS 2022 (MSVC 14.44) with bundled CMake/Ninja; MSYS2 gcc as a second compiler; JDK 24 (Ghidra
needs 21+); Python 3.13 + `capstone`. SDL3 via vcpkg manifest or CMake FetchContent. DOSBox-X is
needed for reference captures but isn't installed yet.
