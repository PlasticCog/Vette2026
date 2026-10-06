# 08 — The Mac release: files, formats and digitized sound (VETTE! 1.02, English)

The Mac VETTE! (Spectrum HoloByte, 1.02, June 1992) ships two applications on one HFS volume, B&W
`VETTE!` and `Color VETTE!`, each with its own copy of `VETTE!.Data`. The data files are the same
231 resources in a different order; all game data and sound sit in resource forks. Readers:
`src/assets/hfs.h` (HFS images), `src/assets/mac_files.h` (any form of the files, resource forks),
`src/assets/mac_sounds.h` (sounds), and the tool `vette_mac` (list, dump, WAV). The DOS side of the
sound mapping is in notes 07 and `src/game/sound_events.h`.

Evidence is the 68000 code of `Color VETTE!` (`CODE` 0–10; segment 9 "sound") and the sound driver
`BGAS` 128 in `VETTE!.Data`, disassembled with capstone. Addresses below are `segment:offset` into
the `CODE` resource bodies (offset 0 = the 4-byte segment header), or `BGAS+offset`. The B&W
application makes the same number of sound calls in the same segments, with its A5 globals at other
offsets. Codex's `Vette_Mac_EN/resource_manifest.json` matched what the readers find, except that it
left out the volume's `Desktop DB`/`Desktop DF`.

## Files (confirmed)

| Path on the volume | Type/creator | Resource fork | Contents |
|---|---|---|---|
| `VETTE! Folder/(Folder) Color VETTE!/Color VETTE!` | APPL/VETT | 1,587,389 | code, 192 `PICT`, 8 `pltt`, dialogs |
| `VETTE! Folder/(Folder) Color VETTE!/VETTE!.Data` | DATA/VETT | 577,498 | 16 `INST`, `BGAS` driver, world (`MAPS`, `OBJS`, `QUAD`, ...), `PHAZ` |
| `VETTE! Folder/(Folder) B&W VETTE!/VETTE!` | APPL/VETT | 523,089 | the B&W application |
| `VETTE! Folder/(Folder) B&W VETTE!/VETTE!.Data` | DATA/VETT | 577,498 | as the Color copy (same resources) |
| `VETTE! Folder/(Folder) B&W VETTE!/Start VETTE!` | APPL/SVET | 3,393 | launcher |
| `VETTE! Folder/VETTE! MouseStick Sets`, `Read me first` | MSet/MSmv, TEXT/ttxt | | MouseStick presets; 1.02 notes (copy protection) |

## Container formats the readers accept

- **HFS** (Inside Macintosh: Files). The master directory block (MDB) is at volume offset 1024
  (`BD`); allocation blocks start at `drAlBlSt`×512. The catalog and extents overflow files are
  B-trees with 512-byte nodes. The reader walks each tree's leaf chain (from the header node's first
  leaf, with loop and bounds limits) instead of searching it. Catalog leaf records are directories
  (type 1, 70 bytes), files (type 2, 102 bytes: Finder info at +4, file id at +20, fork lengths at
  +26/+36, first extents at +74/+86) and threads (ignored). Forks with more than 3 extents continue in
  the extents file, keyed by (file id, fork 00/FF, fork's first allocation block). The `.toast` here
  is a bare volume (MDB at 1024, 12 KB blocks).
- **Wrappers**: a DiskCopy 4.2 header (84 bytes, `0x0100` at 82, disk image from 84); an Apple
  partition map (`ER` block 0, `PM` entries from block 1, partition type `Apple_HFS`; the start is
  tried in 512-byte units and in the map's block size, for CD images). An HFS+ volume (`H+`/`HX`,
  or embedded in an HFS wrapper) is recognised and reported as unsupported.
- **Loose files**: raw fork dumps (`name.rsrc`, with `name.data` or `name` as the data fork);
  MacBinary I/II/III (128-byte header, CRC-16/XMODEM on II/III, forks padded to 128); AppleSingle
  (0x00051600) and AppleDouble (0x00051607: entries 1 data, 2 resource, 3 name, 9 Finder info) as
  `._name`, `%name`, `.AppleDouble/name` or in a zip's `__MACOSX/` tree; BinHex 4.0 (6-bit text,
  0x90 run-length coding, CRC-16/XMODEM per part); on macOS, `name/..namedfork/rsrc`. A container
  whose data fork is a disk image is mounted. StuffIt, zip and compressed `.dmg` files are reported,
  not read.
