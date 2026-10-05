# 03 — 3D renderer and visibility rules (DOS 1.1)

Addresses are image-relative `SEG:OFF`; data is in `DS=124A` unless marked `cs:`. Names are in
`re/symbols.csv`. **confirmed** = read in the disassembly at the cited address; **likely/TBD** = inferred.

## Summary: what limits draw distance

| # | Limit | Where | Value | Lift in a native renderer? |
|---|---|---|---|---|
| 1 | **Cell window** around the camera | `draw_world_cells` 3009:30C6 | camera cell + ≤5 cells: 2 ahead, 1 to **one** side only. 0x800 units per cell, so 4096–6144 units ahead | Render-only for static scenery. The side effects (rows 5–6) must still run on the original window |
| 2 | **Sorted-object far cull** (buildings, vehicles) | `cull_sortables` 3009:4562 / 44D0 | camera depth ≥ 0x1400 (5120) is dropped; lateral \|x\| < depth+0x600 (buildings) or +0x80 (vehicles) | Yes (render-only) |
| 3 | **Per-object LOD / skip rules** written into each object routine | 3009:2B36…2DF8, 7270…76E9, 3790…37C6, B9F6 | axis distances 0x400/0x800/0x1000/0x1800/0x2000; depth key 0x800 | Yes: always pick the near model |
| 4 | **Detail toggles** | DS:19 (`B` key), DS:2ABE (`W` key), DS:2AC7 (mirror), DS:18 (mirror pass) | see table below | DS:19/DS:2ABE look render-only; mirror is **not** (row 6) |
| 5 | **Traffic is a 4×4-cell repeating pattern bound to the view** | 3009:32F8/33C6/34D6 | a vehicle is drawn in whichever window cell matches `(cell & 0x33)`; the draw writes that cell back into the vehicle list | Visual only: draw the periodic replicas from a pure draw pass. The cell write-back stays in the original-window pass (row 6). See "Traffic" below |
| 6 | **Rendering writes simulation state** | 3009:33C6–34C8, 351C–3585 | collision candidates DS:2B7A/2B7C, vehicle cell binding, DS:312E | Keep an original-window "visibility pass" for these writes |
| 7 | 16-bit camera-relative coordinates | 3009:3917, 3D8C, 31EE | positions are words in the camera big tile's frame (big tile = 0x8000) | Use 32-bit/float math for far cells |
| 8 | Horizon backdrop at a fixed distance | `horizon_line` 3009:59B3 | ground point 25000 units ahead; panorama 8 px/degree | Re-think: far geometry will overlap the painted skyline |
| 9 | Highway mode road ring | `highway_frame` 3009:775E | 32 road slices ahead, 11 cars | Separate renderer (see "Highway mode") |

There is **no far clip plane** in the projection. Only the near plane (z ≥ 1) is clipped. Distance is limited
entirely by rows 1–3 (row 9 on highways). No global per-frame object or polygon counter was found.

## Frame render order (inside `frame_loop`, see notes 02)
1. `camera_from_car` 3009:1D89: camera struct DS:2C71 = car DS:2D35 + eye height 10 + DS:2CCF, chase offset
   DS:2C7D (helicopter view). Sets DS:843A = camera big-tile index×2. **confirmed**
2. `set_viewport` 3009:3778 with SI = DS:2AC5 (normal 3579 or full-screen 356B). **confirmed**
3. Camera yaw DS:2C77 = car heading DS:2D3B + view offset DS:2B87 (0 / +85 / −85), pitch DS:2C79 = DS:2BEB (01A5–01E0). **confirmed**
4. `camera_matrix_from_angles` 3009:3F2D → DS:32B1; then DS:316F = diag(+1,−1,+1)/32 · DS:32B1 (DS:001B is
   that constant matrix, product inline at 02EB–0334); `build_axis_table` 3009:39B9 → DS:3184. **confirmed**
5. `draw_sky_ground_horizon` 3009:5A35 (sky/ground fill, then the horizon panorama). **confirmed**
6. Traffic update for the camera's big tile, `BCFB`/`BB72` (simulation, see notes 04), then `cell_lookup` 3009:3589 on the camera. **confirmed**
7. `draw_world_cells` 3009:30C6: back-to-front cell walk; each cell is drawn with `draw_cell` 3009:31EE. **confirmed**
8. Viewport bottom line `draw_view_border` 3009:0E93: line records DS:2B02 (y=119, colour 8) or DS:2B0B
   (y=199, colour 0, full screen), drawn by `draw_line`. **confirmed**
