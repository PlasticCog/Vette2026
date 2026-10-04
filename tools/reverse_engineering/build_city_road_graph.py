#!/usr/bin/env python3
"""
Build a city-scale road graph from MAPS resources.

This uses MAPS_1000 as the primary city point cloud and reconstructs
horizontal/vertical street segments by grouping repeated x/y lines.
"""

from __future__ import annotations

import argparse
import json
import struct
from collections import defaultdict
from pathlib import Path


def read_u16_be(path: Path) -> list[int]:
    data = path.read_bytes()
    return [struct.unpack_from(">H", data, i)[0] for i in range(0, len(data) - (len(data) % 2), 2)]


def decode_maps1000_points(words: list[int]) -> list[tuple[int, int, int]]:
    """
    Heuristic MAPS_1000 decode.
    Word0 is count; remaining words are packed as pairs:
    - x = pair[0]
    - y = high-byte(pair[1])
    - attr = low-byte(pair[1])
    """
    vals = words[1:]
    points: list[tuple[int, int, int]] = []
    for i in range(0, len(vals) - 1, 2):
        x = vals[i]
        packed = vals[i + 1]
        y = (packed >> 8) & 0xFF
        attr = packed & 0xFF
        if x <= 1 or y <= 0:
            continue
        points.append((x, y, attr))
    return points


def build_axis_segments(points: list[tuple[int, int, int]], max_gap: int = 2, min_len: int = 2) -> list[tuple[tuple[int, int], tuple[int, int]]]:
    by_y: dict[int, set[int]] = defaultdict(set)
    by_x: dict[int, set[int]] = defaultdict(set)
    for x, y, _ in points:
        by_y[y].add(x)
        by_x[x].add(y)

    segments: list[tuple[tuple[int, int], tuple[int, int]]] = []

    # Horizontal
    for y, xs in by_y.items():
        line = sorted(xs)
        if len(line) < min_len:
            continue
        start = line[0]
        prev = line[0]
        for x in line[1:]:
            if x - prev <= max_gap:
                prev = x
                continue
            if prev - start >= min_len:
                segments.append(((start, y), (prev, y)))
            start = x
            prev = x
        if prev - start >= min_len:
            segments.append(((start, y), (prev, y)))

    # Vertical
    for x, ys in by_x.items():
        line = sorted(ys)
        if len(line) < min_len:
            continue
        start = line[0]
        prev = line[0]
        for y in line[1:]:
            if y - prev <= max_gap:
                prev = y
                continue
            if prev - start >= min_len:
                segments.append(((x, start), (x, prev)))
            start = y
            prev = y
        if prev - start >= min_len:
            segments.append(((x, start), (x, prev)))

    return segments


def dedupe_segments(segments: list[tuple[tuple[int, int], tuple[int, int]]]) -> list[tuple[tuple[int, int], tuple[int, int]]]:
    seen = set()
    out = []
    for (x1, y1), (x2, y2) in segments:
        a = (x1, y1)
        b = (x2, y2)
        key = (a, b) if a <= b else (b, a)
        if key in seen:
            continue
        seen.add(key)
        out.append((key[0], key[1]))
    return out


def normalize_segments(
    segments: list[tuple[tuple[int, int], tuple[int, int]]],
    target_extent: float = 950.0,
) -> tuple[list[list[list[float]]], dict]:
    xs = []
    ys = []
    for (x1, y1), (x2, y2) in segments:
        xs.extend([x1, x2])
        ys.extend([y1, y2])

    min_x, max_x = min(xs), max(xs)
    min_y, max_y = min(ys), max(ys)
    cx = (min_x + max_x) * 0.5
    cy = (min_y + max_y) * 0.5
    extent = max(max_x - min_x, max_y - min_y, 1)
    scale = target_extent / extent

    out = []
    for (x1, y1), (x2, y2) in segments:
        # z uses inverted Y so north is up in map.
        p1 = [(x1 - cx) * scale, -(y1 - cy) * scale]
        p2 = [(x2 - cx) * scale, -(y2 - cy) * scale]
        out.append([p1, p2])

    meta = {
        "source_bounds": {"min_x": min_x, "max_x": max_x, "min_y": min_y, "max_y": max_y},
        "center": {"x": cx, "y": cy},
        "scale": scale,
    }
    return out, meta


def main() -> None:
    parser = argparse.ArgumentParser(description="Build city road graph JSON from MAPS resource")
    parser.add_argument(
        "--maps1000",
        type=Path,
        default=Path("analysis") / "map_data_export" / "raw" / "MAPS_1000_Main_Map.bin",
    )
    parser.add_argument(
        "--out-analysis",
        type=Path,
        default=Path("analysis") / "map_data_export" / "cityRoadGraph.json",
    )
    parser.add_argument(
        "--out-engine",
        type=Path,
        default=Path("engine") / "src" / "data" / "cityRoadGraph.json",
    )
    args = parser.parse_args()

    words = read_u16_be(args.maps1000)
    points = decode_maps1000_points(words)
    segments = build_axis_segments(points, max_gap=2, min_len=2)
    segments = dedupe_segments(segments)
    norm_segments, meta = normalize_segments(segments)

    payload = {
        "source": str(args.maps1000),
        "point_count": len(points),
        "segment_count": len(norm_segments),
        "meta": meta,
        "segments": norm_segments,
    }

    args.out_analysis.parent.mkdir(parents=True, exist_ok=True)
    args.out_analysis.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    args.out_engine.parent.mkdir(parents=True, exist_ok=True)
    args.out_engine.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    print(f"Points: {len(points)}")
    print(f"Segments: {len(norm_segments)}")
    print(f"Wrote: {args.out_analysis}")
    print(f"Wrote: {args.out_engine}")


if __name__ == "__main__":
    main()
