# 07 — Sound (DOS 1.1)

Addresses are image-relative `SEG:OFF` (`cs:` = 3009, `DS:` = 124A); names are in `re/symbols.csv`. Claims are
**confirmed** at runtime (host emulator, `vette_run --sound-log` and a scratch harness, section 8) or read from
the cited code. Durations: 1 driver tick = 4 × 4096 PIT clocks = **13.73 ms** (72.83 Hz). Frequencies:
1193181.8 Hz / divisor.

## 1. Hardware and mechanisms

DOS 1.1 drives **only the PC speaker**: every `in`/`out` on ports 61h/42h/43h is inside 3009:933E–94FB, and no
code touches an AdLib (388h), Tandy or other sound port. There is one voice, used in two ways:

1. **Tone programs** on PIT channel 2 (mode 3 square wave), stepped by `snd_seq_tick` (9373) from `timer_work`
   at 72.83 Hz. One program plays at a time; `cs:92BE` points at its current entry, `cs:92BC` counts ticks.
2. **Noise**: `snd_noise_crash` (9498) and `snd_noise_grind` (94C9) toggle port 61h bit 1 directly (PIT gate
   off) with CPU-timed `LOOP` delays. They run synchronously: the game is frozen while they click, and their
   length depends on the CPU speed.

### Sequence format and timing (`snd_seq_tick`, **confirmed**)

Entries are `{w ticks, w divisor}`:

- `ticks = FFFFh`: end. On the next tick the speaker goes off (61h &= FCh), `cs:926D = 0`, count = 0. The
  pointer stays on the end entry, so the end branch runs again on every later tick.
- `ticks = 0`: jump back by `divisor` bytes. This takes a tick of its own, during which the last note goes on.
- otherwise: on the tick where the count becomes 1 the divisor is latched (`spk_set_divisor` 9360: 61h |= 3,
  43h = B6h, 42h = lo, hi); `divisor = FFFFh` instead turns the speaker off (a rest). When the count reaches
  `ticks` the pointer moves on and the count restarts at 0.

So an entry sounds for exactly `ticks` ticks, a note starts 0–13.7 ms after `snd_play`, and **repeated notes run
together**: the speaker never stops between two entries with the same divisor (the reprogramming only resets the
counter's phase).

`snd_play` (933E, AX = sequence) returns at once if `sound_enabled` (`cs:9277`) is 0. Otherwise it only sets the
pointer and enables the speaker (61h |= 3). **It doesn't reset the count**; the callers do that (or don't, see
3.3). `snd_stop` (9352) turns the speaker off and points at the idle sequence `cs:92C0` (`FFFF FFFF`).

## 2. The sound list

| Sfx id | DOS sound | Played by (caller → routine) | Condition | Program | Ends / cut off |
|---|---|---|---|---|---|
| `engine` | engine note | frame loop 0156 sets request bit 3 → `snd_request_dispatch` 94FC → `snd_engine_set_target` 93EA → `snd_play(92A6)` every race frame | race running, no skid/siren request, engine noise on (`cs:926E`, E key) | `{2, cs:92A8}` loop; the divisor slides (section 4) | a higher request, `snd_stop` (section 6) |
| `garage_rev` | engine note, fixed pitch | car-select screen (`garage_screen` 8A22), Space → `garage_rev` 8BAF → 93EA (return 8BDC) | none | the engine program at divisor 1300h (245.3 Hz) | `snd_stop` at 8C09 when the exhaust animation is done (`cs:8DB2 = 1`) |
| `skid` | 4-note warble | `vehicle_move` 1774 sets request bit 1 → 94FC → `snd_play_skid` 948A | player's `skid_flag` (DS:2C4B, `player_steer_skid` 4160:037F: \|steer\| ≥ 2 and speed ≥ grip[model][\|steer\|] + level bonus; TRAINEE's +255 makes it rare) | 927E, loop (5.1) | bit cleared when the skid ends (177D), or along with the siren's by the police code |
| `siren` | two-tone | `police_step` 1479 sets request bit 2 → 94FC → `snd_play_siren` 9483 | chase car within 800h units, ticket screen not shown | 9292, loop (5.2) | bit cleared: farther than 800h (1482), pulled over (154B), chase reset (15DC) |
| `title_tune` | melody | `title_screen` C532 → `snd_play_title` 9470 (C560) | sound on (else `snd_stop`) | 9304, once (5.3) | its end (8.79 s), or a key: `snd_stop` at C61B (S toggles the sound instead, C586) |
| `win_tune` | fanfare | `race_end` C9E2 → results screen CA42 → `results_win_picture` CE9B → `snd_play_win` 9578 (CEBD) | race over with `DS:2AFF = 0`: the player reached a finish box (1AFF/1B31/1B60) before the opponent | 92C4, loop (5.4) | `snd_stop` at CEC3 after a key or 5 s (`wait_key_5s` 9968) |
| `crash` | crash noise | `collision_box_event` default path → 1CF6 | ran into a wall/object box, or the opponent's car (`car_contact_check` 1D3B → 1976) | noise, 26 clicks (5.6) | synchronous |
| `crash_car` | crash noise | `player_contact_response` 1720 (after `snd_stop` 171D); highway: `hw_player_collide` 4021:018B → far thunk 7F79 | hit a traffic car or the police patrol car (city), or a highway car | noise | synchronous |
| `crash_rail` | crash noise | `highway_rail_check` 7D6A → 7EC5 | highway lane < 1 or ≥ 5 (pushed back into lane) | noise | synchronous |
| `hit_pedestrian` | crash noise | `player_contact_response` 1720 (knocked down now, after `snd_stop`) or 16D2 (one already down) | pedestrian contact | noise | synchronous |
| `gear_grind` | grind noise | `key_reverse` 098C → 0999; `gear_select` 1F01 → 1F4F | R while moving; a gear whose speed limit is below the current speed, or leaving reverse while moving (the car drops to neutral; on PRO `drivetrain_damage` 23EC) | noise, 41 clicks (5.7) | synchronous |