9. After the simulation and HUD: the optional rear-view mirror `draw_mirror_view` 3009:0666. Then the page flip
   (`page_flip_select` 3009:24FE and `crtc_set_start_vsync` 3009:24D0, which waits for two vertical-retrace edges). **confirmed**

## Video setup
- The race view runs in **EGA mode 0Dh, 320×200×16** (`video_set_mode_ega_0d` 3009:8F25, called before the
  frame loop at 0155). Draw pages are A000/A200 (`cs:000D`/`cs:000F`), the CRTC start addresses
  `cs:0009`/`cs:000B` (0x2000/0). `cs:0005` = current back page, and `cs:0011` = back-buffer segment. **confirmed**
  (PLAN.md and the first notes 02 said 640×200. That is mode 0Eh, used only for menus and screens; notes 02 is corrected.)
- 40 bytes per row; row-offset table `cs:497F` (row×40). **confirmed**
- Off-screen VRAM at A400 (`cs:0013`) holds the three horizon panoramas (see below). **confirmed**
- `crtc_set_start_vsync` 24D0 waits for the start of a vertical retrace, writes the start address, then waits for the
  **next** retrace start. The following frame's first wait cannot finish before one more retrace. So the frame rate is
  capped at **refresh/2** (≈35 fps at 70 Hz, 30 fps at 60 Hz), i.e. `elapsed` ≥ ~8 PIT ticks. **likely** (derived from
  the code; check in the oracle). This bounds the "reference machine profile" in notes 01.
- Runtime (host emulator, 12 MHz 286 profile): `frame_rate` 12–15 during the race; the CPU class was not "fast", so
  `mirror_off` = FF and `windows_off` = FF from the start (no mirror and no window detail unless toggled). The 3D view
  fills rows 0–119 with the dash below, as the viewport table says.

## Viewports (`set_viewport` 3009:3778)
Each descriptor is 7 words, copied to the variables listed at DS:355B: x_left DS:315E, y_top DS:315A,
x_right DS:3160, y_bottom DS:315C, centre_y DS:316B, centre_x DS:3169, height DS:316D. **confirmed**

| Descriptor | x | y | centre | Used for |
|---|---|---|---|---|
| DS:3579 | 0–319 | 0–119 | (160, 60) | normal in-car view (dash visible) |
| DS:356B | 0–319 | 0–199 | (160, 100) | full-screen view (dash off, toggled by 372A) |
| DS:35A3 | 192–319 | 0–35 | (256, 18) | rear-view mirror, front view |
| DS:3587 | 80–127 | 96–119 | (104, 108) | mirror while looking right (+85°) |
| DS:3595 | 200–272 | 84–119 | (236, 102) | mirror while looking left (−85°) |

## Camera, transform and fixed-point formats
- **World coordinates** are 16-bit words. A big tile is 16×16 cells, a cell is 0x800 units, and the local x/y in a big
  tile is 0..0x7FFF; overflow carries into the big-tile row/col (1D89, 31EE). Cell elevation is byte×224. **confirmed**
  Scale estimate (**likely**): 1 unit ≈ 3–4 in (7.5–9.5 cm). Model sizes give ≈ 3 in (notes 05), the HUD
  mph = speed·3/16 formula gives ≈ 3.3 in (notes 04), and the player box (half-extents 24×12, DS:2BC9/2BD9) gives ≈ 3.7 in.
  A cell is then ≈ 510–640 ft, about one SF block; the draw distance below is ≈ 1000–1500 ft.
- **Angles** are integer degrees 0..359. `sincos_deg` 3009:4E3C → [BX]=sin, [BX+2]=cos, **Q15** (0x7FFF ≈ 1.0), from
  the cos(0..90) table at DS:38DE. `atan_deg` 3009:4EC8 takes a Q8 ratio and binary-searches the tan table DS:3994. **confirmed**
