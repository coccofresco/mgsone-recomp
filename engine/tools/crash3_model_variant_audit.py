#!/usr/bin/env python3
"""Classify Crash 3 object-model changes without guessing from face count.

The Vulkan sidecar traces only renderer nodes that emitted a certified packet.
This audit compares consecutive captures for each object and separates material
program changes, animation topology changes, resource switches and multipart
composition.  A resource with more faces is deliberately never called a higher
LOD unless a future intrinsic correspondence witness explicitly proves it.
"""

from __future__ import annotations

import argparse
import collections
import json
import sys
from pathlib import Path
from typing import Any


TRACE_SCHEMA = "ps1trace/v1"
REPORT_SCHEMA = "ps1-crash3-model-variant-audit/v2"
VARIANT_EVENT = "game.crash3.object_model_variant"
PART_EVENT = "game.crash3.object_model_part"

IDENTITY_FIELDS = (
    "signature",
    "program_signature",
    "logical_model_resource",
    "renderer_model_resource",
    "animation_sequence",
    "animation_frame",
    "render_selector",
    "current_frame_resource",
    "next_frame_resource",
    "command_table_resource",
    "faces",
    "parts",
)

ANIMATION_FIELDS = (
    "animation_sequence",
    "animation_frame",
    "current_frame_resource",
    "next_frame_resource",
    "command_table_resource",
)

TEMPORAL_VERTEX_RENDERER = 0x8003F460

PART_FIELDS = (
    "part",
    "renderer_node",
    "logical_model_resource",
    "renderer_model_resource",
    "renderer_program_fingerprint",
    "renderer_geometry_fingerprint",
    "projection_transform_fingerprint",
    "animation_sequence",
    "animation_frame",
    "render_selector",
    "current_frame_resource",
    "next_frame_resource",
    "command_table_resource",
    "material_table_resource",
    "vertex_renderer",
    "scale_shift",
    "faces",
)


def _variant_key(record: dict[str, Any]) -> tuple[int, ...]:
    return tuple(int(record.get(field, 0)) for field in IDENTITY_FIELDS)


def _brief(record: dict[str, Any]) -> dict[str, int]:
    return {field: int(record.get(field, 0)) for field in IDENTITY_FIELDS}


def _brief_part(record: dict[str, Any]) -> dict[str, int]:
    return {field: int(record.get(field, 0)) for field in PART_FIELDS}


def _part_pose_key(record: dict[str, Any]) -> tuple[int, ...]:
    vertex_renderer = int(record.get("vertex_renderer", 0))
    return (
        int(record.get("animation_sequence", 0)),
        int(record.get("animation_frame", 0)),
        int(record.get("current_frame_resource", 0)),
        (
            int(record.get("next_frame_resource", 0))
            if vertex_renderer == TEMPORAL_VERTEX_RENDERER
            else 0
        ),
        vertex_renderer,
    )


def _classify_simultaneous_parts(parts: list[dict[str, Any]]) -> str:
    if len(parts) < 2:
        return "single_part"
    logical_models = {int(item.get("logical_model_resource", 0)) for item in parts}
    renderer_models = {
        int(item.get("renderer_model_resource", 0)) for item in parts
    }
    transforms = {
        (
            int(item.get("projection_transform_fingerprint", 0)),
            int(item.get("scale_shift", -1)),
        )
        for item in parts
    }
    poses = {_part_pose_key(item) for item in parts}
    programs = {
        (
            int(item.get("renderer_model_resource", 0)),
            int(item.get("renderer_program_fingerprint", 0)),
            int(item.get("renderer_geometry_fingerprint", 0)),
        )
        for item in parts
    }
    if (
        len(logical_models) == 1
        and len(renderer_models) > 1
        and len(transforms) == 1
        and len(poses) == 1
    ):
        return "lod_candidate"
    if len(transforms) > 1:
        return "multipart_transform"
    if len(programs) == 1:
        return "duplicate_or_handoff"
    return "multipart_unclassified"


