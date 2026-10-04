# 02 — Main and frame loop structure (DOS 1.1) — first pass

`start` (3009:0025) contains both loops:

- **`game_outer_loop` 3009:005B**: menus, garage and race setup. Re-entered from `008A`, `00B2` and
  `0618` (end of race / quit to menu).
- **`frame_loop` 3009:0135 → 3009:061B** (`JMP 0135`): one iteration per rendered frame.

Video is EGA mode 0Eh (`video_set_mode_ega_0e`, 3009:8F00) with two pages at `A000` and `A400`,
i.e. double buffering. The host's EGA model needs both pages plus CRTC start-address flipping.

## Calls made by the frame loop, in order

Tags are **heuristic**, computed by `re/tools/tag_functions.py --callees-of 3009:0025 --range 0135-061B` from what each function and its callees touch
(depth 3): `EGA` = ports 3C4/3CE, `fps` = reads `frame_rate`, `sfx` = calls the speaker driver,
`DOS` = INT 21h. Names come later, once each function has been read.

| Site | Callee | Size (ins) | Tags | First guess |
|---|---|---|---|---|
| 0153 | 3009:BB00 | 8 | | |
| 015C | 4160:09E5 | 29 | | |
| 0165 | 3009:94FC | 34 | sfx | sound effect trigger |
| 0177 | 3009:3778 | 10 | | |
| 020A | 3009:0EAA | 20 | fps math sfx (deep) | |
| 0225 | 4021:0B7D | 31 | | |
| 023F | 422F:01C8 | 57 | | |
| 0245 | 3009:1D89 | 57 | math | |
| 0248 | 3009:12D2 | 240 | fps math; deep DOS EGA sfx | big simulation step (events → screens?) |
| 025F | 3FFC:0164 | — | | `kbd_clear_state` |
| 0267 | 3009:0EF5 | 261 | fps math sfx | big simulation step (player car?) |
| 028B–0359 | 11FF 5F00 71BF 71A8 61C3 3F2D 39B9 5A35 BCFB BB72 3589 | | EGA + math | 3D world transform and draw |
| 036E–0371 | 30C6, 0E93 | | math / EGA | |
| 038C | 3009:DDED | 36 | sfx; deep DOS EGA | event (crash/ticket screen?) |
| 039E | 3009:3734 | 14 | | |
| 03AF–0405 | 4021:0ED1 13DF 0AF5 0C49 01B7 024C 0143, 3009:775E 7F09 | | fps math | **simulation: divides by `frame_rate`** (vehicle/traffic) |
| 040A | 3009:700F | 6 | | `race_clock_tick` |
| 0424–04E5 | 70F0 6C7F 0666 0BB4 DADE DC6F DD1A DE49 | | EGA | HUD / dashboard draw |
| 051B | — | | | **frame timing** (`frame_rate` = round(291/elapsed)) |
| 0540–0550 | 24FE 24D0 F693 251D | | mouse | input |
| 05AF–05C5 | 07CF 9618 0775 | | joy | input (joystick) |
| 05E1 | 3FFC:0164 | | | `kbd_clear_state` |
| 05FE | 3009:C9E2 | 119 | sfx; deep DOS EGA fps vbios | menu/pause/end-of-race? |
| 0604 | 3009:8F00 | 12 | vbios | `video_set_mode_ega_0e` (conditional: mode reset) |

The ordering (simulate → draw world → simulate the frame_rate-scaled systems → HUD → measure time →
input) matters for 1:1. Input sampled late in frame *N* is consumed by the simulation in frame *N+1*.

## Next
- Read `0EF5`, `12D2` and the `4021:*` group; name the vehicle and traffic state in `124A`.
- Identify the 3D pipeline entry (`5F00`/`61C3`/`5A35` cluster) and the polygon rasterizer.
- Meaning of the `[2AD0]` flag (set before the frame loop, toggled by `0BB4`, gates input at `0549`).