- **Camera matrix** DS:32B1 (3×3 words, row-major, Q15) is built by `camera_matrix_from_angles` 3009:3F2D from the
  negated yaw/pitch/roll at [SI] (= DS:2C77/2C79/2C7B), with saturation to 7FFF/8000. **confirmed** A port must
  transcribe the exact product order and truncation. With s/c = sin/cos of −yaw (a), −pitch (b), −roll (c), and "·" a
  Q15 product (high word of 2xy), the entries read at 3F48–405C are:
  ```
  | sa·sb·sc + ca·cc     sa·sb·cc + ca·sc     −sa·cb |
  | −cb·sc               cb·cc                sb     |
  | ca·sb·sc + sa·cc     −ca·sb·cc + sa·sc    ca·cb  |
  ```
  The triple products are rounded in two steps; the two-term sums saturate. With pitch = roll = 0 a vector
  (Δy, −Δz, Δx) gives Z = Δx·cos yaw + Δy·sin yaw, so yaw 0 faces +x and yaw 90 faces +y. **confirmed** (transcription; check in the oracle)
- **World → camera** (`world_to_camera_point` 3009:3917, `xform_points_to_camera` 3009:3D2F/3D8C): the input vector
  is `(Δy, −Δz, Δx)` (Δ = object − camera, 16-bit), then `v' = v·M` (`vec_mul_mat3` 3009:3D51: Q15 products
  summed in 32 bits, shifted left 1, high word). Camera space: **X right, Y down, Z forward (depth)**. **confirmed**
- **Packed model vertices** (`xform_packed_block` 3879 → `unpack_vertices` 3A64): one word per axis j; word 0 =
  no change. Otherwise low byte = signed selector, high byte = right shift. The selector picks vector
  `DS:317E + byte[3181+2−j] + |sel|` of the 24-entry table DS:3184 (`build_axis_table` 39B9: k·axis for
  k ∈ {1,2,3,5,7,8,9,11} for each of the 3 axes). Large selectors reach the next axis's vectors (real data does
  this). A vertex = previous vertex + Σ sar(±k·axis, shift) with `adc` rounding. The axes are DS:316F = world
  +y, +z (up), +x, each of length 1024, so a delta is k·1024/2^shift world units. **confirmed** (3A94–3AD4;
  the decoder in notes 05 reproduces all 125 map routines)
- Second path: plain {x,y,z} vertex lists rotated by the object yaw DS:3226 (billboards such as trees 383C; some
  terraces): `build_object_matrix` 3DF9, `mat_mul_object_camera` 25CB → DS:331D, `xform_plain_rotated` 393F. **confirmed** (notes 05)
- Third path: the data-driven models in **segment 245A** (cars, pedestrians, landmark buildings; table 245A:6FF8,
  8 bytes per id, 59 ids). The entries B912–BAEA set the id in AX and the yaw in DS:3226, then
  B9BA/B9D6/B9E6 → `model_select_lod` B9F6 (near model if `obj_depth` cs:259E < 0x800, else a generic box) →
  `draw_model` B765, which picks a precomputed painter-ordered face list from three view-sign bits (`model_octant_test`
  9CAF). **confirmed** (format decoded in notes 05)

## Projection (`project_vertices` 3009:A685)
- `sx = (X·256)/Z + centre_x`, `sy = (Y·256)/Z + centre_y`. The focal length is a fixed **256** (the shift by 8 at
  A6A9). Division is signed `idiv`, truncating. **confirmed** `project_points_simple` 4160:086B and `project_points_simple_near` 3009:25A0 do the
  same with the variable DS:3167 (=256).
- **Near plane z = 1**: a vertex with Z < 1 gets flag 2 (A66B) and is later clipped in camera space. **confirmed**
- **Rounding bias** (measured: the Enhanced SceneBuilder's validation mode only matches the frames to the pixel
  with it). Camera-space values come out as floor(c·32767/32768) of the exact value (the Q15 matrix is scaled by
  0x7FFF, and the high word of a product floors), so a ground point 10 units below the eye gets Y = 9; with the
  truncating `idiv` the screen position is then centre + trunc(X·256/Z). A near-plane crossing is at
  t = (1 − z_a)·32768/(z_b − z_a) in Q15. Each shifts edges by up to a pixel, mostly upwards near the camera. **likely**
