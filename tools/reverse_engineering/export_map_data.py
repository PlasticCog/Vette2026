#!/usr/bin/env python3
"""
Export VETTE! map-related resources from Mac VETTE!.Data resource fork.

This provides:
- Raw binary dumps for map/course/traffic resources
- JSON summaries for resource metadata
- Decoded street-name table (STRT)
- Heuristic CLST path dumps as signed 32-bit triplets
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

try:
    import macresources
except ImportError as exc:  # pragma: no cover
    raise SystemExit("macresources is required: py -3 -m pip install macresources") from exc


TARGET_TYPES = {
    "MAPS",
    "CLST",
    "STRT",
    "QUAD",
    "CURV",
    "FREE",
    "FWTM",
    "FWTP",
    "JHPF",
    "TURN",
    "COLL",
    "OBJS",
}


def safe_name(value: str) -> str:
    return "".join(ch if ch.isalnum() or ch in ("-", "_") else "_" for ch in value)


def decode_street_names(data: bytes) -> list[str]:
    """
    STRT appears to be:
    - u16 count (big-endian)
    - Pascal strings (u8 length + bytes, Mac Roman), padded with spaces in content.
    """
    if len(data) < 2:
        return []
    count = struct.unpack_from(">H", data, 0)[0]
    out: list[str] = []
    pos = 2
    for _ in range(count):
        if pos >= len(data):
            break
        ln = data[pos]
        pos += 1
        if pos + ln > len(data):
            break
        s = data[pos : pos + ln].decode("mac_roman", errors="replace").strip()
        pos += ln
        out.append(s)
    return out


def parse_clst_triplets(data: bytes) -> dict:
    """
    Heuristic parser:
    - interpret resource payload as big-endian signed 32-bit values
    - expose both flat values and grouped triplets for downstream analysis
    """
    vals = []
    for i in range(0, len(data) - (len(data) % 4), 4):
        vals.append(struct.unpack_from(">i", data, i)[0])

    triplets = []
    for i in range(0, len(vals) - (len(vals) % 3), 3):
        triplets.append({"a": vals[i], "b": vals[i + 1], "c": vals[i + 2]})

    return {
        "int32_count": len(vals),
        "head_int32": vals[:48],
        "triplet_count": len(triplets),
        "triplets_head": triplets[:64],
    }


def parse_maps_table(data: bytes) -> dict:
    """
    Export MAPS payload as unsigned short table for format reverse engineering.
    """
    words = []
    for i in range(0, len(data) - (len(data) % 2), 2):
        words.append(struct.unpack_from(">H", data, i)[0])
    return {
        "u16_count": len(words),
        "head_u16": words[:128],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Export map data from Mac VETTE!.Data resource fork.")
    parser.add_argument(
        "--input",
        type=Path,
        default=Path("Vette_Mac_EN")
        / "extracted"
        / "VETTE! Folder"
        / "(Folder) B&W VETTE!"
        / "VETTE!.Data.rsrc",
        help="Path to VETTE!.Data.rsrc",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("analysis") / "map_data_export",
        help="Output directory for dumps and JSON",
    )
    args = parser.parse_args()

    res_fork = args.input.read_bytes()
    resources = list(macresources.parse_file(res_fork) or [])

    args.output.mkdir(parents=True, exist_ok=True)
    raw_dir = args.output / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)

    manifest = {
        "source": str(args.input),
        "resource_count": len(resources),
        "selected": [],
        "street_names": [],
    }

    for r in resources:
        rtype = r.type.decode("mac_roman", errors="replace")
        if rtype not in TARGET_TYPES:
            continue
        rname = r.name if isinstance(r.name, str) else (r.name.decode("mac_roman", errors="replace") if r.name else "")
        fname = f"{rtype}_{r.id}_{safe_name(rname or 'unnamed')}.bin"
        out_bin = raw_dir / fname
        out_bin.write_bytes(r.data)

        entry = {
            "type": rtype,
            "id": r.id,
            "name": rname,
            "size": len(r.data),
            "raw_file": str(out_bin),
        }

        if rtype == "STRT":
            streets = decode_street_names(r.data)
            entry["decoded_count"] = len(streets)
            manifest["street_names"] = streets
            (args.output / "street_names.json").write_text(json.dumps(streets, indent=2), encoding="utf-8")

        if rtype == "CLST":
            clst_meta = parse_clst_triplets(r.data)
            entry["clst_parse"] = clst_meta
            clst_json = args.output / f"clst_{r.id}_{safe_name(rname or 'unnamed')}.json"
            clst_json.write_text(json.dumps(clst_meta, indent=2), encoding="utf-8")

        if rtype == "MAPS":
            maps_meta = parse_maps_table(r.data)
            entry["maps_parse"] = maps_meta
            maps_json = args.output / f"maps_{r.id}_{safe_name(rname or 'unnamed')}.json"
            maps_json.write_text(json.dumps(maps_meta, indent=2), encoding="utf-8")

        manifest["selected"].append(entry)

    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    print(f"Exported {len(manifest['selected'])} map-related resources to {args.output}")
    if manifest["street_names"]:
        print(f"Decoded {len(manifest['street_names'])} street names")


if __name__ == "__main__":
    main()