- **Resource fork**: header (data offset, map offset, lengths), data (each resource is a 4-byte
  length + bytes), map (type list with count−1, 12-byte refs: id, name offset, attributes, 24-bit
  data offset; Pascal-string names). `GetNamedResource` ignores case. The game relies on this: it asks
  for "engine" and "signature", and the resources are named `Engine` and `Signature`.

## The sound driver `BGAS` 128 (confirmed)

The game doesn't use the Sound Manager. Segment 9 ("sound", MacsBug names `Bogas*`) is glue. Each
call fills a parameter block at A5−$50 (command word at +0, result long at +2), does
`GetResource('BGAS', 128)`, and calls the handle's code. This only happens while A5−$7A20 (sound
available) is set. The driver is position-independent 68000 code (15,066 bytes): MPW C plus
hand-written mixers.

| Glue (jump table A5+) | Cmd | Parameters | Driver |
|---|---|---|---|
| BogasOpen ($FAA) | 0 | — | install: `PHAZ` 128, volume 300, VBL task (`VInstall`) |
| BogasActivate ($FFA) / Deactivate ($FF2) | 1 / 3 | — | VIA port B bit 7 (sound enable) |
| BogasClose ($FA2) | 2 | — | remove the VBL task |
| BogasSet ($FDA) | 8 | +$10 volume | rebuilds the mix tables; the game sets 300 at start (4:00A0) |
| BogasStart ($FE2) | $11 | — | VBL routine = the 3-channel effects mixer (BGAS+1F06) |
| BogasStop ($FEA) | $13 | — | VBL routine = the music mixer (BGAS+2EC0), buffer silenced. The game calls Stop then Start to cut every channel at once |
| BogasLoad ($FBA) | $16 | +$A name (Pascal), +$E 0 | `GetNamedResource('INST', name)`; returns a slot number |
| BogasPlay ($FC2) | $18 | +$12 slot, +$18 step, +$14 duration, +$1C mode | returns the channel |
| BogasKill ($FB2) | $1A | +$12 channel | stop a channel |
| BogasPitch ($FCA) | $1C | +$12 channel, +$18 step | new step, if the channel is playing |
| BogasPurge ($FD2) | $1E | +$12 slot | make the sound purgeable |

**Output.** The driver writes the Mac Plus/SE sound buffer itself (`SoundBase`, low-memory $266: 370
words per frame, one sample per scan line, the high byte of each word, **22254.5 Hz** = 15.6672
MHz/704; ASC-based Macs emulate this buffer). The effects mixer (BGAS+1F4A) is called twice per VBL
with the two `PHAZ` sections (165 + 20 iterations). It writes each mixed byte to two consecutive
buffer words, so **the mix runs at 11127.3 Hz** and a step of 1.0 plays a sample at 11127 Hz.

**Channels (BGAS+1F4A, end checks BGAS+258C).**
- Channel 0: a 16.16 position (starting at $80000000 against `sample + $8000`, so the index can reach
  65535) plus a 16.16 step. This is the pitched channel.
- Channels 1 and 2: a byte pointer advanced by the step *as an integer*. The game passes 1 (11127 Hz)
  or 2 (22254 Hz).
- After each mixer call, each channel's duration counter drops by 1, so durations count **half VBL
  ticks** (1/120.3 s). At zero the channel stops. Then the end or loop check runs:
  - channels 0 and 1 loop when the header's loop start is nonzero: past the loop end, the index goes
    back by (end − start). Otherwise they stop at the header's 16-bit count.
  - channel 2 never loops. It stops at the 24-bit count in header bytes 5–7, which allows the
    117,550-sample opening song.
- Mix: `out = clamp(128 + ((s0 + s1 + s2 − 384) · (volume/3)) >> 7)`. At volume 300 that is each
  channel's offset from 128 × 100/128, summed and clipped to 8 bits. No sound has its own volume.

**Play modes** (BGAS+2C2, the +$1C word). 0, 1 and 2 pick that channel. 4, 5 and 6 pick it only if it
is free. Any other mode takes a free channel; if none is free, 8 steals the oldest, 9 the one with the
least duration left, and 10 gives up. The game only uses 0, 1 and 2.

**Loading** (BGAS+E5A). The driver loads the INST by name and keeps the handle in a 10-byte slot
(+$34 handle, +$38 = 9999 "by name"). An INST starting with `HCOM` is Huffman-decompressed
(BGAS+B86); none of VETTE!'s are. For unlooped sounds (BGAS+1406), the driver appends 1480 bytes of
$80 (4 frames of silence) unless the sound already ends in silence, and rewrites the count. This is
because the mixer only checks for the end between sections.