Nothing else makes a sound: the start countdown (`BB00`) only changes the lights, and menus, the ticket screen,
the pause and the copy-protection quiz are silent. The engine keeps running under the countdown and the ticket
screen. Unused code is listed in 5.8. The moments the Mac version gives a sound to while DOS stays silent
(countdown, helicopter view, the bay, ...) are in section 9.

## 3. The race-frame dispatcher (`snd_request_dispatch` 94FC)

Called once per frame by the frame loop (0165), right after it sets `cs:926C |= 8`. Returns at once when the sound
is off.

| Variable | Meaning |
|---|---|
| `cs:926C` `snd_request` | b: bit 1 skid (`vehicle_move`), bit 2 siren (`police_step`), bit 3 engine (every frame); bit 0 = one-shot lock (only the unused beeps set it). Cleared at race setup (20B7) and in the garage (8A2D). |
| `cs:926D` `snd_current` | b: the request bit of the program now playing (2/4/8; 1 = beep); set to 0 when a sequence ends |
| `cs:926E` `engine_noise_on` | b: E key (`key_engine_noise_toggle` 0C1D) |
| `cs:9277` `sound_enabled` | b: S key (`key_sound_toggle` 0C0D, also in the garage 8A99 and on the title C589) |

Priority (first match wins): bit 0 (hold until the beep ends, then clear all requests) > **skid** (play 927E,
count = 0, unless already current) > **siren** (play 9292, unless current; count *not* reset) > **engine**
(`cs:926D = 8`; engine noise on → `snd_engine_set_target`, which re-points the program at 92A6 every frame; off →
`snd_stop` every frame).

### 3.1 Interruption (**confirmed** unless marked)

- A skid or siren request cuts the engine at the next frame; the engine comes back the frame after the request is
  cleared. A skid during the siren cuts the siren, which then restarts *from its first note* (926D ≠ 4; code).
- Noise doesn't stop the tone program. Only `player_contact_response` (car, pedestrian knocked down) calls
  `snd_stop` first (171D); walls, the rail, highway cars, a pedestrian already down and the grind play over the
  running program. The two fight over port 61h while the noise runs; the noise's last write leaves bits 0 and 1
  clear, so the program stays silent until its next `snd_play` (the engine: next frame) or next latch (the
  siren: up to 29 ticks, 0.4 s) (code).
- After `snd_stop` the next tick ends the idle sequence and sets `cs:926D = 0`, so the dispatcher restarts the
  highest request on the next frame.

### 3.2 The tick count

`snd_play` keeps `cs:92BC`. It is zeroed by the skid branch (9540), by `vehicle_move` every player frame that
isn't skidding while no siren is requested (178B), and by the police code when it drops the siren
(1488/1544/15E2).

### 3.3 Quirk: the siren's first note

If the siren starts while the count is ≥ 1 (left over from the engine or skid program), the count goes past 1
without latching: the first entry (659.6 Hz) never sounds, the speaker keeps the previous program's pitch for
the rest of its 28 ticks, then 523.3 Hz starts. From the code; every siren start in the test runs had count 0.

## 4. The engine note

