#!/usr/bin/env python3
"""Measure Crash 3 resident-sidecar topology before Vulkan rasterization.

The runtime catalog contains every host-projected resident triangle for one
guest frame.  This tool rasterizes that immutable input independently, welds
projected edges by exact X/Y/W identity, and optionally compares its coverage
with the physical Vulkan color readback.  It therefore distinguishes a hole
already present in resident topology from a triangle lost later in the GPU
presentation path; black color alone is never treated as proof of either.
"""

from __future__ import annotations

import argparse
import collections
import json
import math
import struct
import sys
from pathlib import Path
from typing import Any, Iterable


TRACE_SCHEMA = "ps1trace/v1"
REPORT_SCHEMA = "ps1-crash3-resident-coverage/v1"
CATALOG_EVENT = "game.crash3.resident_projection_catalog"
FACE_EVENT = "game.crash3.resident_projection_face"
MASK64 = (1 << 64) - 1


def _double_from_bits(value: Any, context: str) -> float:
    try:
        bits = int(value) & MASK64
        decoded = struct.unpack("<d", struct.pack("<Q", bits))[0]
    except (TypeError, ValueError, struct.error) as exc:
        raise ValueError(f"{context}: invalid binary64 payload {value!r}") from exc
    if not math.isfinite(decoded):
        raise ValueError(f"{context}: non-finite binary64 payload")
    return decoded


def _load_catalog(path: Path) -> dict[int, tuple[dict[str, Any], list[dict[str, Any]]]]:
    catalogs: dict[int, dict[str, Any]] = {}
    faces: dict[int, list[dict[str, Any]]] = collections.defaultdict(list)
    with path.open(encoding="utf-8") as stream:
        for lineno, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{lineno}: invalid JSON: {exc}") from exc
            if record.get("schema") != TRACE_SCHEMA:
                raise ValueError(
                    f"{path}:{lineno}: schema {record.get('schema')!r}, "
                    f"expected {TRACE_SCHEMA!r}"
                )
            frame = int(record.get("frame", -1))
            if record.get("event") == CATALOG_EVENT:
                if frame in catalogs:
                    raise ValueError(f"{path}: duplicate frame-{frame} catalog")
                catalogs[frame] = record
            elif record.get("event") == FACE_EVENT:
                faces[frame].append(record)
    if not catalogs:
        raise ValueError(f"{path}: no {CATALOG_EVENT} records")

    result = {}
    for frame, catalog in sorted(catalogs.items()):
        catalog_schema = int(catalog.get("catalog_schema", 0))
        face_schema = int(catalog.get("face_schema", 0))
        if catalog_schema not in (1, 2, 3, 4) or face_schema not in (1, 2, 3):
            raise ValueError(f"{path}: frame {frame} has an unsupported catalog")
        members = faces.get(frame, [])
        expected = int(catalog.get("projected_faces", -1))
        if expected != len(members):
            raise ValueError(
                f"{path}: frame {frame} incomplete catalog: "
                f"expected={expected}, records={len(members)}"
            )
        ordinals = sorted(int(item.get("output_ordinal", -1)) for item in members)
        if ordinals != list(range(expected)):
            raise ValueError(f"{path}: frame {frame} has invalid output ordinals")
        capture = int(catalog.get("capture_id", -1))
        if any(
            int(item.get("face_schema", 0)) != face_schema
            or int(item.get("capture_id", -2)) != capture
            for item in members
        ):
            raise ValueError(f"{path}: frame {frame} has a mixed face catalog")
        result[frame] = catalog, sorted(
            members, key=lambda item: int(item["output_ordinal"])
        )
    unexpected = sorted(set(faces) - set(catalogs))
    if unexpected:
        raise ValueError(f"{path}: face records without catalogs at {unexpected}")
    return result


def _decode_face(item: dict[str, Any], context: str) -> list[tuple[float, float, float, tuple[int, int, int]]]:
    result = []
    for index in range(3):
        raw = tuple(int(item[f"v{index}_{axis}_bits"]) & MASK64 for axis in "xyw")
        values = tuple(
            _double_from_bits(item[f"v{index}_{axis}_bits"], f"{context} v{index} {axis}")
            for axis in "xyw"
        )
        result.append((values[0], values[1], values[2], raw))
    return result