**Unused in VETTE!**: a 6-voice wavetable mixer (BGAS+282C, self-modified per channel mask and
step) and a 6-track sequencer for `SONG` resources (byte codes ≥ $B0 are commands; a note picks a
step from a 2^(n/12) table at BGAS+1CA6, relative to the instrument's base note). `VETTE!.Data`
has no `SONG`.

## INST format (confirmed)

| Bytes | Meaning |
|---|---|
| 0–1 | loop start (sample index; 0 = no loop) |
| 2–3 | loop end |
| 4 | base note: the note at which the sample plays at step 1.0 (the sequencer's default when 0 is 37). The recording's rate is 11127 Hz × 2^((37 − n)/12): 37 ($25) marks an 11127 Hz recording, 25 ($19) a 22254 Hz one |
| 5 | flags (bit 7 tested by the sequencer; 0 in every VETTE! sound) |
| 6–7 | sample count (= length − 8 here; 0 for the looped `joel`) |
| 8– | unsigned 8-bit samples, $80 = silence |

This settles PLAN.md's open question. Bytes 4–5 are not a rate code but the base note and a flag
byte, and 0–3 are the loop points.

**Four INSTs have no header**: `mic`, `Signature`, `Opening song` and `splash`. They start straight
with samples (bytes 0–7 are near $80, so the flags byte is nonzero). The driver still skips 8 bytes
and reads those bytes as a header. For `Signature`, `Opening song` and `mic` the "loop" has start =
end, which never moves the index. `splash`'s would start at sample 32894, after the game's 3 s
limit. So all four play once, from byte 8, until the game's duration runs out (`mic` and
`Signature` 3–4 s, the song 10.97 s). `decode_inst` marks them `header_valid = false`, with no loop
and 11127 Hz.

## The sounds and how the game uses them

The game loads each sound once by name (`Load_Sounds`, 2:0746; names in the A5 data at A5−$5B06 to
−$5A92; slots stored at A5−$5A88 to −$5A50). `joel` and `splash` are only loaded when at least 1.5
MB is free (A5−$5950, set at 2:088C). Every play first checks the game's sound switch (A5−$3780).
Rates are 11127 Hz × the step. Durations are the Play duration in half ticks.

| INST (id) | Samples | Base note, loop | Played when | Ch, rate, limit |
|---|---|---|---|---|
| `Engine` (11584) | 6,053 | 25, loop 370–5682 | race: started at the race start or on returning from the helicopter view (1:2F72), pitched every frame by `Calc_RPM` (6:375E, below); toggled by a key (1:315A). Car-select screen: four hot spots pick a car, a fifth revs the chosen one while its picture animates; `Kill(0)` at the end (2:0DD4–0F96). Intro at 10 s (8:0564) | 0; race 4584–14432 Hz, held; garage and intro 11127 Hz, 15 s / 3 s |
| `heli` (17804) | 12,128 | 25, loop 224–11808 | the helicopter view (key 4, A5−$2516) replaces the engine (1:2F56); the other views bring the engine back | 0, 22254 Hz, held |
| `horn` (3523) | 5,606 | 25, loop 1428–4989 | horn key down (1:30E4); `Kill(1)` when it is released (1:2E2C) | 1, 22254 Hz, held |
| `police` (27989) | 6,920 | 37, loop 2384–6344 | siren: a chasing police car within 2048 units (6:0EB0). Killed when it falls back or pulls you over | 1, 11127 Hz, held |
| `joel` (17399) | 31,173 | 37, loop 8593–29693 | pulled over by the police (6:0F68): a male voice (pitch 105–170 Hz) from 0.8 s | 1, 11127 Hz, 2.5 s |
| `skid` (2052) | 17,488 | 37 | sliding: \|slip\| ≥ 250 at a speed over the grip limit, at most once per 2 s (6:3922); also 0.5 s with a hard landing (6:438E) | 1, 11127 Hz, 2 s |
| `crash` (2585) | 17,464 | 37 | a car corner inside one of the cell's solid boxes (building, wall) at speed ≥ 15, at most once per 2 s (6:4720, 2 s). The Vette hits another vehicle (traffic, police, the opponent; tags `VETT`/`OPPO` at car +$54): the cars bounce apart and the speed halves (6:02A0, 1 s). Wrecked: a hit over speed 160–200 (by difficulty), or one damage too many, ends the race after every channel is cut (6:4DA6, 1 s) | 2, 11127 Hz |
| `kill` (28441) | 7,291 | 37 | ran into a pedestrian, who is knocked over (pitch 90) and turned away (1:3F5A) | 1, 11127 Hz, 1 s |
| `thud` (56) | 2,704 | 37 | the car steps 2 units up or down onto a raised edge, kerb or sidewalk (6:415A); hard landing after a jump (vertical speed ≤ −8, 6:4370, with a 0.5 s skid) | 2, 11127 Hz, 1 s |
| `cable car bell` (19354) | 9,826 | 37 | passing the cable car, once per race (6:56B2, 3 s); intro at 3 s (8:0496, channel 1, 2 s) | 2, 11127 Hz |
| `beep1` (24157) | 8,400 | 25 | race start countdown, 1st and 2nd beep, 140 ticks (2.3 s) apart (1:26EC); 463 Hz as played | 2, 11127 Hz, 1.25 s |
| `beep2` (22909) | 4,608 | 25 | countdown, 3rd beep; 618 Hz as played | 2, 11127 Hz, 1.25 s |
| `splash` (5403) | 36,561 | no header | drove into the bay (water cell, 6:5B02), after cutting every channel | 0, 11127 Hz, 3 s |
| `Opening song` (1425) | 117,550 | no header | intro start (8:02F2): a 10.56 s recorded tune, noise-like for its first 0.9 s, then a band | 2, 11127 Hz, 10.97 s |
| `mic` (28215) | 44,591 | no header | intro, when the driving car passes x = 334: `Kill(2)` (the song, if still playing), then this (8:0858). Bursts, then a held ~335 Hz tone from 1.7 to 3 s | 2, 11127 Hz, 3 s |
| `Signature` (12083) | 44,920 | no header | intro at 16 s (8:09A0): short chimes, then an engine-like 70–90 Hz rumble from 1.3 s | 0, 11127 Hz, 4 s |

Two of the base-25 (22 kHz) recordings play an octave low, because the game gives them step 1:
`beep1` and `beep2`. `heli` and `horn` get step 2, their own speed. `MacSound::rate` is the rate
the game uses, and `native_rate` comes from the base note. Pitches and content descriptions come
from an autocorrelation pass over the decoded samples, not from listening. `vette_mac sounds --wav`
writes them out for that.

**Intro** (segment 8, 8:01C2). There are four scenes at 0, 3, 10 and 16 s (ticks +0, +180, +600,
+960), and each runs its sound once. A click (`Button`) ends the intro: Kill 0–2, Stop/Start, purge
`Opening song`, `Signature` and `mic` (8:027A).

**Engine pitch** (`Calc_RPM`, 6:3726). `rpm100` = the player car's revs/100 (car +$44, compared with
the shift tables at A5−$33A6/−$3396):

    step = rpm100 > 12 ? min(15000 + 1000·rpm100, 85000) : 27000      (16.16; BogasPitch(0, step))

