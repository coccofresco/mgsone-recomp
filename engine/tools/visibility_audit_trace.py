#!/usr/bin/env python3
"""Validate and compact Vulkan dual-owner visibility traces.

This tool proves only that the emitted per-frame classification is internally
complete and non-empty when requested. It deliberately does not cluster owner
pairs, infer persistent objects, cap the number of families, or fit/round a
depth factor.
"""

from __future__ import annotations

import argparse
import collections
import json
import sys
from pathlib import Path
from typing import Any


TRACE_SCHEMA = "ps1trace/v1"
REPORT_SCHEMA = "ps1-visibility-audit-summary/v1"
AUDIT_EVENT = "replay.visibility_audit"
CONFLICT_EVENT = "replay.visibility_conflict"


def _integer(record: dict[str, Any], field: str, context: str) -> int:
    value = record.get(field)
    if not isinstance(value, int):
        raise ValueError(f"{context}: missing/non-integer field {field!r}")
    return value


def analyze_visibility_trace(
    path: Path,
    *,
    require_owned_every_frame: bool = False,
    require_conflict_every_owned_frame: bool = False,
) -> dict[str, Any]:
    audits: dict[int, dict[str, Any]] = {}
    conflicts: dict[int, list[dict[str, Any]]] = collections.defaultdict(list)
    relevant_records = 0
    with path.open(encoding="utf-8") as stream:
        for lineno, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{lineno}: invalid JSON: {exc}") from exc
            event = record.get("event")
            if event not in {AUDIT_EVENT, CONFLICT_EVENT}:
                continue
            if record.get("schema") != TRACE_SCHEMA:
                raise ValueError(
                    f"{path}:{lineno}: schema {record.get('schema')!r}, "
                    f"expected {TRACE_SCHEMA!r}"
                )
            frame = _integer(record, "frame", f"{path}:{lineno}")
            relevant_records += 1
            if event == AUDIT_EVENT:
                if frame in audits:
                    raise ValueError(f"{path}:{lineno}: duplicate audit frame {frame}")
                audits[frame] = record
            else:
                conflicts[frame].append(record)

    if not audits:
        raise ValueError(f"{path}: no {AUDIT_EVENT} records")

    errors: list[str] = []
    warnings: list[str] = []
    frame_reports: list[dict[str, Any]] = []
    totals = collections.Counter()
    for frame in sorted(audits):
        audit = audits[frame]
        context = f"frame {frame}"
        names = (
            "status",
            "owners",
            "atlas_x",
            "atlas_y",
            "width",
            "height",
            "samples",
            "agreement",
            "background_agreement",
            "owned_agreement",
            "ot_owned",
            "depth_owned",
            "conflicts",
            "coverage_divergence",
            "indeterminate",
            "inconsistent",
            "conflict_pairs",
        )
        try:
            values = {name: _integer(audit, name, context) for name in names}
        except ValueError as exc:
            errors.append(str(exc))
            continue
        if any(value < 0 for value in values.values()):
            errors.append(f"{context}: negative classification field")
            continue

        width = values["width"]
        height = values["height"]
        expected_samples = width * height
        classified = (
            values["agreement"]
            + values["conflicts"]
            + values["coverage_divergence"]
            + values["indeterminate"]
            + values["inconsistent"]
        )
        if values["status"] != 0:
            errors.append(f"{context}: audit status is {values['status']}, not compared")
        if width == 0 or height == 0 or values["samples"] != expected_samples:
            errors.append(
                f"{context}: region/sample mismatch "
                f"({width}x{height} != {values['samples']})"
            )
        if classified != values["samples"]:
            errors.append(
                f"{context}: classifications sum to {classified}, "
                f"expected {values['samples']}"
            )
        if (
            values["background_agreement"] + values["owned_agreement"]
            != values["agreement"]
        ):
            errors.append(f"{context}: background/owned agreement does not partition")

        frame_conflicts = conflicts.get(frame, [])
        if len(frame_conflicts) != values["conflict_pairs"]:
            errors.append(
                f"{context}: {len(frame_conflicts)} conflict records, "
                f"expected {values['conflict_pairs']}"
            )
        conflict_samples = 0
        directed_pairs: set[tuple[int, int]] = set()
        atlas_right = values["atlas_x"] + width
        atlas_bottom = values["atlas_y"] + height
        for index, conflict in enumerate(frame_conflicts):
            item_context = f"{context} conflict {index}"
            try:
                ot_owner = _integer(conflict, "ot_owner", item_context)
                depth_owner = _integer(conflict, "depth_owner", item_context)
                samples = _integer(conflict, "samples", item_context)
                ot_ordinal = _integer(conflict, "ot_source_ordinal", item_context)
                depth_ordinal = _integer(
                    conflict, "depth_source_ordinal", item_context
                )
                minimum_x = _integer(conflict, "min_x", item_context)
                minimum_y = _integer(conflict, "min_y", item_context)
                maximum_x = _integer(conflict, "max_x", item_context)
                maximum_y = _integer(conflict, "max_y", item_context)
            except ValueError as exc:
                errors.append(str(exc))
                continue
            pair = (ot_owner, depth_owner)
            if pair in directed_pairs:
                errors.append(f"{item_context}: duplicate directed owner pair {pair}")
            directed_pairs.add(pair)
            if (
                ot_owner <= 0
                or depth_owner <= 0
                or ot_owner > values["owners"]
                or depth_owner > values["owners"]
            ):
                errors.append(f"{item_context}: owner token outside catalog")
            if samples <= 0:
                errors.append(f"{item_context}: non-positive sample count")
            else:
                conflict_samples += samples
            if ot_ordinal <= depth_ordinal:
                errors.append(f"{item_context}: OT owner is not later in replay order")
            if not (
                values["atlas_x"] <= minimum_x <= maximum_x < atlas_right
                and values["atlas_y"] <= minimum_y <= maximum_y < atlas_bottom
            ):
                errors.append(f"{item_context}: bounding box outside audit region")
        if conflict_samples != values["conflicts"]:
            errors.append(
                f"{context}: conflict records sum to {conflict_samples}, "
                f"expected {values['conflicts']}"
            )

        owned = values["ot_owned"] > 0 and values["depth_owned"] > 0
        if not owned:
            warnings.append(f"{context}: no owner coverage; result is inconclusive")
        if require_owned_every_frame and not owned:
            errors.append(f"{context}: owned coverage required")
        if require_conflict_every_owned_frame and owned and values["conflicts"] == 0:
            errors.append(f"{context}: conflict required for owned frame")

        frame_reports.append({"frame": frame, "owned": owned, **values})
        for name, value in values.items():
            if name not in {"status", "atlas_x", "atlas_y", "width", "height"}:
                totals[name] += value

    orphan_frames = sorted(set(conflicts) - set(audits))
    if orphan_frames:
        errors.append(f"conflict records without audit frames: {orphan_frames}")

    classifiable_frames = sum(frame["owned"] for frame in frame_reports)
    conflict_frames = sum(frame["conflicts"] > 0 for frame in frame_reports)
    return {
        "schema": REPORT_SCHEMA,
        "valid": not errors,
        "input": str(path),
        "relevant_records": relevant_records,
        "audited_frames": len(frame_reports),
        "classifiable_frames": classifiable_frames,
        "inconclusive_frames": len(frame_reports) - classifiable_frames,
        "conflict_frames": conflict_frames,
        "totals": dict(sorted(totals.items())),
        "frames": frame_reports,
        "errors": errors,
        "warnings": warnings,
        "inference_scope": {
            "local_ot_depth_conflicts_only": True,
            "family_count_inferred": False,
            "scale_factor_inferred": False,
            "persistent_object_identity_inferred": False,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--require-owned-every-frame", action="store_true")
    parser.add_argument(
        "--require-conflict-every-owned-frame", action="store_true"
    )
    args = parser.parse_args()
    try:
        report = analyze_visibility_trace(
            args.trace,
            require_owned_every_frame=args.require_owned_every_frame,
            require_conflict_every_owned_frame=(
                args.require_conflict_every_owned_frame
            ),
        )
    except (OSError, ValueError) as exc:
        print(f"visibility_audit_trace: {exc}", file=sys.stderr)
        return 2
    payload = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(payload, encoding="utf-8")
    else:
        print(payload, end="")
    return 0 if report["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