**When.** Only on race frames: the dispatcher plays it whenever neither a skid nor a siren is requested, with
sound and engine noise on. `engine_speed` (`cs:5891`) is the player's rpm/100 from `engine_rpm_update` (60D7,
notes 04 §2): idle 11/9/8/7, redline 55/72/55/65 by car model.

**Program.** `{2 ticks, cs:92A8}` + jump back: the speaker latches the current `cs:92A8` once every 3 ticks
(every 2–3 when the frame's `snd_play` re-points it). `EngineSound::speaker_hz` is that latch,
`EngineSound::pitch_hz` the slide value.

**Pitch model (**confirmed**: `engine_slide` / `engine_pitch_tick` in `game/sound_events.cpp` match the original
at every call over a whole race, `tests/game_sound_game.cpp`).** The pitch is not a function of the revs; it
*slides* while the revs change:

- Once per frame, `snd_engine_set_target` (93EA) compares `engine_speed` with last frame's (`cs:9273`):
  - revs up → slide `8005h` (pitch rising);
  - same, ≥ 20 (2000 rpm) → slide 0 (hold);
  - revs down, or the same below 20 → slide `40h` (pitch falling) if `engine_speed` ≤ 53; above 5300 rpm the slide
    is left as it was (so it keeps rising at the redline).
- At 72.8 Hz, `snd_engine_pitch_tick` (9428) moves the divisor `cs:92A8`:
  - rising: `divisor -= 5 × (85 − engine_speed)` (16-bit `mul`), stopping below 1400h (**233 Hz**), or below
    1500h (222 Hz) when `engine_speed` ≤ 53. So it climbs fast at low revs (370/tick at idle) and slowly near the
    redline (65/tick at 7200 rpm); from the bottom to the top takes 1.5 s (at idle revs) to 8.4 s (at 7200)
    of rising frames.
  - falling: `divisor += 40h` until it is above B000h (**26.5 Hz**): about 8.5 s from top to bottom.
  - The tick runs whatever the speaker plays, but the slide is only re-evaluated on frames where the engine has
    the speaker. A skid or siren therefore freezes the slide: a rising pitch keeps rising to the top meanwhile,
    and the engine comes back at that pitch. Police run: the siren took over at 42.57 s with the pitch rising;
    the car stood at idle when the engine came back at 50.70 s at 224.7 Hz, then fell (158 Hz at 51 s, 72 Hz
    at 53 s, 40 Hz at 56 s).
- Below 2000 rpm the revs change in whole units every few frames, and a frame without a change makes the slide
  fall: the note wobbles upward as the car pulls away (38 s in the log below: revs rising, slide −1).

**Start pitch.** The garage sets `cs:92A8 = DF00h` (20.9 Hz, 8A65). Since the falling limit only stops a divisor
that has passed B000h, a first idle sits at ~21.6 Hz, *below* the 26.5 Hz a later idle settles at.

**Resync** (`snd_engine_resync` 9588): turning the sound or the engine noise back on sets `cs:9273 =
engine_speed` and the divisor from a table at `cs:932A` indexed by `engine_speed / 8`:

| rpm | 0–700 | 800–1500 | 1600–2300 | 2400–3100 | 3200–3900 | 4000–4700 | 4800–6300 | 6400–7900 |
|---|---|---|---|---|---|---|---|---|
| divisor | BB00 | 8B00 | 7B00 | 2B00 | 14C0 | 1240 | 1100 | 0B00 |
| Hz | 24.9 | 33.5 | 37.9 | 108.4 | 224.6 | 255.4 | 274.2 | 423.7 |

This is the only direct rpm → pitch map in the game (and the only way past the 233 Hz top).

**Log** (README drive, stock car, automatic, full throttle from 37.5 s; one line per second):

```
engine t=  37.0000s on  speaker   21.6 Hz, pitch   21.6 Hz, slide -1, rpm 1100 (idle 1100, redline 5500), throttle 0, gear 0
engine t=  39.0000s on  speaker   31.4 Hz, pitch   31.9 Hz, slide +1, rpm 2200 (idle 1100, redline 5500), throttle 1, gear 1
engine t=  40.0000s on  speaker   53.0 Hz, pitch   53.0 Hz, slide +1, rpm 3000 (idle 1100, redline 5500), throttle 1, gear 1
engine t=  41.0000s on  speaker  143.9 Hz, pitch  143.9 Hz, slide +1, rpm 3800 (idle 1100, redline 5500), throttle 1, gear 1
engine t=  42.0000s on  speaker  225.2 Hz, pitch  225.2 Hz, slide +1, rpm 4600 (idle 1100, redline 5500), throttle 1, gear 1
engine t=  43.0000s on  speaker  239.2 Hz, pitch  239.2 Hz, slide +1, rpm 5400 (idle 1100, redline 5500), throttle 1, gear 1
```

For a replacement engine: `rpm`, `idle_rpm`, `redline_rpm`, `throttle` and `gear` give a smooth synth its input;
`pitch_hz` is the DOS note itself.

## 5. The sounds in detail

Tables are decoded from the unpacked EXE (the observer reads them from the running game, never from the repo).

### 5.1 Skid (927E) — loop of 11 ticks (151 ms)

| # | ticks | ms | divisor | Hz | note |
|---|---|---|---|---|---|
| 0 | 3 | 41 | 094C | 501.3 | B4 |
| 1 | 2 | 27 | 0A14 | 462.5 | A♯4 |
| 2 | 3 | 41 | 09B0 | 481.1 | B4 −45 c |
| 3 | 2 (+1 jump) | 41 | 094C | 501.3 | B4 |

Steps 3 and 0 have the same divisor, so the speaker holds 501 Hz for 6 ticks: a warble 501 → 462 → 481 Hz.

### 5.2 Siren (9292) — loop of 57 ticks (0.78 s)

| # | ticks | ms | divisor | Hz | note |
|---|---|---|---|---|---|
| 0 | 28 | 384 | 0711 | 659.6 | E5 |
| 1 | 28 (+1 jump) | 398 | 08E8 | 523.3 | C5 |

### 5.3 Title tune (9304) — once, 640 ticks (8.79 s)

| # | ticks | ms | divisor | Hz | note |
|---|---|---|---|---|---|
| 0 | 40 | 549 | 0711 | 659.6 | E5 |
| 1 | 40 | 549 | 06AD | 698.2 | F5 |
| 2 | 40 | 549 | 054B | 880.6 | A5 |
| 3 | 200 | 2746 | 05F1 | 784.5 | G5 |
| 4 | 40 | 549 | 054B | 880.6 | A5 |
| 5 | 40 | 549 | 04B7 | 988.6 | B5 |
| 6 | 40 | 549 | 0473 | 1047.6 | C6 |
| 7 | 40 | 549 | 054B | 880.6 | A5 |
| 8 | 160 | 2197 | 07EF | 587.5 | D5 |

### 5.4 Win tune (92C4) — loop of 401 ticks (5.51 s), cut after 5 s

| # | ticks | divisor | Hz | note | | # | ticks | divisor | Hz | note |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 20 | 08E8 | 523.3 | C5 | | 8 | 10 | 05F1 | 784.5 | G5 |
| 1 | 10 | 08E8 | 523.3 | C5 | | 9 | 10 | 054B | 880.6 | A5 |
| 2 | 10 | 08E8 | 523.3 | C5 | | 10 | 40 | 0473 | 1047.6 | C6 |
| 3 | 40 | 05F1 | 784.5 | G5 | | 11 | 20 | 0473 | 1047.6 | C6 |
| 4 | 20 | 05F1 | 784.5 | G5 | | 12 | 10 | 0473 | 1047.6 | C6 |
| 5 | 10 | 05F1 | 784.5 | G5 | | 13 | 10 | 0473 | 1047.6 | C6 |
| 6 | 10 | 05F1 | 784.5 | G5 | | 14 | 160 (+1 jump) | 0473 | 1047.6 | C6 |
| 7 | 20 | 054B | 880.6 | A5 | | | | | | |

The rhythm (20-10-10, 40-20-10-10) is written out, but repeated notes run together on the speaker, so what plays
is C5 0.55 s, G5 1.1 s, A5–G5–A5 (0.27/0.14/0.14 s), C6 3.3 s. `wait_key_5s` (91 BIOS ticks) stops it 4.97 s
in, during the last C6.

### 5.5 Garage rev

The car-select screen's Space key: `garage_rev` saves `cs:92A8`, sets it to 1300h (245.3 Hz) and `engine_speed = 60`,
then calls `snd_engine_set_target` on every pass of the exhaust animation until `cs:8DB2 = 1` (set by C4BB).
1300h is already past the rising limit and steady revs give a hold, so it is a constant 245.3 Hz buzz: 1.08 s
at 12 MHz, 0.09 s at 140 MHz (the animation is CPU-timed). Afterwards `snd_stop`, the divisor is restored and
`engine_speed`/`cs:9273` are zeroed.

### 5.6 Crash noise (`snd_noise_crash` 9498)

Returns if `sound_enabled ≠ 1`. 26 clicks (DX = 19h down to 0): speaker bit 1 on for `rand & 7FFh` LOOPs
(`random_pit` 8D64: PIT channel 0 count + previous value, `ror 3`; 0 means 65536 LOOPs, a 55 ms gap), then off
for `BX += 100` LOOPs, BX starting at 3000 (3100 … 5600). At 12 MHz (10 cycles per LOOP) the click rate falls
from about 290 to 180 Hz over **0.11–0.12 s (confirmed: 110–119 ms)**; at 140 MHz 10 ms; on a 4.77 MHz 8088 it
would last about 0.5 s.

| Return address | Caller | Sfx | Tone stopped first |
|---|---|---|---|
| 16D5 | `player_contact_response` 16D2: a pedestrian already lying (profile EAEA) | `hit_pedestrian` | no |
| 1723, SI = pedestrian | 1720: pedestrian knocked down (EAE8 → EAEA, +100 points, heading ±15°, offence bit 3) | `hit_pedestrian` | yes (171D) |
| 1723, SI = 2D35 | 1720: traffic car or patrol car (speed loss, offence bit 1 at ≥ 140) | `crash_car` | yes (171D) |
| 1CF9 | 1CF6: `collision_box_event` crash (heading bounce, speed and rev_acc halved, offence bit 2) | `crash` | no |
| 7EC8 | 7EC5: `highway_rail_check` (lane out of 1..4) | `crash_rail` | no |
| 7F7C | far thunk 7F79 from `hw_player_collide` 4021:018B | `crash_car` | no |

Side note (**confirmed**): on the car path (170A) `player_contact_response` pops SI twice (170D and 1723), so it
leaves with SI = 1757 and its RET returns straight to `player_step` (0EDF), skipping the rest of `vehicle_move`
for that frame. The pedestrian paths are balanced.

### 5.7 Grind noise (`snd_noise_grind` 94C9)

Returns if `sound_enabled ≠ 1`. 41 clicks: on for `r = rand & 3FFh` LOOPs, off for `r + 10`. Random spacing
averaging about 0.87 ms (a hiss around 1.1 kHz), **33–36 ms at 12 MHz (confirmed)**, 3–4 ms at 140 MHz. Callers:
099C (R while moving), 1F52 (missed shift). At 140 MHz one 100 ms press of R gave 3 grinds, 33 ms apart (the
key is acted on again on later frames while it is down, **likely**); at 12 MHz the same press gave one.

### 5.8 Unused (no caller in DOS 1.1)

- `snd_beep_lo` 95A4 / `snd_beep_hi` 95C0: 659.6 Hz × 20 ticks (92AE) and 880.6 Hz × 10 ticks (92B4), with the
  one-shot lock (`cs:926C = cs:926D = 1`). Plausibly cut start-countdown beeps: the Mac plays low, low, high at
  the start (section 9), and these two are a fourth apart like the Mac's beep1/beep2.
- `snd_play_idle` 9491 (plays 92C0), and the sequence 929E (36.4 Hz × 3 ticks, loop).

## 6. Silence: `snd_stop` callers

| Caller | When |
|---|---|
| 0647 | quit to DOS |
| 0BC7 | pause (P) |
| 0C15 / 0C25 | sound off (S, also in the garage) / engine noise off (E) |
| 9574 | every race frame while the engine noise is off |
| 171D | before the crash noise of a car or pedestrian contact |
| 8A2A | garage screen entry (also clears `cs:926C`) |
| 8C09 | end of the garage rev |
| 9478 | title tune with the sound off |
| C586 / C61B | title: S pressed / title over |
| CA58 | results screen entry |
| CEC3 | end of the win tune |
| D102 (D108) | quitting the race from the Tab menu (0B56; D102 draws the results screen, **likely**) |
| DDEE | race-over picture (`race_message_picture` DDED, picture by DS:2AFF: 6 = into the bay **confirmed**; 5 = wrecked, 1–4 = an opponent won, **likely**) |
| E6A5 | options menu (Esc): also saves `sound_enabled` to `cs:E5FC` and sets it to 0 until EB0C restores it |

## 7. The observer (`src/game/sound_events.*`)

Watches only (no hooks, nothing written):

| Watch | Use |
|---|---|
| 9347 (`snd_play` past its test) | AX = sequence → Sfx. 92A6: [SP] = 9427 and [SP+2] = 8BDC → `garage_rev`, else `engine`. A different sound replaces the current one (stop, start); the same looping sound is a continuation; `title_tune` again is a restart. |
| 9352 (`snd_stop`), 937E (end entry) | stop of the current tone sound |
| 9360 (`spk_set_divisor`) | CX: the latched divisor (`EngineSound::speaker_hz`) |
| 94A1 / 94C8, 94D2 / 94FB | noise start (past the enabled test) / RET; the meaning from the return address (5.6) |
| 94FC | race frame: time, throttle (DS:2D48, still set here), gear; the player's z, pitch, speed (thud); the held states |
| E6B0 / EB0C | options menu mute / restore, for `enabled()` |
| BB00, 198A, 1562, 1B74, C55D/C5A6/C612/C615, 3FFC:0219 | the silent moments (section 9) |

`requested(engine|skid|siren|horn|helicopter)` reads `cs:926C`/`926E`/`9277` (and section 9's state) while race
frames arrive (one within 1 s) and the game hasn't silenced everything since: a `snd_stop` from anything but
the engine-noise switch (0C28, 9577) or the crash into a car or pedestrian (1720) ends them at once (pause,
menus, race-over pictures, sound off).
`program()` decodes the sequences above from memory, and the noise routines' immediates (count, mask, start,
step) into click timings for the emulated CPU clock. `tests/game_sound_game.cpp` checks that a run with the
observer is cycle-, RAM- and audio-identical to one without.

## 8. Validation (highlights)

`vette_run --game Game --skip-manual-check --sound-log ...`, 12 MHz (keys as in README's race script unless noted):

```
README drive:            1.8945 start title_tune   10.6889 stop title_tune   30.1928 start engine
                         45.9903 stop engine   (drove into the bay: race-over picture, DDED)
+ Space in the garage,   15.0001 start garage_rev  16.0755 stop garage_rev
  R at 41 s, 1 at 43.5:  41.0005 start gear_grind  41.0361 stop    43.5371 start gear_grind  43.5700 stop
  left turn into traffic:48.9275 stop engine  48.9276 start crash_car  49.0388 stop crash_car
                         49.1944 start engine  49.1946 stop engine  49.1946 start crash_car ...
PRO, Sledgehammer:       42.2859 start crash  42.4047 stop crash   42.5693 stop engine  42.5693 start siren
                         42.9868 start crash (over the siren) ...  50.4961 stop siren  50.4961 start engine
                         (pulled over: ticket screen)
PRO, random steering:    49.7453 stop engine  49.7453 start hit_pedestrian  49.8640 stop hit_pedestrian
                         49.9455 start engine  49.9456 start hit_pedestrian (lying: engine not stopped)
Toggles:                 E 32.0223 stop engine, E 33.0240 start; S 34.0749 stop, S 35.0596 start;
                         P 36.0775 stop, P 37.0001 start; Esc 38.0467 stop, Esc 41.0032 start
```

Scratch harness (same machine, plus memory pokes to reach the state; the observer itself never writes):

```
speed poked to 180h with full left lock (stock car, PRO):
                         41.2843 stop engine  41.2843 start skid  41.8350 stop skid  41.8350 start engine
cs:3 = FFh (finish) at 40 s, opponent not finished:
                         40.0326 stop engine  40.2661 start win_tune  45.2336 stop win_tune
DS:2AD4 = 1 (on-ramp) at 39 s:
                         43.0632 start crash_rail  43.1751 stop crash_rail  (6 rail hits, engine not stopped)
                         95.6578 start crash_car   95.7711 stop crash_car   (highway traffic, via 7F7C)
```

At 140 MHz (`--cpu-hz 140000000 --idle-skip`): garage_rev 15.0000 → 15.0918, gear_grind 3.0–3.8 ms, crash
10.0–10.3 ms.

## 9. The silent moments (Mac sounds without a DOS sound)

The Mac version (notes 08) plays sounds where DOS 1.1 is silent. The observer reports the DOS moment each one
corresponds to (`game::SfxKind` Cue: a start event only; Held: start and stop events plus `requested()`), so a
replacement can fill them in. The PC speaker plays nothing for them. They respect the game's sound switch.

| Sfx id | Kind | The DOS moment | Read from (**confirmed**) | Mac sound |
|---|---|---|---|---|
| `horn` | Held | the horn key, **X** (scan 2Dh), held in a race. DOS has no horn: an addition (9.1) | `kbd_held` 3FFC:000F bit 08h of byte 14h, while `kbd_game_mode` 3FFC:003C = 1; watched at the key handler (3FFC:0219) and each frame | `horn`, looped |
| `helicopter` | Held | the helicopter view: F4 or keypad + (`key_helicopter_view` 0A49) until F1-F3 (0A74) | `helicopter_view` DS:2ACF ≠ 0, each race frame | `heli`, looped; replaces the engine |
| `countdown_beep` | Cue | the start lights' first two steps: light 0 + "buckle up" (0 s), light 1 + "get ready" (2 s) | `start_countdown_step` BB00: `start_light` DS:2AD8 becomes 1, then 2 | `beep1` |
| `countdown_go` | Cue | light 2 + "go" (3 s); the clock restarts | DS:2AD8 becomes 3 | `beep2` |
| `splash` | Cue | drove into the bay: the water box C606h | `box_into_the_bay` 198A | `splash` |
| `thud` | Cue | the road under the car turns upward by 7° or more (onto an uphill, off a downhill) at speed ≥ 140, or its height steps by 32 or more, in the city (9.2) | the player's z DS:2D39, pitch 2D3D, speed 2D43 between race frames (`is_thud`) | `thud` |
| `pulled_over` | Cue | the police stop you (speed and gear zeroed, before the ticket screen) | `police_step` 1562 with `player_pulled_over` DS:2C59 = 0 | `joel` (a voice) |
| `intro_cable_car` | Cue | title: the cable car starts rolling in (1.9 s) | the first `title_restore_band` step after the reveal (C5A6 after C55D) | `cable car bell` (the intro's use) |
| `intro_car` | Cue | title: the Corvette comes at you (5.2 s) | `title_car_approach` call at C612 | `mic` |
| `intro_logo` | Cue | title: the VETTE! logo drops in (5.5 s) | `title_logo_drop` call at C615 | `Signature` |
| `service_station` | Cue | onto a service station's driveway, where stopping repairs the car (9.3); again only after a frame off it | `box_service_station` 1B74 (`collision_box_event`, box C358h, the player only) | `cable car bell` (the race's use) |

`program()` gives the countdown cues the two unused DOS beeps (5.8) as their "original" notes; the others have
none.

The Mac's "landing after a jump" thud has no DOS counterpart: DOS sets the car's height from the ground every
frame (`ground_shape_height` 4160:0515), so it never leaves it.

### 9.1 The horn's key

DOS has no horn: no key handler touches the speaker or a sound request. During a race (`race_keyboard` DS:2AD0
= FF) the game reads `kbd_isr`'s bitmaps (`read_held_keys` 07CF) and calls the handler of every key down,
from `key_handlers` cs:0D4E. 0B83 (a bare RET) handles 7, 8, 9, Tab, Y, the brackets, Enter, semicolon,
quote, backquote, backslash, X, V, full stop, slash, keypad * and -, Ins, Del, F11 and F12 (Ctrl, Shift, Alt
and the lock keys go to the BIOS; F11/F12 never reach the bitmaps). H, the usual horn key, is taken
(`key_map_toggle` 0CA2; the Mac chart labels H "Map"). X sits by the left hand next to the brake (Space) and
does nothing in the race, so it is the horn. It is still in the game's own list of keys that act once per
press (`keys_no_repeat` DS:2AEE), a leftover with no effect.

### 9.2 Vertical motion

The ground height is the cell's elevation × 224 plus a ramp shape (`ground_shape_height`: shapes 1-9 rise
224 over a cell, linear in x or y or the smaller/larger of the two), and the pitch (`vehicle_set_pitch`
4160:0004) is 0 or ±7° by the slope under the car. So the car's height is continuous on ramps and the pitch
snaps when a slope starts or ends: driving north up the Great Highway's hill at cell (22, 2) gives pitch 0 → +7
at the bottom and +7 → 0 at the top, z rising 8-9 per frame from 0 to 224. The thud is the compression: the
pitch rising by ≥ 7 (the bottom of an uphill, or the end of a downhill) at speed ≥ 140 (the speed DOS treats as
a hard hit, `player_contact_response` 1652). The height check catches any step between cells; a map scan finds
a few mismatched borders, but none on the roads driven here.

### 9.3 The service station (the Mac's "cable car bell" in races)

The Mac sound is named for a cable car, but in races the Mac rings it at **service stations** (**confirmed**):

- The Mac's box handler 6:5688 (notes 08) rings the bell, then, if the car has stopped and isn't already being
  repaired, zeroes its speed, sums its eight damage counters (A5−$346A.., raised by the crash code at
  6:4CD4-4D70 and drawn by the damage display at 6:4E58-506C) and, if there is damage, starts a repair:
  60 ticks per damage point plus 120, the counters cleared.
- Its box is the third of collision class 26, the class of Mac cell type 3 (13 cells, plus type 235 at one
  more). DOS cell type 14 has the same class number, 1Ah, also with six boxes; its third is C358h. The
  coordinates differ (the Mac's were re-authored), the role is the same:
- DOS box C358h (`collision_box_event` 1B74) does the same repair: if the player has stopped there, on ROOKIE
  or PRO, `repair_car` 2454 clears the eight damage bytes DS:3DC0-3DC7, the drivetrain and body damage
  (2C4F, 2C5B, 2C5D) and the steering limits, and the damage display comes up for 2 s.
- DOS cell type 14 is a service station: a red garage with brown bay doors west of the driveway (box C348h)
  and blue pumps east of it. Twelve cells: (6, 13) at 38th Avenue and Santiago, (11, 9), (14, 12), (16, 2) on
  the Great Highway, (22, 35), (28, 23), (29, 40), (30, 29), (34, 16), (35, 31), (37, 23), (39, 29).
- The driveway box is x 400-504, y 1536-1791 in the cell (a 104 × 255 strip, entered heading north or south:
  the garage is west of it and a thin wall, C350h, east).

The observer rings on each entry, as the Mac does: `collision_box_event` runs the C358h case every frame the
car is in the box, and the event fires again only after a race frame without it.

### 9.4 Validation

`vette_run --sound-log`, 12 MHz:

```
README drive + F4 at 40 s, F2 at 42, X held 43-44:
   1.9167 start intro_cable_car   5.1709 start intro_car   5.5381 start intro_logo
  30.4203 start countdown_beep   32.4228 start countdown_beep   33.4241 start countdown_go
  40.0994 start helicopter   42.0686 stop helicopter   43.0000 start horn   44.0000 stop horn
  46.2576 start splash   46.4242 stop engine (the race-over picture)
PRO, Sledgehammer:    50.4133 start pulled_over   50.4961 stop siren
North on the Great Highway (stock car, automatic, K to straighten every 5 s):
 115.0793 start thud   (cell (22, 2): pitch 0 -> 7 at speed 672)
The same, braking at 88.4-90 s, right at 92-92.9 and K, left at 94.8-95.7 and K (keys only):
  96.3220 start service_station   (the Great Highway station, cell (16, 2): east on the cross street, then
                                   north up its driveway)
Harness: put at cell (6, 13), x 200, y 1650 (38th Avenue station) during the countdown, then throttle:
  40.3333 start service_station   (and once more after being put back at 43 s while rolling)
```

Recordings from the game itself (`vette2026 --no-launcher --mute --pc 286 --effects mac --wav ...`, same keys),
each Mac sample found by normalized cross-correlation of the whole sample with the recording around its event:

| Event (vette_run, idle skip) | Sample | Best match | Where |
|---|---|---|---|
| intro_cable_car 1.9167 | cable car bell | 0.958 | 1.917 s |
| intro_car 5.1709 | mic | 0.285 (next best 0.049) | 5.171 s |
| intro_logo 5.5381 | Signature | 0.660 (next 0.107) | 5.538 s |
| countdown_beep 30.4036, countdown_go 33.4074 | beep1, beep2 | 0.925, 0.933 | 30.404, 33.407 s |
| helicopter 40.08-42.07 | heli | 0.995 | in the window |
| horn 43.000 | horn | 0.752 | 43.007 s |
| splash 46.2409 | splash | 0.686 (next 0.043) | 46.241 s |
| pulled_over 50.4133 (police run) | joel | 0.509 (0.074 elsewhere) | 50.397 s |
| thud (hill run) | thud | 0.750 (0.280 elsewhere) | 115.18 s* |
| service_station 40.3333 (station run, `--poke` the position) | cable car bell | 0.477 (next 0.061; 0 at 20-30 s) | 40.333 s |

\*The game's own run drifts from vette_run's by up to 0.2 s over two minutes of driving (its pedestrian hit is
0.2 s later too), so the thud is matched on the hill climb, not on the exact time.

The AdLib recording of the README drive shows the defaults sounding at the events (new spectral peaks after
each): the bell's 660/1320 Hz, the countdown's 660 Hz and 883 Hz (the unused DOS beeps' notes), the horn's
partials (since tuned to 416/520 Hz), the rotor's low partials with the engine level unchanged (it replaced the
engine), and in the station run the bell's 1040 Hz at 40.3 s. `tests/sound_game_audio_moments.cpp` checks on the running game that each moment reaches the
replacement, that the rotor replaces the engine for exactly the view's time, and that with the PC speaker alone
the output is unchanged sample for sample.

## Open questions

- The unused beeps (5.8) as a cut countdown: the Mac's countdown supports it (section 9); the PC-98's is unchecked.
- The Mac's horn key: its handler (1:30C0, released at 1:2E2C) tests a GetKeys bit; which key that is wasn't
  settled. The keyboard chart in the Mac box doesn't list a horn.
- The Mac's other two boxes on the same handler (box types 28 and 29: classes 89 and 90, cell types 151 and 139)
  are on no cell of the main map; maybe another of its maps.
- Whether the siren first-note quirk (3.3) is audible in practice (count ≥ 1 at a siren start never observed).
- The PIT reprogramming between repeated notes resets the counter phase: is there an audible click on a real
  speaker?
