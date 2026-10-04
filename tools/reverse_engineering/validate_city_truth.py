#!/usr/bin/env python3
"""
Validate parity-oriented city_truth export.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def count_failures(payload: dict) -> list[str]:
    failures: list[str] = []
    roads = payload.get("roads", {})
    city_segments = roads.get("city_segments", [])
    freeway_segments = roads.get("freeway_segments", [])
    courses = payload.get("courses", {})
    quad = payload.get("quad", {})
    known = quad.get("known_placements", [])
    unknown = quad.get("unknown_placements", [])

    if len(city_segments) < 120:
        failures.append(f"city_segments too low: {len(city_segments)} < 120")
    if len(freeway_segments) < 20:
        failures.append(f"freeway_segments too low: {len(freeway_segments)} < 20")
    for course_id in ("course1", "course2", "course3", "course4"):
        points = courses.get(course_id, {}).get("points", [])
        if len(points) < 3:
            failures.append(f"{course_id} point count too low: {len(points)}")
    if len(known) < 100:
        failures.append(f"known QUAD placements too low: {len(known)} < 100")
    total_quad = len(known) + len(unknown)
    if total_quad > 0 and (len(unknown) / total_quad) > 0.45:
        failures.append(
            f"unknown QUAD ratio too high: {len(unknown)}/{total_quad} ({len(unknown) / total_quad:.1%})"
        )
    return failures


def render_debug(payload: dict, out_path: Path) -> None:
    try:
        from PIL import Image, ImageDraw
    except Exception:
        return

    city_segments = payload.get("roads", {}).get("city_segments", [])
    freeway_segments = payload.get("roads", {}).get("freeway_segments", [])
    courses = payload.get("courses", {})
    known = payload.get("quad", {}).get("known_placements", [])

    points = []
    for seg in city_segments + freeway_segments:
        points.extend(seg)
    for course in courses.values():
        points.extend(course.get("points", []))
    for p in known:
        points.append(p.get("position", [0, 0]))
    if not points:
        return

    xs = [p[0] for p in points]
    zs = [p[1] for p in points]
    min_x, max_x = min(xs), max(xs)
    min_z, max_z = min(zs), max(zs)

    width = 1400
    height = 1000
    pad = 32
    scale = min((width - 2 * pad) / max(max_x - min_x, 1), (height - 2 * pad) / max(max_z - min_z, 1))

    def m(point: list[float]) -> tuple[float, float]:
        x, z = point
        return (pad + (x - min_x) * scale, height - (pad + (z - min_z) * scale))

    image = Image.new("RGB", (width, height), (8, 12, 24))
    draw = ImageDraw.Draw(image)

    for seg in city_segments:
        draw.line((m(seg[0]), m(seg[1])), fill=(90, 170, 250), width=2)
    for seg in freeway_segments:
        draw.line((m(seg[0]), m(seg[1])), fill=(250, 180, 90), width=3)

    colors = {
        "course1": (255, 90, 90),
        "course2": (80, 255, 120),
        "course3": (255, 220, 90),
        "course4": (200, 120, 255),
    }
    for course_id, course in courses.items():
        pts = course.get("points", [])
        color = colors.get(course_id, (220, 220, 220))
        for i in range(1, len(pts)):
            draw.line((m(pts[i - 1]), m(pts[i])), fill=color, width=2)

    for p in known:
        x, y = m(p.get("position", [0, 0]))
        draw.rectangle((x - 1, y - 1, x + 1, y + 1), fill=(240, 240, 240))

    out_path.parent.mkdir(parents=True, exist_ok=True)
    image.save(out_path)


def main() -> None:
    parser = argparse.ArgumentParser(description="Validate city_truth parity dataset")
    parser.add_argument(
        "--input",
        type=Path,
        default=Path("analysis") / "map_data_export" / "city_truth.json",
    )
    parser.add_argument(
        "--debug-out",
        type=Path,
        default=Path("analysis") / "map_data_export" / "city_truth_debug.png",
    )
    args = parser.parse_args()

    payload = json.loads(args.input.read_text(encoding="utf-8"))
    failures = count_failures(payload)
    render_debug(payload, args.debug_out)

    print(f"Validated: {args.input}")
    print(f"Debug image: {args.debug_out}")
    if failures:
        print("FAIL")
        for failure in failures:
            print(f"- {failure}")
        raise SystemExit(1)
    print("PASS")


if __name__ == "__main__":
    main()
