# VETTE! Reverse Engineering Notes

## Scope
- Primary target: DOS English build (`Vette_DOS_EN`).
- Secondary references:
  - PC-98 Japanese build (`Vette_PC-98_JA/VETTE.hdi`)
  - Mac English build (`Vette_Mac_EN/VETTE_1_02.toast`)

## High-Level Findings
- The DOS build is a packed MZ executable plus external sidecar assets (`*.BIN`).
- The PC-98 build stores mostly uncompressed sidecar files (`*.PIC`, `*.DAT`) that closely mirror DOS content.
- The Mac build is resource-fork heavy:
  - app code/resources in `VETTE!.rsrc` / `Color VETTE!.rsrc`
  - gameplay data in `VETTE!.Data.rsrc`
- Core game systems are strongly data-driven (courses, object sets, map data, car profiles, traffic tables, landmark text, quiz content).

## Version-Specific Structure

### DOS
- Main executable: `VETTE.EXE` (packed, no relocation table, entry point near file end).
- Sidecar assets include title/garage/map/highscore/winner screens, horizons, crash/loser/ticket screens, and config/score tables.
- Many `.BIN` files use a PCX-style RLE stream (`0xC0..0xFF` run markers).
- `HORIZON0/1/2.BIN` are raw planar files (not RLE).
- Garage quiz text in `VETTE.EXE` is obfuscated with `+0x14` decode on byte range `0x20..0x6A`.

### PC-98
- HDI contains a FAT-like filesystem with a `VETTE` directory and 32 game files.
- Asset names and sizes are explicit in directory entries (`*.PIC`, `ANIM.DAT`, `CONFIG.DAT`, `SCORE.DAT`).
- The extracted files provide strong ground truth for DOS sidecar decoding.

### Mac
- Toast image is HFS and can be parsed with `machfs`.
- Data resource file (`VETTE!.Data.rsrc`) includes typed resources:
  - `MAPS`, `CLST` (course paths), `OBJS` (3D object sets), `PERF` (vehicle performance blocks),
  - `FWTM/FWTP/JHPF` (traffic movement/placement tables),
  - `INST` (audio assets), `STRT` (street names), trig/curve tables.
- App resource forks contain UI (`MENU`, `DLOG`, `DITL`, `WIND`, `PICT`) and executable `CODE` resources.

## Confirmed Asset Encodings

### Planar image format (16-color)
- 4 bitplanes, byte-addressed horizontal pixels (8 pixels/byte).
- Known decodes:
  - `64000` bytes -> `640x200`
  - `38400` bytes -> `640x120`
  - `22528` bytes -> `352x128` (CRASH/LOSER family, PC-98)
  - DOS `EGAPIC.BIN` RLE-decodes to `32002`, where payload at `+2` decodes to `320x200`.

### Sprite block format (BIGVET/REDVETTE/SPETRUM/ANIM)
- Header:
  - `u16 width_bytes` (little-endian)
  - `u16 height`
- Payload:
  - `width_bytes * height * 5` bytes
  - interpreted as `mask + 4 color planes`
- Total block length:
  - `4 + width_bytes * height * 5`
- `ANIM.DAT` (PC-98) is a concatenation of these blocks (15 blocks in this build).

### RLE sidecar compression (DOS)
- PCX-like byte stream:
  - If byte `>= 0xC0`, run length is `byte & 0x3F`, next byte is repeated value.
  - Otherwise byte is literal.

## Cross-Version Parity (Key Results)
- Exact DOS->PC-98 parity after DOS RLE decode:
  - `TITLE`, `HIGHSC`, `WINNER`, `MAPPIC`, `PENALTY`, `VX`, `SCORE`
- Exact raw parity:
  - `HORIZON0/1/2`, `SCORE`
- Partial/variant differences:
  - `GARAGE` (~3% byte delta; likely localized/variant overlays)
  - `CRASH/LOSER/TICKET/EGAPIC/EGASKILL` require format-specific handling (size/layout differences)

## Repro Scripts
- Extract PC-98 files from HDI:
  - `py -3 tools/reverse_engineering/extract_pc98_hdi.py`
- Decode DOS/PC-98 assets + dump quiz text:
  - `py -3 tools/reverse_engineering/decode_dos_assets.py`
- Inventory Mac resources and extract forks:
  - `py -3 tools/reverse_engineering/inventory_mac_resources.py`
- Python deps used by the tooling:
  - `py -3 -m pip install pillow machfs macresources`

## Open Items
- Unpack/decompile DOS `VETTE.EXE` for full gameplay loop and physics logic parity.
- Fully decode remaining mixed-format UI/gauge assets (`TICKET`, `SPEED`, `TACHS`, etc.).
- Decode Mac `PERF`, `MAPS`, `CLST` into clean JSON schemas for direct modern-engine import.