- **Divide overflow** is caught by a custom **INT 0 handler** (`int00_div_overflow` 3009:2565, installed by 253D). It
  returns AX=0x7FFF and skips the 2-byte `idiv` when the return address points at F6/F7 (286+ fault semantics;
  on 8086 it just returns). On 0x7FFF the projector recomputes a 32-bit quotient, stores it at +0x400/+0x402 and sets
  flag 1. **confirmed** → the host interpreter must deliver INT 0 on divide overflow.
- Projected vertex record at DS:1A86 + 8·i: {sx, sy, flags(0 ok/1 32-bit/2 behind), ptr to camera vertex}. That
  gives room for 128 vertices per model. **confirmed** (buffer 1A86–1E85, 32-bit copies at 1E86)
- FOV: horizontal 2·atan(160/256) = **64°**. Vertical 26.4° in the 120-row view and 42.7° full-screen. The same
  focal length is used for x and y on 1.2:1 (tall) EGA pixels, so the image is about 1.2× vertically stretched on a
  4:3 screen. **confirmed** (constants) / derived (angles)

## Faces, clipping, fill
- Back-face test, two face formats. `faces5_visibility` 3B78: {w polygon list, b vA, b vB, b colour} (offsets
  are vertex·6); visible iff (V_B − V_A)·V_A > 0 in camera space (V_B is a point behind the face).
  `faces6_visibility` 3BF5: {w polygon list, b v0, b v1, b v2, b colour}, normal from 3 vertices. Flags go in DS:333D,
  count DS:337D, table ptr DS:337E. Drawn by `faces5_draw` 3B0E / `faces6_draw` 3B43. **confirmed**
- `fill_poly_list` 3009:B5B3 (B5BC without loading BX): SI → {w n, (n+1) × w vertex·4}… FFFF (the first index is
  repeated to close the polygon), AL = colour. `poly_gather_clip` B4B2 checks
  `cs:9C7A`: if no vertex is behind the camera or overflowed, it takes a fast copy. Otherwise `clip_near_plane`
  A7C0 interpolates (Q15 parameter) to z=1, then a **guard band**: A8D4/A9F8/AB1F/AC45 clip the 32-bit coordinates
  to ±0x4000: A8D4 x ≥ −0x4000, A9F8 y ≥ −0x4000, AB1F x ≤ 0x4000, AC45 y ≤ 0x4000 (fields read at A8F4/AA18/AB3F/AC65). **confirmed**
- 2D viewport clipping (Sutherland–Hodgman, buffers 0A86/0E86): `clip_left` 40F7 (315E), `clip_top` 41E5 (315A),
  `clip_right` 42D4 (3160), `clip_bottom` 43C2 (315C). **confirmed**
- Optional 2D winding test 9CD8 (enabled by DS:E0D4). It is inverted when DS:18 = FF (mirror pass, 9CFA). **confirmed**
- Edge scan → span tables (left x at DS:002E+2y, right x at DS:01BE+2y, 200 rows) → `fill_spans` 3009:9F3B.
  EGA **set/reset** fill (GC0 = colour, GC1 = 0F, GC8 = bit mask) with edge masks DS:E038/E040. Colour byte: low
  nibble = colour. If the high nibble differs, a second pass (9FFF) overlays the high nibble on a 0x55/0xAA checkerboard
  (two-colour **dither**). **confirmed**
- Lines: `draw_line_list` 405E, and the self-modifying Bresenham `draw_line` 3009:5170 (it patches its own code at
  52E4–5375 and jumps through DS:38D8). **confirmed** → the interpreter must handle self-modifying code.

## World selection: the cell window (`draw_world_cells` 3009:30C6)
- `cell_lookup` 3009:3589 (SI = camera struct) splits x/y into the cell origin (DS:2CC1/2CC3 = pos & 0xF800) and the
  in-cell offset, and stores the cell indices at +0x26/+0x28 and a pointer to the cell entry at +0x19. **confirmed**
- The heading DS:2C77 picks one of 4 sectors. DS:35C3 = facing axis (0 = ±x, 1 = ±y); some road models read it.
  The camera's in-cell offset across the facing axis (`& 0x7FF ≥ 0x400`) picks which **one** side row is drawn.
  Cells are visited **far to near, side row first, the camera's own cell last** (`draw_own_cell` 3686 sets DS:35C4=FF). **confirmed**