def _load_trace(
    path: Path,
) -> tuple[
    list[tuple[int, int, dict[int, dict[str, Any]]]],
    dict[tuple[int, int], dict[int, list[dict[str, Any]]]],
]:
    captures: dict[tuple[int, int], dict[int, dict[str, Any]]] = collections.defaultdict(dict)
    parts: dict[
        tuple[int, int], dict[int, list[dict[str, Any]]]
    ] = collections.defaultdict(lambda: collections.defaultdict(list))
    with path.open(encoding="utf-8") as stream:
        for lineno, line in enumerate(stream, 1):
            if VARIANT_EVENT not in line and PART_EVENT not in line:
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
            event = record.get("event")
            if event not in (VARIANT_EVENT, PART_EVENT):
                continue
            frame = int(record.get("frame", -1))
            capture = int(record.get("capture_id", -1))
            object_address = int(record.get("object", 0))
            if frame < 0 or capture < 0 or object_address == 0:
                raise ValueError(
                    f"{path}:{lineno}: variant lacks frame, capture or object identity"
                )
            if int(record.get("faces", 0)) <= 0:
                raise ValueError(f"{path}:{lineno}: empty object-model record")
            if event == PART_EVENT:
                part = int(record.get("part", -1))
                renderer_node = int(record.get("renderer_node", 0))
                if part < 0 or renderer_node == 0:
                    raise ValueError(
                        f"{path}:{lineno}: model part lacks ordinal or renderer node"
                    )
                object_parts = parts[(frame, capture)][object_address]
                if any(int(item["part"]) == part for item in object_parts):
                    raise ValueError(
                        f"{path}:{lineno}: duplicate part {part} for object "
                        f"0x{object_address:08x} in one capture"
                    )
                object_parts.append(record)
                continue
            if int(record.get("parts", 0)) <= 0:
                raise ValueError(f"{path}:{lineno}: empty object-model variant")
            capture_variants = captures[(frame, capture)]
            previous = capture_variants.get(object_address)
            if previous is not None:
                if _variant_key(previous) != _variant_key(record):
                    raise ValueError(
                        f"{path}:{lineno}: object 0x{object_address:08x} has "
                        "conflicting variants in one capture"
                    )
                raise ValueError(
                    f"{path}:{lineno}: duplicate object 0x{object_address:08x} "
                    "in one capture"
                )
            capture_variants[object_address] = record
    if not captures:
        raise ValueError(f"{path}: no {VARIANT_EVENT} records")
    ordered = [
        (frame, capture, variants)
        for (frame, capture), variants in sorted(
            captures.items(), key=lambda item: (item[0][1], item[0][0])
        )
    ]
    for capture_parts in parts.values():
        for object_parts in capture_parts.values():
            object_parts.sort(key=lambda item: int(item["part"]))
    return ordered, parts


def _classify(previous: dict[str, Any], current: dict[str, Any]) -> str:
    if int(previous.get("parts", 0)) != 1 or int(current.get("parts", 0)) != 1:
        return "multipart_composition"
    same_geometry = int(previous["signature"]) == int(current["signature"])
    same_program = int(previous["program_signature"]) == int(
        current["program_signature"]
    )
    if same_geometry and same_program:
        return "unchanged"
    if same_geometry:
        return "program_only"
    same_model_resources = all(
        int(previous[field]) == int(current[field])
        for field in ("logical_model_resource", "renderer_model_resource")
    )
    animation_changed = any(
        int(previous[field]) != int(current[field]) for field in ANIMATION_FIELDS
    )
    if same_model_resources and animation_changed:
        return "animation_topology"
    if not same_model_resources:
        return "model_resource_switch"
    return "unclassified_topology"


