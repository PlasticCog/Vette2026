# VETTE! 2026

A from-the-disassembly rebuild of **VETTE!** (Spectrum HoloByte, 1989) in C++20 and SDL3. It's built to
run on modern Windows, Linux and macOS.

- **Classic mode** plays 1:1 with the original DOS release.
- **Enhanced mode** adds options on top: high resolution and widescreen, FM/AdLib music, digitized sound
  effects from the Macintosh version, modern controls, and more.

Status: early reverse engineering. See [docs/PLAN.md](docs/PLAN.md) for the approach and roadmap.

## You need the original game

This repository contains **no original game files**. To play, copy the files from your own copy of
VETTE! for DOS into a folder in [`Game/`](Game/README.md) (e.g. `Game/DOS`). The PC-98 and Macintosh
versions, if you have them, go in folders of their own beside it, under any names: VETTE! 2026 scans
`Game/` when it starts, works out which version is where, and shows what it found in the launch menu.

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

**Linux** (needs the X11/Wayland development packages for SDL, and `libssl-dev` for online play):
```
cmake --preset linux
cmake --build --preset linux-release
```

**macOS** (Xcode command line tools, CMake and Ninja; builds `VETTE! 2026.app`):
```
cmake --preset macos
cmake --build --preset macos-release
```

**Downloads.** Every push is built for Windows, Linux and macOS by GitHub Actions (`.github/workflows/`),
and tagged versions are published on the Releases page. Put the `Game` folder next to the program (or
next to `VETTE! 2026.app`), or choose its location in the launch menu. The macOS app isn't notarized:
the first time, right-click it and choose Open.

Run with `--game <dir>` to point at a different game folder. `--dump-frame out.bmp` renders the title
screen to a file without opening a window, as a quick check that your game files are found.

Current state: the original game runs inside the built-in emulator and is fully playable, with
keyboard, mouse, gamepad and PC-speaker sound. Native C++ ports of its routines are being added
one at a time, each verified against the original (see [docs/PORTING.md](docs/PORTING.md)).

**Launch menu.** VETTE! 2026 opens with a launch menu. Choose a preset there (**Classic**, VETTE!
exactly as in 1989, or **Enhanced**), or set each option: frame rate, PC speed, draw distance, its
resolution, depth buffer and skyline, driving and lane centering, the manual check, joystick, window or fullscreen, scaling (sharp pixels, or smooth), sound, and whether the
menu appears at startup. **Game folder** shows
where your game files were found; select it to choose another folder. If the files aren't found, the
menu opens anyway and asks for the folder. It works with the keyboard, mouse or gamepad. Choices
(including the folder) are saved in
`%APPDATA%\VETTE2026\config\settings.ini` (the per-user settings folder on other systems). Command-line
flags override them for one run; `--launcher` brings the menu back if you switched it off.

The original asks a manual-lookup question (copy protection) before the first race. VETTE! 2026 skips
it, since this version of the game accepts any answer anyway. Pass `--manual-check` to see it.

**Smooth frame rate.** The race view is drawn at your display's refresh rate (60, 120, 144 Hz…) while the
game logic keeps its own cadence, exactly as the original. For every display frame, the original's own
3D drawing code runs again on a throwaway copy of the latest game frame, with the camera and cars blended
between the last two game frames. This adds at most one game frame (~33 ms) of delay. `--fps original`
shows only the frames the game draws itself.

**Draw distance.** The original draws only the few city blocks around the car, about two blocks ahead.
With **Maximum** (the Enhanced preset), VETTE! 2026 draws the race view's 3D world itself, with the
whole city in view to the horizon at once: every building, road and landmark with its most detailed
model, and (with the depth buffer, below) all of the game's traffic and pedestrians, which repeat across
the city in a pattern every four blocks, so nothing pops in as you drive. **Extended** draws eight blocks
around the car. The city is taken from your own copy of the game when it starts (about 0.1 s). The
rear-view mirrors and the freeway sections are drawn the same way (at Maximum, the freeway's whole
route). Road markings are painted flat on the road; edges and cables are thin, crisp lines. The dash and
messages are still the game's own. With PC-98 or Mac graphics the mirror is the original's.
`--draw-distance original|extended|maximum` overrides the setting for one run.