| Heading | facing | camera offset across | cells visited (Δx,Δy), in order; * = skipped when DS:18≠0 |
|---|---|---|---|
| 315–44 | +x | y ≥ 0x400 | (2,1)* (1,1) (0,1) (2,0)*ᵃ (1,0) own |
| | | y < 0x400 | (2,−1)*ᵃ (1,−1) (0,−1) (2,0)*ᵃ (1,0) own |
| 45–134 | +y | x ≥ 0x400 | (1,2)* (1,1) (1,0) (0,2)* (0,1) own |
| | | x < 0x400 | (−1,2)* (−1,1) (−1,0) (0,2)* (0,1) own |
| 135–224 | −x | y ≥ 0x400 | (−2,1)* (−1,1) (0,1) (−2,0)* (−1,0) own |
| | | y < 0x400 | (−2,−1)* (−1,−1) (0,−1) (−2,0)* (−1,0) own |
| 225–314 | −y | x ≥ 0x400 | (1,−2)* (1,−1) (1,0) (0,−2)* (0,−1) own |
| | | x < 0x400 | (−1,−2)ᵇ (−1,−1) (−1,0) (0,−2)* (0,−1) own |

ᵃ Placed at x-origin offset **0x0FD0** instead of 0x1000 (36D4, 36F0), i.e. drawn 48 units too close. Only in
this sector; (2,1) at 371B uses 0x1000. Intent unknown. **confirmed** (values)
ᵇ (−1,−2) at 360E is **not** gated by DS:18 (no check at 31D5). **confirmed**

The PC-98 port has the identical walk (PC98 1000:34DD, mirror flag at its DS:1A, the same 0FD0h helpers at
1000:3AC5/3AE1, and the same 0x1400 cull in 1000:4922). This is a cross-check, not a separate design.

- The helpers 35F0…371B load (CX,DX) = world origin offset and (BX,AX) = cell offset, then `jmp draw_cell`. **confirmed**
- So: **≤6 cells per frame (≤4 in the mirror)**. Reach ahead = 2 cells beyond the camera cell (4096–6144 units). The
  opposite side row is never drawn, so it pops in when the camera crosses the cell midline or a 45° sector edge.
- `draw_cell` 3009:31EE: wraps the cell index across big-tile edges (clamped at the map border, which then re-reads
  the same big tile: a quirk). It reads the big-tile pointer `DS:8524[(row·cols+col)·2]` and the 2-byte cell entry
  (type, elevation). Then it draws, in order:
  1. **static list** of the cell type (DS:9D73[type] → {code, dx, dy, dz}…FFFF), in list order, unsorted. Each
     entry calls a hand-written object routine with DS:3220/3222/3224 = position. **confirmed**
  2. `collect_sortables` 463E: the type's **second list** (buildings; skipped when DS:19≠0) goes into DS:35C5
     (8-byte records: code, x, y, z). Then `collect_vehicles` 32F8 (list DS:EF5A[bt]) and `collect_pedestrians` 34D6
     (list DS:EF8C[bt]) append vehicles of this big tile whose cell matches. **confirmed**
  3. `cull_sortables` 4562: depth = Z of the reference point. Statics are kept if −0x400 ≤ Z < 0x1400 and
     |X| < Z+0x600 (sort key Z−0x600). Vehicles continue at 44D0: −0x80 ≤ Z < 0x1400, |X| < Z+0x80 (key
     Z−0x80, or 0x400 if negative). Then `sort_sortables` 4686 (bubble sort, far first). **confirmed**
  4. `draw_sortables` 4603: calls each record's routine; `cs:259E` = its sort key (used for LOD). Resets
     the 16-entry index list DS:3805 from DS:37C5. **confirmed**
- Big multi-cell structures (routines 2AA7, 2C25, 2C33; compound lists DS:A3E7, A053, A275) are listed in every
  cell they cross (e.g. cell types 14h–23h place 2AA7 at x = +8192…−22528). They are drawn **once per frame**,
  guarded by DS:2AC0 (cleared at 02DA/06E0). **confirmed**

