# 11 — Driving: the player's car, and the Improved Driving and Lane Centering layers

Two Enhanced options change how the player's car drives, as native code layered on the original's
physics in the hosted game (`src/game/driving.h`, `src/enhanced/lanes.h`):

- **Improved Driving** (`Settings::improved_driving`, `--driving improved`): the car drifts a little
  through fast corners instead of the original's on/off skid, and leaves the ground over crests at
  speed.
- **Lane Centering** (`Settings::lane_centering`, `--lane-centering on`): while the wheel is centred, a
  slight heading correction toward the direction and centre of the lane the car is in.

Only the player's car changes: the opponent, the police and the traffic run the original's code. With
both options off nothing is installed: the run is bit-identical to the original's (section 6). Both are
off in the Classic preset.

Addresses are image-relative `SEG:OFF` as in `re/symbols.csv` (the emulator adds 1000h to the segment).

## 1. The original's player driving, per frame (confirmed)

In the frame loop (`3009:0025`, notes 02), in this order:

| Where | What |
|---|---|
| `0165` | `snd_request_dispatch` (94FC): the sound requests; `SoundEvents` samples the car here (notes 07) |
| `017A`–`01A0` | the wheel `DS:2B84` drifts 1 a frame toward its rest `DS:2ACD` (steering damage) |
| `01A5`–`01C3` | `heading 2D3B += wheel` if speed `2D43` ≠ 0, wrapped to 0..359 |
| `01C6`–`01E0` | camera yaw `2C77` = heading + view offset `2B87`; camera pitch `2C79` = `DS:2BEB` |
| `020A` | `player_step` 0EAA |
| `0245` | `camera_from_car` 1D89: camera position = car + eye height 10 (+ `2CCF`), chase offset `2C7D` |

`player_step` (0EAA): `[2C51]=0` (the player is the current vehicle), `call 1EC8` at **0ED9** (revs
60D7, automatic gearbox 4160:0939, `player_drivetrain_step` 4160:0045 → speed `2D43` and the frame's
step `2D55 = (speed − 2·wheel_slip 2C4D) / frame_rate`), then `call 1738` at **0EDC**
(`vehicle_move`).

`vehicle_move` (1738), player path:

1. `176A` `call_player_steer_skid` 1E4C → `player_steer_skid` (4160:037F): if `|wheel| ≥ 2` and
   `speed ≥ skid_grip[model][|wheel|] + skid_grip_bonus[model + 4·level]`, the car skids: skid flag
   `2C4B` = 1, speed −5 (−10 on rough ground `2C6D`), `rev_acc` −2 (twice on rough), `wheel_slip 2C4D`
   +20, and the travel direction `2C45` turns by `wheel − level/2 − 2` instead of following the
   facing, so it lags further every frame. Otherwise the travel direction is the facing (from the next
   frame on). `2C47`/`2C49` = the car's pitch and roll.
2. `1792`–`17A9`: `3CE7(travel 2C45, pitch 2C47, d = step)`: `h = d·cos(pitch)`,
   `dx = floor(h·cos(heading)/32768)`, `dy = floor(h·sin(heading)/32768)` (Q15 tables), stored at
   `DS:3261`/`3263`. **17AD** adds them to x/y with the big-tile carry.