def _row_interval(points: list[tuple[float, float]], sample_y: float) -> tuple[float, float] | None:
    intersections = []
    for index, (x0, y0) in enumerate(points):
        x1, y1 = points[(index + 1) % 3]
        if y0 == y1:
            continue
        low, high = sorted((y0, y1))
        if low <= sample_y < high:
            ratio = (sample_y - y0) / (y1 - y0)
            intersections.append(x0 + ratio * (x1 - x0))
    if len(intersections) < 2:
        return None
    return min(intersections), max(intersections)


def _rasterize(
    triangles: Iterable[list[tuple[float, float]]], width: int, height: int
) -> bytearray:
    mask = bytearray(width * height)
    for points in triangles:
        minimum_y = max(0, math.ceil(min(y for _, y in points) - 0.5))
        maximum_y = min(height - 1, math.floor(max(y for _, y in points) - 0.5))
        for y in range(minimum_y, maximum_y + 1):
            interval = _row_interval(points, y + 0.5)
            if interval is None:
                continue
            first = max(0, math.ceil(interval[0] - 0.5))
            last = min(width - 1, math.floor(interval[1] - 0.5))
            if first <= last:
                begin = y * width + first
                mask[begin : begin + last - first + 1] = b"\xff" * (
                    last - first + 1
                )
    return mask


def _clip_line(
    first: tuple[float, float], second: tuple[float, float], width: int, height: int
) -> tuple[tuple[float, float], tuple[float, float]] | None:
    x0, y0 = first
    dx = second[0] - x0
    dy = second[1] - y0
    low, high = 0.0, 1.0
    for p, q in (
        (-dx, x0),
        (dx, width - 1 - x0),
        (-dy, y0),
        (dy, height - 1 - y0),
    ):
        if p == 0.0:
            if q < 0.0:
                return None
            continue
        ratio = q / p
        if p < 0.0:
            low = max(low, ratio)
        else:
            high = min(high, ratio)
        if low > high:
            return None
    return (x0 + low * dx, y0 + low * dy), (x0 + high * dx, y0 + high * dy)


def _mark_line(mask: bytearray, width: int, height: int, line: tuple[tuple[float, float], tuple[float, float]]) -> None:
    (x0, y0), (x1, y1) = line
    steps = max(1, math.ceil(max(abs(x1 - x0), abs(y1 - y0))))
    for step in range(steps + 1):
        ratio = step / steps
        x = min(width - 1, max(0, round(x0 + (x1 - x0) * ratio)))
        y = min(height - 1, max(0, round(y0 + (y1 - y0) * ratio)))
        mask[y * width + x] = 255


def _read_ppm(path: Path) -> tuple[int, int, bytearray]:
    data = path.read_bytes()
    tokens: list[bytes] = []
    cursor = 0
    while len(tokens) < 4:
        while cursor < len(data) and chr(data[cursor]).isspace():
            cursor += 1
        if cursor < len(data) and data[cursor] == ord("#"):
            cursor = data.find(b"\n", cursor)
            if cursor < 0:
                raise ValueError(f"{path}: truncated PPM comment")
            continue
        end = cursor
        while end < len(data) and not chr(data[end]).isspace():
            end += 1
        if end == cursor:
            raise ValueError(f"{path}: truncated PPM header")
        tokens.append(data[cursor:end])
        cursor = end
    if tokens[0] != b"P6" or int(tokens[3]) != 255:
        raise ValueError(f"{path}: expected binary RGB8 PPM")
    if cursor >= len(data) or not chr(data[cursor]).isspace():
        raise ValueError(f"{path}: missing PPM payload separator")
    if data[cursor : cursor + 2] == b"\r\n":
        cursor += 2
    else:
        cursor += 1
    width, height = int(tokens[1]), int(tokens[2])
    pixels = bytearray(data[cursor:])
    if len(pixels) != width * height * 3:
        raise ValueError(f"{path}: PPM payload size mismatch")
    return width, height, pixels