## Caps and fixed buffers
| Buffer | Size | Notes |
|---|---|---|
| projected vertices DS:1A86 | 128 × 8 bytes | per model; reused for each object |
| camera-space vertices DS:266E | ≈182 × 6 (to DS:2AB6) | **likely** bound |
| sortable records DS:35C5 | 64 × 8 | per cell |
| sort index DS:3805 / keys DS:340A | **16** reset per cell | **likely** effective cap of 16 sorted objects per cell; the data has ≤8 static sortables per cell type (scan of all used types) |
| span tables DS:002E / DS:01BE | 200 rows | |
| face flags DS:333D | byte count DS:337D | |
No per-frame object, polygon or time budget was found. **likely** (none seen in 30C6/31EE/4603/B5B3)

## Per-object distance rules (sample; all **confirmed** at the addresses)
Object routines compare |camera − object| per axis (`cdq; xor; shr; adc` abs idiom):
- Road/sidewalk models 7270, 72C4, 7318, 736C, 7414, 74BC, 7510, 7564, 75B8, 7610: if not facing along the piece
  (DS:35C3) or |Δacross| ≥ 0x400 → flat quad (26B0). Else |Δalong| < 0x800 → near model, < 0x1000 → mid model,
  otherwise flat quad. 766E/76B2: 0x400/0x800 tiers.
- Bridge/large-structure parts: 2B36/2B59 draw if |Δx| < 0x2000; 2C85, 2CB9, 2D1F, 2D4C, 2D95, 2DB6 if |Δy| < 0x1800
  (plus camera-above/below tests on z); 2DD7 switches model at |Δy| 0xC00.
- Sorted-object LOD by `cs:259E`: 3790/37A2/37B4/37C6 detailed model if key < 0x800, else 3811. Vehicle model choice
  at B9F6 (model table in segment 245A at 6FF8+8·type): near if key < 0x800, else far.

## Detail toggles
| Flag | Set by | Effect |
|---|---|---|
| DS:19 | `B` key (scancode 30h → 0AEB xor) | ≠0: buildings (second list) are not drawn (463E). **confirmed**. Render-only: **likely** |
| DS:2ABE | `W` key (11h → 0AE5); startup copy of `cpu_slow_flag_b` | ≠0: building "windows" detail faces (2E3C) skipped. Matches the manual's "Windows toggle (downtown)". **confirmed** |
| DS:2AC7 | startup copy of `cpu_slow_flag_a`; **F6** toggles it (scancode 40h → 0BA7); the view keys F1/F2/F3 (092B/094C/090A) and the start countdown (066D) clear it | 0: rear-view mirror drawn (0434→0666). Slow CPUs start with the mirror off. **confirmed** |
| DS:18 | 0666 (mirror pass), 0C48 (highway toggle path) | mirror pass: skips the far cells (table *), inverts the 2D winding test. **confirmed** |
| DS:355A | 372A toggle | full-screen viewport 356B vs dash viewport 3579. **confirmed** |
So `cpu_slow_flag_a` = mirror initially off, and `cpu_slow_flag_b` = building windows initially off.

### View keys
The key handler table at image offset 30E52 holds one near pointer per scancode from 3Bh (F1). F3 (camera yaw = heading + 85)
and F4 were checked in the host with vette_world key scripts: **confirmed**. The others follow from the table: **likely**.

| Key | Handler | Effect |
|---|---|---|
| F1 (3Bh) | 092B | look left: view yaw offset DS:2B87 = −85 (in-car, dash viewport 3579) |
| F2 (3Ch) | 094C | look ahead: DS:2B87 = 0 |
| F3 (3Dh) | 090A | look right: DS:2B87 = +85 |
| F4 (3Eh) | 0A49 | **helicopter view**: DS:2C7D (camera distance behind the car) = 100h, DS:2CCF (extra eye height) = 88h, pitch DS:2BEB = −17, DS:2ACF = DS:2ADD = FF, full-screen viewport (3762). The camera ends up 146 units up |
| F7 (41h) / F8 (42h) | 0A1A / 0A20 | extra eye height DS:2CCF ±8 (not below 0) |

