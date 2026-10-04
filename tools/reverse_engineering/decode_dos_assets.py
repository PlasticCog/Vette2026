#!/usr/bin/env python3
"""
Decode and inspect VETTE! DOS/PC-98 assets.

Capabilities:
- Decode DOS PCX-like RLE sidecar files (`*.BIN`)
- Convert known planar 16-color images to PNG
- Parse 5-plane sprite blocks used by BIGVET/REDVETTE/SPETRUM/ANIM
- Compare DOS assets to extracted PC-98 assets
- Decode obfuscated DOS garage trivia strings from VETTE.EXE
"""

from __future__ import annotations

import argparse
from pathlib import Path

try:
    from PIL import Image
except ImportError as exc:  # pragma: no cover
    raise SystemExit("Pillow is required. Install with: py -3 -m pip install pillow") from exc


EGA_PALETTE = [
    (0, 0, 0),
    (0, 0, 170),
    (0, 170, 0),
    (0, 170, 170),
    (170, 0, 0),
    (170, 0, 170),
    (170, 85, 0),
    (170, 170, 170),
    (85, 85, 85),
    (85, 85, 255),
    (85, 255, 85),
    (85, 255, 255),
    (255, 85, 85),
    (255, 85, 255),
    (255, 255, 85),
    (255, 255, 255),
]


RAW_PLANAR_DOS = {
    "HORIZON0.BIN": (640, 120),
    "HORIZON1.BIN": (640, 120),
    "HORIZON2.BIN": (640, 120),
}


DOS_PC98_COMPARE = [
    ("TITLE.BIN", "TITLE.PIC"),
    ("GARAGE.BIN", "GARAGE.PIC"),
    ("HIGHSC.BIN", "HIGHSC.PIC"),
    ("WINNER.BIN", "WINNER.PIC"),
    ("MAPPIC.BIN", "MAPPIC.PIC"),
    ("HORIZON0.BIN", "HORIZON0.PIC"),
    ("HORIZON1.BIN", "HORIZON1.PIC"),
    ("HORIZON2.BIN", "HORIZON2.PIC"),
    ("EGAPIC.BIN", "EGAPIC.PIC"),
    ("EGASKILL.BIN", "EGASKILL.PIC"),
    ("PENALTY.BIN", "PENALTY.PIC"),
    ("TICKET.BIN", "TICKET.PIC"),
    ("VX.BIN", "VX.PIC"),
    ("SCORE.BIN", "SCORE.DAT"),
    ("CRASH0.BIN", "CRASH0.PIC"),
    ("CRASH1.BIN", "CRASH1.PIC"),
    ("LOSER0.BIN", "LOSER0.PIC"),
    ("LOSER1.BIN", "LOSER1.PIC"),
    ("LOSER2.BIN", "LOSER2.PIC"),
    ("LOSER3.BIN", "LOSER3.PIC"),
]