**Depth buffer.** With **On** (the default), the long-distance view is drawn on the GPU with a depth
buffer: nearer things always cover farther ones, so nothing shows through the scenery or flips in front
of something else as you drive, and the whole city's traffic and pedestrians are drawn. Things lying on
each other (road markings, outlines, windows) still go on top in the original's order. It needs
Direct3D 12, Vulkan or Metal; without, the game works as with **Off**: the original's back-to-front
drawing order, with traffic and pedestrians only near the car, where the original draws them, so they
appear as you get close (`--depth-buffer off`).

**Resolution.** The long-distance view is drawn at your display's full resolution. With **Original
320x200**, it is drawn at the original's own 320x200 instead and enlarged like the rest of the picture:
the whole city and every other enhancement, with the chunky pixels and one-pixel lines of 1989
(`--resolution original`).

**Skyline.** Behind the long-distance view, **Hills** (the default) keeps only the landscape of the
original's painted horizon: its painted buildings, towers and bridges are taken out, since the real city
now stands in front of it. **Painted** keeps the original's backdrop as it is (`--skyline painted`).
Hills is made for DOS VETTE! 1.1's pictures; with others, the backdrop stays painted.

**Driving.** Two options change how your car drives. They're off by default, and the Classic preset turns
them off. With **Improved** driving, your car drifts a little through fast corners, its view pointing into
the turn as it slides wide, and it leaves the ground over the crest of a hill when it's going fast enough
(from about 50 mph), landing with the Mac version's thud. **Lane centering** gently steers your car toward
its lane's direction and centre, found from the city's lane markings, and lets go as soon as you steer.
Either one also evens out the original's rounding of sideways movement, which made a car a degree off
straight creep left but never right; speeds and race times are unchanged. `--driving improved` and
`--lane-centering on` turn them on for one run.

**Online races.** Two players can race each other over the internet, in the original's two-player
mode, with no server in between: the games connect straight to each other. In the launch menu, choose
**Online race**, then **Host a race**: a code like **7K3M-QX9P-2HDA** goes onto your clipboard. Paste it to
your friend in any chat; they choose **Online race > Join a race** (the code is picked up from their
clipboard) and both games go straight into the race. A `vette2026://direct/CODE` link does the same, and
if VETTE! 2026 is already running, that copy joins. The race is on the host's course and with the host's
Driving setting; Lane centering stays each player's own, and jumps show on both screens. Both players
need the same version of VETTE! 2026 and their own copy of DOS VETTE! 1.1.

The host's router must let the friend in. Most routers open the game's port themselves when asked (UPnP,
NAT-PMP or PCP). If yours doesn't, the game says so and gives a **same-network code** that works for a
friend on your own network; for one over the internet, either let your friend host, or forward **TCP port
26989** to your computer in the router's settings, then set **Router** to *Port forwarded by hand* and
enter your **Internet address** (the router's status page shows it). Some connections (mobile networks,
some internet providers that share one address between customers) can't be reached at all: then the
other player hosts. The first time you host, Windows asks whether to let the game through its firewall:
allow it. As in the original, each game runs its own race: the traffic differs between the two screens,
and when both cars cross the line within a moment of each other, both players may see themselves win.
`--online-host` and `--online-join CODE` do the same from the command line. (A relay server for room
codes is in `server/`, [server/README.md](server/README.md), for those who run one: `--online-server URL`.)

