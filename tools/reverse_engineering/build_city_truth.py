#!/usr/bin/env python3
"""
Build canonical city_truth data for parity-focused runtime.

This script intentionally keeps all transforms explicit and reproducible:
- MAPS_4444 / MAPS_4445 decoded as packed map coordinates (x=hi byte, y=lo byte)
- CLST course points decoded from fixed-point int32 pairs (value / 1024)
- QUAD placements decoded as aligned u16 tuples (type_id, x, y, heading)
- QUAD type_id mapped to OBJS resources by table order from manifest
"""

from __future__ import annotations

import argparse
import json
import struct
from collections import Counter, defaultdict
from pathlib import Path


COURSE_GROUPS = {
    "course1": [100, 101, 102, 104, 1200],
    "course2": [200, 201, 202, 203, 204],
    "course3": [300, 301],
    "course4": [400, 401],
}

CLST_COORD_SCALE = 1024.0


def read_u16_be(path: Path) -> list[int]:
    data = path.read_bytes()
    return [struct.unpack_from(">H", data, i)[0] for i in range(0, len(data) - (len(data) % 2), 2)]


def read_i32_be(path: Path) -> list[int]:
    data = path.read_bytes()
    return [struct.unpack_from(">i", data, i)[0] for i in range(0, len(data) - (len(data) % 4), 4)]


def decode_packed_maps_points(words: list[int]) -> list[tuple[float, float]]:
    points: list[tuple[float, float]] = []
    for value in words:
        if value == 0:
            continue
        x = (value >> 8) & 0xFF
        y = value & 0xFF
        if x <= 0 or y <= 0:
            continue
        points.append((float(x), float(y)))
    return points


def build_axis_segments(points: list[tuple[float, float]], max_gap: int = 2, min_len: int = 1) -> list[tuple[tuple[float, float], tuple[float, float]]]:
    by_y: dict[int, set[int]] = defaultdict(set)
    by_x: dict[int, set[int]] = defaultdict(set)
    for x, y in points:
        ix = int(round(x))
        iy = int(round(y))
        by_y[iy].add(ix)
        by_x[ix].add(iy)

    segments: list[tuple[tuple[float, float], tuple[float, float]]] = []

    for y, xs in by_y.items():
        line = sorted(xs)
        if len(line) < 2:
            continue
        start = line[0]
        prev = line[0]
        for x in line[1:]:
            if x - prev <= max_gap:
                prev = x
                continue
            if prev - start >= min_len:
                segments.append(((float(start), float(y)), (float(prev), float(y))))
            start = x
            prev = x
        if prev - start >= min_len:
            segments.append(((float(start), float(y)), (float(prev), float(y))))

    for x, ys in by_x.items():
        line = sorted(ys)
        if len(line) < 2:
            continue
        start = line[0]
        prev = line[0]
        for y in line[1:]:
            if y - prev <= max_gap:
                prev = y
                continue
            if prev - start >= min_len:
                segments.append(((float(x), float(start)), (float(x), float(prev))))
            start = y
            prev = y
        if prev - start >= min_len:
            segments.append(((float(x), float(start)), (float(x), float(prev))))

    return dedupe_segments(segments)


def dedupe_segments(segments: list[tuple[tuple[float, float], tuple[float, float]]]) -> list[tuple[tuple[float, float], tuple[float, float]]]:
    out: list[tuple[tuple[float, float], tuple[float, float]]] = []
    seen = set()
    for a, b in segments:
        key = (a, b) if a <= b else (b, a)
        if key in seen:
            continue
        seen.add(key)
        out.append(key)
    return out


def extract_clst_points(vals: list[int], scale: float = CLST_COORD_SCALE) -> list[tuple[float, float]]:
    points: list[tuple[float, float]] = []
    i = 0
    while i < len(vals) - 1:
        a = vals[i]
        b = vals[i + 1]
        if a < 0 or b < 0:
            i += 1
            continue
        if a > 140000 or b > 140000:
            i += 1
            continue
        if (a % 16 != 0) or (b % 16 != 0):
            i += 1
            continue
        x = a / scale
        y = b / scale
        if x <= 0 or y <= 0:
            i += 2
            continue
        if points and abs(points[-1][0] - x) + abs(points[-1][1] - y) < 0.2:
            i += 2
            continue
        points.append((x, y))
        i += 2
    return points


def dedupe_and_simplify(points: list[tuple[float, float]]) -> list[tuple[float, float]]:
    if not points:
        return []
    out = [points[0]]
    for point in points[1:]:
        px, py = out[-1]
        if abs(point[0] - px) + abs(point[1] - py) < 0.2:
            continue
        out.append(point)
    return out


