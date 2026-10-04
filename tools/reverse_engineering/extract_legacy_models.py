#!/usr/bin/env python3
"""
Extract polygonal OBJS resources into engine-ready JSON meshes.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
from pathlib import Path


DEFAULT_RUNTIME_IDS = [
    2000,   # NuVette
    3300,   # Police
    4000,   # Taxi
    4200,   # Truck
    6600,   # Simplecar
    7100,   # Block1
    7200,   # Block2
    7300,   # Block3
    7400,   # Block4
    8000,   # barri256
    8200,   # fairmont
    9000,   # rx7
    9900,   # Pyramid
    9970,   # CityHall
    10000,  # Hiatt
    10500,  # CityPark
    10600,  # Tree
    13000,  # Bldg1200
    13010,  # Bldg1200s
    13100,  # Bldg600
    13110,  # Bldg600s
    7600,   # Blockall
    7650,   # Blockall alt
]


def parse_words_be_s16(data: bytes) -> list[int]:
    return [struct.unpack_from(">h", data, i)[0] for i in range(0, len(data) - (len(data) % 2), 2)]


def decode_face_sequence(seq: list[int], vertex_count: int) -> list[int] | None:
    """
    Recover one polygon from a `-1`-terminated sequence.
    Vertices are encoded as 16x indices in observed assets.
    """
    if len(seq) < 5:
        return None

    for j in range(len(seq) - 1):
        n = seq[j]
        if n < 3 or n > 24:
            continue

        rem = len(seq) - (j + 1)
        if rem not in (n, n + 1):
            continue

        candidate = seq[j + 1 :]
        if any(v % 16 != 0 for v in candidate):
            continue

        idx = [v // 16 for v in candidate]
        if rem == n + 1:
            if idx[0] != idx[-1]:
                continue
            idx = idx[:-1]

        if len(idx) != n:
            continue
        if any(v < 0 or v >= vertex_count for v in idx):
            continue
        return idx

    return None


def triangulate_fan(polygon: list[int]) -> list[list[int]]:
    tris = []
    if len(polygon) < 3:
        return tris
    for i in range(1, len(polygon) - 1):
        a, b, c = polygon[0], polygon[i], polygon[i + 1]
        if a != b and b != c and a != c:
            tris.append([a, b, c])
    return tris


def parse_objs_resource(data: bytes) -> dict | None:
    words = parse_words_be_s16(data)
    if len(words) < 6:
        return None

    # Observed pattern: second word is (vertex_count - 1)
    vertex_count = max(0, words[1] + 1)
    vertex_start = 2
    vertex_words = vertex_count * 4
    if vertex_start + vertex_words > len(words):
        return None

    vertices = []
    cursor = vertex_start
    for _ in range(vertex_count):
        # Per-vertex records are usually [-1, x, y, z].
        x = words[cursor + 1]
        y = words[cursor + 2]
        z = words[cursor + 3]
        vertices.append([x, y, z])
        cursor += 4

    polygons = []
    while cursor < len(words):
        try:
            end = words.index(-1, cursor)
        except ValueError:
            break

        seq = words[cursor:end]
        cursor = end + 1
        if not seq:
            continue

        polygon = decode_face_sequence(seq, len(vertices))
        if polygon:
            polygons.append(polygon)
        elif polygons and len(seq) > 10:
            # Heuristic: once we entered non-polygon tables, stop.
            break

    if not polygons:
        return None

    triangles = []
    for poly in polygons:
        triangles.extend(triangulate_fan(poly))

    if not triangles:
        return None

    xs = [v[0] for v in vertices]
    ys = [v[1] for v in vertices]
    zs = [v[2] for v in vertices]
    bounds = {
        "min": [min(xs), min(ys), min(zs)],
        "max": [max(xs), max(ys), max(zs)],
        "size": [max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs)],
    }

    return {
        "vertex_count": len(vertices),
        "polygon_count": len(polygons),
        "triangle_count": len(triangles),
        "vertices": vertices,
        "triangles": triangles,
        "bounds": bounds,
    }


def parse_filename(path: Path) -> tuple[int, str]:
    # Example: OBJS_2000_NuVette.bin or OBJS_-25346_Wall896.bin
    m = re.match(r"^OBJS_(-?\d+)_([^.]*)\.bin$", path.name)
    if not m:
        raise ValueError(f"Unexpected OBJS filename: {path.name}")
    return int(m.group(1)), m.group(2)


def safe_key(name: str, rid: int) -> str:
    key = re.sub(r"[^a-zA-Z0-9]+", "_", name).strip("_").lower()
    return key or f"obj_{rid}"


def main() -> None:
    parser = argparse.ArgumentParser(description="Extract VETTE OBJS meshes.")
    parser.add_argument(
        "--raw-dir",
        type=Path,
        default=Path("analysis") / "map_data_export" / "raw",
        help="Directory containing OBJS_*.bin files",
    )
    parser.add_argument(
        "--out-full",
        type=Path,
        default=Path("analysis") / "map_data_export" / "legacy_models.json",
        help="Full parsed model catalog",
    )
    parser.add_argument(
        "--out-runtime",
        type=Path,
        default=Path("engine") / "src" / "data" / "legacyModels.json",
        help="Runtime subset consumed by Three.js engine",
    )
    parser.add_argument(
        "--runtime-ids",
        type=int,
        nargs="*",
        default=DEFAULT_RUNTIME_IDS,
        help="Resource IDs to include in runtime subset",
    )
    parser.add_argument(
        "--runtime-all",
        action="store_true",
        help="Include all parsed OBJS resources in runtime output",
    )
    args = parser.parse_args()

    all_models: dict[str, dict] = {}
    by_id: dict[int, dict] = {}
    parsed = 0

    for path in sorted(args.raw_dir.glob("OBJS_*.bin")):
        rid, name = parse_filename(path)
        parsed_model = parse_objs_resource(path.read_bytes())
        if not parsed_model:
            continue

        parsed += 1
        key = safe_key(name, rid)
        record = {
            "id": rid,
            "name": name,
            **parsed_model,
        }
        all_models[key] = record
        by_id[rid] = record

    full_payload = {
        "source": str(args.raw_dir),
        "parsed_model_count": parsed,
        "models": all_models,
    }
    args.out_full.parent.mkdir(parents=True, exist_ok=True)
    args.out_full.write_text(json.dumps(full_payload, indent=2), encoding="utf-8")

    runtime_models = {}
    if args.runtime_all:
        for rid, model in sorted(by_id.items(), key=lambda item: item[0]):
            runtime_models[str(rid)] = model
    else:
        for rid in args.runtime_ids:
            model = by_id.get(rid)
            if not model:
                continue
            runtime_models[str(rid)] = model

    runtime_payload = {
        "source": str(args.raw_dir),
        "model_count": len(runtime_models),
        "modelsById": runtime_models,
    }
    args.out_runtime.parent.mkdir(parents=True, exist_ok=True)
    args.out_runtime.write_text(json.dumps(runtime_payload, indent=2), encoding="utf-8")

    print(f"Parsed OBJS models: {parsed}")
    print(f"Wrote full model catalog: {args.out_full}")
    print(f"Wrote runtime model subset ({len(runtime_models)} models): {args.out_runtime}")


if __name__ == "__main__":
    main()
