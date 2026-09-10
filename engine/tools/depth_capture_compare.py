#!/usr/bin/env python3
"""Fail-closed equivalence check for two-pass depth-provenance captures.

The light pass is the timing oracle.  The instrumented pass may run later from
the same checkpoint and at different absolute VBlanks, but it is accepted only
when its GTE, GP0 and DMA/OT stream is otherwise identical.  Process-local
sequence, projection and submission IDs are replaced with first-use ordinals;
guest values, packet words, event order and OT topology remain part of the
comparison. Absolute or relative host VBlank placement is deliberately not a
heavy-pass invariant: the instruction DAG can stretch one guest render across
more host ticks, which is why the light pass remains the timing oracle.
"""

from __future__ import annotations

import argparse
import collections
import json
import sys
from pathlib import Path
from typing import Any


SCHEMA = "ps1trace/v1"
REPORT_SCHEMA = "ps1-depth-capture-equivalence/v1"
RELEVANT_EVENTS = {"gte.projection", "gpu.gp0_word", "gpu.dma_node"}
TRACE_METADATA = {"schema", "source", "seq", "host_us", "frame", "event"}
VALUE_FLOW_FIELDS = {
    "value_flow_complete",
    "model_fingerprint_complete",
    "model_source_fingerprint",
    "model_program_fingerprint",
    "vxy_expr",
    "vz_expr",
    "rt0_expr",
    "rt1_expr",
    "rt2_expr",
    "rt3_expr",
    "rt4_expr",
    "tr0_expr",
    "tr1_expr",
    "tr2_expr",
}
GP0_VALUE_FLOW_FIELDS = {
    "model_source_fingerprint",
    "model_program_fingerprint",
}
MODEL_VALUE_FLOW_PROVENANCE = 1 << 7


def read_trace(path: Path) -> tuple[list[dict[str, Any]], int]:
    records: list[dict[str, Any]] = []
    value_expressions = 0
    with path.open(encoding="utf-8") as stream:
        for lineno, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{lineno}: invalid JSON: {exc}") from exc
            if record.get("schema") != SCHEMA:
                raise ValueError(
                    f"{path}:{lineno}: schema {record.get('schema')!r}, "
                    f"expected {SCHEMA!r}"
                )
            event = record.get("event")
            if event == "mips.value_expr":
                value_expressions += 1
            elif event in RELEVANT_EVENTS:
                records.append(record)
    if not records:
        raise ValueError(f"{path}: no GTE/GP0/DMA records")
    return records, value_expressions


def _projection_key(kind: str, ordinal: int) -> str:
    return f"{kind}:{ordinal}"


def canonicalize(
    path: Path, records: list[dict[str, Any]], value_expressions: int
) -> tuple[list[dict[str, Any]], dict[str, Any], list[str]]:
    errors: list[str] = []
    projections = [r for r in records if r["event"] == "gte.projection"]
    captured_ids: dict[int, int] = {}
    for ordinal, record in enumerate(projections):
        projection_id = int(record.get("projection_id", 0))
        if projection_id == 0:
            errors.append(f"GTE projection {ordinal} has zero projection_id")
        elif projection_id in captured_ids:
            errors.append(
                f"duplicate GTE projection_id {projection_id} at ordinals "
                f"{captured_ids[projection_id]} and {ordinal}"
            )
        else:
            captured_ids[projection_id] = ordinal

    external_ids: dict[int, int] = {}
    submission_ids: dict[int, int] = {}

    def projection_label(raw: Any) -> str:
        projection_id = int(raw or 0)
        if projection_id == 0:
            return "none"
        if projection_id in captured_ids:
            return _projection_key("captured", captured_ids[projection_id])
        if projection_id not in external_ids:
            external_ids[projection_id] = len(external_ids)
        return _projection_key("external", external_ids[projection_id])

    def submission_label(raw: Any) -> int:
        submission_id = int(raw or 0)
        if submission_id not in submission_ids:
            submission_ids[submission_id] = len(submission_ids)
        return submission_ids[submission_id]

    frames = [int(record.get("frame", 0)) for record in records]
    base_frame = min(frames)
    canonical: list[dict[str, Any]] = []
    for record in records:
        event = record["event"]
        ignored = set(TRACE_METADATA)
        if event == "gte.projection":
            ignored |= VALUE_FLOW_FIELDS | {"projection_id"}
        elif event == "gpu.gp0_word":
            ignored |= GP0_VALUE_FLOW_FIELDS | {
                "projection_id", "submission_id"
            }
        elif event == "gpu.dma_node":
            ignored |= {"submission_id"}

        fields = {}
        for key in sorted(record):
            if key in ignored:
                continue
            value = record[key]
            if event == "gpu.gp0_word" and key == "intrinsic_provenance":
                # The heavy pass alone certifies this diagnostic bit. Preserve
                # every ordinary provenance bit in the scene comparison.
                value = int(value) & ~MODEL_VALUE_FLOW_PROVENANCE
            fields[key] = value
        item: dict[str, Any] = {
            "event": event,
            "fields": fields,
        }
        if event == "gte.projection":
            item["projection"] = projection_label(record.get("projection_id", 0))
        elif event == "gpu.gp0_word":
            item["projection"] = projection_label(record.get("projection_id", 0))
            item["submission"] = submission_label(record.get("submission_id", 0))
        elif event == "gpu.dma_node":
            item["submission"] = submission_label(record.get("submission_id", 0))
        canonical.append(item)

    event_counts = collections.Counter(record["event"] for record in records)
    complete_projections = sum(
        int(record.get("value_flow_complete", 0)) == 1
        for record in projections
    )
    summary = {
        "path": str(path),
        "base_frame": base_frame,
        "frame_range": [min(frames), max(frames)],
        "records": len(records),
        "event_counts": dict(sorted(event_counts.items())),
        "captured_projections": len(projections),
        "complete_value_flow_projections": complete_projections,
        "mips_value_expressions": value_expressions,
        "external_projection_id_classes": len(external_ids),
        "submission_classes": len(submission_ids),
    }
    return canonical, summary, errors