def analyze_model_variants(path: Path) -> dict[str, Any]:
    captures, parts_by_capture = _load_trace(path)
    counts: collections.Counter[str] = collections.Counter()
    transitions: list[dict[str, Any]] = []
    previous_objects: dict[int, dict[str, Any]] = {}
    previous_frame: int | None = None
    previous_capture: int | None = None
    observed_objects: set[int] = set()
    multipart_objects: set[int] = set()
    part_records = 0
    part_capture_objects = 0
    simultaneous_classifications: collections.Counter[str] = collections.Counter()
    simultaneous_candidates: list[dict[str, Any]] = []
    logical_renderer_mismatches: list[dict[str, Any]] = []

    for frame, capture, current_objects in captures:
        capture_parts = parts_by_capture.get((frame, capture), {})
        for object_address, object_parts in sorted(capture_parts.items()):
            part_records += len(object_parts)
            part_capture_objects += 1
            aggregate = current_objects.get(object_address)
            if aggregate is None:
                raise ValueError(
                    f"{path}: part records for object 0x{object_address:08x} "
                    f"at capture {capture} have no aggregate variant"
                )
            expected_parts = int(aggregate.get("parts", 0))
            expected_faces = int(aggregate.get("faces", 0))
            if len(object_parts) != expected_parts:
                raise ValueError(
                    f"{path}: object 0x{object_address:08x} at capture {capture} "
                    f"has {len(object_parts)} part records, expected {expected_parts}"
                )
            observed_faces = sum(int(item.get("faces", 0)) for item in object_parts)
            if observed_faces != expected_faces:
                raise ValueError(
                    f"{path}: object 0x{object_address:08x} at capture {capture} "
                    f"has {observed_faces} part faces, expected {expected_faces}"
                )
            classification = _classify_simultaneous_parts(object_parts)
            simultaneous_classifications[classification] += 1
            if classification != "single_part":
                candidate = {
                    "classification": classification,
                    "frame": frame,
                    "capture_id": capture,
                    "object": object_address,
                    "parts": [_brief_part(item) for item in object_parts],
                }
                if classification == "lod_candidate":
                    simultaneous_candidates.append(candidate)
            for item in object_parts:
                logical = int(item.get("logical_model_resource", 0))
                renderer = int(item.get("renderer_model_resource", 0))
                if logical != renderer:
                    logical_renderer_mismatches.append(
                        {
                            "frame": frame,
                            "capture_id": capture,
                            "object": object_address,
                            "part": int(item.get("part", -1)),
                            "logical_model_resource": logical,
                            "renderer_model_resource": renderer,
                        }
                    )
        current_ids = set(current_objects)
        previous_ids = set(previous_objects)
        observed_objects.update(current_ids)
        multipart_objects.update(
            object_address
            for object_address, variant in current_objects.items()
            if int(variant.get("parts", 0)) > 1
        )

        counts["entries"] += len(current_ids - previous_ids)
        counts["exits"] += len(previous_ids - current_ids)
        for object_address in sorted(current_ids & previous_ids):
            previous = previous_objects[object_address]
            current = current_objects[object_address]
            classification = _classify(previous, current)
            counts[classification] += 1
            if classification == "unchanged":
                continue
            transitions.append(
                {
                    "classification": classification,
                    "object": object_address,
                    "previous_frame": previous_frame,
                    "frame": frame,
                    "previous_capture_id": previous_capture,
                    "capture_id": capture,
                    "previous": _brief(previous),
                    "current": _brief(current),
                    "face_delta": int(current["faces"]) - int(previous["faces"]),
                }
            )
        previous_objects = current_objects
        previous_frame = frame
        previous_capture = capture

    resource_switches = [
        item for item in transitions if item["classification"] == "model_resource_switch"
    ]
    return {
        "schema": REPORT_SCHEMA,
        "source": str(path),
        "captures": len(captures),
        "objects": len(observed_objects),
        "entries": counts["entries"],
        "exits": counts["exits"],
        "unchanged_comparisons": counts["unchanged"],
        "program_only_transitions": counts["program_only"],
        "animation_topology_transitions": counts["animation_topology"],
        "model_resource_switches": counts["model_resource_switch"],
        "multipart_composition_transitions": counts["multipart_composition"],
        "unclassified_topology_transitions": counts["unclassified_topology"],
        "multipart_objects": len(multipart_objects),
        "part_records": part_records,
        "part_capture_objects": part_capture_objects,
        "part_instrumentation_present": bool(parts_by_capture),
        "simultaneous_single_part": simultaneous_classifications["single_part"],
        "simultaneous_duplicate_or_handoff": simultaneous_classifications[
            "duplicate_or_handoff"
        ],
        "simultaneous_multipart_transform": simultaneous_classifications[
            "multipart_transform"
        ],
        "simultaneous_multipart_unclassified": simultaneous_classifications[
            "multipart_unclassified"
        ],
        "simultaneous_lod_candidates": len(simultaneous_candidates),
        "simultaneous_lod_candidate_records": simultaneous_candidates,
        "logical_renderer_resource_mismatches": len(logical_renderer_mismatches),
        "logical_renderer_resource_mismatch_records": logical_renderer_mismatches,
        "highest_detail_selection_authorized": False,
        "resource_switch_candidates": resource_switches,
        "transitions": transitions,
        "qualification": (
            "Face count is observational only. A simultaneous attached-part "
            "candidate requires equal intrinsic pose/transform and distinct "
            "renderer resources; even then, highest-detail selection remains "
            "disabled until a guest-authored LOD rank or primitive "
            "correspondence is proven."
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--compact", action="store_true")
    args = parser.parse_args()
    try:
        report = analyze_model_variants(args.trace)
    except (OSError, ValueError) as exc:
        print(f"crash3_model_variant_audit: {exc}", file=sys.stderr)
        return 2
    payload = json.dumps(report, indent=None if args.compact else 2, sort_keys=True)
    if args.output:
        args.output.write_text(payload + "\n", encoding="utf-8")
    else:
        print(payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