F1–F3 return through 0A74, which clears DS:2CCF, DS:2BEB and DS:2C7D (back in the car). 0ABB/0AC1 change DS:2C7D
by ±10h and 0A2D/0A3B the pitch by ±5 (|pitch| ≤ 5Ah); their keys are not identified yet. The digit keys (0962…)
are the manual gear selection (`gear_select` 1F01), not views.

## Rear-view mirror (`draw_mirror_view` 3009:0666)
Yaw +180 (or ±95 in the side views), pitch negated. The matrix column 0 is negated (mirror image). DS:18=FF.
Viewport 35A3/3587/3595. Sky/ground only (`5A2E`, **no horizon panorama**), then `30C6`. It runs **after** the
simulation step, with the camera position from the start of the frame. DS:2C77/2C79 are left modified until the next
frame recomputes them. **confirmed**

## Sky, ground and horizon (`draw_sky_ground_horizon` 3009:5A35)
- `horizon_line` 59B3: projects (0, 25000·sin(pitch) + cam_z, 25000) with 4160:086B. The row is clamped to the viewport,
  and DS:40E0 = rows of the panorama hidden below the viewport. **confirmed**
- `fill_sky_ground` 5A5B: rows top..horizon get sky colour `cs:57DF` = 0Bh (constant); horizon..bottom get the ground colour
  `cs:57E0` = DS:8556[big tile] (6 in highway mode DS:2AD4≠0, reset to 7 by `highway_exit` 7743). **confirmed**
- `blit_horizon` 6734/6773: copies 24 rows × 40 bytes with EGA write mode 1 (latch copy) from A400:
  `DS:3CA7[DS:3C8E[bt]] + heading + row·400`. Each panorama is 400 bytes/row = 360 bytes for 360° plus 40 bytes of
  wrap, i.e. **8 px per degree**, while the 3D projection gives ≈4.5 px/degree at screen centre. The backdrop
  scrolls about 1.8× faster than the geometry. **confirmed**
- HORIZON0/1/2.BIN = 4 planes × 24 rows × 400 bytes (38400 bytes). They are loaded at FE8F–FEC1 and copied plane by plane to A400:0000,
  2580h, 4B00h by `load_horizon_planes` 6793. The map is 5×5 big tiles, and DS:3C8E picks the panorama per big tile. **confirmed**

## Traffic and the render pass's side effects (important for Classic 1:1)
- Vehicle lists per big tile: {w entity ptr, w cell index (x·16+y)}. Entity +0 = draw routine, +2/+4/+6 = x/y/z. **confirmed**
- The 25 per-big-tile pointers share only four list A headers and two list B headers. **confirmed** (data dump)

| List | Contents |
|---|---|
| A DS:F00C | slots 0–3 only: 2D33 (player; struct at 2D35), 2F07 (opponent), 3063 (chase car), E1F0 (patrol car). No traffic |
| A DS:F01E | the same slots 0–3 + **32 traffic entities**, about 2 per (x mod 4, y mod 4) class |
| A DS:F0B0 / F0E6 | slots 0–2, a patrol car (EB28 / ED3A) and 9 traffic entities, all with y≡2 (F0B0) or x≡2 (F0E6) (one road line per 4 cells) |
| B DS:EFC0 / EFBE | 16 pedestrians E8E8…EAC8 (20h apart), one per class / empty |

The traffic is therefore one small set of entities repeated over the city with a 4-cell period. The window (≤3 cells
along an axis) keeps any entity from being drawn twice in one frame.

- List A slots 0–2 (byte offsets 0/4/8: player, opponent, chase car) match exact cells only. Slot 1 is used only when
  DS:842B=0 (opponent not on a highway) and DS:2B70 (its big tile) is this cell's big tile. Slot 2 is used only during a chase
  (DS:F7C2≠0, big tile DS:2B6E), and it then skips slot 3, the patrol car (3322–3357). Notes 04 names the slots.
  All other vehicles match when `(entry_cell & 0x33) == (cell & 0x33)`, i.e. x mod 4
  and y mod 4. Their draw position is (entity x/y & 0x7FF) + the **current cell's** origin, and 3397/3431 **write the
  current cell index into the list entry**. So the traffic pattern repeats every 4 cells and is bound to whichever
  window cell matched. **confirmed** (code). The pedestrian list (B) is never rewritten. The traffic AI (BCFB → D150,
  C0E0, BDDE) then uses the full rewritten cell index, so **traffic turns depend on the previous frame's view and draw
  order** (notes 04 §7).
