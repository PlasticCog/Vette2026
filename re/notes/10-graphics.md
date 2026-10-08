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
| Opponent selection | `EGAPIC.BIN`: word = packed size, then RLE | 320×200 | 0Dh | full screen (loaded by `3009:E074`, copied A800→A200/A000) | selected name strip turns yellow, curves in the graph, statistics text, turning 3D car (drawn by the race renderer: each frame E0AC copies the car's box, 27 bytes × 99 rows at A800:0FD5, from the kept picture to the draw page; E44E draws model `[FA6E + [FA6C]]` placed by the 7 words at FA92 (E43D); then the pages flip, 24FE/24D0, and E42F turns it on; the Enhanced view draws it again from draw_model's camera-space vertices, enhanced/menu_car.h) |
| Course map | `MAPPIC.BIN`, RLE | 640×200 | 0Eh | off-screen; the menu shows the left 160 columns (part of the city map) and the small map top right, with white text panels drawn by the game | course text, route markers |
| High scores | `HIGHSC.BIN`, RLE | 640×200 | 0Eh | full screen | names and times (**TBD**: not reached headlessly) |
| Winner | `WINNER.BIN`, RLE | 640×200 | 0Eh | full screen | **TBD** |
| Race dashboard | **no file**: RLE inside VETTE.EXE at program-image offset 100h (100h–1D07h) | 320×80 | 0Dh | rows 120–199 of both pages, from the off-screen copy cs:0023 (`3009:5F00`) | hands, gauge bars, speed/RPM digits, cruise/auto lights, steering arrows, shift arrow, clock and message display, sign panel (speed limit, turn icons), the right hand on the shifter |
| Dashboard looking left (F1) / right (F3) | **no file**: RLE in VETTE.EXE at 0A226h / 08C95h (unpacked at race start into cs:001F / cs:001D, `3009:5C64` / `5C29`) | 320×80 | 0Dh | rows 120–199 when DS:2B87 = −85 / +85 (`3009:71A8`) | the side mirror above it |
| Crash | `CRASH0.BIN`, `CRASH1.BIN`, RLE | 176×128 | 0Dh | over the 3D view, e.g. (72,30), dissolved in | — (0 = into a truck, 1 = into the water) |
| Lost the race | `LOSER0-3.BIN`, RLE | 176×128 | 0Dh | (72,36), like the crash pictures (`DDED` → `065A`) | — (0 Porsche, 1 Lamborghini, 2 Testarossa, 3 F40; speech bubble in the picture) |
| Penalty | `PENALTY.BIN`, RLE | 208×121 | 0Eh | (360,20) over the high scores (`D007`) | each offence's ticket count, the penalty time (see the police stop below) |
| Police stop | `TICKET.BIN`: one RLE stream of three pictures: an 8×7 check mark (packed 0–1Bh), the ticket 96×121 (from 1Ch), the officer 96×120 (from EB7h); 11596 bytes unpacked | | 0Dh | ticket (40,0) (`DC6F`), officer (0,1) (`DD1A`) | check marks (`DC9D`) |
| Horizons | `HORIZON0-2.BIN`, **raw** (not RLE) | 3200×24 each | 0Dh | off-screen in A400h (`load_horizon_planes` 3009:6793), latch-copied per frame by `blit_horizon` | — |
| Approaching car | `VX.BIN`, RLE | 5 frames: 128×16, 144×26, 176×38, 208×54, 224×51 (at packed offsets 0, 333h, 8C8h, 12F8h, 22E9h) | 0Eh | title, x = 320 → 344, y 137 → 97, each unpacked to both pages | — |
| Logo, garage car, "presents" | `BIGVET.BIN`, `REDVETTE.BIN`, `SPETRUM.BIN`: **masked sprites**, raw: word width/8, word height, then mask + 4 planes | 496×78, 368×45, 392×12 | 0Eh | masked blits `3009:8534`/`858F` | — |
| Scores, config | `SCORE.BIN` (text table), `CONFIG.BIN` | | | | |

`vette_gfx --trace-io` logs the file opens/reads and every `3009:8666` unpack with its caller.

**The dashboard's dynamic parts** (front view; `3009:61C3` walks a table of 7 values at DS:3B0B with
handlers at cs:579E, redrawing a part when its value changes) **confirmed**:

| Part | Value | Drawn by | Frame rectangle |
|---|---|---|---|
| Left glove on the wheel | DS:2B84 steering: sprite = 4 if negative, else min(s/2, 3) | `5F39` (5 sprites at cs:001B) | (0,123)–(64,199) |
| Wheel top mark | DS:2B84 | `6066` | (120,121) 6×12 |
| Speed bar + digits | DS:2D43 → DS:3AA1 = mph (`hud_speed_value`), bar (mph−3)/2 units of 40 | `5E4C`, digits `6230` at 1C52h | (65..104, 148..195) |
| Rev bar + digits | player +2Ch → cs:588F (×100 rpm) | `6136`, digits at 1C5Dh | (151..192, 148..195) |
| Shift light | DS:2D51 | `600D` | (144,144) 16×10 |
| CC / AUTO lights | DS:2AD7 (cruise) / DS:2D50 (automatic) | `5F6D` / `5F7E` | (112,136) / (128,136) 16×7 |
| Steering arrows | DS:2B84 sign, blinking (DS:3A98) | `62CC` → `601C`/`6022` | (112,170) / (128,170) 16×10 |
| Hand on the shifter + gear gate | DS:2D45 gear; shown in neutral, while DS:2ACB, and for 2 s (DS:3A9D = 2 × frame_rate) after a change | `63B1` → `6372` | (223..320, 165..200) |
| Clock, messages | race clock; street names, warnings | `6C60`, `5DC3`, ... (green, colour 2) | (224..320, 136..166) |
| Road signs | speed limit, no-turn, arrows | `64E6` | (240..320, 120..135) |

**The course map screen** (`3009:FBCC`): MAPPIC.BIN is unpacked to A800h; DS:FD10 is the course on show
(1–4, arrow keys), DS:FD0E the selection. For each course the screen is rebuilt on A400h: MAPPIC
copied, a white panel (`88AF`: course 1 at (160,0) 320×200, course 2 at (0,60) 480×140, course 3 at
(0,0) 288×136, none for course 4), the course title and historical landmark text (`FD7F`/`FDA7`/
`FDCF`), the right-hand column (`FD4F`, always at (480,60): course, route, on-ramps, the red "Arrow Key
to view" / "Enter Key selects" at rows 180–199), and the course's part of the overview map highlighted
by XORing colour 1 into the rest of it (line records at DS:FC6C/FCA2/FCBD drawn by `4CBF`).

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

**Where the Mac draws them.** The Color VETTE! application's globals are initialised from MPW's
compressed data in CODE 10 (from 1ABEh: records `count, A5 offset, bytes`; a count byte 1xh means a
12-bit count with the next byte; 60h records are relocations; below-A5 size 7A28h). From them:
the course map's route rectangles per course (4208 at (0,92), 4358 at (175,92), 19759 at (0,164),
30266 at (0,97)), the course boxes (6398, 5383, 27402, 15714 at (300,10), (30,150), (150,10),
(300,10), each 193×100), the five buttons (y 297–317, x 38/132/226/320/413, 64 wide) and the
opponent screen's labels. The dashboard pieces are drawn at start into an off-screen sheet (their
rectangles there are also in the data) and copied to the screen by code: their screen places were
found by matching the "off" pieces against the dashboard picture 24055 (speedometer 16939 at (120,75),
tachometer 12609 at (272,75), CRUISE 4343 at (184,70), AUTO 13406 at (232,70), steering arrows 24445
at (192,91) and 12092 at (232,91)); the hands sit at (0,36), and the shifter, its gate and the
digits where the Mac's layout has room (see `mac_dash.cpp`).