def split_on_large_jumps(points: list[tuple[float, float]], max_jump: float = 110.0) -> list[list[tuple[float, float]]]:
    if len(points) < 2:
        return [points[:]] if points else []
    segments: list[list[tuple[float, float]]] = []
    current = [points[0]]
    for point in points[1:]:
        px, py = current[-1]
        dist = ((point[0] - px) ** 2 + (point[1] - py) ** 2) ** 0.5
        if dist > max_jump:
            if len(current) >= 2:
                segments.append(current)
            current = [point]
        else:
            current.append(point)
    if len(current) >= 2:
        segments.append(current)
    return segments


def stitch_segments(segments: list[list[tuple[float, float]]], stitch_gap: float = 130.0) -> list[tuple[float, float]]:
    if not segments:
        return []
    path = segments[0][:]
    for segment in segments[1:]:
        if not path:
            path = segment[:]
            continue
        end = path[-1]
        d_start = ((segment[0][0] - end[0]) ** 2 + (segment[0][1] - end[1]) ** 2) ** 0.5
        d_end = ((segment[-1][0] - end[0]) ** 2 + (segment[-1][1] - end[1]) ** 2) ** 0.5
        candidate = segment if d_start <= d_end else list(reversed(segment))
        gap = min(d_start, d_end)
        if gap > stitch_gap:
            continue
        if path and candidate and abs(path[-1][0] - candidate[0][0]) + abs(path[-1][1] - candidate[0][1]) < 0.2:
            path.extend(candidate[1:])
        else:
            path.extend(candidate)
    return dedupe_and_simplify(path)


def remove_large_jumps(points: list[tuple[float, float]], max_jump: float = 130.0) -> list[tuple[float, float]]:
    if not points:
        return []
    out = [points[0]]
    for point in points[1:]:
        px, py = out[-1]
        dist = ((point[0] - px) ** 2 + (point[1] - py) ** 2) ** 0.5
        if dist <= max_jump:
            out.append(point)
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


def nearest_point(candidates: list[tuple[int, int]], target: tuple[float, float]) -> tuple[int, int] | None:
    if not candidates:
        return None
    tx, ty = target
    best = None
    best_dist = float("inf")
    for point in candidates:
        px, py = point
        dist_sq = (px - tx) * (px - tx) + (py - ty) * (py - ty)
        if dist_sq < best_dist:
            best_dist = dist_sq
            best = point
    return best


