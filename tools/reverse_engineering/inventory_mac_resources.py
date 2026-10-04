#!/usr/bin/env python3
"""
Inventory and optionally extract files/resources from the Mac VETTE! .toast image.
"""

from __future__ import annotations

import argparse
import json
from collections import Counter, defaultdict
from pathlib import Path

try:
    import machfs
except ImportError as exc:  # pragma: no cover
    raise SystemExit("machfs is required. Install with: py -3 -m pip install machfs") from exc

try:
    import macresources
except ImportError as exc:  # pragma: no cover
    raise SystemExit("macresources is required. Install with: py -3 -m pip install macresources") from exc


def clean_name(name: str) -> str:
    # Mac classic icon files use '\r' in name.
    return name.replace("\r", "_")


def decode_fourcc(value: object) -> str:
    if isinstance(value, (bytes, bytearray)):
        return value.decode("mac_roman", errors="replace")
    if value is None:
        return ""
    return str(value)


def resource_summary(resource_fork: bytes) -> dict:
    by_type: Counter[str] = Counter()
    by_type_ids: defaultdict[str, list[int]] = defaultdict(list)
    for res in macresources.parse_file(resource_fork) or []:
        rtype = decode_fourcc(res.type)
        by_type[rtype] += 1
        by_type_ids[rtype].append(res.id)

    summary = {}
    for rtype, count in by_type.items():
        ids = by_type_ids[rtype]
        summary[rtype] = {
            "count": count,
            "id_min": min(ids),
            "id_max": max(ids),
        }
    return summary


def main() -> None:
    parser = argparse.ArgumentParser(description="Inventory Mac VETTE! toast image resources.")
    parser.add_argument(
        "--toast",
        type=Path,
        default=Path("Vette_Mac_EN") / "VETTE_1_02.toast",
        help="Path to .toast image.",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path("Vette_Mac_EN") / "resource_manifest.json",
        help="Path to output JSON manifest.",
    )
    parser.add_argument(
        "--extract",
        type=Path,
        default=Path("Vette_Mac_EN") / "extracted",
        help="Directory to write extracted .data/.rsrc forks.",
    )
    parser.add_argument(
        "--no-extract",
        action="store_true",
        help="Only produce manifest, do not extract forks.",
    )
    args = parser.parse_args()

    volume = machfs.Volume()
    volume.read(args.toast.read_bytes())

    manifest_entries = []

    for path_tuple, node in volume.iter_paths():
        if isinstance(node, dict):
            continue

        parts = [clean_name(p) for p in path_tuple]
        rel = Path(*parts)
        data_fork = getattr(node, "data", b"") or b""
        rsrc_fork = getattr(node, "rsrc", b"") or b""
        ftype = decode_fourcc(getattr(node, "type", b""))
        creator = decode_fourcc(getattr(node, "creator", b""))

        resource_types = resource_summary(rsrc_fork) if rsrc_fork else {}

        entry = {
            "path": str(rel),
            "type": ftype,
            "creator": creator,
            "data_size": len(data_fork),
            "resource_size": len(rsrc_fork),
            "resource_types": resource_types,
        }
        manifest_entries.append(entry)

        if not args.no_extract:
            out_base = args.extract / rel
            out_base.parent.mkdir(parents=True, exist_ok=True)
            if data_fork:
                Path(str(out_base) + ".data").write_bytes(data_fork)
            if rsrc_fork:
                Path(str(out_base) + ".rsrc").write_bytes(rsrc_fork)

    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(
        json.dumps(
            {
                "toast": str(args.toast),
                "entries": manifest_entries,
            },
            indent=2,
        ),
        encoding="utf-8",
    )

    print(f"Indexed {len(manifest_entries)} files from {args.toast}")
    if not args.no_extract:
        print(f"Extracted forks to {args.extract}")
    print(f"Manifest written to {args.manifest}")


if __name__ == "__main__":
    main()