def pcx_like_rle_decode(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        b = data[i]
        i += 1
        if b >= 0xC0 and i < n:
            out.extend([data[i]] * (b & 0x3F))
            i += 1
        else:
            out.append(b)
    return bytes(out)


def palette_image_from_indexes(indexes: bytes, width: int, height: int) -> Image.Image:
    image = Image.frombytes("P", (width, height), indexes)
    pal = []
    for r, g, b in EGA_PALETTE:
        pal.extend([r, g, b])
    pal.extend([0, 0, 0] * (256 - len(EGA_PALETTE)))
    image.putpalette(pal)
    return image


def decode_planar_4bpp(data: bytes, width: int, height: int) -> Image.Image:
    if width % 8 != 0:
        raise ValueError("width must be divisible by 8 for planar decode")
    plane_size = width * height // 8
    if len(data) < plane_size * 4:
        raise ValueError(f"planar payload too small ({len(data)} bytes)")

    planes = [data[i * plane_size : (i + 1) * plane_size] for i in range(4)]
    out = bytearray(width * height)
    row_bytes = width // 8

    for y in range(height):
        base = y * row_bytes
        for xb in range(row_bytes):
            i = base + xb
            p0, p1, p2, p3 = planes[0][i], planes[1][i], planes[2][i], planes[3][i]
            for bit in range(8):
                mask = 1 << (7 - bit)
                color = (
                    (1 if p0 & mask else 0)
                    | ((1 if p1 & mask else 0) << 1)
                    | ((1 if p2 & mask else 0) << 2)
                    | ((1 if p3 & mask else 0) << 3)
                )
                out[y * width + xb * 8 + bit] = color

    return palette_image_from_indexes(bytes(out), width, height)


def parse_sprite_blocks(data: bytes) -> list[tuple[int, int, int, bytes]]:
    """Parse concatenated sprite blocks: [w_bytes,u16][h,u16][mask+p0+p1+p2+p3]."""
    blocks: list[tuple[int, int, int, bytes]] = []
    off = 0
    while off + 4 <= len(data):
        w_bytes = int.from_bytes(data[off : off + 2], "little")
        height = int.from_bytes(data[off + 2 : off + 4], "little")
        if w_bytes <= 0 or height <= 0 or w_bytes > 512 or height > 512:
            break
        payload_size = w_bytes * height * 5
        total = 4 + payload_size
        if off + total > len(data):
            break
        blocks.append((off, w_bytes, height, data[off + 4 : off + total]))
        off += total
    return blocks


def decode_sprite_block(block: tuple[int, int, int, bytes]) -> Image.Image:
    _, w_bytes, height, payload = block
    width = w_bytes * 8
    plane_size = w_bytes * height
    mask = payload[0:plane_size]
    planes = [payload[(i + 1) * plane_size : (i + 2) * plane_size] for i in range(4)]

    rgba = bytearray(width * height * 4)
    for y in range(height):
        row = y * w_bytes
        for xb in range(w_bytes):
            i = row + xb
            m, p0, p1, p2, p3 = mask[i], planes[0][i], planes[1][i], planes[2][i], planes[3][i]
            for bit in range(8):
                bitmask = 1 << (7 - bit)
                color_idx = (
                    (1 if p0 & bitmask else 0)
                    | ((1 if p1 & bitmask else 0) << 1)
                    | ((1 if p2 & bitmask else 0) << 2)
                    | ((1 if p3 & bitmask else 0) << 3)
                )
                r, g, b = EGA_PALETTE[color_idx]
                alpha = 0 if (m & bitmask) else 255
                px = (y * width + xb * 8 + bit) * 4
                rgba[px : px + 4] = bytes([r, g, b, alpha])

    return Image.frombytes("RGBA", (width, height), bytes(rgba))


def decode_dos_quiz_blob(exe_bytes: bytes) -> str:
    # Observed trivia block in this executable build.
    start = 0xFD00
    end = min(0x10B00, len(exe_bytes))
    blob = exe_bytes[start:end]
    decoded = bytearray()
    for byte in blob:
        if 0x20 <= byte <= 0x6A:
            decoded.append(byte + 0x14)
        else:
            decoded.append(byte)
    # Keep printable content and normalize control chars.
    text = []
    for byte in decoded:
        if byte in (0x0A, 0x0D):
            text.append("\n")
        elif 32 <= byte < 127:
            text.append(chr(byte))
        else:
            text.append("\n")
    joined = "".join(text)
    # Collapse very long control runs.
    while "\n\n\n" in joined:
        joined = joined.replace("\n\n\n", "\n\n")
    return joined.strip() + "\n"


def write_image(image: Image.Image, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path)


def reconstruct_horizon(image: Image.Image) -> Image.Image:
    """
    Horizon files contain sparse scanline dithering intended for CRT blending.
    Reconstruct a stable skyline silhouette for modern displays.
    """
    if image.mode != "P":
        image = image.convert("P")

    width, height = image.size
    src = list(image.tobytes())
    sky_index = 11
    water_start = max(1, int(height * 0.68))

    # Expand non-sky structures vertically so skyline/buildings are continuous.
    mask = [1 if value != sky_index else 0 for value in src]
    grown = [0] * (width * height)
    for y in range(height):
        for x in range(width):
            keep = 0
            for dy in (-2, -1, 0, 1, 2):
                yy = y + dy
                if 0 <= yy < height and mask[yy * width + x]:
                    keep = 1
                    break
            grown[y * width + x] = keep

    out = src[:]
    valid_colors = {0, 2, 3, 4, 7, 8, 10, 12, 15}

    for y in range(height):
        for x in range(width):
            i = y * width + x
            if not grown[i]:
                out[i] = 9 if y >= water_start else sky_index
                continue

            window = []
            for dy in (-2, -1, 0, 1, 2):
                yy = y + dy
                if 0 <= yy < height:
                    value = src[yy * width + x]
                    if value != sky_index:
                        window.append(value)

            if not window:
                out[i] = 9 if y >= water_start else sky_index
                continue

            # Most common color in vertical window.
            value = max(set(window), key=window.count)
            if y >= water_start and value in {0, 7, 8, 15}:
                out[i] = 9
            elif value in valid_colors:
                out[i] = value
            else:
                out[i] = 8 if (x + y) % 2 else 0

    rebuilt = Image.frombytes("P", (width, height), bytes(out))
    rebuilt.putpalette(image.getpalette())
    return rebuilt


def process_dos_assets(dos_dir: Path, output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)

    for bin_file in sorted(dos_dir.glob("*.BIN")):
        name = bin_file.name.upper()
        raw = bin_file.read_bytes()

        if name in RAW_PLANAR_DOS:
            w, h = RAW_PLANAR_DOS[name]
            img = decode_planar_4bpp(raw, w, h)
            img = reconstruct_horizon(img)
            write_image(img, output_dir / f"{bin_file.stem.lower()}_{w}x{h}.png")
            continue

        # Sprite container files (BIGVET/REDVETTE/SPETRUM) are not RLE.
        sprite_blocks = parse_sprite_blocks(raw)
        if sprite_blocks and sprite_blocks[0][0] == 0:
            for idx, block in enumerate(sprite_blocks):
                _, w_bytes, h, _ = block
                sprite = decode_sprite_block(block)
                suffix = f"_sprite{idx:02d}_{w_bytes * 8}x{h}.png"
                write_image(sprite, output_dir / f"{bin_file.stem.lower()}{suffix}")
            # Single-block sprite files do not need further decode.
            if len(sprite_blocks) == 1:
                continue

        decoded = pcx_like_rle_decode(raw)

        if len(decoded) == 64000:
            img = decode_planar_4bpp(decoded, 640, 200)
            write_image(img, output_dir / f"{bin_file.stem.lower()}_640x200.png")
        elif len(decoded) == 32002:
            img = decode_planar_4bpp(decoded[2:], 320, 200)
            write_image(img, output_dir / f"{bin_file.stem.lower()}_320x200.png")
        elif name.startswith(("CRASH", "LOSER")) and len(decoded) == 11264:
            # DOS stores these at half the PC-98 size; leave as raw dump for now.
            (output_dir / f"{bin_file.stem.lower()}_decoded_11264.bin").write_bytes(decoded)


def process_pc98_assets(pc98_dir: Path, output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    for pic in sorted(pc98_dir.glob("*.PIC")):
        data = pic.read_bytes()
        if len(data) == 64000:
            img = decode_planar_4bpp(data, 640, 200)
            write_image(img, output_dir / f"{pic.stem.lower()}_640x200.png")
        elif len(data) == 38400:
            img = decode_planar_4bpp(data, 640, 120)
            img = reconstruct_horizon(img)
            write_image(img, output_dir / f"{pic.stem.lower()}_640x120.png")
        elif len(data) == 22528:
            # CRASH/LOSER assets decode cleanly to this resolution.
            img = decode_planar_4bpp(data, 352, 128)
            write_image(img, output_dir / f"{pic.stem.lower()}_352x128.png")

    anim = pc98_dir / "ANIM.DAT"
    if anim.exists():
        blocks = parse_sprite_blocks(anim.read_bytes())
        for idx, block in enumerate(blocks):
            _, w_bytes, h, _ = block
            sprite = decode_sprite_block(block)
            write_image(sprite, output_dir / f"anim_sprite{idx:02d}_{w_bytes * 8}x{h}.png")


def compare_dos_pc98(dos_dir: Path, pc98_dir: Path) -> None:
    print("\nDOS vs PC-98 asset compare")
    print("dos_file -> pc98_file | dos_size | rle_size | pc98_size | raw_match | rle_match")
    for dos_name, pc_name in DOS_PC98_COMPARE:
        dos_path = dos_dir / dos_name
        pc_path = pc98_dir / pc_name
        if not dos_path.exists() or not pc_path.exists():
            continue
        dos_raw = dos_path.read_bytes()
        dos_rle = pcx_like_rle_decode(dos_raw)
        pc_raw = pc_path.read_bytes()
        print(
            f"{dos_name:11} -> {pc_name:11} | "
            f"{len(dos_raw):7d} | {len(dos_rle):7d} | {len(pc_raw):8d} | "
            f"{str(dos_raw == pc_raw):8} | {str(dos_rle == pc_raw):8}"
        )


def main() -> None:
    parser = argparse.ArgumentParser(description="Decode VETTE DOS/PC-98 assets.")
    parser.add_argument("--dos-dir", type=Path, default=Path("Vette_DOS_EN"), help="DOS asset directory.")
    parser.add_argument(
        "--pc98-dir",
        type=Path,
        default=Path("Vette_PC-98_JA") / "extracted",
        help="PC-98 extracted files directory.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("analysis") / "decoded_assets",
        help="Output directory for PNG/debug files.",
    )
    parser.add_argument(
        "--skip-pc98",
        action="store_true",
        help="Skip PC-98 decode/compare stages.",
    )
    args = parser.parse_args()

    dos_out = args.output / "dos"
    pc98_out = args.output / "pc98"

    process_dos_assets(args.dos_dir, dos_out)
    print(f"DOS decode outputs written to {dos_out}")

    exe_path = args.dos_dir / "VETTE.EXE"
    if exe_path.exists():
        quiz_text = decode_dos_quiz_blob(exe_path.read_bytes())
        quiz_path = args.output / "dos_garage_quiz_decoded.txt"
        quiz_path.parent.mkdir(parents=True, exist_ok=True)
        quiz_path.write_text(quiz_text, encoding="utf-8")
        print(f"Decoded trivia text written to {quiz_path}")

    if not args.skip_pc98 and args.pc98_dir.exists():
        process_pc98_assets(args.pc98_dir, pc98_out)
        print(f"PC-98 decode outputs written to {pc98_out}")
        compare_dos_pc98(args.dos_dir, args.pc98_dir)


if __name__ == "__main__":
    main()