def build_course1_fallback(
    existing_points: list[tuple[float, float]],
    city_points: list[tuple[float, float]],
    freeway_points: list[tuple[float, float]],
) -> list[tuple[float, float]]:
    map_points = sorted({(int(round(x)), int(round(y))) for x, y in [*city_points, *freeway_points]})
    if not map_points:
        return existing_points

    min_x = min(point[0] for point in map_points)
    max_y = max(point[1] for point in map_points)

    seed_candidates = []
    for point in existing_points:
        nearest = nearest_point(map_points, point)
        if nearest is not None:
            seed_candidates.append(nearest)
    seed_candidates = sorted(set(seed_candidates), key=lambda p: (p[1], p[0]))

    seeds: list[tuple[int, int]] = []
    if seed_candidates:
        seeds.append(seed_candidates[0])
        middle = seed_candidates[len(seed_candidates) // 2]
        if middle != seeds[-1]:
            seeds.append(middle)
        if seed_candidates[-1] != seeds[-1]:
            seeds.append(seed_candidates[-1])

    left_points = [point for point in map_points if point[0] <= min_x + 12]
    by_y: dict[int, list[int]] = defaultdict(list)
    for x, y in left_points:
        by_y[y].append(x)
    spine = [(min(xs), y) for y, xs in sorted(by_y.items())]
    if seeds:
        spine = [point for point in spine if point[1] >= seeds[-1][1]]

    compact_spine: list[tuple[int, int]] = []
    for point in spine:
        if not compact_spine or point[1] - compact_spine[-1][1] >= 2:
            compact_spine.append(point)

    top_band = [point for point in map_points if point[1] >= max_y - 2]
    by_x: dict[int, list[int]] = defaultdict(list)
    for x, y in top_band:
        by_x[x].append(y)
    top_line = [(x, max(ys)) for x, ys in sorted(by_x.items()) if not compact_spine or x >= compact_spine[-1][0]]

    stitched: list[tuple[int, int]] = []
    for point in [*seeds, *compact_spine, *top_line]:
        if stitched and point == stitched[-1]:
            continue
        if point in stitched:
            continue
        stitched.append(point)

    fallback = [(float(x), float(y)) for x, y in stitched]
    if len(fallback) >= 3:
        fallback = rdp(fallback, epsilon=1.0)
    return fallback if len(fallback) >= 4 else existing_points


def decode_courses(
    raw_dir: Path,
    city_points: list[tuple[float, float]],
    freeway_points: list[tuple[float, float]],
) -> dict[str, dict]:
    by_id: dict[int, list[tuple[float, float]]] = {}
    for path in raw_dir.glob("CLST_*.bin"):
        try:
            rid = int(path.name.split("_")[1])
        except Exception:
            continue
        vals = read_i32_be(path)
        points = dedupe_and_simplify(extract_clst_points(vals, scale=CLST_COORD_SCALE))
        if points:
            by_id[rid] = points

    courses: dict[str, dict] = {}
    map_max_y = 0.0
    map_points = [*city_points, *freeway_points]
    if map_points:
        map_max_y = max(point[1] for point in map_points)

    for course_id, ids in COURSE_GROUPS.items():
        selected_segments: list[list[tuple[float, float]]] = []
        for rid in ids:
            points = dedupe_and_simplify(by_id.get(rid, []))
            if len(points) < 2:
                continue
            segments = split_on_large_jumps(points, max_jump=28.0)
            if not segments:
                continue
            for segment in segments:
                if len(segment) >= 2:
                    selected_segments.append(segment)

        cleaned = stitch_segments(selected_segments, stitch_gap=34.0)
        cleaned = remove_large_jumps(cleaned, max_jump=36.0)
        if len(cleaned) >= 3:
            cleaned = rdp(cleaned, epsilon=0.45)
        if course_id == "course1":
            course_max_y = max((point[1] for point in cleaned), default=0.0)
            needs_fallback = len(cleaned) < 8 or (map_max_y > 0 and course_max_y < map_max_y * 0.82)
            if needs_fallback:
                cleaned = build_course1_fallback(cleaned, city_points, freeway_points)
        courses[course_id] = {
            "source_clst_ids": ids,
            "points_map": cleaned,
        }
    return courses


def decode_quad_placements(words: list[int]) -> tuple[list[dict], dict]:
    placements: list[dict] = []
    counts: Counter[int] = Counter()
    seen = set()

    for i in range(4, len(words) - 3, 4):
        type_id, x_raw, y_raw, heading = words[i : i + 4]
        if not (1 <= type_id <= 4095):
            continue
        if not (0 <= x_raw <= 65535 and 0 <= y_raw <= 65535):
            continue
        if not (0 <= heading <= 360):
            continue

        x = x_raw / 16.0
        y = y_raw / 16.0
        if x <= 0 or y <= 0:
            continue
        # Clamp to observed city-ish coordinate envelope.
        if x > 4096 or y > 4096:
            continue

        key = (type_id, round(x, 4), round(y, 4), heading)
        if key in seen:
            continue
        seen.add(key)

        placement = {
            "type_id": int(type_id),
            "position_map": [x, y],
            "heading_deg": int(heading),
        }
        placements.append(placement)
        counts[type_id] += 1

    meta = {
        "word_count": len(words),
        "placement_count": len(placements),
        "type_histogram": dict(counts.most_common()),
    }
    return placements, meta


def decode_objs_index(manifest_path: Path) -> list[dict]:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    objs = [entry for entry in manifest.get("selected", []) if entry.get("type") == "OBJS"]
    indexed = []
    for idx, entry in enumerate(objs, start=1):
        indexed.append(
            {
                "type_id": idx,
                "resource_id": int(entry["id"]),
                "name": entry.get("name", ""),
            }
        )
    return indexed


def map_quad_types_to_objs(placements: list[dict], objs_index: list[dict]) -> tuple[list[dict], list[dict], list[int]]:
    by_type = {entry["type_id"]: entry for entry in objs_index}
    known = []
    unknown = []
    unknown_ids: set[int] = set()

    for placement in placements:
        mapped = by_type.get(int(placement["type_id"]))
        if mapped:
            known.append(
                {
                    **placement,
                    "resource_id": mapped["resource_id"],
                    "resource_name": mapped["name"],
                }
            )
        else:
            unknown.append(placement)
            unknown_ids.add(int(placement["type_id"]))

    return known, unknown, sorted(unknown_ids)


def map_to_world_builder(points_xy: list[tuple[float, float]], target_extent: float = 4200.0):
    xs = [p[0] for p in points_xy]
    ys = [p[1] for p in points_xy]
    min_x, max_x = min(xs), max(xs)
    min_y, max_y = min(ys), max(ys)
    cx = (min_x + max_x) * 0.5
    cy = (min_y + max_y) * 0.5
    span = max(max_x - min_x, max_y - min_y, 1.0)
    scale = target_extent / span

    def to_world(point: tuple[float, float]) -> list[float]:
        x, y = point
        return [(x - cx) * scale, -(y - cy) * scale]

    meta = {
        "source_bounds": {
            "min_x": min_x,
            "max_x": max_x,
            "min_y": min_y,
            "max_y": max_y,
        },
        "center_map": [cx, cy],
        "scale_to_world": scale,
    }
    return to_world, meta


def to_serializable_segments(segments: list[tuple[tuple[float, float], tuple[float, float]]], to_world) -> list[list[list[float]]]:
    out = []
    for a, b in segments:
        out.append([to_world(a), to_world(b)])
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description="Build canonical city_truth.json")
    parser.add_argument(
        "--raw-dir",
        type=Path,
        default=Path("analysis") / "map_data_export" / "raw",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path("analysis") / "map_data_export" / "manifest.json",
    )
    parser.add_argument(
        "--out-analysis",
        type=Path,
        default=Path("analysis") / "map_data_export" / "city_truth.json",
    )
    parser.add_argument(
        "--out-engine",
        type=Path,
        default=Path("engine") / "src" / "data" / "cityTruth.json",
    )
    args = parser.parse_args()

    maps4444_words = read_u16_be(args.raw_dir / "MAPS_4444_Real_world_Map_Data.bin")
    maps4445_words = read_u16_be(args.raw_dir / "MAPS_4445_Freeway_Map_Data.bin")
    maps1000_words = read_u16_be(args.raw_dir / "MAPS_1000_Main_Map.bin")
    maps1100_words = read_u16_be(args.raw_dir / "MAPS_1100_Freeway_Map.bin")
    quad_words = read_u16_be(args.raw_dir / "QUAD_1000_Quad_Discripter_Data.bin")

    city_points = decode_packed_maps_points(maps4444_words)
    freeway_points = decode_packed_maps_points(maps4445_words)
    city_segments = build_axis_segments(city_points, max_gap=2, min_len=1)
    freeway_segments = build_axis_segments(freeway_points, max_gap=3, min_len=1)

    courses = decode_courses(args.raw_dir, city_points, freeway_points)
    quad_raw, quad_meta = decode_quad_placements(quad_words)
    objs_index = decode_objs_index(args.manifest)
    quad_known, quad_unknown, quad_unknown_ids = map_quad_types_to_objs(quad_raw, objs_index)

    world_anchor_points: list[tuple[float, float]] = []
    world_anchor_points.extend(city_points)
    world_anchor_points.extend(freeway_points)
    for course in courses.values():
        world_anchor_points.extend(course["points_map"])
    for placement in quad_known:
        world_anchor_points.append((placement["position_map"][0], placement["position_map"][1]))

    if not world_anchor_points:
        raise SystemExit("No points decoded for world transform.")

    to_world, transform_meta = map_to_world_builder(world_anchor_points, target_extent=4200.0)

    courses_world = {}
    for course_id, course in courses.items():
        courses_world[course_id] = {
            "source_clst_ids": course["source_clst_ids"],
            "points": [to_world(point) for point in course["points_map"]],
        }

    quad_known_world = []
    for placement in quad_known:
        p = (placement["position_map"][0], placement["position_map"][1])
        quad_known_world.append(
            {
                "type_id": placement["type_id"],
                "resource_id": placement["resource_id"],
                "resource_name": placement["resource_name"],
                "heading_deg": placement["heading_deg"],
                "position": to_world(p),
            }
        )

    quad_unknown_world = []
    for placement in quad_unknown:
        p = (placement["position_map"][0], placement["position_map"][1])
        quad_unknown_world.append(
            {
                "type_id": placement["type_id"],
                "heading_deg": placement["heading_deg"],
                "position": to_world(p),
            }
        )

    payload = {
        "source": {
            "raw_dir": str(args.raw_dir),
            "manifest": str(args.manifest),
        },
        "transform": transform_meta,
        "maps": {
            "maps1000_word_count": len(maps1000_words),
            "maps1100_word_count": len(maps1100_words),
            "maps4444_point_count": len(city_points),
            "maps4445_point_count": len(freeway_points),
        },
        "roads": {
            "city_segments": to_serializable_segments(city_segments, to_world),
            "freeway_segments": to_serializable_segments(freeway_segments, to_world),
        },
        "courses": courses_world,
        "quad": {
            "meta": quad_meta,
            "known_placements": quad_known_world,
            "unknown_placements": quad_unknown_world,
            "unknown_type_ids": quad_unknown_ids,
        },
        "objs_index": objs_index,
    }

    args.out_analysis.parent.mkdir(parents=True, exist_ok=True)
    args.out_analysis.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    args.out_engine.parent.mkdir(parents=True, exist_ok=True)
    args.out_engine.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    print(f"City segments: {len(city_segments)}")
    print(f"Freeway segments: {len(freeway_segments)}")
    print(f"QUAD known placements: {len(quad_known_world)}")
    print(f"QUAD unknown placements: {len(quad_unknown_world)}")
    print(f"Wrote: {args.out_analysis}")
    print(f"Wrote: {args.out_engine}")


if __name__ == "__main__":
    main()
