# VETTE! 2026

A from-the-disassembly rebuild of **VETTE!** (Spectrum HoloByte, 1989) in C++20 and SDL3. It's built to
run on modern Windows, Linux and macOS.

- **Classic mode** plays 1:1 with the original DOS release.
- **Enhanced mode** adds options on top: high resolution and widescreen, FM/AdLib music, digitized sound
  effects from the Macintosh version, modern controls, and more.

Status: early reverse engineering. See [docs/PLAN.md](docs/PLAN.md) for the approach and roadmap.

## You need the original game

This repository contains **no original game files**. To play, copy the files from your own copy of
VETTE! for DOS into the [`Game/`](Game/README.md) folder.

VETTE! 2026 is an unofficial fan project. It is not affiliated with or endorsed by the rights holders
of VETTE!.

## Building

Requires CMake 3.24+ and a C++20 compiler. SDL3 is used from your system if it's installed;
otherwise CMake fetches and builds it statically, so the exe has no runtime dependencies.

**Windows (Visual Studio 2022):**
```
build.bat            # Debug  -> build\windows-msvc\Debug\vette2026.exe
build.bat release    # Release
```

**Linux** (needs the X11/Wayland development packages for SDL):
```
cmake --preset linux
cmake --build --preset linux-release
```

Run with `--game <dir>` to point at a different game folder. `--dump-frame out.bmp` renders the title
screen to a file without opening a window, as a quick check that your game files are found.

Current state: the original game runs inside the built-in emulator and is fully playable, with
keyboard, mouse, gamepad and PC-speaker sound. Native C++ ports of its routines are being added
one at a time, each verified against the original (see [docs/PORTING.md](docs/PORTING.md)).

The original asks a manual-lookup question (copy protection) before the first race. VETTE! 2026 skips
it, since this version of the game accepts any answer anyway. Pass `--manual-check` to see it.

## Controls

- **Keyboard:** every key goes to the game, Esc included. F11 or Alt+Enter toggles fullscreen.
- **Mouse:** acts as the PC's mouse. When the game shows the mouse pointer, it's drawn over the picture
  as the classic DOS arrow, and the Windows pointer is hidden while it's over the window.
- **Gamepad:** acts as the PC's analog joystick. The D-pad, Start and Back also work as keys for the
  keyboard-driven menus.

| Gamepad | PC |
|---|---|
| Left stick, left/right | Joystick X |
| Right trigger / left trigger | Joystick forward (up) / back (down). Both triggers combine. With both released, the left stick's up/down is used instead. |
| A / B (south / east) | Joystick buttons 1 / 2 |
| D-pad | Arrow keys |
| Start / Back | Enter / Esc |

The left stick has a small radial deadzone (15%). The first connected gamepad is used, and gamepads
can be plugged in or removed at any time.

DOS games look for a joystick once, at startup, so the emulated PC gets a game port only if a gamepad is
connected when VETTE! 2026 starts. `--joystick` adds the game port anyway, and the stick reads
centered until a pad is plugged in. `--no-joystick` leaves it out even with a pad connected; the pad's
menu keys still work.

## Reverse engineering

The disassembly work is reproducible from your own `Game/` files. Requirements: Python 3 with
`capstone`, Ghidra 12.x, and JDK 21+.

```
python re/tools/ghidra_build.py            # unpack VETTE.EXE, import into Ghidra, apply names, export
python re/tools/ghidra_build.py --refresh  # re-apply re/symbols.csv after editing it
python re/tools/vdis.py 3009:0025 40       # quick disassembly at an image-relative SEG:OFF
```

**Headless runs.** `vette_run` boots the original game in the built-in emulator with no window. It
can inject keys, take screenshots and print memory words. This script reaches a race, waits for the
countdown, selects the automatic gearbox (`A`) and 1st gear, turns right onto the Great Highway and
drives north. Unsteered, it crashes a little later.

```
vette_run --game Game --seconds 50 --skip-manual-check --key 13:39 --key 17:1C --key 21:1C \
  --key 25:1C --key 30:1C --key 37:1E --key 37.3:02 --hold 37.5:49:48 --hold 38.5:39.3:4D \
  --shot 45 --watch 224A:2CD3 --out shots
```

Add `--verify all` to check every native port against the original during the run (see
[docs/PORTING.md](docs/PORTING.md)). `vette_fuzz` checks them with random inputs.

Scan codes are set 1 in hex. Emulator addresses are image-relative + `1000h` on the segment, the
same as Ghidra (`224A:2CD3` is `frame_rate`).

[`re/symbols.csv`](re/symbols.csv) is the shared, committed record of every named function and
variable. The Ghidra database and decompiler exports are derived from the copyrighted binary, so they
stay local (`re/ghidra/`, `re/out/`). Findings are written up in [`re/notes/`](re/notes/).

## Repository layout

| Path | Contents |
|---|---|
| `Game/` | Where players put their original game files (not tracked) |
| `src/` | The game: `platform/` (SDL3 video), `core/` (game-file discovery), `assets/` (decoders) |
| `docs/` | Plan |
| `re/` | Symbol map, notes, Ghidra scripts and RE tools |
| `tools/reverse_engineering/` | Earlier asset/resource extraction scripts (reference, unverified) |