- In the camera's own cell (DS:35C4=FF), 3483–34C8 tests each vehicle's box against the player's half-extents
  (DS:2B8B/2B8D from DS:2BC9/2BD9 by heading octant) and stores the hit in **DS:2B7A**. 351C–3585 does the same for
  list B → **DS:2B7C** (main view only). 32F8/33C6 set DS:312E=FFFF when slot 1 (the opponent) is drawn; the 1P opponent AI (D8B7) reads it. The simulation reads these
  (161A, 1738, D8B7). **confirmed**
- The mirror pass repeats the own-cell pass (list A collision included) after the simulation step. So mirror on/off
  (and the CPU class default) can change traffic binding and collision timing. **likely**: verify with the oracle.

## Highway mode (DS:2AD4 = FF)
On the named freeways and bridges the city walk is skipped (0342–034E: only drawn again once DS:8411 marks the
end of the road). `highway_frame` 775E, which is simulation and drawing together, builds a ring of **32 road slices** ahead
(7A0E: slice types from the highway's segment list; slices 6–18 take their model from DS:7A72, the others from
DS:7A88, **likely** a detail band). It draws them far to near (77A1 loop, slice 31→0; per-slice data in segment 2243
from offset 0382h). `highway_draw_cars` 7F09 then draws up to 11 highway cars depth-sorted (`sort_sortables` 4686; a
negative key stops the loop) with the segment-245A models (B9D6). Sky/ground/horizon is still `draw_sky_ground_horizon`.
So the highway draw distance is **32 slices** (slice length TBD). **confirmed** (structure); details TBD.
775E also steers the player along the lane (7B92–7BCF writes DS:2D3B), so it cannot be skipped by a native renderer.

## How to lift each limit without changing the simulation
1. Split the original pass into (a) a **visibility pass** that runs the exact original window, matching, cell binding
   and collision tests with the original camera, mirror flag and order, writing DS:2B7A/2B7C/312E and the list cells. Run it
   always, even when nothing is shown. And (b) a pure **draw pass** that reads state only.
2. Draw pass: walk all cells within radius R (up to the whole 80×80-cell map), back to front by cell distance, or use a
   depth buffer. Keep each cell's static list order (roads, kerbs and markings are coplanar and rely on it), e.g. with a per-
   list-index depth bias.
3. Do the coordinate maths in 32-bit/float relative to the camera, adding big-tile offsets ×0x8000, because the
   original 16-bit deltas wrap beyond ±32767 (16 cells).
4. Drop the Z<0x1400 sortable cull and the |X| margins. Force the near LOD (or scale the thresholds with resolution).
   Keep the DS:2AC0 once-per-frame rule for compound structures (draw each once, at its true position).
5. Traffic: the original world model *is* periodic. Each traffic entity exists in every cell of its 4×4 class, and
   the small window only hides the repeats. A pure draw pass can therefore show each entity in every matching cell
   within the enhanced radius, at (entity x/y & 0x7FF) + cell origin. That is exactly what a larger original window would
   show. The repetition (same car every 8192 units) becomes visible at long range, so offer it alongside a "bound
   cells only" mode that draws each entity once at the cell the visibility pass bound it to. Either way the draw pass
   must not write the list cells.
6. Horizon: draw it first as now. With far geometry the painted skyline (bridges, downtown) will double up. Options:
   re-project the panorama at the true angular scale, or treat it as a sky/hill backdrop behind a draw distance of ~25000.
7. Mirror in Enhanced: draw it with the full window. The side effects still come from the original reduced
   (DS:18) pass in step 1.

## Open questions / next
- Highway mode renderer details: slice length and geometry (7AE3/7CAD/7D6A), the mirror's highway path 78E2, and the
  3009:0C2D toggle (it flips DS:2AD4, enters highway mode, and sets DS:18=FF; no key table entry found, so it may be a
  debug path). Segment-245A model format (see notes 05).
- Catalogue every object routine (code pointers from DS:9D73 lists; see notes 05) with its distance rules.
- Verify in the host oracle that toggling the mirror changes DS:2B7A timing.
