# 12 — Two-player game over the serial link (DOS 1.1)

Addresses are image-relative `SEG:OFF` (`cs:` = 3009, `DS:` = 124A); the serial library is the far segment
**422F** with its own data segment **2F98**; the menus' data is in **0ACB**. Names are in `re/symbols.csv`.
Claims are **confirmed** by the cited code and, where marked, at runtime: two hosted games linked by an emulated
UART and an in-memory cable (`vette_link`, `tests/game_two_player.cpp`), and two `vette2026` windows over TCP.

Summary: the two-player mode is a null-modem or Hayes-modem link between two copies. Nothing is negotiated but
who starts in which grid slot; each game sends its car's state every frame and shows the other car from the
latest packet, dead-reckoning it in between. There are no timeouts that latency can trip except a one-second-ish
"opponent not ready" after 31 frames without a packet. The start isn't synchronised, the course isn't checked,
and the single packet that says a player finished can be dropped by the receiver.

## 1. Choosing two players (confirmed)

**The options menu** (`options_menu` 3009:E635) opens with **Esc** in the garage (`garage_screen` 8AAC), on the
course map (`course_select` FBCC, FC14) and in the race (`key_options` 0AFC). Its bar: *File* (Return to game,
Tour mode, Quit to garage, Quit to DOS), *Control* (Mouse, Joystick, Calibrate), *Communications*: **Two players
OFF/ON** and **Hang up line** (greyed unless connected through a modem: `0ACB:730B`, EEC1–EEDC). The manual
(pp. 35–37) describes the same.

**The setup screen** (`comm_setup_screen` 3009:ED5C) is full-screen text; Up/Down move between rows (Up from the
first row wraps to the last), Left/Right change a row's value (wrapping), Enter on a row activates it. The rows
(`0ACB:743B` → row records `{label, 0, item, handler, ..., FFFF}`), their values and what the handlers set:

| Row | Values | Handler effect |
|---|---|---|
| Connection | Direct, Modem | `0ACB:772A` bit 1 (modem) |
| Port | COM1, COM2 | bit 0 (COM2) |
| Baud rate | 1200, 2400, 4800, 9600, 19.2k, 38.4k, 57.6k | `0ACB:772C` = 0..6 |
| Mode | Call, Answer | bit 2 (answer) |
| Procedure | Initiate call, Take over call | nothing: both handlers are a bare `RET` (F01E, F01F) |
| Dial | Standard, Special | bit 3 (special: the text below is sent verbatim) |
| Line type | Tone, Pulse | bit 4 (pulse: `ATDP`) |
| Phone | Phone Number, Dial string | Enter: a 40-character text field (F0F5), length in `0ACB:772E`, text at `0ACB:7691` |
| Save options | Yes, No | written to `config.bin` when the screen closes with Save and DONE? both on Yes (EE83) |
| DONE? | Yes, Cancel | F03C / F0BD below |

