#!/usr/bin/env python3
"""
Build engine-ready legacy course polylines from exported CLST resources.
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


COURSE_GROUPS = {
    # 104/1200 are tiny marker-like resources and distort reconstruction.
    "course1": [100, 101, 102],
    "course2": [200],
    "course3": [300, 301],
    "course4": [400, 401],
}


def read_int32_be(data: bytes) -> list[int]:
    vals = []
    for i in range(0, len(data) - (len(data) % 4), 4):
        vals.append(struct.unpack_from(">i", data, i)[0])
    return vals


def looks_like_coord(v: int) -> bool:
    return 1024 <= v <= 120000 and (v % 16 == 0)


def extract_points(vals: list[int]) -> list[tuple[float, float]]:
    pts = []
    i = 0
    while i < len(vals) - 1:
        a = vals[i]
        b = vals[i + 1]
        if a == -1:
            i += 2
            continue
        if looks_like_coord(a) and looks_like_coord(b):
            x = a / 256.0
            z = b / 256.0
            pts.append((x, z))
            i += 2
            continue
        i += 1
    return pts


def dedupe_and_simplify(points: list[tuple[float, float]]) -> list[tuple[float, float]]:
    if not points:
        return []
    out = [points[0]]
    for p in points[1:]:
        px, pz = out[-1]
        if abs(p[0] - px) + abs(p[1] - pz) < 0.25:
            continue
        out.append(p)
    return out


def rdp(points: list[tuple[float, float]], epsilon: float) -> list[tuple[float, float]]:
    if len(points) < 3:
        return points[:]

    x1, y1 = points[0]
    x2, y2 = points[-1]
    dx, dy = x2 - x1, y2 - y1
    line_len_sq = dx * dx + dy * dy

    max_dist = -1.0
    index = -1

    for i in range(1, len(points) - 1):
        px, py = points[i]
        if line_len_sq == 0:
            dist = ((px - x1) ** 2 + (py - y1) ** 2) ** 0.5
        else:
            t = ((px - x1) * dx + (py - y1) * dy) / line_len_sq
            projx = x1 + t * dx
            projy = y1 + t * dy
            dist = ((px - projx) ** 2 + (py - projy) ** 2) ** 0.5
        if dist > max_dist:
            max_dist = dist
            index = i

    if max_dist > epsilon:
        left = rdp(points[: index + 1], epsilon)
        right = rdp(points[index:], epsilon)
        return left[:-1] + right
    return [points[0], points[-1]]


def remove_large_jumps(points: list[tuple[float, float]], max_jump: float = 600.0) -> list[tuple[float, float]]:
    if not points:
        return []
    out = [points[0]]
    for p in points[1:]:
        px, pz = out[-1]
        if ((p[0] - px) ** 2 + (p[1] - pz) ** 2) ** 0.5 <= max_jump:
            out.append(p)
    return out


def split_on_large_jumps(points: list[tuple[float, float]], max_jump: float = 110.0) -> list[list[tuple[float, float]]]:
    if len(points) < 2:
        return [points[:]] if points else []
    segments: list[list[tuple[float, float]]] = []
    current = [points[0]]
    for p in points[1:]:
        px, pz = current[-1]
        dist = ((p[0] - px) ** 2 + (p[1] - pz) ** 2) ** 0.5
        if dist > max_jump:
            if len(current) >= 2:
                segments.append(current)
            current = [p]
        else:
            current.append(p)
    if len(current) >= 2:
        segments.append(current)
    return segments


def stitch_segments(segments: list[list[tuple[float, float]]], stitch_gap: float = 130.0) -> list[tuple[float, float]]:
    if not segments:
        return []
    path = segments[0][:]
    for seg in segments[1:]:
        if not path:
            path = seg[:]
            continue
        end = path[-1]
        d_start = ((seg[0][0] - end[0]) ** 2 + (seg[0][1] - end[1]) ** 2) ** 0.5
        d_end = ((seg[-1][0] - end[0]) ** 2 + (seg[-1][1] - end[1]) ** 2) ** 0.5
        candidate = seg if d_start <= d_end else list(reversed(seg))
        gap = min(d_start, d_end)
        if gap > stitch_gap:
            continue
        if path and candidate and abs(path[-1][0] - candidate[0][0]) + abs(path[-1][1] - candidate[0][1]) < 0.25:
            path.extend(candidate[1:])
        else:
            path.extend(candidate)
    return dedupe_and_simplify(path)


def normalize(points: list[tuple[float, float]], cx: float, cz: float, scale: float) -> list[list[float]]:
    if not points:
        return []
    return [[(x - cx) * scale, -(z - cz) * scale] for x, z in points]


def main() -> None:
    parser = argparse.ArgumentParser(description="Build legacy course JSON from CLST dumps.")
    parser.add_argument(
        "--raw-dir",
        type=Path,
        default=Path("analysis") / "map_data_export" / "raw",
        help="Directory containing CLST_*.bin files.",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=Path("engine") / "src" / "data" / "legacyCourses.json",
        help="Output JSON file path.",
    )
    args = parser.parse_args()

    clst_by_id: dict[int, list[tuple[float, float]]] = {}
    for path in args.raw_dir.glob("CLST_*.bin"):
        try:
            rid = int(path.name.split("_")[1])
        except Exception:
            continue
        vals = read_int32_be(path.read_bytes())
        pts = dedupe_and_simplify(extract_points(vals))
        if pts:
            clst_by_id[rid] = pts

    # Compute one global transform for all courses so map relationships are preserved.
    all_points = [p for pts in clst_by_id.values() for p in pts]
    if all_points:
        xs = [p[0] for p in all_points]
        zs = [p[1] for p in all_points]
        cx = (min(xs) + max(xs)) * 0.5
        cz = (min(zs) + max(zs)) * 0.5
        extent = max(max(xs) - min(xs), max(zs) - min(zs), 1.0)
        scale = 900.0 / extent
    else:
        cx = 0.0
        cz = 0.0
        scale = 1.0

    courses = {}
    for course_id, ids in COURSE_GROUPS.items():
        selected_segments: list[list[tuple[float, float]]] = []
        for rid in ids:
            pts = dedupe_and_simplify(clst_by_id.get(rid, []))
            if len(pts) < 2:
                continue
            segments = split_on_large_jumps(pts, max_jump=110.0)
            if not segments:
                continue
            longest = max(segments, key=len)
            if len(longest) >= 3:
                selected_segments.append(longest)

        merged = stitch_segments(selected_segments, stitch_gap=130.0)
        merged = remove_large_jumps(merged, max_jump=130.0)
        merged = rdp(merged, epsilon=2.0)
        courses[course_id] = {
            "source_clst_ids": ids,
            "point_count_raw": len(merged),
            "points": normalize(merged, cx, cz, scale),
        }

    output = {
        "source": str(args.raw_dir),
        "courses": courses,
    }

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(output, indent=2), encoding="utf-8")
    print(f"Wrote legacy course data: {args.out}")


if __name__ == "__main__":
    main()
