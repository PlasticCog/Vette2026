# VETTE! 2026 — Rebuild Plan

Goal: a C++20/SDL3 rebuild of VETTE! (Spectrum HoloByte, 1989) built from a disassembly of the DOS
version. **Classic** mode plays 1:1 with the original. **Enhanced** mode adds high resolution, FM/AdLib
music, digitized sound effects, and other options, drawing on the Mac and PC-98 versions.

## What we have

| Source | Version | Notes |
|---|---|---|
| `Vette_DOS_EN/VETTE.EXE` | 1.1 (EN) | EXEPACK-packed. Unpacked with `re/tools/unexepack.py` → `re/bin/VETTE_unpacked.exe` (272,304-byte image, 368 relocs, entry `3009:0025`, game DS = `124Ah`). EGA 640×200×16, PC speaker only, mouse, joystick, modem two-player. |
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
- [ ] Locate: main loop body, EGA primitives, polygon rasterizer, 3D transform, RLE loader,
      two-player link code.
- [ ] Find which simulation quantities scale by `frame_rate`.
- [ ] Find where DOS keeps world, object and route data, and diff it against Mac `MAPS`/`OBJS`/`CLST`.

Exit: annotated map of the main loop and every hardware touchpoint.

### Phase 1 — Host runtime (playable original)
- CMake + C++20 + SDL3 skeleton (MSVC 2022 first, Linux/macOS kept buildable).
- Real-mode interpreter for the 8086/186/286 subset Vette uses, with in-memory EXEPACK unpack.
- HLE for int 21h (files, memory), int 10h/16h, and int 33h. Device models for EGA (planar, latches,
  write modes, map mask, palette), PIT and IRQ0, keyboard and port 60h, PC speaker, and joystick port
  201h (fed from SDL gamepads).

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
  user-selectable (e.g. "286-12", "386-33"), defaulting to a typical 1989 machine.
- Whether DOS and Mac world data are identical. This decides how much Mac content can be mixed in.
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
