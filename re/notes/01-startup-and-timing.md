# 01 — Startup, timers and the timing model (DOS 1.1)

Addresses are image-relative `SEG:OFF` (see `re/symbols.csv`). Main code segment `3009`, game data
segment `124A`, far library segments `3FFC`, `4160`, ….

## Character of the code
The game is **hand-written 8086/286 assembly**, not compiled C. It has a custom startup (no C runtime),
many variables addressed via the `cs:` prefix inside the code segment, register-based calling
conventions, and far calls into library segments. Ghidra's decompiler output is a reading aid only. The
port is a careful translation of assembly into C++, verified against the original running in the host
interpreter.

## Startup (`start`, 3009:0025)
`DS = 124Ah` → keyboard hook (`3FFC:0183`) → … → `detect_cpu_class` → `timer_init` → main menu loop.

## Interrupts
| Vector | Handler | Rate | Job |
|---|---|---|---|
| 08h (IRQ0) | `int08_timer` 3009:98FD | 291.3 Hz | `++pit_ticks`; every 4th IRQ → `timer_work` |
| — | `timer_work` 3009:9918 | 72.8 Hz | PC-speaker driver (`snd_engine_pitch_tick`, `snd_seq_tick`); every 4th call chains to BIOS INT 08h |
| 1Ch | `int1c_joystick_buttons` 3009:985C | 18.2 Hz | joystick buttons → `joy_buttons` |
| 09h | `kbd_isr` 3FFC:01C6 | per key | keyboard |

`timer_init` programs PIT channel 0 with divisor **0x1000** (1193182 / 4096 = **291.27 Hz**). The
BIOS still sees 18.2 Hz because only every 16th IRQ is chained.

**No game simulation runs in interrupts.** The ISRs only do sound, joystick buttons and the keyboard.

## The timing model: variable timestep
Every frame, the main loop (around `3009:051B`) does:

```
elapsed = xchg(pit_ticks, 0)          ; PIT ticks since last frame (291.27 Hz units)
if elapsed == 0: elapsed = 1
frame_rate = round(582 / elapsed / 2) ; == round(291 / elapsed)
if frame_rate < 4: frame_rate = 4
[124A:2CD3] = frame_rate
```

Per-frame simulation is scaled by this integer `frame_rate`. **The original's gameplay feel therefore
depended on how fast the player's PC rendered.** At low frame rates, integer rounding in the per-frame
math differs from high ones.

The **race clock** is separate and frame-rate independent. `race_ticks` (dword at `124A:3DE8`) adds up
raw PIT ticks, and `race_seconds = race_ticks / 291`. Since the true rate is 291.27 Hz, the original
clock runs about 0.1% slow, and a 1:1 port must reproduce that.

`detect_cpu_class` (3009:0E28) uses the 8088 `push sp` quirk, then counts `idiv` iterations for 292 PIT
ticks. It returns 0/1/2. Class 2 (fast) clears `cs:0E26`/`cs:0E27`, which startup copies into
`124A:2AC7`/`124A:2ABE`. These are probably detail or effect toggles; their meaning is TBD.

## Consequences for the port
1. **Classic mode needs a reference machine profile.** "1:1" means the frame cadence of a chosen
   reference PC: a fixed `elapsed` ticks per frame (or a recorded distribution), plus the CPU class it
   would detect. The host interpreter gets this naturally from its emulated cycle budget. The native
   port feeds the same `elapsed` sequence.
2. **High-refresh Enhanced play is native to the design.** The original code already handles any
   `elapsed ≥ 1` (up to 291 fps). Enhanced mode can run the *original* simulation math at e.g. 60 fps
   (alternating 4/5 ticks) with no interpolation hacks. This is a gameplay-affecting option, because
   rounding changes the feel, so it lives in the gameplay category.
3. Demo recordings for lockstep testing must capture `elapsed` per frame along with inputs.

## Open
- Which per-frame quantities divide by `frame_rate` (vehicle physics, traffic, police)? → next pass.
- `engine_speed` (`cs:5891`): speed or RPM?
- Meaning of the `cpu_slow_flag_*` toggles.
