# 04 — Simulation state (DOS 1.1) — first pass

Addresses are image-relative `SEG:OFF`; data is `DS=124A` unless written `cs:`. Names are in `re/symbols.csv`.
Every claim is tagged **confirmed** (with evidence address) or **likely**/**TBD**. Units: world unit = 1 (a cell is
0x800, a big tile 0x8000; ≈ 3–4 in per unit, **likely**, see notes 03); speed = world units per second; angles =
integer degrees 0..359 (0 = +x = north, 90 = +y = east, notes 05).

The render pass also writes simulation state (collision candidates, traffic cell binding). See notes 03,
"Traffic and the render pass's side effects", and §7 below.

## 1. Frame rate usage — summary

`frame_rate` (`fr`, DS:2CD3) is an integer ≥4; it is set to 10 before the first frame (**confirmed** 3009:0122).
The code uses it in six patterns (A–F). **There is no fractional position accumulator anywhere.** Position
deltas are whole world units per frame, truncated.

| Pattern | Formula | Where (all **confirmed** at the address) |
|---|---|---|
| A. rate/s → per-frame step with remainder carry | `t = rate + rem; step = t / fr; rem = t % fr` (signed `idiv`) | 4160:00BA (RPM accumulator), 4160:01A7/01DB (gravity on slopes), 4160:04BB/04E4 (AI speed) |
| B. speed → per-frame distance, truncated, no carry | `d = speed / fr` | 4160:0369 (player), 4160:050B (AI/police), BDD5 (traffic), BC19 (pedestrians), 1389 (police spawn), 4021:12BD (highway traffic), 4021:0508/094A (highway opponent), 4021:0E02 (2P remote on highway), 0FFA (2P remote dead-reckoning, `max(1, ...)`) |
| C. angle delta / fr | `heading += wrap180(h_last − h_prev) / fr` | 0FC0–0FF7 (2P remote heading extrapolation) |
| D. seconds → frames | `timer = fr*2`; `delay = max(fr−3, 1)` | 1B9D 21FC 2261 2286 22E3 2347 23A4 2404 (HUD message timer DS:3DEE), 63F6 (gear display timer DS:3A9D), 4021:0F63 (highway exit delay DS:8412) |
| E. frames → PIT ticks | `ticks = (dist / per_frame_dist) * 291 / fr` | 4160:0C3C (opponent finish-time estimate) |
| F. closing speed per frame | `(v_player − v_car) / fr` vs gap | 4021:0094 (highway collision prediction) |

The full list of 2CD3 readers (`rd.py x 2cd3`) is covered by the table above; the writers are 0122 (=10) and
053D (per-frame measure).

**Not scaled by frame rate (per-frame constants)**. These matter for 1:1 and for an Enhanced high-fps mode:

| Quantity | Per-frame change | Evidence |
|---|---|---|
| Car heading | `heading += steer` (steer DS:2B84, clamped to [2AEB]..[2AEA] = −8..+8 deg at race start) when speed ≠ 0 | 01A5–01C3, init 2454/2459 — **confirmed** |
| Steering wheel | keyboard ±2/frame (0854/08AF); joystick sets it directly, clamped and negated in reverse (079D/07C5); drifts 1/frame toward DS:2ACD unless cs:F4D6 or cs:9271 is set (017A–01A0) | **confirmed** |
| Speed ↔ engine coupling | `speed += (target−speed)/4` (snap if gap < 20 up / < 10 down) | 4160:02B0–0319 — **confirmed** |
| Brake | speed −28/frame, RPM-acc −4/frame; ≤14 → 0 | 4160:0214–0243 — **confirmed** |
| Rolling resistance | speed −1/frame (no throttle, no brake, not in coast mode) | 4160:0156 — **confirmed** |
| Engine-revs decay | RPM-acc −1/frame in gear, −10/frame in neutral | 4160:0112–0131 — **confirmed** |
| Skid (oversteer) | speed −5/frame (−10 on rough surface), slip +20/frame | 4160:03F2–040A — **confirmed** |
| Wheel slip | +10/frame when gap ≥ 80, else min(slip,100) −15 | 4160:02BE–02E8 — **confirmed** |
| Traffic acceleration | speed += 4/3/2/1 per frame (by stage) | BDAC — **confirmed** |
| Pedestrian acceleration | speed += 1/frame | BC08 — **confirmed** |
| Highway opponent | +20/frame accel, −28/frame decel, lane change 16 units/frame × 3 frames | 4021:07FF, 0812, 0853/08A6 — **confirmed** |

## 2. Player car struct (DS:2D35, size 0x32 = [2D67])

The same layout is used for the opponent/remote car (DS:2F09) and the police chase car (DS:3065). It is
also used for the car templates (player models DS:2EBD/2DCB/2E1D/2E6F via the pointer table DS:2D19[model],
opponents DS:2F3B/2FCF/2F85/3019 via DS:2D21[opp]). Each template is followed by its accel and gear tables
(**confirmed** 1F89–1FB5 copy `[2D67]` bytes; table pointers +15/+17 point just after the struct).

The word **before** each struct is the entity header (draw-routine pointer), so the entity pointer stored
in the vehicle lists is `struct−2` (2D33=0x28D3, 2F07=0x27D3, 3063=0xC187): **confirmed** by data and the
list contents at DS:F00C.

| Off | Abs (player) | Size | Name | Meaning / format | Evidence |
|---|---|---|---|---|---|
| +00 | 2D35 | w | x | big-tile-local x, 0..0x7FFF (integer units) | 1738 carry logic — **confirmed** |
| +02 | 2D37 | w | y | big-tile-local y, 0..0x7FFF | 17D8 — **confirmed** |
| +04 | 2D39 | w | z | ground height = ramp height + elevation×224 (city); 0 or 7 on highway | 184B–1852, 4160:0794, 4021:0EF8 — **confirmed** |
| +06 | 2D3B | w | heading | facing, degrees 0..359; 0 = +x, 90 = +y | 3CE7, 53B1 — **confirmed** |
| +08 | 2D3D | w | pitch | hill slope, degrees (±7 from DS:2B40 table by heading quadrant) | 4160:0004 — **confirmed** |
| +0A | 2D3F | w | roll | always 0 for cars (only zeroed, 4021:0F1D) | **likely** |
| +0C | 2D41 | w | ? | no reader found | **TBD** |
| +0E | 2D43 | w | speed | units/s, ≥ 0 (reverse is a flag, not a sign). Display mph = speed×3/16 (truncated) | 5E4C — **confirmed**; also at runtime: speed 153 shows "28 MPH" (host emulator, course 1) |
| +10 | 2D45 | w | gear | 0 = neutral, 1..max, max+1 = reverse | 098C, 1F01 — **confirmed** |
| +12 | 2D47 | b | max_gear | 4/6/6/5 by player model; forced to 2 in "stolen" mode (010C) | templates — **confirmed** |
| +13 | 2D48 | b | throttle | 1 while accelerate input held, cleared each frame (0293) | 080E — **confirmed** |
| +14 | 2D49 | b | brake | 1 while brake input held, cleared each frame (028E) | 0842 — **confirmed** |
| +15 | 2D4A | w | accel_tbl | → per-gear accel (units/s²) used by the AI speed model | 4160:04DA — **confirmed** |
| +17 | 2D4C | w | gear_spd_tbl | → per-gear speed limit / shift points | 4160:04F3, 0959, 1F3C — **confirmed** |
| +19 | 2D4E | w | cell_ptr | → cell word in the big-tile map (type, elevation) | 3589 — **confirmed** |
| +1B | 2D50 | b | automatic | 1 = automatic gearbox (toggled by key, 1F5C) | 4160:097E — **confirmed** |
| +1C | 2D51 | b | shift_flag | set when an upshift is due (0939) or after a missed shift (1F56) | **likely** |
| +1D | 2D52 | b | ? | | **TBD** |
| +1E | 2D53 | w | target_ptr | AI only: → current target point (opponent DS:FA2C, police DS:2B91) | 2076, 1409 — **confirmed** |
| +20 | 2D55 | w | step | distance moved this frame (= speed/fr, signed for player reverse) | 4160:036F/0511 — **confirmed** |
| +22 | 2D57 | w | bt_x | big-tile index along x (row) | 17BF–17CC — **confirmed** |
| +24 | 2D59 | w | bt_y | big-tile index along y (col) | 17EB–17F8 — **confirmed** |
| +26 | 2D5B | w | cell_x | x >> 11 (0..15) | 35BD — **confirmed** |
| +28 | 2D5D | w | cell_y | y >> 11 (0..15) | 35E5 — **confirmed** |
| +2A | 2D5F | w | rev_acc | engine accumulator (rises by rev rate/s while throttle held) | 4160:00C3, 60F1 — **confirmed** |
| +2C | 2D61 | w | rpm | rev_acc / gear_divisor, clamped to redline; units of 100 rpm | 60D7 — **confirmed**; ×100 **likely** |
| +2E | 2D63 | w | rem | remainder for pattern-A divisions | 4160:00C0, 04C1 — **confirmed** |
| +30 | 2D65 | w | ? | | **TBD** |

Other player state: travel heading DS:2C45, travel pitch 2C47, roll 2C49 (input to 3CE7, = facing unless
skidding, 4160:037F); skid flag 2C4B; slip 2C4D; slope accel 2C61 (±20 or 0) with remainder 2C63; reverse flag
2AD5 (+ 2AD6 "reverse selected"); coast flag 2AD7; rough-surface flag 2C6D; collision-box side 2B20; heading octant
2B89 and car half-extents 2B8B/2B8D (from DS:2BC9/2BD9 by octant, 01E3–0207) — all **confirmed** by the cited code.

### Player model data (by model = cs:8DAE, copied to DS:2C67)

| Table | Model 0 | 1 | 2 | 3 | Evidence |
|---|---|---|---|---|---|
| gears / auto (template +12/+1B) | 4 / auto | 6 / manual | 6 / manual | 5 / manual | templates — **confirmed** |
| idle rpm cs:5887 | 11 | 9 | 8 | 7 | 60DE — **confirmed** |
| redline cs:586F | 55 | 72 | 55 | 65 | 6114 — **confirmed** |
| rev rate DS:2C39 (/s) | 8 | 16 | 32 | 24 | 4160:009F — **confirmed** |
| speed ratio DS:2CD9[model*8+gear] | 0,9,17,26,30,6r | 0,8,13,17,24,28,24,6r | 0,9,13,17,22,30,38,7r | 0,8,13,17,29,42,6r | 4160:0279 — **confirmed** |
| gear divisor cs:5877[gear] (all models) | 0,1,2,3,4,6,6,6 | | | | 60FE — **confirmed** |
| cornering grip DS:2BF1[model*9+|steer|] | 9 words per model | | | | 4160:03C1 — **confirmed** |
| grip bonus 4160:0373[model+4*level] | level 0: +255, 1: +100, 2: +0 | | | | 4160:03D9 — **confirmed** |

("r" = the reverse gear entry, index max_gear+1.)

## 3. Player physics step (per frame)

Order inside the frame (see notes 02): heading update in the loop (01A5) → `0EAA` player step → … →
traffic/police. **Confirmed** flow of `0EAA` (1-player and 2-player, when DS:17 = 0):

1. `[2C51]=0` (= "current vehicle is the player"), `[2B7E]=0` (vehicle slot 0).
2. `1EC8` → `60D7` engine: `if rev_acc < idle: rev_acc = rpm = idle; else rpm = rev_acc / gdiv[gear]`
   (no divide in neutral); `if rpm ≥ redline: rpm = redline, rev_acc −= 4`; `engine_speed (cs:5891) = rpm`.
3. `1EDB` → `4160:0939` automatic shifting (only if +1B): down when `speed < spd_tbl[gear−1]` and no throttle,
   up when `speed > spd_tbl[gear]` with throttle (auto upshift capped at 5th, 6th for model 2).
4. `4160:0045` drivetrain:
   - throttle: `rev_acc += pattern-A(rate)` with `rate = (gear ? revrate[model] − 2*automatic : 30) − dmg[2C5B]` (≥ 0).
   - slope: `[2C61] = −20*sign(pitch)` (sign flipped in reverse); applied with pattern A to `rev_acc` (in gear, rate ±4)
     or to speed (neutral).
   - brake / rolling / coast-mode per-frame terms (table above).
   - engine coupling (not braking, in gear): `r = ratio[model][gear] − [2C5D]/2`, and if `r > 0` then
     `r = max(0, r − [2C4F]/2)` (damage); `target = r * floor(rpm/2)` (unsigned); `target < 10 → speed = 0`;
     otherwise `speed` moves 1/4 of the gap per frame. The −1/frame rolling term above runs first, so it only
     matters in neutral (**likely**).
   - `step = (speed − 2*slip) / fr` (forward, ≥ 0) or `(−speed + 2*slip) / fr` (reverse, ≤ 0) → +20.
5. `1738` move: steering/skid `4160:037F` (sets travel heading 2C45: if `|steer| ≥ 2` and
   `speed ≥ grip[model][|steer|] + bonus`, the car skids: travel heading lags the facing, speed −5/frame);
   then `3CE7(heading=2C45, pitch=2C47, d=step)`:
   `h = d*cos(pitch)`, `dz = d*sin(pitch)`, `dx = floor(h*cos(heading)/32768)`, `dy = floor(h*sin(heading)/32768)`
   (Q15 sincos, `imul` + `shl/rcl` → floor). `x += dx`, `y += dy`, wrapping 0..0x7FFF into the big-tile
   index (+22/+24) and setting DS:2B6A/2B6C = −1 on a crossing (17AD–1802).
6. `3589` cell update, `1888` write the vehicle's cell index into its big-tile list slot, `4160:0794` cell type
   → DS:2B14 (height code) and 2B16 (collision-box set), `18E0` collision boxes → `1976` event dispatcher
   (special boxes: highway on-ramps set DS:2AD4/8156, rough surface 2C6D, …; default = wall crash 1BAB),
   `1D3B` car–car contact with the opponent, `z = 4160:0515 + [2CBB]`, `4160:0004` pitch.
7. `21A2` speeding check: `speed − [2C4F] ≥ cs:219A[[3B61]]` (468/624/680/736) → police alert (DS:2C57=1, reason bit 0).

**Quirk (likely, needs runtime check)**: 3CE7 computes `h` with an *unsigned* `mul` (3D09). A negative
step (player reversing, step ≤ 0) gives `h ≈ d + 2*cos(pitch) − 65536`, i.e. about `d − 2` on flat ground but
about `d − 490` on a ±7° slope.

**Quirk (confirmed)**: in the wall-crash path the y tile-carry test uses DX instead of AX (1BE7 `cmp dx,0x4000`,
the x path at 1BC9 uses AX).

## 4. Frame-loop simulation functions

| Function | Called | What it does | Frame-rate use |
|---|---|---|---|
| **3009:0EAA** | 020A always | player step (section 3); if DS:17≠0 copies 2F09→2D35 instead (DS:17 is never set, notes 12) | via 4160 (A, B) |
| **3009:12D2** | 0248 | police chase (section 6) | 1389: `police.step = entity.speed / fr` (B, once at spawn) |
| **3009:0EF5** | 0267 | opponent car: 1P computer driver or 2P remote car (section 5) | 0FC0–1009 (B, C) |
| **4021:0ED1** | 03AF when DS:2AD4 ∉ {0,FF} | enter highway mode: player at (0x40A0,0x4000,z 7) heading 90, highway id DS:8156, road state reset, DS:2AD4=FF, exit delay `[8412]=max(fr−3,1)` | 0F63 (D) |
| **3009:775E** | 03B4 when DS:2AD4≠0 | highway mode per frame: build 32 road slices from segment list DS:8154, place highway cars (4021:1190, which advances each car with `step = speed/fr`, 12BD), spawn new cars (4021:1007), exit when DS:8411 set and `[8412]` counts down to 0 (7743 → DS:2AD4=0) | via 4021:12BD (B) |
| **4021:13DF** | 03C9 (2P, remote on highway) | copy remote highway position (DS:7406/7408/740A) into the highway-opponent record (81EC/81CE/81D0) | — |
| **4021:0AF5** | 03D4 (2P, remote on highway, packet this frame) | place remote car in highway slot 10 of DS:82F4 if it is on the same highway near the player; DS:842A = visible | — |
| **4021:0C49** | 03DC (2P, remote on highway, no packet) | dead-reckon remote along road (0C69): `d = [7414]/fr` | 4021:0E02 (B) |
| **4021:01B7** | 03F1 (1P, DS:842B∉{0,FF}); 10CC/115C | computer opponent enters highway DS:81EC: init record DS:81CE.., speed from 2F17, DS:842B=FF | — |
| **4021:024C** | 03F6 (1P, opponent on highway) | highway opponent step: off-screen 4021:0314 or on player's highway 4021:0664 (lane AI) + 08D6 | 0508, 094A: `d = [81DC]/fr` (B) |
| **3009:7F09** | 0402 when DS:2AD4≠0 | draw the 11 highway cars (render only) | — |
| **4021:0143** | 0405 when DS:2AD4≠0 | player vs highway-car collision: for each active car (DS:8420[i]) within half-size+25, predict contact (4021:002D: `(v_p − v_car)/fr ≥ gap`); on hit cap speed to lane speed DS:83E6[[814E]−1], halve rev_acc, crash damage (21C8) | 4021:0094 (F) |

## 5. Opponent car (DS:2F09) — 3009:0EF5

Skipped if the opponent finished (DS:FA45=FF) or the start countdown is running (DS:2AD8<5). Always sets
gear ≥1, brake 0, throttle 1 (0F1D–0F2C), DS:2B70 = `bt_x*5 + bt_y` (hard-coded 5 columns, **confirmed** 0F0C).

**2-player (cs:2≠0)** — `422F:0176` returns the received packet (checksummed; DS=2F98): the first one completed
since the last call, later ones being dropped meanwhile (notes 12 §4).
Packet = `{w len, w status (lo = sender's DS:2, hi = 0 city / FF highway), payload}`.
- City payload (len 0x32): the first 0x2E bytes of the sender's 2D35 struct → copied over 2F09; DS:842B=0,
  DS:2B01=0 (frames since packet); DS:2B62 = previous, DS:2B60 = new remote heading. Sent each frame by 022D.
- Highway payload (len 0x16): `{8156 id, 8232 seg, 8230 pos, 8, x−slice.x, y−slice.y, heading, speed, [840F]}`
  (4021:0B7D) → DS:7406.. (4021:0C43), DS:842B=FF.
- No packet: `DS:2B01 = min(+1, 0x7F)`, DS:2B00=FF and dead reckoning (**confirmed** 0FC0–1051):
  `heading += (wrap180(h_last − h_prev) / fr)` (wrap 0..359), `d = speed / fr` (unsigned), `if d==0: d=1`,
  then 3CE7 with heading 2F0F / pitch 2F11 and the same tile-carry as the player.
  Quirk: a stopped remote car creeps 1 unit/frame.
- Finally the remote's cell index is written into slot 1 of its big-tile list (1054–1070).

**1-player (computer opponent)** — **confirmed** 1073–11FE:
- Route state: `DS:F7C0` → route list, `DS:FA22` → waypoint list, `DS:FA24/FA26` = next raw waypoint,
  `DS:FA2C` (8 B) = current target point, `DS:FA34` (8 B) = next target point (both `{x, y, heading, type}`),
  `DS:2B64` = distance to the current target. The opponent's +1E points at FA2C.
- If `dist(car, FA24) < 0x200`, or the current target is reached (`dist(car, FA2C) < step`, then FA2C←FA34) and
  `dist(FA24, FA2C) < 0x100`, the raw waypoint advances (`FA22 += 4`). At an FFFF terminator the route list
  advances (see below). `D150(si=FA2C, di=FA24, bx=FA34)` builds the next lane-following target.
- `D8B7` throttle/brake: when the next segment's heading differs (`[FA30] ≠ [FA38]`) and `speed > 140`, it brakes
  if `dist < (speed−140)*2.5` (DS:2B72), else full throttle. If DS:312E≠0 (set once the opponent has been in
  a drawn cell) it first runs the traffic-avoidance logic D8FD (**TBD**).
- `1EBF` → 4160:0939 (autoshift) + 4160:04A7 (AI speed: pattern A on `accel_tbl[gear]`, coast −160/s,
  clamp to `spd_tbl[gear]` in top gear; `step = speed/fr`).
- `1EE9`: heading snaps each frame to `atan2(target − pos)` (53B1).
- `1738` move with slot 1; on tile crossing FA2C/FA34/FA24 x or y += 0x8000.

Route data (**confirmed** 1FB7–1FD8, 10B3–10F5, 4021:0AB4):
- `DS:F7B8[course−1]` → 3 words (by level DS:FC4F 0..2) → route list.
- Route list = pairs `{w waypoint_list_ptr, w highway_id | FFFF}`. After a waypoint list ends: if the highway id
  ≠ FFFF the opponent drives that highway (4021:01B7) and re-enters the city at DS:7572[id] = `{x, y, heading,
  bt_x, bt_y, z}` (4021:0AB4); then the next list starts. FFFF ends the race unless DS:9 (set when course 4 is
  chosen, 009C; 1FFA resets course to 1 and counts legs in DS:2D31) chains the next list.
- Waypoint list = 8-byte initial target `{x, y, heading, type}` then 4-byte `{x, y}` points, FFFF-terminated.
  Coordinates are 16-bit, relative to the opponent's current big tile modulo 0x10000.
- Start positions: DS:2D69[course−1] → `{player x,y,z, opponent x,y,z, heading, bt_x, bt_y}` (1FE3–202C; e.g. DS:2D71 = (FE0,11C8,C), (FA0,11C8,C), 270°, bt (4,0)).
- Finish: 11FF sets DS:FA45=FF and stores the time (703F) in cs:C9D4..C9D7.

## 6. Police

| Item | Meaning | Evidence |
|---|---|---|
| patrol car | entity in **slot 3** of every big-tile list (E1F0 in F00C/F01E, EB28 in F0B0, ED3A in F0E6), profile DS:E7AE | data, BD2A — **confirmed** |
| DS:EF4C | patrol car of the camera tile is active (its +1C) | BD98–BDA2 — **confirmed** |
| DS:2C57 | offence detected (police alerted) | 21B8, 166A, 16C9, 1BB8 — **confirmed** |
| cs:DADD | offence bits: 0 speeding (21A2), 1 hit a car at ≥140 (165C), 2 hit an object/wall (1BB2), 3 hit a pedestrian (16C3), 4 still moving 10 s after the siren started (143D) | **confirmed** (meanings **likely**) |
| DS:F7C2 | chase active | 1308, 1601 — **confirmed** |
| DS:3065 | chase car (0x32 struct), slot 2 (entity 3063) | 132C–140C — **confirmed** |
| DS:2BA1 | chase target (= 2D35) | 1363 — **confirmed** |
| DS:2B91/2B99 | police current/next target points | 13D2–13F8 — **confirmed** |
| DS:2B66 / 2B64 | distance police→player / police→target | 144E, 1459 — **confirmed** |
| DS:2C59 / 2C5F | player pulled over / ticket screen active | 1562, 14F7 — **confirmed** |
| DS:FC4F | level 0..2 (menu DF98); 0 = no police and +255 grip bonus | 12E1, 21C8, 4160:03C5 — **confirmed** (name **likely** difficulty) |

Chase (`12D2`, **confirmed**): not before the start countdown ends. Needs DS:FC4F≠0 and an active patrol car. On
an offence, `124E` starts the chase (DS:F7C2=1) at once in F00C/F01E tiles (12CE `clc`). In F0B0/F0E6 tiles it
starts only if the patrol car's x (F0B0) or offset (F0E6) is on the correct side of the player for the player's
heading octant DS:2B89 (exact geometry **TBD**). First chase frame: the chase car takes the patrol car's
position (cell base + entity x/y), z=9, gear 1, throttle on, `step = patrol.speed / fr`; the patrol entity is hidden
while the chase runs (BD31). Each frame: distance ≥ 0xC00 → give up (15D6); ≤ 0x800 → siren
(cs:926C bit 2); if the waypoint is farther than the player, aim at a point 100 units ahead of the player
(3CE7 with the player heading); speed via 4160:0939/04A7 (scaled), heading snaps to the target, move with slot 2.
Within 100 units: player speed=0, gear=0, police speed=0, pulled over (DS:2C59=1); in "stolen" mode
(DS:2AEC≠1) the game ends (4160:0BAF, cs:4=FF). Driving 100+ units away after the ticket screen counts the offences
(cs:C9D8+2*bit), resets the chase and returns the patrol car (121C). On the highway the chase is cancelled.

## 7. Traffic, pedestrians, big-tile lists

`DS:EF5A[bt]` (25 big tiles) → one of four shared lists: F00C (15 tiles: slots 0–3 only), F01E (6 tiles: patrol
+ 32 civilian cars), F0B0 (3 tiles: patrol + 9) and F0E6 (2 tiles: patrol + 9, all heading 90°, or 270° on course 3,
BD69). Entries
are `{w entity_ptr, w cell}` with `cell = cx*16 + cy`. Slots 0/1/2/3 = player / opponent / chase car / patrol car.
`DS:EF8C[bt]` → EFC0 (16 pedestrians, the same 6 tiles as F01E) or EFBE (empty). Pedestrian cells use only
`cell & 0x33` (a 4×4-cell pattern). Lists and entity records are shared between tiles, and only the camera
tile ([843A]) is simulated each frame. **Confirmed** by the data and BCFB/BB72.

**Traffic entity (0x2E bytes)**, updated by `BCFB` (from slot 3):

| Off | Meaning | Evidence |
|---|---|---|
| +00 | draw stub (3009:Cxxx: `mov si,&ent+8; mov ax,model; jmp 283A`) | C0F7 — **confirmed** |
| +02/+04 | x, y within the cell (0..0x7FF) | BE2A/BE61 — **confirmed** |
| +06 | z | data — **likely** |
| +08/+0A | heading, pitch (input to 3CE7) | BDE7 — **confirmed** |
| +0E | profile ptr (E798, E7C4, E7DA = civilian types, E7AE = police) | BDB2 — **confirmed** |
| +10/+12 | set to 1/0 when stopped | BD54 — **TBD** |
| +14 | speed (units/s) | BDBA — **confirmed** |
| +16 | accel stage 0..3 | BDCA — **confirmed** |
| +18 | → route (list of `{dx, dy}` cell moves, FFFF then the loop pointer) | BEA6, data E7F0 — **confirmed** |
| +1A | → path sub-record at +1E: `{w tx, w ty, w heading, w bt, w nx, w ny, w nheading, w ?}` (target, next target) | BE97–BEF6 — **confirmed** |
| +1C | moving flag (FFFF/0) | BF10 — **confirmed** |

Profile (E798 / E7AE / E7C4 / E7DA): `+0` half-length 80/28/28/100, `+2` half-width 18/12/12/22 (swapped for
N/S headings, 346A), `+4` = 0 = speed-loss shift on contact (170A: `speed −= speed >> p[4]`, so hitting any
vehicle stops the player), `+6..+C` accel per stage (4,3,2,1 per frame), `+E..+14` speed cap per stage
(50,100,150,224). All **confirmed** from data and the cited code. Per frame: accelerate (BDAC), `d = speed/fr`,
follow targets (BE86: reach → shift next→current, new next from route via D150), heading = atan2 to target,
turn decision per intersection (BF10 → C0E0 reads a byte map DS:D2AE[bt] + cell, dispatched by heading quadrant
via DS:EF52 → BF61/BFB5/C00C/C05E; **likely** intersection rules), move with cell carry (BDDE: cell nibbles ±1 mod
16, target points ∓0x800).

**Pedestrian entity (0x20 bytes, `BB72`)**: +02/+04 x,y in cell, +08 heading, +0A pitch (90 = knocked over),
+0E state profile (EAE8 walking, EAEA hit), +10 reset profile, +12 → route (square of 0x11A sides, DS:EB00),
+14 = 1, +16 speed (+1/frame up to 28), +18/+1A distance accumulators. When hit (16D9), the profile becomes EAEA,
pitch 90 and DS:2AD9 += 100. It is restored when the player is ≥2 cells away (BBA7). **Confirmed** for the code
paths; "pedestrian" is **likely**.

### Traffic is a 4×4-cell repeating pattern bound to the view (**confirmed**)

- The renderer's per-cell gather (33C6 / 3302 for the EF5A list, 34D6 / 351C for EF8C) draws every slot-≥3 entity
  whose `entry.cell & 0x33` equals `drawn_cell & 0x33`, at world position `(entity.x & 0x7FF) + [2CC7]`,
  `(entity.y & 0x7FF) + [2CC5]` (base of the drawn cell). So one entity appears in every visible cell congruent
  mod 4.
- For EF5A lists it also **rewrites the entry's cell word to the drawn cell** (339F, 3435). The last cell drawn wins.
  Pedestrian (EF8C) cell words are never rewritten; they only change mod 4 (BC3E–BCA2).
- BCFB then uses that full cell index: `[F004] = entry.cell` (BD14) drives D150's big-tile-edge handling
  (`cell & 0xF0 == 0xF0 / 0xE0`, D2AE–D30E). C0E0 reads the intersection byte `[[D2AE + bt*2] + entry.cell]`, and
  BDDE moves the full x/y nibbles ±1 mod 16. The police spawn (132C) and the patrol return (121C) use the slot-3
  cell word.
- Consequence for 1:1: **the traffic AI's turns depend on which cell the renderer drew last**, i.e. on the view and
  draw order of the previous frame. The port must reproduce the gather order or this side effect.

Collisions are detected in that same render pass. The vehicle AABB is profile extents + player half-extents
[2B8B]/[2B8D] (3489–34C8 → DS:2B7A = list byte offset, slots ≥ 3 only, police skipped during a chase). The
pedestrian AABB is 12 + [2B8B]/[2B8D] (3548–357D → DS:2B7C). They are handled in the next player step
(161A, from 1738): vehicle hit → speed loss as above, crash sound, `[2B76]=8` (8-frame counter, decremented in
the loop at 016F), offence bit 1 if `speed ≥ 140`. Pedestrian hit → player heading ±15°, offence bit 3,
pedestrian knocked over. All **confirmed**.

## 8. Highway mode (DS:2AD4 ≠ 0)

DS:2AD4: 0 city; 1/2/3 = just touched an on-ramp box (1A18–1ACD, sets highway id DS:8156 0..8); FF = in highway
mode (4021:0ED1). Strings near DS:3BA7 name the 9 roads (doyle dr., 480, 280, presidio, central skyway, embarcadero
fwy, hwy 1, 80/bay bridge, golden gate; order **TBD**). Road = segment list DS:7560[id]+2 (`{b type, b length}`, count
DS:754E[id]) with a 32-slice ring at DS:8234. Highway cars: DS:82F4, 11 × 0x16 bytes (slot 10 = opponent), active
flags DS:8420[11]:

| Off | Meaning |
|---|---|
| +00/+02 | segment index, position in segment |
| +04/+06 | x, y |
| +08/+0A | lateral offset from the slice centre |
| +0C | heading |
| +0E | speed (lane minimum DS:83E6[lane] = 252/308/364/420) |
| +10 | slice index |
| +12/+13 | lane 0..3 (lateral offset DS:83EE = 32/96/160/224), type |
| +14/+15 | half-width, half-length (collision) |

The opponent's highway record is DS:81CE (same layout plus 81E4 next heading, 81E6 max speed = DS:2B18[opp]
672/812/896/1120, 81E8 segment count, 81EA segment list, 81EC highway id). **Confirmed** by 4021:01B7, 1007, 11C4.

## 9. engine_speed (cs:5891)

**It is the engine RPM in units of 100 rpm** (**confirmed** writer 3009:6131 = player +2C `rpm`; the dashboard tach is
labelled "RPM/100" and showed 53–55 at full revs in neutral in a host-emulator run; idle 7–11, redline 55–72). Other writers: 8BD2 (=60) and 8C13 (=0), in the garage engine-rev
sequence. Readers are the PC-speaker engine note (93EA, 940E, 944E, 9464, 9588). The dashboard tach uses the same
value (cs:588F, 6148).

## Open questions
- Struct +0C, +1D, +30; traffic +10/+12; pedestrian half-extents.
- D150 path planner and D8FD opponent traffic avoidance: details.
- Speed unit → real mph: the HUD shows speed×3/16 (runtime-checked). If that is true mph, 1 unit ≈ 3.2 in.
- DS:2AEC "stolen Vette" mode looks like copy protection (4160:0A40 asks for a word from a table): to confirm.
- DS:2AD7 "coast mode" (shown on the dashboard, 61C3; DS:2BEF = 0) — meaning.
- DS:2AD9 (5-digit HUD number, +100 per pedestrian) — score?
- Highway id ↔ road-name order; the 124E patrol-position test in city tiles.
- Runtime check of the reverse-on-slope `mul` quirk (3D09).
