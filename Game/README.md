# Game files go here

VETTE! 2026 doesn't include any of the original game's files. You need your own copy of
**VETTE! for DOS** (Spectrum HoloByte, 1989). Copy its files into this folder.

## Required: DOS version

Copy everything from your DOS VETTE! directory into `Game/`:

```
Game/
  VETTE.EXE
  BIGVET.BIN    CONFIG.BIN    CRASH0.BIN    CRASH1.BIN    EGAPIC.BIN
  EGASKILL.BIN  GARAGE.BIN    HIGHSC.BIN    HORIZON0.BIN  HORIZON1.BIN
  HORIZON2.BIN  LOSER0.BIN    LOSER1.BIN    LOSER2.BIN    LOSER3.BIN
  MAPPIC.BIN    PENALTY.BIN   REDVETTE.BIN  SCORE.BIN     SPETRUM.BIN
  TICKET.BIN    TITLE.BIN     VX.BIN        WINNER.BIN
```

File name case doesn't matter.

## Optional: the other versions, for Enhanced mode

The launch menu's Sound and Graphics options can use the other releases of VETTE!, if you own them.
Put them in subfolders here:

- `Game/Mac/`: the Macintosh version (1.02), for its digitized sounds and its colour art.
  Any of these works:
  - the disk image (e.g. `VETTE_1_02.toast`; HFS, also `.dsk`, `.img`, `.hfv`, DiskCopy 4.2 or
    partitioned images);
  - the files themselves, with their resource forks: MacBinary (`.bin`), AppleSingle, AppleDouble
    (`._name` files or a `__MACOSX` folder from a zip), BinHex (`.hqx`), or raw forks saved as
    `name.rsrc`. On a Mac, plain copies of the files work too.
- `Game/PC98/`: the PC-98 version (1.02J), for its YM2203 FM music and its art. Its hard disk image
  (`.hdi`, also FDI, NHD, THD, D88, NFD or raw images) or the files themselves.

Classic mode only needs the DOS files.
