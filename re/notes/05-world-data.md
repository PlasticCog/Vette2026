# 05 — World data: city map, cell types, objects, courses (DOS 1.1)

Addresses are image-relative `SEG:OFF`; data segment `124A` unless a segment is given. Code `3009`. Names are in
`re/symbols.csv`. **confirmed** = read in the disassembly (address given) or reproduced by a dumper. The dumpers
(Python + unicorn) are session scripts, kept locally in the gitignored `re/out/re-session/world/`; see "Dumpers" at the end.
**likely**/**TBD** = not proven.

## 0. Where the data comes from
- **confirmed:** the whole map is static data inside the EXE. Nothing writes the grid, tile, or type
  tables (`rd.py x` on 856F/8571/8524/9D73 shows reads only). The only data files the game opens are
  images (`*.BIN` names at 0ACB:5A3D.., 0ACB:776B.., FC51/FC5C), plus CONFIG.BIN (written at 3009:EEA4).
  HORIZON0-2.BIN are read at 3009:FE8F (3 × 0x9600 bytes, unpacked to offsets 0/2580/4B00 of a horizon
  buffer). MAPPIC.BIN is read at 3009:FBEE inside a keyboard menu loop (**likely** the course-selection
  map screen).
- **confirmed:** the race view only draws cells near the camera (see §2). Distant scenery is the
  horizon picture.

## 1. Coordinates and units
| Item | Value | Evidence |
|---|---|---|
| World axes | +X = **north**, +Y = **east**, +Z = up (left-handed) | heading 0 adds cos·D to x (3009:17AD via 3CE7); compass index = round(heading/45)&7 (3009:01E3) → DS:3D84 `" n","ne"," e",...` (**confirmed**) |
| Big tile | 0x8000 × 0x8000 units; local x/y are 15-bit; big-tile **row ↔ X**, **col ↔ Y** | 3009:17B1-1802 (x overflow → +0x22 row, y → +0x24 col) (**confirmed**) |
| Cell | 0x800 units; 16 × 16 per big tile; cell x = x>>11 (+0x26), cell y = y>>11 (+0x28) | 3009:3589 (**confirmed**) |
| Elevation | z = elev × 224 (0xE0); map uses elev 0..3 | 3009:3291-32A1, 4160:079E-07AE (**confirmed**); values from dump |
| Scale | 1 unit ≈ 3–4 in (cars 24 × 54 units ≈ 6 × 14 ft; cell ≈ 512–640 ft ≈ one SF block) | **likely**: models give ≈3 in; the HUD mph formula (notes 04) ≈3.3 in; the player box ≈3.7 in (notes 03) |

## 2. City map
### Grid (all **confirmed**, dump in `map_types.txt`)
| Addr | Size | Meaning |
|---|---|---|
| 856F | w | big-tile rows = **5** |
| 8571 | w | big-tile cols = **5** |
| 8524 | 25 w | near pointer to the 16×16 cell array of each big tile, index `row*cols+col` (code: `[bp-7ADC]`, `[bx-7ADC]` at 3009:327A, 359C, 4160:05E7; `imul cl` at 3009:188F/1E28 limits rows·cols to a byte) |
| 8573-9D72 | 12 × 0x200 | the 12 unique big-tile arrays (T0..T11 = 8573, 8773, …, 9B73) |

Cell array: 16 × 16 cells × 2 bytes, **offset = cx·32 + cy·2** (x-major), byte0 = **cell type**, byte1 = **elevation**
(3009:3270-3291, 3009:35B4-35E9). The car struct's `+0x19` is a pointer to its cell word.

Map size: **5 × 5 big tiles = 80 × 80 cells = 163840 × 163840 units** (≈ 7.8 mi if the scale is right).
Big tiles, rows north (4) to south (0), columns west → east:

| row | col 0 | 1 | 2 | 3 | 4 |
|---|---|---|---|---|---|
| 4 | T10 | T10 | T10 | T10 | T11 |
| 3 | T5 Golden Gate | T8* | T9* | T10 | T10 |
| 2 | T4 Presidio / Marina | T3 | T2 NE + Embarcadero | T10 | T10 |
| 1 | T1 | T6 | T7 (Market) | T8 Bay Bridge | T9 Bay Bridge east end |
| 0 | T0 SW (Zoo) | T11 | T11 | T10 | T10 |

- The drivable city is T0, T1, T6, T7, T4, T3, T2: about cells X 0..47 × Y 0..47 (`map_topdown.png`).
  The landmark names come from the start/finish cells and models (§6, §7). They are **likely** and were
  not checked against the PDF map.
- T10 (9973) is a water filler. It holds a copy of the Vista Point end (cells cx0-4, cy1-3) and of the
  Bay Bridge east end (cx13-15). These copies sit in every T10, but only the copy in big tile (4,0) can
  be reached. T8/T9 also appear again at (3,1),(3,2) as an unreachable second bridge. T11 (9B73) is
  water ringed by barrier walls (types 246-250, model 51) with a park island (type 210).
  (**confirmed** data; "unreachable" is **likely**.)

### Per-big-tile tables (index = big-tile index = `[843A]/2`, `[843A]` = camera bt·2 set at 3009:035C)
| Addr | Size | Meaning | Status |
|---|---|---|---|
| 8556 | 25 b | ground colour of the big tile, copied to cs:57E0 by 3009:5A35 (6 when `[2AD4]`≠0, i.e. on the freeway). Values 7 (land) / 9 (water) | **confirmed** (5A41-5A4B) |
| 3C8E | 25 b | horizon picture: 0/2/4 → word DS:3CA7[] = 0000/2580/4B00 = HORIZON0/1/2 slot (3009:6724). **Note:** indexed via 3009:685E = `row*5+col` with a **hard-coded 5** | **confirmed** |
| 3D22 | 25 b | street-set id 1..3 for the diagonal-street layer (only bt 7=3, 11=1, 12=2 are non-zero) | **confirmed** read at 71D2/6877; meaning **likely** |
| EF5A | 25 w | vehicle list A per big tile: `{w entity, w cell_idx(cx*16+cy)}`… FFFF. F00C (empty: player/opp/police only), F01E (city traffic), F0B0 (Golden Gate tiles 10,15,20), F0E6 (Bay Bridge tiles 8,9). cell_idx is rewritten at runtime (3009:18B4) | **confirmed** |
| EF8C | 25 w | list B: EFC0 (16 pedestrians E8E8..EAC8, cell idx 00..33) for city tiles 0,5,6,7,11,12; EFBE (empty) elsewhere | **confirmed** (content); pedestrians per notes 04 §7 |
| D2AE | 25 w | pointer to a 16×16 byte array per big tile (D2E0 = blank, D4E0..D9E0, DE12, DF12). 2-bit fields used by the dash sign display 3009:65E9 | **confirmed** layout, meaning **TBD** |
| DAE0 | 25 w | second 16×16 byte array per tile (DB12 blank, DC12 for tiles 0,5) | **TBD** |
| 2243:0682 | 25 w | per-tile pointer into street-name data in segment 2243 (strings "sloat", "wawona", "geary", …) used by 3009:690C.. | **confirmed** pointer use, format **TBD** |

`DD12` (256 b, one shared 16×16 layer, index cx·16+cy of the **player** cell 2D5B/2D5D): bits 0-1 = street-set
(compared with 3D22[bt]); bits 2..6 one-hot = name via DS:3D3B → "columbus", "market", "embarcadero",
"cervantes", "marina". When the player stands on a matching cell inside the sub-area x&7FF<0x680 and
y&7FF>0x180, 3009:71BF sets `[40E2]=1`. `[40E2]=1` bypasses the heading snap to N/E/S/W at 3009:09BD
(**confirmed** code; reading it as "diagonal street" is **likely**). 3009:6871 shows the street name.

## 3. Cell types (DS:9D73, 256 near pointers; 161 types used by the map)
Record (**confirmed**, 3009:32A5-32D6 and 3009:463E):
```
w header          lo byte = ground-shape class 0..9, hi byte = collision class (index into DS:C0A6)
list1: {w code, w dx, w dy, w dz}* , w FFFF     drawn immediately in list order (ground layer)
list2: {w code, w dx, w dy, w dz}* , w FFFF     "sortables": copied to DS:35C5 with absolute positions,
                                                merged with this cell's vehicles (EF5A/EF8C, 32F8/34D6),
                                                depth-sorted (4562/4686, reject depth outside -0x400..0x1400)
                                                and drawn far-to-near (4603); [0019]!=0 skips list2
                                                (vehicles are still added)
```
`code` = near address in 3009 of a draw routine. dx, dy, dz are offsets from the cell origin (x, y,
elevation·224). Before the call, the routine gets the absolute object position in `[3220..3224]`, and for
sortables `cs:[259E]` = camera depth (LOD). Records share tails in memory. Unused type slots point at
garbage.

**Ground-shape class** (header lo, **confirmed**): it is passed to `4160:0515` → jump table DS:2B22.
The class gives the height offset inside the cell; the cell's z = that offset + elev·224
(3009:184B-1852, 2875-287F). Inside the cell, u = min(x&7FF, 0x700) and v = max((y&7FF)−0x100, 0). Each
class returns h>>3 (0..0xE0) and a slope code in BX:

| class | h | BX |
|---|---|---|
| 0, 5 | 0 (flat) | 0 |
| 1 | min(u, v) | 1 (u) / 4 (v; 0 if v=0) |
| 2 | u | 1 |
| 3 | min(u, 0x700−v) | 1 / 3 |
| 4 | v | 2 |
| 6 | 0x700−v | 3 |
| 7 | min(0x700−u, v) | 4 / 3 |
| 8 | 0x700−u | 4 |
| 9 | min(0x700−u, 0x700−v) | 4 / 3 |

So a ramp cell rises one elevation step (224) across 0x700 units. The meaning of the BX slope codes is
**TBD**: classes 1 and 7 return 4/3 for the v-axis where 2 would be expected, which may be an original
quirk. The ground height is disabled while `[2AD4]`≠0.

**Collision class** (header hi, **confirmed** 3009:181E-1828, 18E0): DS:C0A6[class] → list of boxes
`{w xmin, w ymin, w xmax, w ymax}` (cell-local), terminated by FFFF. 3009:18E0 tests the car's in-cell
position (2CBF/2CBD) with margins (2B8B/2B8D, per compass octant from DS:2BC9/2BD9). The handler at
3009:1976 compares the **address** of the box that was hit:

| box | effect (3009:1984..1B6E) |
|---|---|
| C606 (class 3F = type 1, water) | `[2C69]=FF`, `cs:[3]=FF` → race ends. Runtime: driving west from the course 1 start shows the car-in-the-water picture and ends the race (host emulator) |
| C5AC (class 3A, type 75 Vista Point) | course 1 finish; in loop mode (`[9]`) switches to course 2 |
| C28A (class 0F, type 74 Bay Bridge east end) | course 2 finish (loop → course 3) |
| C5B6 (class 3B, type 208 Zoo) | course 3 finish (loop → course 4 = end) |
| C278, C294, C2BE, C554, C5E2, C5F4, C610, C62C, C64E | freeway entry: `[2AD4]=1/2/3`, `[8156]` = freeway route 0..8 (some are ignored on a given course) |
| C29C, C2A4, C2AC, C63C | `[si+0E]=0x20` (speed clamp, **likely** toll booths) |
| C67C (type 61), C66A (types 53-60, 65-72: Bay Bridge deck) | `[8]=FF` + 3009:1954 (if `[2ACF]`: `[2CCF]`=0x2E eye height, pitch `[2BEB]`=−8 or −17). For C66A the 1954 call is skipped on course 3. Purpose **likely** bridge-deck camera |
| C210, C3BC | `[2C6D]=1` (**TBD**) |
| C358 | opponent/police event (`[FC4F]`, 2454) (**TBD**) |

Which cells carry these boxes is drawn in `map_topdown.png` (magenta F1-F3 / W0-W8).

## 4. Rendering window
See notes 03. Only the camera cell and ≤5 neighbours are drawn (3009:30C6 → 31EE): 2 cells ahead and one side row. Off-map
cells clamp to the edge big tile while keeping the wrapped cell index (3009:3216-325B). The "+2 north" entries 36D4/36F0
use CX=0x0FD0 instead of 0x1000 (drawn 0x30 units too close). **confirmed**

## 5. Static object geometry (code-drawn objects)
### Packed vertex block (**confirmed** 3009:3879 → 3D2F, 3A64; axis table 3009:39B9)
```
+0  w x, w y, w z                origin (routines copy [3220..3224] in; some first adjust it, e.g. 2EAF -0x300)
+6  count × {w a0, w a1, w a2}    count = [3446]; vertex i = vertex i-1 + Σ delta(a_j); output count+1 vertices
```
- `a_j == 0` adds nothing. Otherwise lo = signed selector (a multiple of 6) and hi = right shift.
  The vector read is at `DS:317E + byte[3181+2−j] + |lo|`. The bytes at 3181..3183 are 60 30 00, so
  `g = (0x30·j + |lo|)/6 − 1` indexes the 24-vector table DS:3184 (8 vectors per axis):
  axis = g/8, k = {1,2,3,5,7,8,9,11}[g%8]. Word j normally selects axis j, but a larger |lo| reaches
  the next axes (seen in real data).
- delta = sar(±k·A, hi), rounded by adding the last bit shifted out (`sar; adc 0`). A = axis vector of
  length 0x400 (1024). 3009:02EE-0334 builds the three axes by rotating DS:001B
  {(400,0,0),(0,−400,0),(0,0,400)} (camera-input order (y, −z, x)) with the camera matrix DS:32B1.
  So **axis 0 = world +Y (east), axis 1 = +Z (up), axis 2 = +X (north)**, and with an unrotated camera
  delta = ±k·1024 >> shift.
- `[321A]` (set to 1 by 3879) = number of origin + count groups in the stream (3A64 outer loop). Only 1
  has been seen.
- This matches notes 03; the cross-axis selectors are the addition.

### Polygons and faces (**confirmed**)
- Polygon list for 3009:B5B3 (AL = colour, BX=266E) and B5BC: `{w n, (n+1) × w idx}` … `w FFFF`.
  idx = vertex·4, and the list **repeats the first index** to close the polygon (B5C6-B5CB steps
  2·(n+1)).
- Face table, 5-byte form (3009:3B78 vis, 3B0E draw): `{w poly_list, b va, b vb, b colour}` × `[337D]`, table
  `[337E]`. va and vb are vertex·6 offsets into the camera-space array 266E. The face is visible iff
  `(V[vb]−V[va])·V[va] > 0`, i.e. vb is a point behind the face. Flags go to DS:333D.
- 6-byte form (3009:3BF5 vis, 3B43 draw): `{w poly_list, b v0, b v1, b v2, b colour}`. The normal comes
  from 3 vertices (3C76).
- 3009:38A0: list `{w poly_list, b colour}` … FFFF, no culling.
- Line list (3009:405E, list `[3269]`, colour `[3337]`): `{b 6·n (not read), b n}` then n × `{b idx0·4, b idx1·4}`.
- Plain-xyz billboard (3009:393F/3DAC): CX × `{w x, w y, w z}`, rotated by yaw `[3226]` (383C sets it
  to the camera yaw).

### Catalogue (`world/obj_static/catalogue.tsv`, OBJ per routine)
All 125 routines referenced by used cell types were run under unicorn (`emu_objects.py`) with the
renderer entry points hooked. Every routine finished without error. Main groups:

| Routines | What | Notes |
|---|---|---|
| 276A, 2772..27BC | cell ground quad 0x700×0x700, colour 7 (2772/2788/27A6/27BC: two triangles, sloped variants ±224) | block 5855.. |
| 7238/7246 (256² corner, c8), 724C/7270/72C4 (N-S road strip 0x700×0x100, with lane-marking lines), 7252/7318/736C (E-W strip), 7258..7610 sloped versions | road surface: every cell has a road along its west edge (y 0..0x100) and north edge (x 0x700..0x800) | |
| 26F6/2724/26E6 | full-cell quad c9 (water) / c2 | type 1 = water |
| 2EAF family: 2EE9, 2EF4, 2F39..2FFA, 486F | 1536×1536 box building, heights 96..1984, with window quads (2E3C, skipped if `[2ABE]`≠0); variants patch the shared block (e.g. [6900]=1E) | collision box (128,384)-(1664,1920) |
| 3015 | Transamerica-style pyramid (4096 high) | |
| 2AA7 | Golden Gate span (one object, drawn from 16 cells at dx 8192..−22528, red cables) | types 20-35 |
| 2C25 / 2C33 | Bay Bridge truss / deck pieces (types 53-60 / 65-72, dy steps of 2048) | LOD-dependent |
| 2B8C | toll booths | |
| 3790/37A2/37B4/37C6 | street lamp in 4 orientations (vis6 faces + pole line); a 1-vertex stub when depth ≥ 0x800 | |
| 383C | tree billboard | |
| 4752/4758/475E, 482D.., 46D9..474D | stairs / terraces of the stepped cell types 48/49 | |
| 487C, 48FD, 4928 | line-only markings (white) | |
| 47B8/47C8/47D8 → block 5F19; 4788/4798/47A8 → block 6015 + `[38D7]=FF`; 481D (posts, drawn if `[38D7]`) | start (5F19) / finish (6015) banners, **only drawn when `[2CD5]` = their course** | types 74, 75, 208 |
| B912..BAEA stubs | `[3226]=yaw; AX=model; jmp B9BA` (rotated) or `jmp B9E6` (axis-aligned): data-driven models below | |

## 6. Data-driven models (segment 245A) — `world/models_dos.py`, OBJ in `world/obj_models/`
- **confirmed** 3009:B9F6: table **245A:6FF8**, 8 bytes per id (ids 0..58, 40 empty):
  `{w near_hdr, w (unused), w far_hdr, w far_colour}`. If `cs:[259E]` ≥ 0x800, the far header is used
  (generic boxes 245A:6A4E..6A66), with far_colour patched into the box faces at 245A:69B6+0x12·i.
- Header (copied to E01E by 3009:B765): `{w seg(=245A), w nverts, w vert_ptr, w octant_table}`.
- Vertices: `{w x, w y, w z}` local, with **x = east, y = down, z = north** at yaw 0. Evidence: B8DC/B848
  feed them straight into the camera matrix whose input order is (Y, −Z, X). Vertices 0-3 = reference
  frame (0,0,0),(−50,0,0),(0,0,50),(0,50,0), used only by 3009:9CAF to pick the octant (3 sign bits).
- Octant table: 8 word pointers → `{w skip-countdown…, FFFF}` (vertices not transformed in this view, B20F)
  then `{w face_ptr…, FFFF}` (painter order).
- Face: `{w flags, w colour, prims…, FFFF}`, prim = `{w n, (n+1) w idx·4}`. flags bit 14 = skip.
  Bits 1-3 select the primitive through cs:9C72: 0 = fill B5BC, 2 = fill variant B63F (**TBD** what
  differs), 4/6 = lines B6E1. Bits 0, 13 and 15 set E0D4/E0D5/E0D3 (**TBD**).
- Placement: B9BA = yaw `[3226]` (pitch/roll 0), B9D6 = vehicles (yaw/pitch/roll from the entity),
  B9E6 = no rotation. Matrix setup: 9C7B / B8DC.
- Identification via the Mac OBJS names (§8): 0 Taxi, **1 player Corvette** (3009:28D3 draws model 1 in
  chase view; no Mac twin), 2 cable car, 3 generic car, 4 fire truck, 5 ambulance, 6 Porsche, 7 police,
  8 bus, 9 Lamborghini, 10 F40, 11 motorbike?, 12 Testarossa, 13 truck, 14 Chinatown gate, 15-17/19
  Block1-4, 18 windmill, 20 Coit Tower, 22/29-35 pedestrians (Crossing, DrugDeal, jogger, juggler, nun,
  lawyer, blindman), 24 Zoo, 25 pier, 26 Ferry Building, 27 pier 39, 36 Ghirardelli, 37/58 barriers,
  38 Fisherman's Wharf, 39 Hyatt, 41 Holiday Inn, 43 Doda, 44 Fairmont, 45 St Mary's, 46 Bank of
  America, 47 St Peter, 48 Goddess, 54 Japantown, 55 Palace of Fine Arts, 56 Holiday park; 51 = 2048-long
  wall (tile-border barrier). (Names are **likely**; vertex sets match at ≥0.88.)

## 7. Courses (per-course data; the map itself is shared)
- **confirmed** `[2CD5]` = course 1..4 (4 = loop 1→2→3). Start record pointer DS:2D69[course−1]
  (3009:1FDC) → `{w x, y, z (player), w x, y, z (opponent), w heading, w bt_row, w bt_col}`:

| course | record | player (x,y,z) | heading | big tile (row,col) → cell | finish (box/type) |
|---|---|---|---|---|---|
| 1 Zoo → Vista Point | 2D95 | 4064, 4480, 12 | 270 | (0,0) → cell (1,2) | C5AC / type 75 at cell (65,2) |
| 2 Golden Gate → Bay Bridge | 2D71 | 4064, 4552, 12 | 270 | (4,0) → (65,2) | C28A / type 74 at (18,75) |
| 3 Bay Bridge → Zoo | 2D83 | 6048, 22912, 12 | 270 | (1,4) → (18,75) | C5B6 / type 208 at (1,2) |
| 4 loop | 2D95 | as course 1 | | | switches course at each finish (1AE1-1B6B) |

  Each course starts on the previous course's finish cell. The start and finish banners are course-gated
  objects in the same cells (§5).
- Car templates: DS:2D19[cs:8DAE] → player struct image (size `[2D67]`=0x32), DS:2D21/[000A] → opponent
  (3009:1F89-1FB5). **confirmed** copy; the meaning of cs:8DAE (selected car) is **likely**.
- Opponent route: DS:F7B8[course−1] → table indexed by `[FC4F]` (0..3) → route = sequence of
  `{w street-segment ptr | w freeway route id (0..3) or −1}` (3009:10B3-10EF). A segment starts with
  {x, y, heading, ?} followed by {x, y} waypoints … −1 (F9A2, F9C0, F9E2, …). **likely**; format details
  **TBD**.
- Freeway mode: `[2AD4]`≠0. Route `[8156]` 0..8 uses tables DS:7560 / 754E (path) and 75B4 (exit
  record {x, y, ?, row, col, heading}) (4021:0F2C, 3009:785B). **TBD**.

## 8. DOS vs Mac (`Vette_Mac_EN/.../(Folder) Color VETTE!`, big-endian) — `world/compare_mac.py` → `compare_mac.txt`
- **Map: different.** Mac `MAPS 1000 Main_Map` = word count + **52 × 47 cells × 4 bytes** `{w type, w attr}`
  (2444 cells; width from autocorrelation), a flat grid with no big-tile indirection. DOS = 5 × 5 big tiles
  of 12 unique 16 × 16 arrays of `{b type, b elev}` = 80 × 80 cells. Type numbers differ. (**confirmed**)
- **QUAD = DOS cell-type records** in big-endian form: `{w ground, w coll, {w obj_id, dx, dy, dz}*, −1,
  {…}*, −1}`, 257 records. Index built by the 68k loader in Color VETTE! CODE 1 @0x2C64 (word scan for
  two −1s, 300-entry pointer table). One record (191) is malformed in the Mac data. 46 DOS types have an
  exact Mac twin (same header and every offset), but only types 0 and 1 share the index. 69 more share the
  header plus one list. Conclusion: common source data, re-authored and renumbered for the Mac.
  Consistent object-id mapping, e.g. 276A→1, 7238→4, 7270→6, 7318→7, 2AA7→17/18, 2C25/2C33→41.
- **OBJS = the DOS segment-245A models.** Mac `{w size_words, w nverts−1, nverts × {w −1, x, y, z},
  w nfaces, faces {w flags, w colour, w n, (n+1) idx, −1}…}` (index scale **TBD**). The vertex sets match
  with **Mac = 20 × DOS** for vehicles, people, piers, Chinatown gate, Ferry Building, windmill,
  Doda, St Mary's and Goddess, and **Mac = 1 × DOS** for the large buildings (Block1-4, Coit, Zoo,
  Ghirardelli, Hyatt, Fairmont, BofA, St Peter, Japantown, Palace, Holiday park). DOS has precomputed
  octant face orders that the Mac lacks. No twin was found for DOS models 1 (Corvette), 21, 23, 51-53, 57.
- The DOS code-drawn objects (§5) were not matched to Mac OBJS. **TBD**: the Mac draws ground pieces by
  QUAD object id.

## 9. Connectivity (game/drivable.h, `vette_world --drivable [--no-freeways]`)
Drivable = not inside a collision box that isn't a trigger (the finishes, on-ramps, toll speed limits,
bridge-deck camera boxes, C210/C3BC/C358), less the car's smallest half-size (12; DS:2BC9/2BD9 give 12-24 by
octant), sampled every 32 units. The original city is in parts that only the freeways join (**confirmed**):
the central city (about 390 cells' worth, with the 480, 80, Central Skyway and Embarcadero on-ramps and both
Doyle Drive exits); the Sunset and Zoo (170: course 1's start, the Hwy 1 and Presidio on-ramps, 280's exit);
the Marina (66, no course or freeway point); the Bay Bridge with its end of town (course 2's finish, course 3's
start, the 280 on-ramp and the four freeways' exit at 18,49); the Golden Gate and Vista Point (course 1's
finish, course 2's start, the Doyle Drive on-ramps and the Presidio/Hwy 1 exit at 45,2). Their borders: the F7
barrier column at cell y 15 (Sunset / central) and y 9 (Marina / central), the FF diagonal from 16,37 to
23,44 (the Bay Bridge's end of town), water between the Great Highway's end (27,2) and the Marina, and between
the Golden Gate approach's end (43,2) and the Marina's street (38,*). The cell types: 01 water (the race
ends), 00, 0F, 10, E0, E1, F6-FA, FF full or near-full walls (F6-FA, FF: a yellow-topped barrier and its
posts), 29 the Golden Gate approach's deck, 2B/2D/2E open street cells (2B draws nothing: the big tile's
ground shows, water in the Marina), 3E park (a trigger box, posts), C1 diagonal street, city blocks
(CE/CF/D7/D8, 64-66...) a building box in the middle and streets round it.

## Open questions
1. World-unit scale (≈3 in?) should be confirmed against the speed or odometer constants.
2. Meaning of the slope code BX from 4160:0515 (pitch/roll of the car?), and the classes 1/7 v-axis codes.
3. D2AE / DAE0 / 2243:0682 per-tile dash tables (street names, signs, speed limits) need decoding.
4. ~~EF8C list B~~: pedestrians, and the `(idx & 0x33)` rule is a 4-cell periodic pattern (notes 03, notes 04 §7).
5. Freeway route tables 7560/754E/75B4 and opponent route segments F9xx need a full format.
6. B63F "fill variant" vs B5BC, and the model face flag bits.
7. The game palette may not be the default EGA palette (renders use the default palette).

## Dumpers (local only: `re/out/re-session/world/`, gitignored)
These were written during the RE session as throwaway scripts: `mapdata.py` (parsers); `dump_map.py`, which writes a top-down PNG
of the decoded cell polygons plus type/elevation text dumps; `emu_objects.py` (runs all 125 cell-type draw routines
under unicorn with the renderer entry points hooked, 6 variants); `objgeom.py` (packed-vertex decoder);
`export_static_objects.py` and `models_dos.py` (OBJ export of code-drawn and segment-245A models); `compare_mac.py`,
`macres.py` and `mac68k.py` (Mac resource and 68k loader readers). Their outputs (`out/map_topdown.png`, OBJ
files, `compare_mac.txt`) are next to them. Moving the useful ones into `re/tools` is a proposed next step.
