# 10 — Graphics: the DOS pictures, their PC-98 and Mac counterparts, and replacing them

The launcher's **Graphics** option (DOS / PC-98 / Mac) shows another version's artwork in place of
the DOS screens, at the display's resolution. The DOS game keeps running unchanged underneath; the
substitution layer only looks at the frame it displays. Code: `src/assets/pict.*` (Mac PICT),
`src/assets/pc98_pic.*` (PC-98 .PIC and palette), `src/graphics/` (the layer), `src/tools/vette_gfx.cpp`
(headless review tool). Addresses are image-relative `SEG:OFF` like the other notes (the emulator and
Ghidra show the segment + 1000h). **confirmed** = seen in the code or in emulator traces; **TBD** = not
verified.

## 1. The DOS pictures

All pictures are 4-bit planar, **plane after plane** (all rows of plane 0, then plane 1, ...; MSB =
leftmost pixel; index = p0 | p1<<1 | p2<<2 | p3<<3, the EGA's planes), mostly packed with the PCX-style
RLE (`assets/rle.h`: a byte ≥ C0h is a run of `b & 3Fh` copies of the next byte). The game unpacks
them straight into video memory with `3009:8666` (AX = destination segment, DS:SI = packed data,
DI = offset, BH = bytes per row, BP = rows; plane by plane through the sequencer map mask, rows
80 bytes apart) **confirmed**. Files are read whole into the buffer segment `cs:C9CE` (5FF1h at run
time) by `3009:CBDA/CBF0`; full-screen pictures go to the off-screen page A800h first
(`3009:CCEB`) and are then latch-copied to the two display pages (A000h/A400h) by `3009:8820`.

| Picture | File / format | Size | Mode | Where it goes | Dynamic content drawn over it |
|---|---|---|---|---|---|
| Title | `TITLE.BIN`, RLE | 640×200 | 0Eh | full screen, revealed by a random-block dissolve | cable car, "Spectrum HoloByte presents" (`SPETRUM.BIN`), logo (`BIGVET.BIN`), approaching car (`VX.BIN`), man in white (sprites in VETTE.EXE, seg 0ACB), credits text. Copyright line is in the picture |
| Garage | `GARAGE.BIN`, RLE | 640×200 | 0Eh | full screen | menu bar highlight (selected car: red background, green text), statistics text, graph grid + labels + curve (grid is *not* in the picture), skill plates, `REDVETTE.BIN` car driving in/out, mechanic |
| Skill plates | `EGASKILL.BIN`: word = packed size, then RLE | 160×99 | 0Eh | (176,21) on both pages (`3009:DEB4`); selected plate marked by `3009:886E` (160×33 box) | — |
| Opponent selection | `EGAPIC.BIN`: word = packed size, then RLE | 320×200 | 0Dh | full screen (loaded by `3009:E074`, copied A800→A200/A000) | selected name strip turns yellow, curves in the graph, statistics text, turning 3D car (drawn by the race renderer) |
| Course map | `MAPPIC.BIN`, RLE | 640×200 | 0Eh | off-screen; the menu shows the left 160 columns (part of the city map) and the small map top right, with white text panels drawn by the game | course text, route markers |
| High scores | `HIGHSC.BIN`, RLE | 640×200 | 0Eh | full screen | names and times (**TBD**: not reached headlessly) |
| Winner | `WINNER.BIN`, RLE | 640×200 | 0Eh | full screen | **TBD** |
| Race dashboard | **no file**: RLE inside VETTE.EXE at program-image offset 100h (100h–1D07h) | 320×80 | 0Dh | rows 120–199 of both pages | hands, gauge bars, speed/RPM digits, cruise/auto lights, shift arrow, clock and message display, sign panel (speed limit, turn icons), damage |
| Crash | `CRASH0.BIN`, `CRASH1.BIN`, RLE | 176×128 | 0Dh | over the 3D view, e.g. (72,30), dissolved in | — (0 = into a truck, 1 = into the water) |
| Lost the race | `LOSER0-3.BIN`, RLE | 176×128 | 0Dh | as the crash pictures (**TBD** position) | — (0 Porsche, 1 Lamborghini, 2 Testarossa, 3 F40; speech bubble in the picture) |
| Notice to appear | `PENALTY.BIN`, RLE | 208×121 | ? | **TBD** | ticket text **TBD** |
| Ticket | `TICKET.BIN`, RLE | ? (11596 bytes unpacked) | ? | **TBD**: layout not decoded | |
| Horizons | `HORIZON0-2.BIN`, **raw** (not RLE) | 3200×24 each | 0Dh | off-screen in A400h (`load_horizon_planes` 3009:6793), latch-copied per frame by `blit_horizon` | — |
| Approaching car | `VX.BIN`, RLE | 5 frames: 128×16, 144×26, 176×38, 208×54, 224×51 (at packed offsets 0, 333h, 8C8h, 12F8h, 22E9h) | 0Eh | title, x = 320 → 344, y 137 → 97, each unpacked to both pages | — |
| Logo, garage car, "presents" | `BIGVET.BIN`, `REDVETTE.BIN`, `SPETRUM.BIN`: **masked sprites**, raw: word width/8, word height, then mask + 4 planes | 496×78, 368×45, 392×12 | 0Eh | masked blits `3009:8534`/`858F` | — |
| Scores, config | `SCORE.BIN` (text table), `CONFIG.BIN` | | | | |

`vette_gfx --trace-io` logs the file opens/reads and every `3009:8666` unpack with its caller.

## 2. PC-98 (1.02J)

PC-98 addresses are image-relative too (its code segment is image segment 0; Ghidra and
`re/out/VETTE_PC98.exe` show it as 1000h).

**Display:** 640×**200**, not 640×400: `0000:F2C0` sets INT 18h AH=42h CH=80h (200-line graphics) and
one 16-entry analog palette (`0000:F274`, table right after the routine, ports A8h/ACh/AAh/AEh =
index/R/G/B, 4 bits each) **confirmed**. The palette is the EGA's except that index 6 is dark yellow
(11,11,0) instead of brown, and the levels are 11/4/15 instead of 10/5/15: skin, wood and the hands on
the wheel look olive on the PC-98. `assets/pc98_pic.h` has the table (`kPc98Palette`) and reads it from
the player's VETTE.EXE (`read_pc98_palette`).

**Files:** `.PIC` = the DOS format without RLE or header (`assets/pc98_pic.h`); sizes live in the code
(`0000:8AF8` blits BH = width in DOS-320 units, doubled by `cs:8F21`; BP = rows). Loaders `0000:FF6C/
FF8C/FFAC` take DX = offset of the name in segment 2EE5h.

| PC-98 file | Size | vs DOS |
|---|---|---|
| TITLE, HIGHSC, MAPPIC, WINNER `.PIC` | 640×200 | **byte-identical** to the DOS pictures after RLE |
| GARAGE.PIC | 640×200 | "NORMAL" instead of "STOCK" in the menu bar; the graph's grid and labels are in the picture |
| EGAPIC.PIC | 640×200 | the opponent screen at **twice** the DOS width |
| DASH.PIC | 640×80 | the dashboard at twice the DOS width (same layout) |
| CRASH0/1, LOSER0-3 `.PIC` | 352×128 | twice the DOS width. **LOSER0-3 have the speech bubbles in Japanese** |
| PENALTY, EGASKILL `.PIC` | 208×121, 160×99 | identical |
| HORIZON0-2 `.PIC` | 3200×24 | identical to the DOS files |
| VX.PIC, TICKET.PIC | | VX = the DOS frames unpacked; TICKET 23192 bytes = twice DOS (**TBD**) |
| ANIM.DAT | 15 masked sprites (DOS format) | blocks 0, 9, 10 are byte-identical to BIGVET, REDVETTE, SPETRUM; the others (136×60, 3× 80×36, 4× 16×10, 2× 72×44, 2× 48×22) are sprites the DOS version keeps in VETTE.EXE |
| SPEED, TACHS, RPM1, GEARS, LEFTWHEE, WHEELBAR, RIDER, SPIN `.PIC` | | dashboard gauges/hands/wheel at 640 width, loaded by `0000:53FB` into separate segments. Layout **TBD** (not single images) |

Text the PC-98 game draws at run time (menus, messages) is Japanese, but we run the DOS game, so its
text stays English; only the LOSER pictures carry Japanese. With `SubstitutionOptions::english_text`
(default on) the DOS picture's English bubble stays on top there.

## 3. Mac Color VETTE! (1.02)

`Color VETTE!` has 192 `PICT`s (same ids in the B&W application), 8 `pltt` palettes (16 colours) and
draws in a 512×342 window; most screens are 512×322 (the menu bar takes 20 rows). All pictures decode
with `assets/pict.h`: version 2 PackBitsRect pixmaps (4- and 8-bit with 16- or 160-entry colour
tables), two DirectBitsRect (6482, 16709), 1-bit version 1 pictures (masks, route lines, text boxes).
Text opcodes appear only in the course boxes (5383, 6398, 15714, 27402: "Course One: Start: ...").
`vette_gfx --dump` writes them all as PNG.

| Purpose | PICT ids |
|---|---|
| Title background (no cable car, has the man in white) | 24592 (512×323) |
| Logo + mask; "Spectrum HoloByte presents" (1-bit); © line (1-bit) | 20793 + 31198; 31166; 21981 |
| Approaching car stages + masks; cable car + mask | 198/25396, 3499/439, 7083/22525; 6482/16709 |
| Title: man in white and lamppost animation frames (with background) | 20289, 23616, 26882, 28963, 30295; 25025, 29223; 5853, 12522, 15871 |
| Garage (buttons STOCK/ZR1/TWIN TURBO/SLEDGE HAMMER/ACCEPT, display, TEST) | 17313 |
| Car statistics cards (per car, two pages), skill plates | 68, 8099, 8232, 9766, 19522, 21566, 22261, 28562; 17619 |
| Garage animations (mechanic, hood) | 150–157 |
| Opponent selection (graph, 4 cards, stats panel, 3D box) | 12670; opponent stat cards 4701, 5204, 5424, 19349 |
| Course map (aerial) with course buttons; route lines; course boxes | 26478; 19759, 30266, 4208, 4358; 5383, 6398, 15714, 27402 |
| Dashboard; info strip (speed/RPM/gear/navigation) | 24055 (512×146); 179 |
| Dashboard pieces: MPH fill, RPM fill, gear patterns (4/5/6-speed), digits, signs, hands, shifter, cruise/auto | 16939/16972; 4612/12609/28429; 10000–10020; 3726, 18439; 29556; 3738, 16018, 16269, 27381, 30004; 9999; 4343, 7166, 9582, 13406 |
| Side views (look left/right) | 1091, 28120 |
| Horizon strips | 500–535 (36 × 512×24) |
| Lost to Porsche / Lamborghini / Testarossa / F40 | 135 / 136 / 137 / 138 (402×286; the drivers are paired with different cars than on DOS) |
| Crash into the water; crashed and towed | 140; 147 |
| Joe's Garage (repair) | 149 |
| Police officer; notice to appear; excuses dialog; "What's your excuse?" | 144; 145; 139; 142, 143 |
| High scores; victory party | 134 (512×342); 141 (512×342) |
| About box; credits | 2860; 148 |

## 4. Mapping and status

| DOS | PC-98 | Mac | Recognised by | Status |
|---|---|---|---|---|
| TITLE.BIN | TITLE.PIC (palette only) | 24592, letterboxed; DOS sprites + credits on top; DOS © line kept below | full picture, ≥ 50% | **done** |
| GARAGE.BIN | GARAGE.PIC; DOS menu bar kept | 17313; DOS menu bar in the top letterbox; statistics panel and graph curve moved into the Mac display; selected car framed on the Mac buttons; DOS car and mechanic hidden | ≥ 55% | **done** |
| EGAPIC.BIN | EGAPIC.PIC (2× width) | 12670; graph curves, statistics and 3D car moved into the Mac panels; selected opponent framed | ≥ 50% | **done** |
| HIGHSC.BIN | HIGHSC.PIC | 134 (same layout) | ≥ 50% | done, unverified in a live run (stills only) |
| WINNER.BIN | WINNER.PIC | 141 (same scene) | ≥ 50% | done, unverified live |
| dashboard (VETTE.EXE) | DASH.PIC (2× width) | 24055 stretched over the DOS rows; gauge bars, digits, lights and messages moved onto the Mac gauges (approximate) | rows 120–199, ≥ 60% | PC-98 **done**; Mac **first pass** (needs native gauges) |
| CRASH0/1.BIN | CRASH0/1.PIC | 147 / 140, scaled to cover the DOS rectangle | searched anywhere (byte-aligned), ≥ 60% | **done** |
| LOSER0-3.BIN | LOSER0-3.PIC + English bubbles | 135–138 | searched | done, unverified live |
| MAPPIC.BIN | (identical) | — | — | later: needs a native layout |
| PENALTY, TICKET, EGASKILL, sprites, horizons | identical / TBD | 145, 144, 17619, sprite PICTs, strips 500–535 | — | later |

## 5. The substitution layer (`src/graphics/substitution.h`)

**Detection: by content.** Each frame is compared with the DOS pictures (decoded at load from the
player's DOS files; the dashboard from the running program's memory, linear `10000h + 100h`, once the
EXEPACK stub has unpacked it). A quarter of the pixels is compared (every second pixel of every second
row, staggered); a picture is
recognised when the fraction of equal pixels reaches its threshold, and stays recognised down to 75%
of it (hysteresis: dissolves, menus drawn over it). Full-screen pictures compete (best wins); insets
(dashboard, crash, loser) are found independently, the floating ones by a search over byte-aligned
columns (the game's blits are byte-aligned) with a 64-point probe first. No game state, hook or
address is needed, so it is robust against everything the game draws, works the same with Smooth and
Enhanced rendering, and can't disturb Classic mode's timing. Cost: 0.1–0.4 ms per frame (PC-98),
0.3–2.3 ms (Mac, mostly the hole filling), measured by `vette_gfx`. Alternatives kept in reserve: a
CPU watch on the INT 21h entry for file opens (`vette_gfx --trace-io` does this), or on `3009:8666`
for every picture unpack with its destination; useful for sprites (below).

**What stays on top.** Inside a recognised picture, every pixel that differs from the DOS picture is
the game's own drawing (text, highlights, sprites, gauges, a game-drawn pointer) and is kept, in the
DOS frame's coordinates (`Composite::over`). For Mac art, which differs under the DOS text, one- and
two-pixel holes in that mask are closed (3×3 closing) so glyph counters don't show the art. Per screen
and art set the table (`make_table()` in substitution.cpp) adds:
- `keep` rectangles (DOS pixels always on top: the garage menu bar, the title's © line, the PC-98
  loser bubbles' English text),
- `hide` rectangles (no DOS pixels: the DOS car and mechanic in the Mac garage),
- `remaps`: a DOS rectangle drawn at another place of the art's layout (`Composite::pieces`), either
  whole or only its dynamic pixels, optionally leaving one colour behind (the DOS graph grid),
- `indicators`: a DOS rectangle showing a colour (the selected menu item) frames a rectangle of the art.

**Placement.** All rectangles are in the DOS frame's pixels; the frame is shown 4:3, so a frame pixel
is 4/W × 3/H display units (W×H = 640×200 or 320×200). PC-98 art has the DOS geometry (640×200, or
twice the width of the 320-wide race pictures) and is stretched onto the DOS rectangle exactly. Mac
art has square pixels and is placed `Contain` (letterboxed: 512×322 fills the width and 167.7 of the
200 rows, 16.1-row bars; 512×342 takes 178.1 rows), `Stretch` (the dashboard: 512×146 onto 320×80 is a
5% vertical stretch) or `Cover` (crash/loser pictures, 402×286 over 176×128: wider than the DOS
rectangle, the DOS pixels under the overflow are dropped).

**The PC-98 palette** replaces the EGA colours everywhere (also in frames nothing is recognised in), so
the PC-98 set looks like the PC-98 throughout.

## 6. What the presenter needs (integration)

`Substitution::compose(FrameView, Composite&)` per displayed frame; false = show the frame as today.
A `Composite` (`src/graphics/composite.h`) is drawn inside the same 4:3 picture rectangle as the frame
(`Presenter::fit()`), all rectangles in frame pixels scaled by the picture's size / (W, H):
1. if `base` is empty, fill the picture with `backdrop` (letterbox bars);
2. `base` (indices through `palette`, 0xFF transparent): like `over_` today, nearest/"sharp" scaled;
3. each `layers[i]`: an SDL texture per `Image*` (cache by pointer; the images live as long as the
   Substitution and never change), `SDL_SCALEMODE_LINEAR`, `SDL_RenderTexture(tex, &src, &dst_px)`;
   the art is straight-alpha RGBA (`SDL_PIXELFORMAT_ARGB8888`, blend mode BLEND);
4. `over` as an indexed frame with transparency (the existing `upload(..., transparency=true)` path);
5. `moved` uploaded the same way, then for each piece `SDL_RenderTexture(moved_tex, &src, &dst_px)`.

With the Enhanced 3D view, call compose() on SmoothRenderer's `Layers::over` instead of the frame:
transparent pixels stay transparent in `base`/`over`, so the order is `under`, scene, composite (no
backdrop, since `base` is non-empty in the race). Settings: `Art` (DOS/PC-98/Mac) and
`SubstitutionOptions::english_text`; build the files with `ArtFiles::from_game_dir(game_dir, &notes)`
(PC-98 through `Pc98Files`, Mac through `MacFiles`), call `set_program_memory(machine.memory().ram())`
after boot, and gray out an art set whose `available()` is empty (log `warnings()`). The launcher
option needs nothing else from the game.

`graphics::render(composite, w, h)` is a CPU reference of the same drawing (used by `vette_gfx` and
the tests); it could serve as a fallback (one texture upload per frame).

## 7. Later

- **Sprites on the Mac title and garage:** replace the DOS logo/car/cable car/"presents"/man with the
  Mac's masked PICTs. Positions are known from the blits (`3009:8534` masked sprite, `3009:8666` VX
  frames: a CPU watch gives sprite, frame and position), so this is a lookup, not image search.
- **Mac dashboard, natively:** speed, RPM, gear, lights and messages from the game's state, drawn with
  the Mac gauge pictures; the Mac horizon strips 500–535 for the panorama.
- **Mac garage statistics** with the Mac stat cards (one per car and page) instead of the moved DOS text.
- **Course map** for the Mac: aerial map 26478, route line PICTs and the course boxes, course from state.
- **PC-98 gauges** (SPEED/TACHS/...): decode their layouts from `0000:53FB` and the gauge drawing code.
- **Penalty/ticket/high-score/winner** screens in live runs (scripts that finish or lose a race and get
  a ticket) to place their dynamic text; TICKET.BIN's layout.
- Horizons and the 3D view in the PC-98 set are DOS pixels with the PC-98 palette; the PC-98 draws its
  race at 640 wide, which the Enhanced renderer already exceeds.