That gives 0.412 (4584 Hz) up to 1200 rpm, 0.427 at 1300 rpm, and 1.297 (14432 Hz) from 7000 rpm.
It is linear in the revs, with no slide; the car model doesn't matter. `mac_engine_rate(rpm)` is the
same formula, continuous.

## Mapping to the DOS sounds (`game/sound_events.h`)

| DOS `Sfx` | Mac sound (`mac_sound_for_dos`) | Notes |
|---|---|---|
| `engine` | `Engine`, looped, rate = `mac_engine_rate(rpm)` | DOS slides a square-wave pitch toward the revs; the Mac jumps there each frame |
| `garage_rev` | `Engine` at 11127 Hz, until the animation ends (at most 15 s) | the same moment: DOS revs on Space and stops when the exhaust animation is done |
| `skid` | `skid` | |
| `siren` | `police`, looped | |
| `title_tune` | `Opening song` | a recorded band (bass line around 70–140 Hz) in place of the speaker melody. Whether it's the same tune hasn't been checked by ear |
| `crash` | `crash` (the 2 s building/wall use) | |
| `crash_car` | `crash` (the 1 s vehicle use) | |
| `crash_rail` | `crash` (as `crash`) | the Mac treats rails as walls |
| `hit_pedestrian` | `kill` | |
| `win_tune` | — | **the Mac has no results fanfare** |
| `gear_grind` | — | **the Mac has no missed-shift sound** |

The Mac's other sounds play at moments DOS passes in silence. The DOS observer reports those moments too
(notes 07 section 9), so they map as well:

