#!/usr/bin/env python3
"""
Extract files from the bundled PC-98 VETTE!.HDI image.

This parser is intentionally narrow and targets the disk layout present in this
repository:
- 0x1000-byte HDI header
- FAT-like partition where cluster 2 begins at 0xFC00
- 2048-byte clusters
- root directory table at 0xBC00 (512 entries)
"""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable

HDI_HEADER_OFFSET = 0x1000
ROOT_DIR_OFFSET = 0xBC00
ROOT_DIR_SIZE = 0x4000
CLUSTER2_OFFSET = 0xFC00
CLUSTER_SIZE = 2048
ENTRY_SIZE = 32


@dataclass
class DirEntry:
    name: str
    attr: int
    cluster: int
    size: int
    offset: int

    @property
    def is_directory(self) -> bool:
        return bool(self.attr & 0x10)


def parse_dir_entries(buf: bytes, table_offset: int) -> list[DirEntry]:
    entries: list[DirEntry] = []
    for i in range(0, len(buf), ENTRY_SIZE):
        entry = buf[i : i + ENTRY_SIZE]
        if len(entry) < ENTRY_SIZE:
            break
        first = entry[0]
        if first == 0x00:
            break
        if first == 0xE5:
            continue
        name = entry[:8].decode("ascii", errors="replace").rstrip()
        ext = entry[8:11].decode("ascii", errors="replace").rstrip()
        fullname = name + (f".{ext}" if ext else "")
        entries.append(
            DirEntry(
                name=fullname,
                attr=entry[11],
                cluster=int.from_bytes(entry[26:28], "little"),
                size=int.from_bytes(entry[28:32], "little"),
                offset=table_offset + i,
            )
        )
    return entries


def cluster_offset(cluster: int) -> int:
    if cluster < 2:
        raise ValueError(f"invalid cluster number: {cluster}")
    return CLUSTER2_OFFSET + (cluster - 2) * CLUSTER_SIZE


def extract_files(image: bytes, entries: Iterable[DirEntry], output_dir: Path) -> list[dict]:
    output_dir.mkdir(parents=True, exist_ok=True)
    manifest: list[dict] = []
    for entry in entries:
        if entry.is_directory:
            continue
        start = cluster_offset(entry.cluster)
        end = start + entry.size
        payload = image[start:end]
        out_path = output_dir / entry.name
        out_path.write_bytes(payload)
        record = asdict(entry)
        record["extracted_to"] = str(out_path)
        manifest.append(record)
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description="Extract files from VETTE.hdi (PC-98).")
    parser.add_argument(
        "--hdi",
        type=Path,
        default=Path("Vette_PC-98_JA") / "VETTE.hdi",
        help="Path to HDI file.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("Vette_PC-98_JA") / "extracted",
        help="Directory to write extracted files.",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path("Vette_PC-98_JA") / "extracted_manifest.json",
        help="JSON manifest path.",
    )
    args = parser.parse_args()

    image = args.hdi.read_bytes()
    signature = image[HDI_HEADER_OFFSET : HDI_HEADER_OFFSET + 8]
    if signature[:4] != b"\xEB\x0A\x90\x90":
        raise RuntimeError("Unexpected IPL signature. This script expects the bundled VETTE.hdi layout.")

    root_buf = image[ROOT_DIR_OFFSET : ROOT_DIR_OFFSET + ROOT_DIR_SIZE]
    root_entries = parse_dir_entries(root_buf, ROOT_DIR_OFFSET)

    vette_dir = next((e for e in root_entries if e.name == "VETTE" and e.is_directory), None)
    if vette_dir is None:
        raise RuntimeError("Could not locate VETTE directory entry in root table.")

    vette_offset = cluster_offset(vette_dir.cluster)
    vette_buf = image[vette_offset : vette_offset + CLUSTER_SIZE]
    vette_entries = parse_dir_entries(vette_buf, vette_offset)

    files_manifest = extract_files(image, vette_entries, args.output)

    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(
        json.dumps(
            {
                "hdi": str(args.hdi),
                "output": str(args.output),
                "layout": {
                    "hdi_header_offset": HDI_HEADER_OFFSET,
                    "root_dir_offset": ROOT_DIR_OFFSET,
                    "root_dir_size": ROOT_DIR_SIZE,
                    "cluster2_offset": CLUSTER2_OFFSET,
                    "cluster_size": CLUSTER_SIZE,
                },
                "root_entries": [asdict(e) for e in root_entries],
                "vette_entries": [asdict(e) for e in vette_entries],
                "extracted_files": files_manifest,
            },
            indent=2,
        ),
        encoding="utf-8",
    )

    print(f"Extracted {len(files_manifest)} files to {args.output}")
    print(f"Manifest written to {args.manifest}")


if __name__ == "__main__":
    main()