def _write_pgm(path: Path, width: int, height: int, pixels: bytearray) -> None:
    path.write_bytes(f"P5\n{width} {height}\n255\n".encode() + pixels)


def _write_ppm(path: Path, width: int, height: int, pixels: bytearray) -> None:
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode() + pixels)


def _region_stats(
    coverage: bytearray,
    color: bytearray | None,
    width: int,
    height: int,
    first_x: int,
    last_x: int,
    black_threshold: int,
) -> dict[str, int]:
    result = collections.Counter()
    for y in range(height):
        for x in range(first_x, last_x):
            pixel = y * width + x
            covered = coverage[pixel] != 0
            result["pixels"] += 1
            result["projected_covered_pixels" if covered else "projected_uncovered_pixels"] += 1
            if color is not None:
                black = max(color[pixel * 3 : pixel * 3 + 3]) <= black_threshold
                key = ("black" if black else "nonblack") + (
                    "_inside_projection_pixels" if covered else "_outside_projection_pixels"
                )
                result[key] += 1
    return dict(result)


def analyze_catalog(
    path: Path,
    *,
    frame: int | None = None,
    scale: int = 1,
    width: int = 512,
    height: int = 256,
    native_margin: int = 64,
    readback: Path | None = None,
    black_threshold: int = 4,
    background_underlay_scale: float = 4.0 / 3.0,
    coverage_output: Path | None = None,
    overlay_output: Path | None = None,
) -> dict[str, Any]:
    if scale < 1 or width < 2 or height < 2:
        raise ValueError("scale must be positive and the viewport must exceed 1x1")
    if (
        not math.isfinite(background_underlay_scale)
        or background_underlay_scale < 1.0
    ):
        raise ValueError("background underlay scale must be finite and at least one")
    if native_margin < 0 or native_margin * 2 >= width:
        raise ValueError("native margin must leave a non-empty central aperture")
    catalogs = _load_catalog(path)
    if frame is None:
        if len(catalogs) != 1:
            raise ValueError(f"{path}: select --frame from {sorted(catalogs)}")
        frame = next(iter(catalogs))
    if frame not in catalogs:
        raise ValueError(f"{path}: frame {frame} not found")
    catalog, face_records = catalogs[frame]

    decoded = [
        _decode_face(item, f"{path}: frame {frame} face {item['output_ordinal']}")
        for item in face_records
    ]
    triangles = [
        [(vertex[0] * scale, vertex[1] * scale) for vertex in face]
        for face in decoded
    ]
    foreground_triangles = [
        [(vertex[0] * scale, vertex[1] * scale) for vertex in face]
        for item, face in zip(face_records, decoded)
        if int(item.get("resource_flags", 0)) != 1
    ]
    background_triangles = [
        [(vertex[0] * scale, vertex[1] * scale) for vertex in face]
        for item, face in zip(face_records, decoded)
        if int(item.get("resource_flags", 0)) == 1
    ]
    # The renderer draws opaque flags-1 resources twice: the ordinary 16:9
    # projection remains the primary image, while a farther copy restores the
    # source angular span underneath it. Reconstruct that second layer from
    # the same intrinsic resource flag instead of inferring a backdrop from
    # pixel colour. The report can then distinguish an input-topology hole, a
    # hole sealed by the underlay, and one which survives both layers.
    native_center_x = width * 0.5
    background_underlay_triangles = [
        [
            (
                (
                    native_center_x
                    + (vertex[0] - native_center_x)
                    * background_underlay_scale
                )
                * scale,
                vertex[1] * scale,
            )
            for vertex in face
        ]
        for item, face in zip(face_records, decoded)
        if int(item.get("resource_flags", 0)) == 1
        and int(item.get("semi_transparent", 0)) == 0
    ]
    scaled_width, scaled_height = width * scale, height * scale
    coverage = _rasterize(triangles, scaled_width, scaled_height)
    foreground_coverage = _rasterize(
        foreground_triangles, scaled_width, scaled_height
    )
    background_coverage = _rasterize(
        background_triangles, scaled_width, scaled_height
    )
    background_underlay_coverage = _rasterize(
        background_underlay_triangles, scaled_width, scaled_height
    )
    presented_union_coverage = bytearray(
        left | right
        for left, right in zip(coverage, background_underlay_coverage)
    )

    edge_owners: dict[
        tuple[tuple[int, int, int], tuple[int, int, int]], list[tuple[int, int]]
    ] = collections.defaultdict(list)
    edge_points: dict[
        tuple[tuple[int, int, int], tuple[int, int, int]],
        tuple[tuple[float, float], tuple[float, float]],
    ] = {}
    for face_index, face in enumerate(decoded):
        for corner in range(3):
            following = (corner + 1) % 3
            endpoints = (face[corner][3], face[following][3])
            key = tuple(sorted(endpoints))
            edge_owners[key].append((face_index, corner))
            edge_points[key] = (
                (face[corner][0] * scale, face[corner][1] * scale),
                (face[following][0] * scale, face[following][1] * scale),
            )
    boundary_mask = bytearray(scaled_width * scaled_height)
    visible_boundary_edges = 0
    side_boundary_edges = 0
    boundary_worlds: collections.Counter[int] = collections.Counter()
    for key, owners in edge_owners.items():
        if len(owners) != 1:
            continue
        clipped = _clip_line(*edge_points[key], scaled_width, scaled_height)
        if clipped is None:
            continue
        visible_boundary_edges += 1
        face_index, _ = owners[0]
        boundary_worlds[int(face_records[face_index]["world"])] += 1
        if min(clipped[0][0], clipped[1][0]) < native_margin * scale or max(
            clipped[0][0], clipped[1][0]
        ) >= (width - native_margin) * scale:
            side_boundary_edges += 1
        _mark_line(boundary_mask, scaled_width, scaled_height, clipped)

    color = None
    if readback is not None:
        image_width, image_height, color = _read_ppm(readback)
        if (image_width, image_height) != (scaled_width, scaled_height):
            raise ValueError(
                f"{readback}: extent {image_width}x{image_height}, expected "
                f"{scaled_width}x{scaled_height}"
            )
    if coverage_output is not None:
        _write_pgm(coverage_output, scaled_width, scaled_height, coverage)
    if overlay_output is not None:
        if color is None:
            raise ValueError("--overlay-output requires --readback")
        overlay = bytearray(len(color))
        for pixel in range(scaled_width * scaled_height):
            rgb = color[pixel * 3 : pixel * 3 + 3]
            black = max(rgb) <= black_threshold
            if boundary_mask[pixel]:
                output = (0, 255, 255)
            elif black and not presented_union_coverage[pixel]:
                output = (255, 32, 32)
            elif black and not foreground_coverage[pixel]:
                output = (255, 32, 255)
            elif black:
                output = (255, 160, 32)
            else:
                output = tuple(max(16, channel // 2) for channel in rgb)
            overlay[pixel * 3 : pixel * 3 + 3] = bytes(output)
        _write_ppm(overlay_output, scaled_width, scaled_height, overlay)

    bands = {
        "left_widescreen": (0, native_margin * scale),
        "native_core": (native_margin * scale, (width - native_margin) * scale),
        "right_widescreen": ((width - native_margin) * scale, scaled_width),
        "complete": (0, scaled_width),
    }
    return {
        "schema": REPORT_SCHEMA,
        "source": str(path),
        "frame": frame,
        "capture_id": int(catalog["capture_id"]),
        "topology_fingerprint": int(catalog["topology_fingerprint"]) & MASK64,
        "viewport": {
            "width": scaled_width,
            "height": scaled_height,
            "scale": scale,
            "native_margin": native_margin * scale,
        },
        "catalog": {
            "source_faces": int(catalog["source_faces"]),
            "resident_faces": int(catalog["resident_faces"]),
            "projected_faces": len(face_records),
            "near_generated_faces": int(catalog["near_generated_faces"]),
            "near_rejected_faces": int(catalog["near_rejected_faces"]),
            "source_selected_projected_faces": sum(
                int(item["source_selected"]) for item in face_records
            ),
            "semitransparent_projected_faces": sum(
                int(item["semi_transparent"]) for item in face_records
            ),
            "retained_projected_faces": sum(
                int(item.get("retained_resource", 0)) for item in face_records
            ),
            "opaque_background_underlay_faces": len(
                background_underlay_triangles
            ),
        },
        "projected_topology": {
            "unique_exact_edges": len(edge_owners),
            "nonmanifold_exact_edges": sum(
                len(owners) > 2 for owners in edge_owners.values()
            ),
            "exact_boundary_edges": sum(
                len(owners) == 1 for owners in edge_owners.values()
            ),
            "visible_exact_boundary_edges": visible_boundary_edges,
            "widescreen_band_boundary_edges": side_boundary_edges,
            "visible_boundary_edges_by_world": {
                str(world): count for world, count in sorted(boundary_worlds.items())
            },
        },
        "coverage": {
            name: _region_stats(
                coverage,
                color,
                scaled_width,
                scaled_height,
                first,
                last,
                black_threshold,
            )
            for name, (first, last) in bands.items()
        },
        "foreground_coverage": {
            name: _region_stats(
                foreground_coverage,
                color,
                scaled_width,
                scaled_height,
                first,
                last,
                black_threshold,
            )
            for name, (first, last) in bands.items()
        },
        "background_coverage": {
            name: _region_stats(
                background_coverage,
                color,
                scaled_width,
                scaled_height,
                first,
                last,
                black_threshold,
            )
            for name, (first, last) in bands.items()
        },
        "background_underlay_coverage": {
            name: _region_stats(
                background_underlay_coverage,
                color,
                scaled_width,
                scaled_height,
                first,
                last,
                black_threshold,
            )
            for name, (first, last) in bands.items()
        },
        "presented_union_coverage": {
            name: _region_stats(
                presented_union_coverage,
                color,
                scaled_width,
                scaled_height,
                first,
                last,
                black_threshold,
            )
            for name, (first, last) in bands.items()
        },
        "background_underlay_scale": background_underlay_scale,
        "qualification": (
            "Coverage is computed from every projected resident sidecar triangle "
            "before Vulkan. The presented union additionally contains the opaque "
            "flags-1 far underlay reconstructed from its intrinsic resource flag. "
            "Black pixels outside that union prove neither resident layer can reach "
            "those samples. In the overlay red marks that case, magenta marks "
            "black samples covered only by a background layer, and orange marks "
            "black samples also covered by ordinary scenery; the latter two still "
            "need depth/material/discard diagnosis. Exact boundary edges are welded by complete "
            "projected X/Y/W identity, without object labels or fitted screen "
            "coordinates."
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("catalog", type=Path)
    parser.add_argument("--frame", type=int)
    parser.add_argument("--scale", type=int, default=1)
    parser.add_argument("--width", type=int, default=512)
    parser.add_argument("--height", type=int, default=256)
    parser.add_argument("--native-margin", type=int, default=64)
    parser.add_argument("--readback", type=Path)
    parser.add_argument("--black-threshold", type=int, default=4)
    parser.add_argument(
        "--background-underlay-scale", type=float, default=4.0 / 3.0
    )
    parser.add_argument("--coverage-output", type=Path)
    parser.add_argument("--overlay-output", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--compact", action="store_true")
    args = parser.parse_args()
    try:
        report = analyze_catalog(
            args.catalog,
            frame=args.frame,
            scale=args.scale,
            width=args.width,
            height=args.height,
            native_margin=args.native_margin,
            readback=args.readback,
            black_threshold=args.black_threshold,
            background_underlay_scale=args.background_underlay_scale,
            coverage_output=args.coverage_output,
            overlay_output=args.overlay_output,
        )
    except (OSError, ValueError) as exc:
        print(f"crash3_resident_coverage: {exc}", file=sys.stderr)
        return 2
    text = json.dumps(report, indent=None if args.compact else 2, sort_keys=True)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