| DOS `Sfx` (silent in DOS) | Mac sound | The DOS moment |
|---|---|---|
| `horn` | `horn`, looped | the X key held in a race (DOS has no horn; X does nothing there) |
| `helicopter` | `heli`, looped; replaces the engine | the helicopter view (F4, keypad +) |
| `countdown_beep` | `beep1` | start lights: "buckle up", then "get ready" 2 s later |
| `countdown_go` | `beep2` | start lights: "go", 1 s after |
| `splash` | `splash` | drove into the bay (the water box) |
| `thud` | `thud` (the kerb use) | the road under the car turns upward (bottom of an uphill, end of a downhill) at speed, or its height steps |
| `pulled_over` | `joel` | the police stop you |
| `intro_cable_car` | `cable car bell` (the intro's use: channel 1, 2 s) | title: the cable car rolls in |
| `intro_car` | `mic` | title: the Corvette comes at the viewer |
| `intro_logo` | `Signature` | title: the VETTE! logo drops in |
| `service_station` | `cable car bell` (the race's use) | onto a service station's driveway, where stopping repairs the car |

Still without a DOS moment: the landing `thud` (DOS cars never leave the ground), the wreck `crash`, and the
intro's `Engine`.

**The cable car bell in races rings at service stations** (**confirmed**). Its handler, 6:5688, is one of
the Mac's box handlers:

- **Dispatch** (6:3FEE): the box test (6:4064) finds the car's cell in the map (A5−$250C, 52 cells a row;
  the car's +$6E and +$72 >> 11 are the column and row), its type's QUAD record (A5−$4DDA) and the record's
  collision class, whose box list (A5−$424E[class]) holds `{w row-axis min, w column-axis min, w max, w max}`
  boxes, FFFF-terminated. A box that holds the car is looked up in a 44-entry pointer table at A5−$30C0;
  its index picks the handler from A5−$300C.
- **Bell box**: box 3, the third box of class 26. Boxes 28 and 29 (classes 89 and 90) share the handler but
  are on no cell of the main map.
- **Each entry**: each frame 1:2746 clears an "in the box" flag (A5−$33E8). The handler sets it and rings
  the bell unless it already rang for this visit (A5−$33EA), which 1:295C clears once a frame passes outside.
- **The repair**: then, if the car has stopped, the handler repairs it, like DOS's box C358h (notes 07
  section 9.3).
- **Where**: class 26 belongs to Mac cell type 3, the service station (the same cell objects as types 151 and
  139). It is on 13 Main_Map cells, (column, row) (49, 5), (43, 6), (17, 7), (13, 12), (16, 15), (6, 17),
  (40, 20), (28, 26), (44, 26), (34, 28), (36, 33), (28, 35) and (34, 37), and type 235 at (50, 14).
  DOS has 12 (cell type 14).

**The A5 globals** these tables live in are built at launch by CODE 10's initializer (10:0004-0118). It
reads a block header `{l size, l base (0: below A5), w records, w unused}` at 10:011A, zero-fills, then
replays 2228 records: a flag byte (low nibble plus bit 4/bit 11 extension bytes give a byte count; bit 7
adds an unused repeat count), a 15- or 23-bit offset, then the bytes to copy (bit 5 clear) or a value of
that many bytes to add (bit 5 set). Bit 6 adds A5 to the long at the offset. The table at A5−$30C0 is then
44 pointers to boxes and −1; at A5−$300C, 44 jump table addresses.

The countdown answers a question in notes 07. DOS 1.1 has two unused beeps: `snd_beep_lo` (659.6 Hz) and
`snd_beep_hi` (880.6 Hz), and its countdown is silent. The Mac plays beep1, beep1, beep2, which is the
same rising fourth (463 → 618 Hz as played, ratio 1.335). That supports the guess that DOS cut a
countdown: low, low, high, 2.3 s apart on the Mac.

DOS has one speaker voice chosen by priority (skid > siren > engine). The Mac mixes three channels:
engine/heli/splash on 0; horn, siren, skid, pedestrian and voice on 1; crashes, thuds, bell and beeps
on 2. A new sound on a busy channel replaces the one there (horn and siren share channel 1).

## Open questions

- The content of `mic` and `Signature` has not been identified by listening (see the table).
- `joel`'s words. Its loop end (29693, 2.67 s in) lies past the game's 2.5 s limit, so it never loops.
- The exact cell types that trigger `thud` (the list at A5−$3116) and `splash`. These are needed only to
  mirror the Mac's events in places where DOS has none.
- Which map the Mac's service station cell types 151 and 139 (bell boxes 28 and 29) are used on.
- The horn key: the handler at 1:30C0 tests a GetKeys bit (released at 1:2E0C, byte 0 bit 6 of the KeyMap
  copy at A5−$39B6); which key that is depends on the KeyMap's bit order, not settled.
- Whether the B&W application uses other durations anywhere. Its call sites match in number and
  shape, but only Color VETTE!'s parameters were read.