- The screen loads `config.bin` (0x3C bytes: ten words = each row's item offset, then the 40-byte text) every
  time it opens (ED5C). The copy shipped with DOS 1.1 says Direct, COM2, 9600, Answer, Take over call, Special,
  Pulse, Dial string, Save Yes, DONE? Yes.
- **Quirk:** the flags and the baud index are set only by the handlers of the rows the cursor *visits* (arriving
  on a row runs its item's handler, EDDE/EE0E/EE28; Left/Right run the new item's). `0ACB:772A`/`772C` start at 0.
  So a player who goes straight Up to DONE? gets Direct, COM1, 1200 baud whatever the screen shows.

**DONE? Yes** (`comm_setup_done` 3009:F03C): the menu label becomes "Two players ON"; if two players weren't on
already, the flags go to `2F98:0028`, the baud index to `2F98:002A`, the text (with ATDT/ATDP and a CR) to
`2F98:002C..`; `cs:1` = FFh in Answer mode, else 0; `serial_open` (422F:0000); in **Direct** mode the handshake
`link_handshake` (422F:0424) sets `cs:1`; finally `cs:2` = FFh (two players on). **Cancel** (F0BD) closes the port
(`serial_close` 422F:00E1) and clears `cs:2`. **Hang up line** (`menu_hang_up` ED43) does the same. Quitting to
DOS closes it too (062F). Two players stay on from race to race until switched off.

Runtime (`vette_run`, no link attached): with no serial port at the BIOS's COM1/COM2 addresses (`0040:0000` = 0)
`serial_open` programs ports 0–6 (the DMA controller's) and the game then waits in the handshake forever
(`522F:0187`, inside `serial_recv_packet` called from 0440). VETTE! 2026 behaves exactly so when no link is
attached; with one, the PC has COM1/COM2 (section 7).

## 2. Roles (confirmed)

The protocol is symmetric: no master or slave. The only asymmetry is `cs:1`, used once, by `race_setup_cars`
(3009:2040): with two players on and `cs:1` ≠ 0, the player's and the opponent's start positions (x, y, z) are
swapped, so the two cars take the two grid slots.

- **Modem:** `cs:1` = FFh on the side set to *Answer*; the players agree beforehand.
- **Direct:** `link_handshake` 422F:0424 (no timeout anywhere):
  1. send two 3-byte packets; wait for any packet from the other side;
  2. send two more; then send a 4-byte packet with a random word (`random_pit` 3009:8DB5: the PIT count and a seed);
  3. wait for a packet of length ≥ 4; equal randoms: back to 2 with a new number; else `AX` = FFFFh if ours is
     greater (signed), 0 if not → `cs:1`.

  Packets sent before the other side has opened its port are lost; the protocol tolerates it (both sides
  re-send). On two identical emulated machines in lockstep the randoms would be equal forever; `vette_link`
  starts side B 7 ms later.

## 3. Before the race (confirmed)

Nothing is exchanged before or after the handshake. Each player separately chooses a car (garage), a level
(TRAINEE/ROOKIE/PRO plates), an "opponent" and a course (map). Consequences:

- **The course is not checked.** The manual (p. 37): "decide... which course you'll be racing on. You will not be
  allowed to race simultaneously on different courses" — nothing enforces it. On different courses each game runs
  its own finish line; the cars meet only where the routes do.
- **The other player's car looks like the opponent chosen here**: the remote car is the opponent struct
  `DS:2F09`, drawn by its entity's stub (27D3) with the model `DS:3144` = `DS:2D29[DS:A]` set from the opponent
  selection (1F9D). Packets overwrite only `2F09+0..2Dh`. Runtime: side B, having chosen the Lamborghini, sees A's
  Corvette as a yellow Lamborghini. The opponent's AI route is set up as in a race against the computer, unused.
- **Levels may differ** (police, grip): each game applies its own to its own car.
- **The start isn't synchronised.** Each race starts when its player presses Enter on the map; the countdown
  (`DS:2AD8`) is local. Packets go out from the first race frame (023F runs during the countdown); the receiver
  ignores the remote car until its own countdown is over (0EFF). Each side's race clock is its own.

## 4. Packets (confirmed)

**On the line** (`serial_send_packet` 422F:01C8): `'I' 'D' 'N'`, a checksum byte, then `len` bytes starting with
the little-endian word `len`. The checksum is the 8-bit sum of those `len` bytes. The packet is built at
`2F98:015B` (the sync bytes and checksum precede it at 0157–015A).

| Kind | len | Body after `len` | Sent by |
|---|---|---|---|
| City | 32h | status word (lo = sender's `DS:2`: 0 racing, 4 finished; hi = 0), then the player struct `DS:2D35+0..2Dh` | frame loop 0215–023F, every frame, after `player_step` |
| Freeway | 16h | status FFFFh, road id `8156`, ring segment `8232`, slice `8230`, 8, x and y relative to the slice, heading, speed, `840F` | `hw_build_packet` 4021:0B7D, every frame on a freeway |
| Status | 8 | status word = event (1 paused, 2 wrecked), then 4 stale bytes | `serial_send_status4` 422F:0482: four times, 4 BIOS ticks (220 ms) apart, the game blocked meanwhile (~0.9 s) |
| Handshake | 3, 4 | 1 stale byte / the random word | `link_handshake` |

City packet 54 bytes on the line: 9.4 ms at 57.6k, 14 at 38.4k, 28 at 19.2k, **56 ms at 9600** (slower than 30
frames a second), 450 ms at 1200. Freeway 26 bytes.

**Sending.** Two transmit buffers (`2F98:0463`, `0567`); a packet goes into the one not being transmitted,
replacing any packet already waiting there (`2F98:000E`). The first byte is written after polling LSR for THRE
(0219); the rest go out from the THRE interrupt. So at a low baud rate the newest packet always goes next and the
ones between are dropped at the source.

**Receiving.** The interrupt (`serial_isr` 422F:0248 → 025F) collects bytes into `2F98:025B` or `035F`
(`2F98:000C`): index 0–2 must be "IDN" (a wrong byte is ignored without restarting, so `I x D N` also syncs),
byte 6 gives the remaining count, lengths over 100h restart. A complete packet is published in `2F98:000A`
**only if the slot is empty**; otherwise it is dropped and the next one is collected into the same buffer. So
the game gets the *first* packet completed since it last took one. `serial_recv_packet` (422F:0176, called by
`opponent_step` at 0F41, once a frame) copies it to `2F98:0056`, empties the slot and checks the checksum: bad
→ as if none (`ZF` = 1). It returns with `DS` = 2F98 (the caller restores it).

**On arrival** (`opponent_step` 0F48–0F90): status lo → `DS:3` (the other side's state); city: the struct over
`DS:2F09` (z and pitch included: the receiver never sets the remote car's height from its own ground),
`DS:2B60/2B62` = new/previous heading, `DS:2B01` (frames since a packet) = 0, `DS:842B` = 0; freeway:
`hw_store_packet` (4021:0C43) → `DS:7406..`, `DS:842B` = FFh, placed into freeway car slot 10 (`hw_remote_place`).

**No packet this frame** (0F94–1051, notes 04 §5): `DS:2B00` = FFh, `DS:2B01` += 1 (max 7Fh); if `DS:3` ≠ 0 the
car stays where it is; else dead reckoning in the city: heading += Δ(last two packets)/frame rate, the car moves
speed/frame rate (at least 1 unit) along it, x and y only (z and pitch held). Quirk: a freeway packet's status is
FFFFh, so `DS:3` = FFh until the next city packet: on a freeway the remote car is never dead-reckoned and the
timeout below never fires (`hw_remote_step` 4021:0C49 is effectively unreachable).

**Timeout:** at 0271, if `DS:3` = 0 and 31 frames passed without a packet, `DS:3` = 3 ("opponent not ready"): the
remote car freezes. About 1 s at 30 frames a second, 3 s at 10. The next packet clears it.

**Messages** (`draw_opponent_status` DE49, from 04C1): when `DS:3` changes (`DS:0` = the last shown, `DS:1` =
frames shown) the dash message area shows, for 33 frames, `0ACB:6071[DS:3]`: 1 "OPPONENT PAUSED", 2 "OPPONENT
WRECKED", 3 "OPPONENT NOT READY", 4 "OPPONENT FINISHED".

A status packet (len 8) takes the city path: its 4 stale bytes (the last city packet's x and y) go over the
remote car's x and y, `DS:842B` = 0.

**Pause** (`key_pause` 0BC7, P): sends status 1 four times (0.9 s), then pauses. The other game shows the
message and freezes the car until city packets resume (status 0).

**Wreck / into the bay** (`race_end` C9E2 with `DS:2AFF` > 4): status 2 four times, then the menus.

**Finish:** the finish boxes (1AFF, 1B31, 1B60) set `cs:3` = FFh (the race ends after this frame) and `DS:2` = 4,
so the frame's packet carries status 4, once. The receiver: `DS:3` = 4 → `opponent_finished` (11FF): `DS:FA45` =
FFh and the opponent's time = **the receiver's own race clock** when the packet arrived; "OPPONENT FINISHED".
When it finishes too, `race_end` shows the loser's picture for the chosen opponent (`DS:2AFF` = `DS:A` + 1);
whoever's game didn't hear of a finish first shows the win. In two-player races the results screen leaves out
the opponent's time line (CDFC/CE13).

- **The single finish packet can be lost**: if the packet before it is still in the receiver's slot when it
  completes (two packets within one receiver frame: the two PCs' frame rates differ, and a network bunches
  packets). Then `DS:3` stays 0, the timeout makes it 3, and both players see themselves win. Measured over 20
  emulated races each (A at ~12, B at ~10 frames a second): B heard of A's finish in **16 of 20** with the original
  receiver, with no latency and with 50–150 ms alike; **20 of 20** with `LinkPacer` (section 7).
- Two finishes within the link's latency of each other also both win (each game decides alone).

**Link lost:** no detection beyond the timeout; the race goes on with the other car frozen and "OPPONENT NOT
READY"; packets resuming restore it. Between races two players stay on.

`DS:17` is **never set** (only cleared, 20F5): `player_step` would copy the remote car over the player's (0EAA),
the car contact checks are skipped (182E, 185B) and the remote car isn't drawn (27D3). An unused spectator or
replay mode; not the modem game (notes 11 said so: corrected there).

## 5. The serial library: segment 422F, data 2F98 (confirmed)

| Address | Name | What |
|---|---|---|
| 422F:0000 | `serial_open` | Open the port: UART, modem dialling, interrupt vectors (below) |
| 422F:00E1 | `serial_close` | Modem hang-up, restore the UART and the vectors |
| 422F:0176 | `serial_recv_packet` | Take the received packet (checksum checked) → `DS:SI` = 2F98:0057, ZF=1 if none |
| 422F:01C8 | `serial_send_packet` | Send the packet at 2F98:015B |
| 422F:0248 | `serial_isr` | INT 0Bh and 0Ch: `serial_isr_service`, non-specific EOI, IRET |
| 422F:025F | `serial_isr_service` | Loop on IIR until no interrupt: receive or transmit a byte |
| 422F:032C | `modem_dial` | AT commands, dial or answer, read the result |
| 422F:03B8 | `serial_write_polled` | CX bytes from DS:SI, polling THRE |
| 422F:03CD | `modem_read_result` | Read numeric result codes by polling LSR (no timeout) |
| 422F:0407 | `wait_bios_ticks` | Wait CX changes of the BIOS tick count (55 ms each) |
| 422F:0424 | `link_handshake` | Direct connection: who takes the second start position (section 2) |
| 422F:0482 | `serial_send_status4` | Status packet AL, four times 4 BIOS ticks apart |

**UART programming** (`serial_open`): base = `0040:[(flags & 1) * 2]` (the BIOS's COM1 or COM2 address).
LCR (base+3) saved, `80h` (DLAB); DLM (base+1) saved, 0; DLL (base+0) saved, divisor `2F98:066B[baud]` =
60h 30h 18h 0Ch 6 3 2 (1200 … 57600 baud); LCR = 3 (8N1); RBR read (flush); MCR (base+4) saved, **0Fh** (DTR,
RTS, OUT1, OUT2); modem: `modem_dial`, then a 3.3 s wait; INT 0Bh and 0Ch vectors saved and both pointed at
`serial_isr` (so either COM port's IRQ works); RBR read again; IER (base+1) saved, **3** (received data and THRE
interrupts); `out 21h, 0` (**all** IRQs unmasked, also at entry); a 65536-iteration `LOOP` delay. The line-status
and modem-status interrupts are never enabled; MSR is never read: no CTS/DSR/DCD checks, no flow control.

**Interrupt service:** read IIR (base+2); bit 0 set → done; read LSR (base+5) and ignore its error bits; IIR bit
1 → transmit (next byte of the current buffer, or start the waiting one, or idle), else receive (RBR, base+0).
EOI is `out 20h, 20h`.

**`serial_close`:** modem: 3.3 s, `+++`, 1.6 s, `ATH0`, 1.1 s, `ATZ0`, 1.6 s. Then `out 21h, 18h` (IRQ3 and 4
masked, all others unmasked) and the UART restored — **off by one register**: LCR=80h, DLL, DLM, LCR=0, IER,
then the saved LCR goes to base+2 (FCR on a 16550, nothing on an 8250) and the saved MCR & FEh to base+3 (LCR).
MCR is never restored: DTR, RTS and OUT2 stay on. Then the two vectors.

**Modem** (`modem_dial`): `ATZ0` (3.3 s) `ATE0V0Q0` (0.27 s: no echo, numeric results); Answer: `ATS0=1`
(auto-answer), then two result codes are read; Call: `ATDT` (or `ATDP`, Pulse) + number + CR, or with Special the
typed string as it is; one result code. Result 1 or 10 (CONNECT, CONNECT 2400) or 5 (1200) count as connected
(returned in DI, which the caller ignores). `modem_read_result` polls for ever: with no modem answering, the game
hangs at DONE?. The status strings at `2F98:0697` ("Modem Status:", "Dialing...", "Connect...", "Ringing...", "No
Carrier...", "Error...", "Waiting...") are never shown: their display routine (422F:00E0) is a bare `RET`.

**Data 2F98:** 0008 base port; 000A received packet waiting for the game (0: none); 000C buffer being filled;
000E packet waiting to be sent; 0010 packet being sent (0: idle); 0012 receive index; 0014 transmit index; 0016
receive count; 0018 transmit count; 001A–0021 saved INT 0Bh/0Ch; 0022–0026 saved DLL, DLM, IER, LCR, MCR; 0028
flags (as `0ACB:772A`); 002A baud index; 002C dial string length; 002E "ATDT" + 0032 number/string; 0056 the taken
packet (checksum + body); 0157 "IDN" + checksum + 015B packet to send; 025B, 035F receive buffers; 0463, 0567
transmit buffers (104h bytes each); 066B divisors; 0672 the modem strings.

## 6. Latency (VETTE! 2026, measured)

Two games at 12 MHz (10–12 frames a second, both at full speed, 810 units/s, side by side on the Great Highway),
`vette_link --delay/--jitter/--stall`, 50 s of racing:

| Each way | Other car shown behind its real position (avg, A / B) | Race frames with a packet | Remote's per-frame step ÷ expected (B: median, p10–p90) |
|---|---|---|---|
| none | 6 / 14 units | 82–99 % | 0.91, 0.45–1.68 |
| 50–150 ms (in order, bunched) | 39 / 85 | 81–98 % | 0.96, 0.58–1.64 |
| 50–150 ms + 150 ms stalls (0.5/s) | 39 / 85 | 81–98 % | 0.96, 0.64–1.60 |

- Nothing stalls or desyncs with latency: the handshake and menus have no timeouts; in the race the only one is
  the 31-frame timeout, which a gap over ~1 s (30 fps) would trip, and which recovers.
- The remote car is about one network delay behind (0.1 s ≈ 80 units at full speed). Its frame-to-frame motion is
  uneven with or without latency: the two games' frame rates alias (0, 1 or 2 of the sender's frames per receiver
  frame), and dead reckoning overshoots and snaps back when a packet comes. That is the original's behaviour.
- Bytes are paced into the UART at its baud rate (57.6k: 9.4 ms per packet), so a burst of packets drains in a
  few milliseconds each; the game keeps one per frame.
- Start skew: the two handshakes complete one one-way delay apart; `TwoPlayerStart` then starts both races the same
  way, so they begin about one network delay apart (two windows over TCP on one PC: the same moment, within the
  measurement's ~0.1 s).

## 7. In VETTE! 2026

- **UART** `host/uart.h`: an 8250/16450 (no FIFO) with THR/RBR, IER, IIR, LCR (word length, parity, stop bits,
  DLAB), the divisor, MCR (DTR, RTS, OUT1, OUT2, loopback), LSR, MSR and the scratch register. Characters take
  their time on the line at the programmed rate; received bytes are paced the same way and never overrun (the next
  waits until RBR is read). Its IRQ goes through the PIC (rising edge raises, a request gone before it was
  acknowledged is withdrawn), gated by OUT2.
- **`Machine::attach_serial(SerialLink*)`**: gives the PC COM1 (3F8h, IRQ4) and COM2 (2F8h, IRQ3) in the BIOS
  data area; the link is the cable on the port the game opened last (MCR written with DTR). Bytes go to the link at
  the end of each `run_for`. Never called: no serial ports, as before (the classic game is unchanged, ports and
  BIOS data area included).
- **`host/serial_link.h`** `SerialLink` (send / receive / connected), the transport's interface; MSR's CTS, DSR
  and DCD follow `connected()` (the game doesn't read them). `host/loopback_link.h`: `LoopbackCable` (two ends in
  one process), `DelayedLink` (delay, jitter, stalls, in order). `host/tcp_link.h`: the development TCP link.
- **`game::LinkPacer`** (`game/two_player.h`): between the link and the UART. Packets pass as they arrive, except
  one whose status differs from the last passed (finished, paused, wrecked, freeway, handshake): it waits until no
  packet is on its way in and the receive slot `2F98:000A` is empty, newer packets replacing it meanwhile. Same
  lag as without it; the finish always arrives (section 4).
- **`game::TwoPlayerStart`**: drives both games' own menus with keys, watching each screen's key loop (title
  C57E, garage F74B, level DF3B, opponent E2A6, course F7F8, setup FA9F, handshake 422F:0440/0465, race 0135):
  car, level, opponent, course; Esc, Communications, Two players; on the setup screen every row in turn
  (Direct, COM1, 57.6k, Save No), DONE? Yes; after the handshake, Enter on the map. The host's course and driving
  physics (`TwoPlayerSetup`, "vette2p/1 course=N improved=0|1") must reach the guest first.
- **Improved Driving** (notes 11) works for the local car as in any race. Its packets carry the struct as the layer
  left it (height and pitch in the air). Two words the original never reads carry the flight: struct **+0Ch** (0
  on the ground; in the air `vz·2 << 1 | 1`, units/s) and **+1Eh** (the ground's height under the car). The
  receiver shows the remote car's height from the packets (the original does), and on frames without one, while
  the last said airborne, continues it under gravity down to that ground instead of holding it (watches at 0F84
  and 1054).
- **The smooth renderer** and the Enhanced world tracer run on scratch CPUs whose I/O reaches only the EGA (or
  nothing) and which have no interrupt controller: they can't touch the UART or the link.
- **`game::IntroLink`** (`game/race_intro.h`): once the cable is connected, each game first sends its intro
  ("VETTE2026 INTRO 1", the length, then `name=` and, from the host, `map` and its map's text), and takes the
  other's off the front of what arrives, before the UART sees anything: the players' names, and an online race in
  the host's own map (installed at 3009:0025 like any map, before the guest's game starts). The original's
  bytes before the intro has gone out are dropped, as on a cable nobody listens to yet.
- **Name tag and arrow** (`enhanced/player_markers.h`): drawn from the frame's DS, the remote car `DS:2F09`
  (+22h/+24h its big tile) against this one `DS:2D35`, through the camera `DS:2C71` and the viewport (the
  mirror's from `DS:2B87`, as in section "Rear-view mirror" of notes 03). Classic takes DS at 3009:036E (the
  world about to be drawn), keeps it until the page flip has shown that frame (0546), and notes the mirror
  (0666). Nothing on a freeway: `DS:2AD4` (this car) or `DS:842B` (the other's last packet a freeway's).

## Open questions

- Freeway packets: how `hw_remote_place` decides visibility (DS:842A) and what `840F` is.
- Whether the original hardware's modem latency (V.22bis, ~50–100 ms) made the finish race visible in 1989.
- A modem emulation (AT commands answered locally) would let the Modem connection work over the internet too.