3. `1805` cell update, collision boxes (181E), car contact (1835).
4. `1838`: on the freeway (`2AD4` ≠ 0) z = 0. In the city **184B–1855**: z `2D39` = cell elevation·224
   + `ground_shape_height` (4160:0515), pitch `2D3D` = 0 or ±7 (`vehicle_set_pitch` 4160:0004);
   **1858** `jmp 1887`. (Only the player takes this path; with `DS:17` ≠ 0, the modem game, the
   player's height is set at 1876–1884 instead.)

`skid_grip` (`DS:2BF1`, words, `[model·9 + lock]`, lock 0..8) and `skid_grip_bonus` (`4160:0373`,
bytes `[model + 4·level]`), read from the running game:

| | lock 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|---|
| model 0 | 1000 | 724 | 650 | 528 | 472 | 388 | 312 | 280 | 252 |
| model 1 | 2000 | 778 | 724 | 650 | 528 | 472 | 388 | 312 | 280 |
| model 2 | 2000 | 840 | 778 | 724 | 650 | 528 | 472 | 388 | 312 |
| model 3 | 2000 | 1008 | 840 | 778 | 724 | 650 | 528 | 472 | 388 |

Bonus: TRAINEE 255, level 1: 100, PRO: 0 (all models). Speed is in world units per second (1 unit
≈ 3 inches; mph = speed·3/16), so model 0 at full lock skids from 507 (95 mph) on TRAINEE and from
252 (47 mph) on PRO.

Things the original does that matter here:

- **The cockpit view never pitches with the car.** The camera pitch is `2BEB` (0 in the car, −17 in
  the helicopter view, −8/−17 on bridge decks), not the car's pitch: on a 7° ramp the horizon stays
  level and only the eye height changes.
- **The step rounds down on both axes.** A car one degree left of north creeps left a unit every
  frame (`floor(−0.7) = −1`) at any speed; one degree right of north doesn't move sideways at all until
  `step·sin 1° ≥ 1` (a step of 58: speed 580 at 10 frames a second, 1740 at 30). Keyboard steering
  leaves the heading a few degrees off, so the car wanders off the road one way and not the other. On
  axis-aligned roads the forward part loses up to a unit a frame too (cos 0 = 32767/32768).
- The skid's lag keeps growing as long as the wheel is held (34° after 0.5 s in the S-bend below).

## 2. The layer points

All are emulator watches (they run before the original instruction, which then runs) except the
steer/skid replacement, a code hook. Each checks, the first time a race frame reaches one, that the
code is the DOS 1.1 build's (the bytes below) and removes everything if not.

| Address | Bytes | Layer |
|---|---|---|
| `3009:01A5` | `A1 3B 2D` | in the air: skip `heading += wheel` (AX = heading, continue at **01C3** `A3 3B 2D`); Lane Centering |
| `3009:0248` | `E8 87 10` | in the air, cockpit view: tilt `cam_pitch 2C79` with the nose |
| `3009:0ED9` | `E8 EC 0F` | speed before the drivetrain (kept in the air) |
| `3009:0EDC` | `E8 59 08` | in the air: speed restored, step = speed / frame rate (no traction) |
| `4160:037F` | `83 3E 4B 2C 00` | **replaced**: the drift model (RETF emulated) |
| `3009:17AD` | `8B 16 61 32` | the step's sideways rounding carried over; Lane Centering's glide (player only: `[2C51]` = 0) |
| `3009:1858` | `EB 2D` | the vertical motion: z and pitch after the original set them from the ground |

Improved Driving uses all of them; Lane Centering alone uses 01A5, 17AD and 1858 (the last only for
its telemetry). Constants are `DrivingTuning`'s members (`src/game/driving.h`); per-second rates are
scaled by the game's own frame rate `DS:2CD3`, so the feel is the same at 10 and at 30 frames a second.

## 3. Improved Driving: drift

`player_steer_skid` is replaced. With the original's own threshold `grip = skid_grip[model][lock] +
bonus` (lock = |wheel|, at most 8, at least 2; not in reverse):

- `beyond = speed / grip − drift_onset` (0.85: the slide starts a little below the original's skid
  speed);
- target slip = `min(drift_max, drift_per_grip · beyond)` degrees (24 per 1.0, at most 18), the
  wheel's sign;
- the slip moves toward the target at `drift_build` (30°/s) when growing, `drift_relax` (20°/s)
  when shrinking (the steering eased, or the speed dropped);
- travel direction `2C45` = facing − slip (the nose points into the corner, the car runs a little wide
  while the slide builds and tucks in as it settles);
- skid flag `2C4B` (the skid sound) only while |slip| ≥ `drift_sound` (8°);
- speed scrub `drift_scrub` (5 units/s² per degree of slip, double on rough ground): about the
  original's −5 a frame in an 11° slide. The original's `wheel_slip` and `rev_acc` penalties are not
  applied;
- a stopped car (a crash) sets off with no slip.

Measured (TRAINEE, model 0, a fast S-bend at speed 660 on the Great Highway, `vette_run --driving-log`,
10 frames a second):

| Frame (wheel) | +1 | +2 | +3 | +4 | +5 | +6 | +7 | +8 |
|---|---|---|---|---|---|---|---|---|
| original slip (°) | 0 | 0 | 0 | 0 | −1 → skid | 18 | 22 | 26, 30, 34 … |
| improved slip (°) | 0 | 0 | 0 | 1.4 | 4.5 | 7.8 | 9.7 (skid sound) | 11.4, 9.7, 11.5, 10.0, 11.6 |

The original's slip jumps 18° in one frame and keeps growing; the improved one builds at most 3° a
frame at 10 fps and settles near 11° at full lock. At full lock the drift starts from 431 (81 mph) on
TRAINEE; at PRO from 214.

## 4. Improved Driving: jumps

At 1858 the original has just set z `2D39` and pitch `2D3D` from the ground. The layer keeps a vertical
velocity:

- **On the ground** z and pitch are the ground's, and `vz` = the ground's rate of climb.
- **Take-off** when `speed ≥ jump_min_speed` (267, 50 mph; 450 until the user asked for jumps from 50 mph)
  and the ground has fallen more than a unit (kHeightStep) below the path the car would fly on: on the
  ground the layer carries that path from the last frame the ground kept up with the car (its climb, over
  0.1 s, less `lift_off`, now 0; 25 at first), under gravity. Heights are whole units, so the climb
  measured frame to frame wobbles by a unit a frame (30 units/s at 30 fps); the first rule, `vz −
  ground_vz > lift_off`, took that wobble on a steep ramp for the ground falling away (one-frame hops),
  and a single frame of a wrong ground height (the original placing the car at a ramp's foot) launched
  the car thousands of units up. A change of height in one frame beyond a slope's (`kJoltSlope` of the
  distance moved plus `kJoltSlack`) now sets the car on the ground as the original does, and in flight
  such a frame isn't taken for a landing. Measured over the Great Highway ramp (10 fps): no jump at
  49 mph, 6 in and 0.25 s at 57 mph, 9 in at 59, 18 in and 0.5 s at 80, 9.5 ft and 1.5 s at 151. The car leaves with `vz − lift_off`.
  A 7° ramp climbs at 0.12·speed, so its top would launch from 203; the minimum speed keeps ordinary
  crests at ordinary speeds on the ground.
- **In the air**: `vz −= gravity·dt` (115 units/s², about 0.9 g for a little more hang time; 128.7 = 9.81 m/s² at first), `z += vz·dt`; the nose eases
  toward the flight path `atan(vz/speed)` at `air_pitch_rate` (20°/s, within ±`air_pitch_max` 12°);
  the cockpit view (`2C79`, only in the car) tilts by the nose's change since take-off (at most
  `air_view_max` 10°); the wheel doesn't turn the car, the travel direction stays the take-off's, the
  speed stays (no throttle or brakes), and the step covers all of it; the skid sound stops; the move's
  pitch `2C47` is 0 (the height is the layer's). Collisions, cell events and the cell update run as
  usual.
- **Landing** when z ≤ ground: `impact = ground_vz − vz`; at `hard_landing` (45 units/s) or more,
  `on_hard_landing` (the app and `vette_run` report the `thud` sound event, the Mac landing sound).
- On the freeway (its own heights), after a pause (no player frame for 0.4 s) or a jump in position of
  over 600 units (the game placed the car), the car is back on the ground.

The hill on the Great Highway northbound: cell (22,2) ramps from 0 to 224 over x 45056–46900, then
cell (23,2) onwards is flat at 224. At full speed (810, 152 mph; 9–10 frames a second), with the first tuning
(gravity 128.7, lift_off 25):

| t (s) | x | z | ground | vz | pitch | |
|---|---|---|---|---|---|---|
| 108.755 | 46850 | 224 | 224 | +100 | +7 | the ramp's top |
| 108.855 | 46929 | 230 | 224 | +62 | +5 | airborne |
| 109.255 | 47254 | 242 | 224 | +9 | +1 | apex 18 units (4.5 ft) |
| 109.706 | 47597 | 231 | 224 | −47 | −3 | the view pitched 10° down (the limit) |
| 109.940 | 47773 | 224 | 224 | 0 | 0 | landed after 1.17 s, ~920 units (230 ft); impact 75: thud |

The original's own thud test (`is_thud`, notes 07) is held while the car flies and on the frame it
lands (`SoundEvents::thud_hold` ← `Driving::flying()`), so the flight's height and pitch changes never
sound a thud; the ramp's foot still does (106.6 s).

## 5. The sideways rounding (both options)

At 17AD (player only, in the city) the layer computes the exact step along the travel direction
(`d·cos(pitch)·(cos h, sin h)`), takes the sideways part of what `floor` lost (`(exact − applied) · n`,
n the travel direction's right), accumulates it (within ±3) and adds a unit to `DS:3261` or `3263` (the
axis nearest n) whenever it reaches ±1, before the original adds the step with its own carry. The
forward part stays the original's, so the speed and race times are unchanged; a car a degree off now
drifts at `speed·sin 1°` either way, as it points.

## 6. Lane Centering

**Lane markings.** `enhanced::lane_lines(World)` takes every `Line` primitive of colour 0Fh (white
lane dashes, 128 long with 128 gaps) or 0Eh (the yellow centre line) from the cells' ground layers
(`list1`, packed parts, absolute = entry position + vertex), level or along a ramp: 30,459 lines in
the shipped city. The standard strips: north-south road 7270 at cell y 0..256 (markings at 64, 128
yellow, 192; lanes 64 wide, centres 32, 96, 160, 224), east-west 7318 at cell x 1792..2048, plus the
intersection 7238, sloped and diagonal strips (76F8 135°, 76E9 45°, 7734/7716 153° …).
`game::LaneMap` indexes them by map cell.

**The lane** (`LaneMap::find`): in the 3×3 cells around the car, lines at least 96 long, within
`lane_max_angle` (15°) of the heading and within 320 units ahead or behind; the nearest on each side
(signed distance along the car's right). Both within 40..100 of each other: the midpoint; else the one
within 72: half a lane (32) from it. The direction is theirs (averaged).

**The assist** (at 01A5, before the heading update, and 17AD), only while the wheel is at 0 and has
been for `lane_resume_delay` (0.4 s, so a tap of the keys takes effect first), the race is on (start
light `2AD8` = 5), in the city (`2AD4` = 0), moving forward at `lane_min_speed` (60) or more, on the
ground and not sliding (|slip| < 2°). Any steering drops it at once; a lane change past 15° finds no
lane until the car straightens; on the freeway it is off (the game's own freeway steering is untouched).
The decisions are `LaneKeeper` (pure, so `driving_lane_keeper_smooth` drives it without the game):

- **Direction.** The target is the lane's direction rounded to a whole degree, kept while the measured
  direction stays within 0.75° of it (the roads run at 0, 45, 90, 135° and their opposites, and
  153.4°: 7716/7734). While the heading differs, it turns 1° toward it, at most once every
  `1 / lane_align_rate` s (3°/s). It only ever turns toward the target, so it never flips a degree and
  back.
- **Centre.** No heading change: the car glides sideways along the lane's right normal at
  `lane_centre_gain · (offset − slack)` units/s (0.6/s, easing in as it arrives), at most
  `lane_centre_rate` (10 units/s) and never faster than a path `lane_centre_angle` (2°) off the lane
  would drift at the current speed, and not within `lane_centre_slack` (2 units) of the centre. The
  glide's fractions of a unit accumulate per axis and go into the step at 17AD (`DS:3261`/`3263`), whole
  units at a time, before the original adds it with its own carry, cell update and collisions; the
  smooth renderer blends the position between game frames, so it reads as a glide, and the scenery far
  ahead doesn't move. On the 153.4° roads the heading stays at 153 and the glide takes up the 0.43° drift
  (about 7 units off the centre at speed 400).

The first version steered toward the centre (up to 4° off the lane's direction) and lined up with the
unrounded direction; every centring was a turn out and a turn back in 1° steps (each a 4.5-pixel jump
of the horizon in the race view), and on the 153.4° roads the heading flipped between 153 and 154 for
good. User feedback on 0.1.5: "choppy". Measured (Great Highway northbound, hands-off, throttle held,
placed at 41 s; `lane_stats` harness: per frame from 41 to 60 s):

| Start | 0.1.5: heading turns (reversals), time to within 4 units of the centre | now |
|---|---|---|
| 20 units left of centre, heading 0, 10 fps | 6 (1), 4.2 s | **0 turns**, 3.7 s |
| same, 30 fps | 8 (1), 4.8 s | **0 turns**, 3.8 s |
| 30 units left, heading 2°, 10 / 30 fps | 6 (1) / 6 (1), 4.6 / 5.5 s | 2 turns (0), 5.2 / 5.4 s |
| centred, heading 357° | 3 (0) | 3 (0), 0.7–0.8 s |
| 153.4° road, 15 units off, heading 150 (simulated, 30 s) | 18 (12), still flipping | 3 (0), heading 153 |

Sideways steps stay at most 1 unit a frame. With the keys tapped (right, then left 1.5 s later, every
3 s), each tap is unwound a degree every 1/3 s after the 0.4 s pause, as before; those reversals follow
the taps. Against the original (placed at y 4320 heading 357): the original creeps left (10 units/s at
10 fps, 30 at 30 fps) across the centre line (48.5 s / 44 s) and off the road; with the assist, heading
0 within 0.8 s of the placement and 2 units off the centre (`enhanced_lane_centering_real_game`).

## 7. Untouched when off

`Driving` with both options off installs nothing, and the app doesn't construct it then
(`src/main.cpp`, `DrivingAids`). `driving_off_is_bit_identical` runs a 70-second session (the start, the
Great Highway at full throttle, a fast S-bend into the original's skid and a crash) with no layer and
with a layer whose options are both off: the machines end with identical memory (1 MB), registers and
cycle counts. `SoundEvents` behaves as before unless `thud_hold` is set (only with the layer).

## 8. Where things live, and tuning

- `src/game/driving.h/.cpp`: `DrivingTuning` (every constant above, with its default), `LaneMap`,
  `Driving` (the hooks, telemetry `Driving::Telemetry`, `on_hard_landing`, `flying()`),
  `set_tuning(name, value)`.
- `src/enhanced/lanes.h/.cpp`: the city's lane markings.
- `src/main.cpp`: `DrivingAids` (constructs `Driving` when either option is on; lanes from the
  Enhanced view's world, or its own extraction when the view is off; the landing thud to the sound
  replacement).
- `vette_run --driving improved --driving-log [--tune name=value]... [--poke T:OFF:VAL]`: the car every
  frame (position, height, pitch, facing, travel, slip, speed, wheel, skid; with Improved Driving also
  ground, vz, flight). Lane Centering needs the extracted city, which `vette_run` doesn't link; the
  enhanced test drives it (and the measurements above came from a scratch harness linking
  `vette_enhanced`, with the same `Driving`).
- Tests: `tests/game_driving.cpp`, `tests/enhanced_driving.cpp`.

More drift: lower `drift_onset` (0.75), raise `drift_per_grip` (35) and `drift_max` (25); a quicker
slide: `drift_build`. Less: the opposite, or `drift_max` 8. Bigger jumps: lower `lift_off` (15) or
`jump_min_speed`; floatier: lower `gravity`. Lane Centering: a gentler or quicker straightening:
`lane_align_rate` 2 or 4 (degrees a second, in 1° turns); a slower or quicker glide to the centre:
`lane_centre_rate` 5 or 15 and `lane_centre_gain` 0.3 or 1 (`lane_centre_rate` 0: direction only); it
waits `lane_resume_delay` after the keys are let go.

## 9. Open questions

- The modem game (`DS:17` ≠ 0) sets the player's height at 1876–1884: no jumps there (the layer just
  doesn't see a ground frame); the drift and rounding layers would still apply.
- Whether `player_steer_skid` runs in freeway mode (4021) was not checked; the drift layer applies
  wherever it is called.
- Lane markings on the compound structures (Golden Gate, Bay Bridge decks) are not collected (they are
  drawn through `World::compounds`); those roads have no assist.
