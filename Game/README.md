# Game files go here

VETTE! 2026 doesn't include any of the original game's files. You need your own copy of
**VETTE! for DOS** (Spectrum HoloByte, 1989). Optionally, the **PC-98** and **Macintosh** versions add
their music, sounds and art.

Give each version a folder of its own in here. The names don't matter: VETTE! 2026 looks through this
folder (and up to three levels of subfolders) when it starts and works out which version is where from
the files themselves. The launch menu shows what it found.

```
Game/
  DOS/          your DOS VETTE! files (required)
  PC-98/        the PC-98 version: its files, or its disk image (optional)
  Mac/          the Macintosh version: its disk image, or its files (optional)
```

The DOS files can also go straight into `Game/`, as in earlier versions.

## Required: the DOS version

Every file of your DOS VETTE! directory, in one folder:

```
VETTE.EXE
BIGVET.BIN    CONFIG.BIN    CRASH0.BIN    CRASH1.BIN    EGAPIC.BIN
EGASKILL.BIN  GARAGE.BIN    HIGHSC.BIN    HORIZON0.BIN  HORIZON1.BIN
HORIZON2.BIN  LOSER0.BIN    LOSER1.BIN    LOSER2.BIN    LOSER3.BIN
MAPPIC.BIN    PENALTY.BIN   REDVETTE.BIN  SCORE.BIN     SPETRUM.BIN
TICKET.BIN    TITLE.BIN     VX.BIN        WINNER.BIN
```

File name case doesn't matter.

## Optional: the PC-98 version (1.02J)

For its YM2203 FM music and its art. Either its files (`VETTE.EXE` with its `.PIC` pictures) or its
hard disk or floppy image: `.hdi`, FDI, NHD, THD, D88, NFD or a raw image.

## Optional: the Macintosh version (1.02)

For its digitized sounds and its colour art. Any of these works:

- the disk image (e.g. `VETTE_1_02.toast`; HFS, also `.dsk`, `.img`, `.hfv`, DiskCopy 4.2 or
  partitioned images);
- the files themselves, with their resource forks: MacBinary (`.bin`), AppleSingle, AppleDouble
  (`._name` files or a `__MACOSX` folder from a zip), BinHex (`.hqx`), or raw forks saved as
  `name.rsrc`. On a Mac, plain copies of the files work too.

Classic mode only needs the DOS files.