**The police stop** (ROOKIE and PRO; DS:FC4F is the skill, 0–2). An offence sets a bit of cs:DADD
and the police alert DS:2C57: speeding (1, `speeding_check` 21A2), hitting a car at speed 140 or more
(2, "moving violation", `player_contact_response` 1664), hitting a wall or object (4, "reckless
driving", `collision_box_event` 1BB2), a pedestrian (8, "vehicular manslaughter", 16C3), not stopping
within 10 s of the siren (10h, "evading arrest", `police_step` 143D). `police_step` sends a patrol car
after the player (DS:F7C2); within 100 units it pulls the player over (DS:2C59, speed and gear zeroed),
loads TICKET.BIN (`DD00`) and the race loop shows, until the player drives off, first the excuse list
(`DADE`: boxes `88AF`, text `DBD0`, the chosen line XOR-highlighted `DCDC`; Enter picks one and
`DC51` decides at random against a table at DS:5B1B whether the officer accepts it: cs:DADC = 0) and
then the officer (`DD1A`, DADC = 0) or the ticket with its check marks (`DC6F`, DADC = 1; a pedestrian
skips the excuses). Driving off counts the ticket: cs:C9D8 + 2i (offence i) goes up by one; at the end
of the race `CEC8` turns the counts and the seconds per ticket (cs:C9D9 + 2i) into text at DS:5A82 +
4i and the penalty time at DS:5A96, adds it to the player's time and `D007` shows PENALTY.BIN.
The Mac's notice to appear (145) lists other offences: speeding → 106 speeding, moving violation →
173 hit and run, reckless driving → 123 reckless driving, vehicular manslaughter → 180, evading arrest
→ by "failure to respond" (no Mac line). In `vette_gfx` the stop is reached with
`--poke 40:40.2:224A:2C57:1` (alert) and `--poke 40:40.2:4009:DADD:<bits>` on a PRO race standing at
the start (the patrol car comes at about 49 s), a loss with `--poke 40:40.3:224A:FA45:FF --poke
40.2:40.3:4009:0003:FF` (DS:000A picks the opponent's picture), the penalty with counts poked into
cs:C9D8 + 2i before the race ends. The game's own `--poke` writes the data segment only: a loss there
is DS:FA45 = FF before driving into the bay and DS:2C69 = 0 poked every 20 ms through the splash (the
crash picture would win otherwise).

**High scores** on the Mac: each top-ten time is moved onto its own Mac rank number (the Mac's list
is spaced 17.9 rows apart against the DOS 11 × 1.545), not as one block.

## 4. Mapping and status

| DOS | PC-98 | Mac | Recognised by | Status |
|---|---|---|---|---|
| TITLE.BIN | TITLE.PIC (palette only) | 24592, letterboxed; Mac "presents" (31166), logo (20793 + mask 31198) and the approaching car (198, 3499, 7083 with masks) in place of SPETRUM/BIGVET/VX; DOS cable car, man in white and credits on top; DOS © line kept below | full picture, ≥ 50%; the sprites at their fixed places over it (≥ 80%) | **done** |
| GARAGE.BIN | GARAGE.PIC; DOS menu bar kept | 17313; DOS menu bar in the top letterbox; statistics panel and graph curve moved into the Mac display; selected car framed on the Mac buttons; DOS car and mechanic hidden | ≥ 55% | **done** |
| EGAPIC.BIN | EGAPIC.PIC (2× width) | 12670; graph curves, statistics and 3D car moved into the Mac panels; selected opponent framed | ≥ 50% | **done** |
| MAPPIC.BIN | MAPPIC.PIC (identical) | 26478 with the course's route and box, the course button framed, the DOS instructions in the bar under the map (`mac_map.cpp`; course from DS:FD10) | the overview map (480,0) 160×60, XOR 1 tolerated, ≥ 60% | **done** (live) |
| HIGHSC.BIN | HIGHSC.PIC | 134; course number, times and the top ten moved next to the Mac labels | ≥ 50% | **done** (live, with `--poke`) |
| WINNER.BIN | WINNER.PIC | 141 (same scene, nothing dynamic) | ≥ 50% | **done** (live, with `--poke`) |
| dashboard (VETTE.EXE) | DASH.PIC (2× width) | 24055 stretched over rows 120–199; arcs, digits, lights, hands, shifter and gate drawn from the game's state (`mac_dash.cpp`); clock/messages (colour 2) and road signs moved onto the Mac displays | rows 120–199, ≥ 60% | **done** (live) |
| side dashboards (VETTE.EXE) | — (DOS pixels, PC-98 colours) | 1091 (F1, look left) / 28120 (F3, look right), full width standing on the bottom (91 / 82 rows) | rows 120–199, ≥ 60% | **done** (live) |
| CRASH0/1.BIN | CRASH0/1.PIC | 147 / 140, scaled to cover the DOS rectangle | searched anywhere (byte-aligned), ≥ 60% | **done** |
| LOSER0-3.BIN | LOSER0-3.PIC, the English bubbles painted into the art | 135–138 (by car) | searched | **done** (live, with `--poke`) |
| TICKET.BIN: ticket | TICKET.PIC (2× width) | 145, the DOS offences checked with 146 on their nearest Mac lines | (40,0), ≥ 60% | **done** (live) |
| TICKET.BIN: officer | TICKET.PIC, the English words painted in | 144 with his words 142 beside him | (0,1), ≥ 60% | **done** (live) |
| excuse list (no picture) | — | "List of Excuses" 139 and "What's your excuse?" 143 over the dashboard 24055, the chosen line framed | two boxes at (0,100), ≥ 60% | **done** (live) |
| PENALTY.BIN | PENALTY.PIC (identical) | 145 with the counts in the offences' boxes and the DOS penalty time on a strip under it, below the Mac's "TOP TEN DRIVERS" | (360,20), ≥ 60% | **done** (`--poke`) |
| EGASKILL, garage car, horizons | identical | 17619, 24443 + 21053, strips 500–535 | — | later |

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
Enhanced rendering, and can't disturb Classic mode's timing. Cost per frame (`vette_gfx`, Release):
PC-98 0.1–0.6 ms; Mac 0.2 ms (course map) to 0.7–1.4 ms (title with its sprites, garage, dashboard),
2.2 ms on the high scores; the live game logged 1.4 ms on average over menus and a race. With the
draw tracker (below) PC-98 is 0.4–1.0 ms and Mac 0.3–1.9 ms. Alternatives kept in reserve: a
CPU watch on the INT 21h entry for file opens (`vette_gfx --trace-io` does this), or on `3009:8666`
for every picture unpack with its destination; useful for sprites (below).

**What stays on top.** Inside a recognised picture, every pixel that differs from the DOS picture is
the game's own drawing (text, highlights, sprites, gauges, a game-drawn pointer) and is kept, in the
DOS frame's coordinates (`Composite::over`). A pixel the game drew in the colour the picture has
there doesn't differ, so an art pixel most of whose 5×5 neighbourhood is DOS pixels becomes one too
(`support_mask`: no art slivers or hairlines inside the game's drawing). For Mac art, which differs
under the DOS text, one- and two-pixel holes in that mask are also closed (3×3 closing) so glyph
counters don't show the art. Per screen and art set the table (`make_table()` in substitution.cpp) adds:
- `keep` rectangles (DOS pixels always on top: the garage menu bar, the title's © line, the PC-98
  loser bubbles' English text),
- `hide` rectangles (no DOS pixels: the DOS car and mechanic in the Mac garage),
- `remaps`: a DOS rectangle drawn at another place of the art's layout (`Composite::pieces`), either
  whole or only its dynamic pixels, optionally leaving one colour behind (the DOS graph grid),
- `indicators`: a DOS rectangle showing a colour (the selected menu item) frames a rectangle of the art,
- `course_panels`: the course map's white text panel for the course on show (DS:FD10), all DOS pixels
  (PC-98; where the map under a panel is white too, the PC-98 map showed through the panel's white as
  faint lines before; the draw tracker sees the `88AF` fill as well).

**Text and sprites, exactly** (`draw_tracker.h`, after `Substitution::attach(machine)`). A pixel the
game draws in the picture's own colour doesn't differ from the picture, so the difference mask loses
it: black credits over the title's black, holes in strokes, and on the Mac letterbox bars a line of
text the bars' colour all but vanished. CPU watches on the game's drawing routines therefore keep a
shadow of video memory (A0000h–AFFFFh, all pages): per pixel the colour of the text or solid
rectangle drawn there, "a sprite pixel", or nothing, plus per byte the text cell it belongs to. A
shadow pixel counts only while the frame still shows its colour there (so nothing stale survives an
erasure the tracker doesn't see), and only on the page on display (`Ega::display_start()`).

| Routine (3009:) | What | Registers |
|---|---|---|
| `88EB` | 8×10 text, 640 mode (credits, garage stats, high scores) | DS:SI zero-terminated, DI, AH colour; page cs:8DAB; glyph DS 124Ah:F3DE + (c−20h)×10, rows 50h apart; spaces are skipped but are cells |
| `F3E1` | 8×10 text (menus, map panels) | DS:SI count-prefixed; DI = (cs:[8F4A+2·BX] >> cs:926A) + CX; page cs:8DA9; row stride byte DS:7763; even/odd rows masked by cs:E5FE/E5FF; chars < 20h skipped |
| `5D55` | 8×7 text, 320 mode (garage opponents, dash messages) | DX page, DI, AH colour; c ≥ 3Ah: c &= DFh; glyph seg 3243h (run time):14C0 + (c−2Eh)×7, rows 28h apart |
| `858F` from `854C` | masked sprite, mask pass (return address 854Fh; colour passes ignored) | page cs:8DAB, dest cs:7FB7 (+skip 7FB3), source DS:SI+cs:7FB5 (+skip 7FB1), rows 7FBB, bytes 7FB9; first mask FFh >> (7FA1 & 7), last FFh if 7FA5−7FAB ≥ 8 else FFh << ((7FA5 & 7) ^ 7); written = mask & ~source |
| `88AF` | solid rectangle (course map text panels) | AX:DI, BH bytes × BP rows, BL colour (set/reset), stride 28h + cs:926B |
| `8666` | picture unpack: clears | AX:DI, BH × BP, stride 50h |
| `4160:0896` (via `065A`) | picture unpack in 320 mode: clears (crash, lost race, ticket, officer, check marks) | AX:DI, BH × BP, stride 28h |
| `8820` | page copy (latches): copies | AX:SI → DX:DI, BH × BP, stride 28h + cs:926B |
| `886E` | XOR rectangle (highlight bars): XORs the colours | AX:DI, BH × BP, BL, stride 50h >> cs:926A |
| `CD22`, `C9A5`, `884B` | byte runs: A800:SI → draw page:DI (the title restoring its credits area each frame), draw page → display page, A800 → draw page:1F40 | CX bytes (884B: 1F40h) |
| `F318`, `E600` | byte-run erasures: clear | A000:DI, CX bytes; A000:DI, BL bytes × BH rows, stride byte [1ACB:7763] |
| `8F00`, `8F25` | mode sets: clear everything | |
| `7FE7` … `82B1` | the screen transition (only the title calls it): page BX onto the displayed page AX, as DS:E01C picks (0 at first: the dissolve `8239`, one byte × 5 rows at a time copied at `8285`; afterwards `82C1` picks one of the slides/scrolls 4–7); every way ends at `82B1` | until then, what the dissolve hasn't brought in counts as drawn (the old, black screen): the art dissolves in block by block, exactly |

In `compose()` a tracked pixel stays on top of the art whatever colour is under it (a sprite pixel
not where the sprite's own art replaces it; text drawn over such a sprite stays); inside text cells
the difference mask is used as is (no hole closing, which smeared glyphs); and on the background
picture a text cell outside or across the art's edge (the Mac letterbox bars) keeps its whole DOS
cell, background colour included, so every line stays readable.
Without `attach()` (only `set_program_memory()`) the layer falls back to the colour difference. The
credits are one block of eight lines that scrolls up from 11.3 s to about 15.5 s (one row per frame,
redrawn at a new DI after `CD22` restored the area), then the attract mode stops on the garage. Seen
in runs: `88EB` (credits, garage statistics, map panels, high scores), `5D55` (opponents, dashboard
messages), `F3E1` (the Esc options menu's bar over the race view), `88AF` (map panels only).
`vette_gfx --every 17` composes every displayed frame as the game does and logs what is recognised
and how well: the only partial frames in a whole session are the title's dissolve (1.2–1.9 s, 50% →
99%); every other screen change is instant (mode set or page copy).

**Placement.** All rectangles are in the DOS frame's pixels; the frame is shown 4:3, so a frame pixel
is 4/W × 3/H display units (W×H = 640×200 or 320×200). PC-98 art has the DOS geometry (640×200, or
twice the width of the 320-wide race pictures) and is stretched onto the DOS rectangle exactly. Mac
art has square pixels and is placed `Contain` (letterboxed: 512×322 fills the width and 167.7 of the
200 rows, 16.1-row bars; 512×342 takes 178.1 rows), `Stretch` (the dashboard: 512×146 onto 320×80 is a
5% vertical stretch) or `Cover` (crash/loser pictures, 402×286 over 176×128: wider than the DOS
rectangle, the DOS pixels under the overflow are dropped).

**Screens drawn from the game's state** (`screen_handler.h`): after the table's work for a screen, a
handler may add layers and moved pieces. It reads the game's values from the emulator's memory
(`game_state.h`: data segment 124Ah and code segment 3009h at +1000h) and probes the DOS frame
itself where that is simpler and exact: a light is on when the DOS game drew over the dashboard
picture in its rectangle. Images never change once handed out (the presenter caches textures by
pointer): the gauge arcs are 65 precomputed steps each, revealed along the band from its start
(distance along the lit pixels, calibrated at the Mac scale's marks); the course boxes with their
text are made once, when the DOS font (DS:F3DE, 8×10) can first be read.
- *Mac dashboard*: everything of the DOS dashboard is hidden except the clock/messages (only colour 2
  moves, so the shifter hand that overlaps them in DOS doesn't; DOS rows 138–164, the clock and both
  message lines, go into the Mac display's black, art rows 55–81, clear of the shifter window below)
  and the road signs. Values: speed
  DS:3AA1, revs cs:588F (both what the DOS digits show, at most one game frame apart from the frame
  on screen), gear DS:2D45 and gearbox DS:2D47 (gate pictures 10000–10005, 10006–10012, 10013–10020:
  neutral, the gears, reverse), steering DS:2B84 (hands 16269, 3738, 16018, 27381; 30004 = no hand,
  for the other way, like the DOS sprite choice). Lights, arrows, shift light and the shifter's
  visibility are probed in the DOS frame.
- *Mac course map*: course = DS:FD10; route PICT recoloured red, the course box with its text in
  the DOS font (Chicago 12 title drawn twice for weight, Monaco 9 lines), the button framed; the DOS
  screen is hidden except its two red instruction lines, moved under the map.

**Sprites over a full-screen picture** (`ScreenSpec::parent`): looked for only when their parent
picture is recognised, at fixed places (the title's sprites are always drawn at the same spots);
masked sprite pixels don't count, frame pixels that still show the parent (a sprite wiping in) don't
stay on top, and `Fit::Native` draws the Mac sprite at the parent art's scale, centred on (or standing
on the bottom of) the DOS sprite.

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
(PC-98 through `Pc98Files`, Mac through `MacFiles`), call `attach(machine)` after boot (the
dashboards, the gauges and the course map read the game's memory every frame, and the draw tracker
watches its text and sprite routines; the machine must outlive the Substitution), and gray out an art set
whose `available()` is empty (log `warnings()`). The launcher
option needs nothing else from the game.

`graphics::render(composite, w, h)` is a CPU reference of the same drawing (used by `vette_gfx` and
the tests); it could serve as a fallback (one texture upload per frame).

## 7. Later

- **Garage**: the Mac statistics cards (one per car and page) instead of the moved DOS text; the
  DOS car driving in stays hidden (the Mac garage has its own car; 24443 + 21053 is its side view).
- **Title**: the cable car (6482 + mask 16709) and the man in white with the lamppost (20289... and
  25025...) are the DOS game's sprites from VETTE.EXE, still drawn as DOS pixels.
- **Mac dashboard extras**: the Mac's sign strip 29556 for the road signs (needs the sign codes from
  `64E6`), the info strip 179 for the full-screen view, the horizon strips 500–535 for the panorama.
- **Course map text**: the DOS historical-landmark paragraph has no place in the Mac layout and is
  not shown in Mac mode; the course box text uses the DOS font (the Mac's Chicago and Monaco are in
  the System file, not the game).
- **PC-98 gauges** (SPEED/TACHS/...): decode their layouts from `0000:53FB` and the gauge drawing code;
  PC-98 side views (LEFTWHEE.PIC is 640×76, maybe the left view).
- Horizons and the 3D view in the PC-98 set are DOS pixels with the PC-98 palette; the PC-98 draws its
  race at 640 wide, which the Enhanced renderer already exceeds.