**Sound effects and music.** The original only had the PC speaker. VETTE! 2026 can instead play its
sounds on an emulated **AdLib** card (the Enhanced preset; DOS VETTE! never supported one), or use the
**Macintosh** version's digitized sounds. The **music** can be the original's tunes, or the **PC-98**
version's FM soundtrack (its YM2203 chip, emulated). That soundtrack adds music to the menus and the
loser's screen, where DOS is silent. With AdLib or the Mac sounds, the engine keeps running under a skid
or the siren, where the original's single speaker voice cut it off. They also play the Mac version's
extra sounds at the matching moments, which the DOS game passes in silence: the start countdown, the
helicopter view's rotor, the horn (X), splashing into the bay, thuds over hill crests, the police
officer pulling you over, and the title sequence's bell and voices. The Mac and PC-98 options need those
versions' files (see [`Game/`](Game/README.md)). `--effects` and `--music` override the settings for one
run.

**Sound editor.** `vette_sfx` edits the AdLib sounds: every sound's FM instrument and pitch, with the
original PC-speaker version (read from your own copy of the game) to compare. A game that's running
picks up a saved change within a second.

**Graphics.** With **PC-98** or **Mac** graphics, VETTE! 2026 recognises the DOS pictures on screen
(title, garage, opponent selection, dashboard, crash pictures) and shows the other version's art in
their place, at your display's resolution. Everything the game draws over them (text, gauges,
selections) stays on top or moves into the new layout. The game itself runs unchanged.

**PC speed.** By default the emulated PC is fast enough that the game runs at its own built-in limit of
30 fps, as it did on fast 386/486 PCs; the game then also enables its rear-view mirror and building
windows. `--pc 286` emulates a 12 MHz PC/AT of 1989 (about 12-17 fps); `--cpu-hz <n>` sets any clock.

## Controls

- **Keyboard:** every key goes to the game, Esc included. F11 or Alt+Enter toggles fullscreen.
  **X** is the horn (with AdLib or the Mac sounds): the DOS game has none, so it plays the Mac's.
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

**Two players without a network.** The original's two-player race (a serial cable between two PCs,
[re/notes/12-two-player.md](re/notes/12-two-player.md)) runs on an emulated UART. `vette_link` runs two
games side by side on an in-memory cable, takes both through the original's menus into the race, and
plays scripted keys on each (times after the race start; `a:` and `b:` pick the side); `--delay`/`--jitter`
add network-like latency, `--log-frames` prints the packets, `--log-cars` both cars. Two windows on one PC:
`vette2026 --link-listen 5000` and `vette2026 --link-connect 127.0.0.1:5000` (development only; the
listening side picks the course with `--link-course`, `--link-delay`/`--link-jitter` add latency).

```
vette_link --game Game --seconds 20 --key a:7:02 --hold a:7.2:20:48 --shot 15 --delay 50 --jitter 100 --summary
```

**World extraction.** `vette_world` extracts the 3D city from the running original (the whole 80x80-cell
map, every object with all its levels of detail, and the car and building models) for the Enhanced
renderer's extended draw distance. To check the result, it redraws the original's view from the extracted
world and compares: every race frame and random whole-map view tested so far is pixel-identical. Its
source file header lists the options.

[`re/symbols.csv`](re/symbols.csv) is the shared, committed record of every named function and
variable. The Ghidra database and decompiler exports are derived from the copyrighted binary, so they
stay local (`re/ghidra/`, `re/out/`). Findings are written up in [`re/notes/`](re/notes/).

## Repository layout

| Path | Contents |
|---|---|
| `Game/` | Where players put their original game files (not tracked) |
| `src/` | `host/` (the emulator that runs the original), `game/` (native ports, smooth renderer), `platform/` (SDL3 window, sound, input), `ui/` (launch menu, program icon), `core/` (game files, settings), `enhanced/` (world extraction for the Enhanced renderer), `net/` (online play: rooms on the relay server, direct connections between the games, the serial link over either), `tools/` (`vette_run`, `vette_link`, `vette_fuzz`, `vette_world`, `vette_icon`, `vette_netcheck`) |
| `server/` | The relay server for online two-player races (a Cloudflare Worker); how to deploy it is in [server/README.md](server/README.md) |
| `docs/` | Plan, porting guide |
| `re/` | Symbol map, notes, Ghidra scripts and RE tools |
| `tools/reverse_engineering/` | Earlier asset/resource extraction scripts (reference, unverified) |
