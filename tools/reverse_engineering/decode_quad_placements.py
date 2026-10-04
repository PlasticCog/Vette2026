#!/usr/bin/env python3
"""
Decode heuristic object placements from QUAD_1000 resource.

Observed format characteristics:
- Big-endian u16 payload
- Many useful placement tuples appear aligned to 4-word boundaries
- Candidate tuple shape: (type_id, x, y, heading)
"""

from __future__ import annotations

import argparse
import json
import struct
from collections import Counter
from pathlib import Path


def read_u16_be(path: Path) -> list[int]:
    data = path.read_bytes()
    return [struct.unpack_from(">H", data, i)[0] for i in range(0, len(data) - (len(data) % 2), 2)]


def decode_quad_placements(words: list[int]) -> tuple[list[dict], dict]:
    placements: list[dict] = []
    counts: Counter[int] = Counter()
    seen = set()

    # Word 0 is typically payload word-count marker in this resource.
    start = 4
    for i in range(start, len(words) - 3, 4):
        type_id, x, y, heading = words[i : i + 4]

        if not (5 <= type_id <= 600):
            continue
        if not (0 <= x <= 2048 and 0 <= y <= 2048):
            continue
        if not (0 <= heading <= 360):
            continue

        key = (type_id, x, y, heading)
        if key in seen:
            continue
        seen.add(key)

        placements.append(
            {
                "type_id": type_id,
                "x": x,
                "y": y,
                "heading": heading,
            }
        )
        counts[type_id] += 1

    meta = {
        "word_count": len(words),
        "placement_count": len(placements),
        "type_histogram": dict(counts.most_common()),
    }
    return placements, meta


def normalize(placements: list[dict], extent: float = 1800.0) -> list[dict]:
    if not placements:
        return []
    xs = [p["x"] for p in placements]
    ys = [p["y"] for p in placements]
    min_x, max_x = min(xs), max(xs)
    min_y, max_y = min(ys), max(ys)
    cx = (min_x + max_x) * 0.5
    cy = (min_y + max_y) * 0.5
    span = max(max_x - min_x, max_y - min_y, 1)
    scale = extent / span

    out = []
    for p in placements:
        out.append(
            {
                "type_id": p["type_id"],
                "position": [(p["x"] - cx) * scale, -(p["y"] - cy) * scale],
                "heading": p["heading"],
            }
        )
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description="Decode QUAD object placements")
    parser.add_argument(
        "--input",
        type=Path,
        default=Path("analysis") / "map_data_export" / "raw" / "QUAD_1000_Quad_Discripter_Data.bin",
    )
    parser.add_argument(
        "--out-analysis",
        type=Path,
        default=Path("analysis") / "map_data_export" / "quad_placements.json",
    )
    parser.add_argument(
        "--out-engine",
        type=Path,
        default=Path("engine") / "src" / "data" / "quadPlacements.json",
    )
    args = parser.parse_args()

    words = read_u16_be(args.input)
    placements, meta = decode_quad_placements(words)
    norm = normalize(placements, extent=1900.0)

    payload = {
        "source": str(args.input),
        "meta": meta,
        "placements": norm,
    }

    args.out_analysis.parent.mkdir(parents=True, exist_ok=True)
    args.out_analysis.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    args.out_engine.parent.mkdir(parents=True, exist_ok=True)
    args.out_engine.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    print(f"Decoded placements: {len(norm)}")
    print(f"Wrote: {args.out_analysis}")
    print(f"Wrote: {args.out_engine}")


if __name__ == "__main__":
    main()

