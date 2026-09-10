#!/usr/bin/env python3
"""Measure exact 16:9 edge admission from a visibility-owner catalog.

The tool consumes the complete owner-schema-v3 catalog emitted by the Vulkan
visibility audit.  It does not infer a culling rule from pixels.  Instead it
intersects every accepted high-precision triangle with the two requested
viewport edges and reports the covered/gap intervals plus the guest renderer
PCs which supplied those triangles.
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
REPORT_SCHEMA = "ps1-widescreen-edge-trace/v1"
AUDIT_EVENT = "replay.visibility_audit"
CATALOG_EVENT = "replay.visibility_catalog"
SURFACE_CATALOG_EVENT = "replay.presented_surface_catalog"
OWNER_EVENT = "replay.visibility_owner"


def _double_from_bits(value: Any, context: str) -> float:
    try:
        # The runtime serializes the bit-cast through its signed JSON integer
        # carrier. Preserve all 64 payload bits when the sign bit is set.
        bits = int(value) & ((1 << 64) - 1)
        decoded = struct.unpack("<d", struct.pack("<Q", bits))[0]
    except (TypeError, ValueError, struct.error) as exc:
        raise ValueError(f"{context}: invalid binary64 payload {value!r}") from exc
    if not math.isfinite(decoded):
        raise ValueError(f"{context}: non-finite binary64 payload")
    return decoded


def _load(path: Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
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
            if record.get("event") in {
                AUDIT_EVENT,
                CATALOG_EVENT,
                SURFACE_CATALOG_EVENT,
                OWNER_EVENT,
            }:
                records.append(record)
    if not records:
        raise ValueError(f"{path}: no visibility catalog records")
    return records


def _triangle_edge_interval(
    points: list[tuple[float, float]], edge_x: float, y_min: float, y_max: float
) -> tuple[float, float] | None:
    epsilon = 1e-9
    intersections: list[float] = []
    for index, first in enumerate(points):
        second = points[(index + 1) % len(points)]
        x0, y0 = first
        x1, y1 = second
        on0 = abs(x0 - edge_x) <= epsilon
        on1 = abs(x1 - edge_x) <= epsilon
        if on0:
            intersections.append(y0)
        if on1:
            intersections.append(y1)
        if (x0 < edge_x < x1) or (x1 < edge_x < x0):
            ratio = (edge_x - x0) / (x1 - x0)
            intersections.append(y0 + ratio * (y1 - y0))
    if not intersections:
        return None
    low = max(y_min, min(intersections))
    high = min(y_max, max(intersections))
    if high + epsilon < low:
        return None
    return low, high


def _merge_intervals(
    intervals: Iterable[tuple[float, float]], low: float, high: float
) -> tuple[list[list[float]], list[list[float]]]:
    merged: list[list[float]] = []
    for first, last in sorted(intervals):
        first = max(low, first)
        last = min(high, last)
        if last < first:
            continue
        if merged and first <= merged[-1][1] + 1e-7:
            merged[-1][1] = max(merged[-1][1], last)
        else:
            merged.append([first, last])

    gaps: list[list[float]] = []
    cursor = low
    for first, last in merged:
        if first > cursor + 1e-7:
            gaps.append([cursor, first])
        cursor = max(cursor, last)
    if cursor < high - 1e-7:
        gaps.append([cursor, high])
    return merged, gaps


def _rounded_intervals(intervals: list[list[float]]) -> list[list[float]]:
    return [[round(first, 6), round(last, 6)] for first, last in intervals]


def _edge_report(
    owners: list[dict[str, Any]],
    triangles: dict[int, list[tuple[float, float]]],
    edge_x: float,
    y_min: float,
    y_max: float,
) -> dict[str, Any]:
    intervals: list[tuple[float, float]] = []
    groups: dict[tuple[int, ...], list[dict[str, Any]]] = collections.defaultdict(list)
    for owner in owners:
        interval = _triangle_edge_interval(triangles[id(owner)], edge_x, y_min, y_max)
        if interval is None:
            continue
        intervals.append(interval)
        pcs = tuple(sorted({int(owner[f"v{i}_game_profile_gte_pc"]) for i in range(3)}))
        groups[pcs].append(owner)

    merged, gaps = _merge_intervals(intervals, y_min, y_max)
    grouped = []
    for pcs, members in sorted(groups.items(), key=lambda item: (-len(item[1]), item[0])):
        grouped.append(
            {
                "gte_pcs": [f"0x{pc:08X}" for pc in pcs],
                "owners": len(members),
                "dma_nodes": len(
                    {int(item["v0_dma_node"]) for item in members}
                ),
                "transform_lineages": len(
                    {int(item["v0_transform_lineage"]) for item in members}
                ),
                "object_sources": sorted(
                    {
                        f"0x{int(item['v0_game_profile_object_source']):08X}"
                        for item in members
                    }
                ),
            }
        )
    return {
        "x": edge_x,
        "owners": len(intervals),
        "covered_length": round(sum(last - first for first, last in merged), 6),
        "covered_intervals": _rounded_intervals(merged),
        "gap_intervals": _rounded_intervals(gaps),
        "groups": grouped,
    }


def analyze_catalog(
    path: Path,
    *,
    page_x: float,
    page_y: float = 0.0,
    width: float = 512.0,
    height: float = 256.0,
) -> dict[str, Any]:
    if width <= 1 or height <= 1:
        raise ValueError("viewport width and height must both exceed one")
    records = _load(path)
    audits = {int(item["frame"]): item for item in records if item["event"] == AUDIT_EVENT}
    visibility_catalogs = {
        int(item["frame"]): item for item in records if item["event"] == CATALOG_EVENT
    }
    surface_catalogs = {
        int(item["frame"]): item
        for item in records
        if item["event"] == SURFACE_CATALOG_EVENT
    }
    if visibility_catalogs and surface_catalogs:
        raise ValueError(
            f"{path}: contains both visibility and presented-surface catalogs; "
            "capture one complete owner domain at a time"
        )
    catalogs = visibility_catalogs or surface_catalogs
    catalog_event = CATALOG_EVENT if visibility_catalogs else SURFACE_CATALOG_EVENT
    owners_by_frame: dict[int, list[dict[str, Any]]] = collections.defaultdict(list)
    for item in records:
        if item["event"] == OWNER_EVENT:
            owners_by_frame[int(item["frame"])].append(item)
    if not catalogs:
        raise ValueError(f"{path}: missing {CATALOG_EVENT}")

    frames = []
    for frame, catalog in sorted(catalogs.items()):
        if frame not in audits:
            raise ValueError(f"{path}: frame {frame} has no {AUDIT_EVENT}")
        if int(catalog.get("catalog_schema", 0)) != 1:
            raise ValueError(f"{path}: frame {frame} is not catalog schema 1")
        if int(catalog.get("owner_schema", 0)) < 3:
            raise ValueError(f"{path}: frame {frame} requires owner schema 3")
        owners = owners_by_frame.get(frame, [])
        if catalog_event == CATALOG_EVENT:
            emitted = int(catalog.get("emitted_owners", -1))
            eligible = int(catalog.get("eligible_owners", -1))
        else:
            emitted = int(catalog.get("eligible_surface_owners", -1))
            eligible = emitted
        if emitted != len(owners) or eligible != emitted:
            raise ValueError(
                f"{path}: frame {frame} incomplete catalog: eligible={eligible}, "
                f"emitted={emitted}, records={len(owners)}"
            )
        owner_ids = [int(item["owner"]) for item in owners]
        if len(set(owner_ids)) != len(owner_ids):
            raise ValueError(f"{path}: frame {frame} has duplicate owner tokens")

        triangles: dict[int, list[tuple[float, float]]] = {}
        pc_counts: collections.Counter[int] = collections.Counter()
        x_values: list[float] = []
        for owner in owners:
            if int(owner.get("owner_schema", 0)) < 3:
                raise ValueError(f"{path}: frame {frame} contains a legacy owner")
            points = []
            for index in range(3):
                x = _double_from_bits(
                    owner.get(f"v{index}_precise_x_bits"),
                    f"{path}: frame {frame} owner {owner['owner']} v{index} x",
                ) - page_x
                y = _double_from_bits(
                    owner.get(f"v{index}_precise_y_bits"),
                    f"{path}: frame {frame} owner {owner['owner']} v{index} y",
                ) - page_y
                points.append((x, y))
                x_values.append(x)
                pc_counts[int(owner[f"v{index}_game_profile_gte_pc"])] += 1
            triangles[id(owner)] = points

        y_min = 0.0
        y_max = height - 1.0
        frame_report = {
            "frame": frame,
            "catalog_event": catalog_event,
            "catalog_owners": len(owners),
            "conflict_referenced_owners": int(catalog.get("conflict_referenced_owners", 0)),
            "semi_transparent_owners": int(
                catalog.get("eligible_semi_transparent_owners", 0)
            ),
            "audit_region": {
                key: int(audits[frame][key])
                for key in ("atlas_x", "atlas_y", "width", "height")
            },
            "precise_x_range": [round(min(x_values), 6), round(max(x_values), 6)],
            "vertex_gte_pc_counts": {
                f"0x{pc:08X}": count for pc, count in sorted(pc_counts.items())
            },
            "left": _edge_report(owners, triangles, 0.0, y_min, y_max),
            "right": _edge_report(
                owners, triangles, width - 1.0, y_min, y_max
            ),
        }
        frames.append(frame_report)

    unexpected_owner_frames = sorted(set(owners_by_frame) - set(catalogs))
    if unexpected_owner_frames:
        raise ValueError(
            f"{path}: owner records without catalogs at frames "
            f"{unexpected_owner_frames}"
        )
    return {
        "schema": REPORT_SCHEMA,
        "source": str(path),
        "viewport": {
            "page_x": page_x,
            "page_y": page_y,
            "width": width,
            "height": height,
        },
        "frames": frames,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("catalog", type=Path)
    parser.add_argument("--page-x", type=float, required=True)
    parser.add_argument("--page-y", type=float, default=0.0)
    parser.add_argument("--width", type=float, default=512.0)
    parser.add_argument("--height", type=float, default=256.0)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--compact", action="store_true")
    args = parser.parse_args()
    try:
        report = analyze_catalog(
            args.catalog,
            page_x=args.page_x,
            page_y=args.page_y,
            width=args.width,
            height=args.height,
        )
    except (OSError, ValueError) as exc:
        print(f"widescreen_edge_trace: {exc}", file=sys.stderr)
        return 2
    text = json.dumps(report, indent=None if args.compact else 2, sort_keys=True)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
