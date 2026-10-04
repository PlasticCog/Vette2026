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

Current state: the skeleton finds and verifies your game files and shows the title screen. Gameplay
arrives with Phase 1 (see the plan).

## Reverse engineering

The disassembly work is reproducible from your own `Game/` files. Requirements: Python 3 with
`capstone`, Ghidra 12.x, and JDK 21+.

```
python re/tools/ghidra_build.py            # unpack VETTE.EXE, import into Ghidra, apply names, export
python re/tools/ghidra_build.py --refresh  # re-apply re/symbols.csv after editing it
python re/tools/vdis.py 3009:0025 40       # quick disassembly at an image-relative SEG:OFF
```

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