def _field_diff(left: dict[str, Any], right: dict[str, Any]) -> dict[str, Any]:
    return {
        key: {"oracle": left.get(key), "instrumented": right.get(key)}
        for key in sorted(set(left) | set(right))
        if left.get(key) != right.get(key)
    }


def compare_captures(oracle_path: Path, instrumented_path: Path) -> dict[str, Any]:
    oracle_records, oracle_expressions = read_trace(oracle_path)
    instrumented_records, instrumented_expressions = read_trace(instrumented_path)
    oracle, oracle_summary, oracle_errors = canonicalize(
        oracle_path, oracle_records, oracle_expressions
    )
    instrumented, instrumented_summary, instrumented_errors = canonicalize(
        instrumented_path, instrumented_records, instrumented_expressions
    )

    first_mismatch: dict[str, Any] | None = None
    for index, (left, right) in enumerate(zip(oracle, instrumented)):
        if left != right:
            first_mismatch = {
                "index": index,
                "oracle_event": left.get("event"),
                "instrumented_event": right.get("event"),
                "differences": _field_diff(left, right),
            }
            break
    if first_mismatch is None and len(oracle) != len(instrumented):
        first_mismatch = {
            "index": min(len(oracle), len(instrumented)),
            "oracle_event": (
                oracle[len(instrumented)]["event"]
                if len(oracle) > len(instrumented) else None
            ),
            "instrumented_event": (
                instrumented[len(oracle)]["event"]
                if len(instrumented) > len(oracle) else None
            ),
            "differences": {
                "remaining_records": {
                    "oracle": len(oracle) - min(len(oracle), len(instrumented)),
                    "instrumented": (
                        len(instrumented) - min(len(oracle), len(instrumented))
                    ),
                }
            },
        }

    instrumented_complete = (
        instrumented_summary["captured_projections"] > 0
        and instrumented_summary["complete_value_flow_projections"]
        == instrumented_summary["captured_projections"]
        and instrumented_summary["mips_value_expressions"] > 0
    )
    stream_equal = first_mismatch is None
    equivalent = (
        stream_equal
        and instrumented_complete
        and not oracle_errors
        and not instrumented_errors
    )
    return {
        "schema": REPORT_SCHEMA,
        "equivalent": equivalent,
        "stream_equal": stream_equal,
        "instrumented_value_flow_complete": instrumented_complete,
        "oracle": oracle_summary,
        "instrumented": instrumented_summary,
        "validation_errors": {
            "oracle": oracle_errors,
            "instrumented": instrumented_errors,
        },
        "first_mismatch": first_mismatch,
        "normalization": {
            "vblank_placement_compared": False,
            "vblank_note": (
                "the light pass is the timing oracle; heavy value-flow "
                "instrumentation may stretch the same ordered guest work"
            ),
            "projection_ids_replaced_by_first_use_ordinals": True,
            "submission_ids_replaced_by_first_use_ordinals": True,
            "ignored_trace_metadata": sorted(TRACE_METADATA - {"frame"}),
            "ignored_instrumentation_fields": sorted(VALUE_FLOW_FIELDS),
            "ignored_gp0_instrumentation_fields": sorted(
                GP0_VALUE_FLOW_FIELDS
            ) + ["intrinsic_provenance.bit7"],
            "guest_values_packet_words_and_ot_topology_compared": True,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("oracle", type=Path, help="light checkpoint trace")
    parser.add_argument("instrumented", type=Path, help="heavy trace")
    parser.add_argument("--output", type=Path, help="write JSON report")
    parser.add_argument("--json", action="store_true", help="print JSON report")
    args = parser.parse_args()
    try:
        report = compare_captures(args.oracle, args.instrumented)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        verdict = "EQUIVALENT" if report["equivalent"] else "MISMATCH"
        print(f"depth capture: {verdict}")
        print(
            "records: "
            f"{report['oracle']['records']} oracle / "
            f"{report['instrumented']['records']} instrumented"
        )
        print(
            "instrumented value flow: "
            f"{report['instrumented']['complete_value_flow_projections']}/"
            f"{report['instrumented']['captured_projections']} projections, "
            f"{report['instrumented']['mips_value_expressions']} expressions"
        )
        if report["first_mismatch"]:
            print("first mismatch: " + json.dumps(report["first_mismatch"]))
        for side, errors in report["validation_errors"].items():
            for error in errors:
                print(f"{side} validation: {error}")
    return 0 if report["equivalent"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
