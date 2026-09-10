#!/usr/bin/env python3
"""Build a reloadable runtime dispatch manifest for an explicit gauge hypothesis.

The tool only serializes a caller-authored exact-rational candidate after the
complete visibility constraints and the cross-validated v2 runtime dispatch
map both pass. It never chooses a multiplier, rounds one, or limits the number
of observed relation components. A valid output remains a raster-test
hypothesis, not proof of object identity, family count, or presentation scale.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any

from visibility_gauge_candidate import validate_visibility_gauge_candidate


REPORT_SCHEMA = "ps1-visibility-runtime-manifest-audit/v1"
MANIFEST_SCHEMA = "ps1-visibility-runtime-manifest/v1"
DISPATCH_SCHEMA = "ps1-visibility-runtime-dispatch-map/v2"
HEX64 = re.compile(r"[0-9a-f]{16}")
SHA256 = re.compile(r"[0-9a-f]{64}")


def _plain_int(value: Any, minimum: int, maximum: int, context: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{context}: expected an integer")
    if value < minimum or value > maximum:
        raise ValueError(f"{context}: outside {minimum}..{maximum}")
    return value


def _exact_bool(value: Any, context: str) -> bool:
    if not isinstance(value, bool):
        raise ValueError(f"{context}: expected a boolean")
    return value


def _hex(value: Any, pattern: re.Pattern[str], context: str) -> str:
    if not isinstance(value, str) or pattern.fullmatch(value) is None:
        raise ValueError(f"{context}: invalid lowercase hexadecimal value")
    return value


def _canonical_dispatch_entry(entry: Any, index: int) -> tuple[Any, ...]:
    context = f"dispatch entry {index}"
    if not isinstance(entry, dict):
        raise ValueError(f"{context}: expected an object")
    component = _hex(entry.get("component_sha256"), SHA256, f"{context} component")
    transform = _hex(
        entry.get("transform_lineage_hex"), HEX64, f"{context} transform lineage"
    )
    triangle = _plain_int(entry.get("triangle_index"), 0, 255, f"{context} triangle")
    vertices = entry.get("vertices")
    if not isinstance(vertices, list) or len(vertices) != 3:
        raise ValueError(f"{context}: vertices must contain exactly three records")
    canonical_vertices = []
    seen_indices = set()
    for vertex_index, vertex in enumerate(vertices):
        vertex_context = f"{context} vertex {vertex_index}"
        if not isinstance(vertex, dict):
            raise ValueError(f"{vertex_context}: expected an object")
        index_value = _plain_int(vertex.get("index"), 0, 3, f"{vertex_context} index")
        if index_value in seen_indices:
            raise ValueError(f"{context}: repeated vertex index")
        seen_indices.add(index_value)
        source_present = _exact_bool(
            vertex.get("model_source_present"), f"{vertex_context} source-present"
        )
        source = _hex(
            vertex.get("model_source_hex"), HEX64, f"{vertex_context} model source"
        )
        flow_source = _hex(
            vertex.get("model_flow_source_hex"),
            HEX64,
            f"{vertex_context} flow source",
        )
        flow_program = _hex(
            vertex.get("model_flow_program_hex"),
            HEX64,
            f"{vertex_context} flow program",
        )
        offset = _plain_int(
            vertex.get("gp0_node_offset"),
            0,
            0x1FFFFF,
            f"{vertex_context} GP0-node offset",
        )
        if int(flow_source, 16) == 0 or int(flow_program, 16) == 0:
            raise ValueError(f"{vertex_context}: zero exact model-flow fingerprint")
        if source_present != (int(source, 16) != 0):
            raise ValueError(f"{vertex_context}: model source disagrees with presence")
        canonical_vertices.append(
            (index_value, source_present, source, flow_source, flow_program, offset)
        )
    return component, transform, triangle, tuple(canonical_vertices)


def build_visibility_runtime_manifest(
    lineage: Any, candidate: Any
) -> tuple[dict[str, Any], str | None]:
    errors: list[str] = []
    candidate_audit = validate_visibility_gauge_candidate(lineage, candidate)
    if not candidate_audit["input_valid"]:
        errors.extend(candidate_audit["errors"])
    elif not candidate_audit["candidate_satisfies_all_measured_constraints"]:
        errors.append("candidate does not satisfy every measured strict constraint")

    dispatch: Any = None
    if isinstance(lineage, dict):
        affine = lineage.get("numeric_affine_cohorts")
        if isinstance(affine, dict):
            dispatch = affine.get("runtime_dispatch")
    if not isinstance(dispatch, dict):
        errors.append("lineage report has no runtime dispatch map")
    else:
        if dispatch.get("schema") != DISPATCH_SCHEMA:
            errors.append("runtime dispatch schema is not v2")
        checks = (
            ("candidate_dispatch_ready", True),
            ("compact_model_fingerprints_cross_audited_against_full_dag", True),
            ("all_reported_component_signatures_dispatched", True),
            ("uses_screen_position", False),
            ("uses_colour", False),
            ("uses_observed_depth", False),
            ("uses_numeric_transform_values", False),
            ("uses_family_count_prior", False),
            ("uses_integer_or_tenth_prior", False),
        )
        for field, expected in checks:
            try:
                actual = _exact_bool(dispatch.get(field), f"runtime dispatch {field}")
                if actual != expected:
                    errors.append(
                        f"runtime dispatch {field} is {actual}, expected {expected}"
                    )
            except ValueError as exc:
                errors.append(str(exc))
        try:
            if _plain_int(
                dispatch.get("compact_model_fingerprint_schema"),
                0,
                2**31 - 1,
                "runtime fingerprint schema",
            ) != 1:
                errors.append("runtime fingerprint schema is not 1")
        except ValueError as exc:
            errors.append(str(exc))

    assignments: dict[str, tuple[int, int]] = {}
    for value in candidate_audit.get("normalized_presentation_multipliers", []):
        if isinstance(value, dict):
            component = value.get("component_sha256")
            numerator = value.get("numerator")
            denominator = value.get("denominator")
            if (
                isinstance(component, str)
                and SHA256.fullmatch(component)
                and isinstance(numerator, int)
                and not isinstance(numerator, bool)
                and isinstance(denominator, int)
                and not isinstance(denominator, bool)
                and numerator > 0
                and denominator > 0
            ):
                assignments[component] = (numerator, denominator)

    canonical_entries: list[tuple[Any, ...]] = []
    if isinstance(dispatch, dict):
        entries = dispatch.get("entries")
        if not isinstance(entries, list):
            errors.append("runtime dispatch entries must be a list")
        else:
            expected_count = dispatch.get("entry_count")
            if (
                isinstance(expected_count, bool)
                or not isinstance(expected_count, int)
                or expected_count != len(entries)
            ):
                errors.append("runtime dispatch entry count is inconsistent")
            for index, entry in enumerate(entries):
                try:
                    canonical_entries.append(_canonical_dispatch_entry(entry, index))
                except ValueError as exc:
                    errors.append(str(exc))

    dispatch_keys = [entry[1:] for entry in canonical_entries]
    if len(set(dispatch_keys)) != len(dispatch_keys):
        errors.append("runtime dispatch contains duplicate keys")
    entry_components = {entry[0] for entry in canonical_entries}
    if entry_components != set(assignments):
        missing = sorted(set(assignments) - entry_components)
        extra = sorted(entry_components - set(assignments))
        if missing:
            errors.append("dispatch is missing candidate components: " + ", ".join(missing))
        if extra:
            errors.append("dispatch contains unknown components: " + ", ".join(extra))
    if not canonical_entries:
        errors.append("runtime dispatch has no entries")

    manifest: str | None = None
    if not errors:
        lines = [
            f"schema {MANIFEST_SCHEMA}",
            "fingerprint_schema 1",
            "scope hypothesis",
            "family_count_prior_used 0",
            "integer_or_tenth_prior_used 0",
        ]
        for component, (numerator, denominator) in sorted(assignments.items()):
            lines.append(f"component {component} {numerator} {denominator}")
        canonical_entries.sort()
        for component, transform, triangle, vertices in canonical_entries:
            fields = ["entry", component, transform, str(triangle)]
            for (
                vertex_index,
                source_present,
                source,
                flow_source,
                flow_program,
                offset,
            ) in vertices:
                fields.extend(
                    (
                        str(vertex_index),
                        "1" if source_present else "0",
                        source,
                        flow_source,
                        flow_program,
                        str(offset),
                    )
                )
            lines.append(" ".join(fields))
        lines.append(f"end {len(assignments)} {len(canonical_entries)}")
        manifest = "\n".join(lines) + "\n"

    report = {
        "schema": REPORT_SCHEMA,
        "input_valid": not errors,
        "manifest_ready": manifest is not None,
        "components": len(assignments),
        "entries": len(canonical_entries),
        "manifest_sha256": (
            hashlib.sha256(manifest.encode("utf-8")).hexdigest()
            if manifest is not None
            else None
        ),
        "candidate_audit": candidate_audit,
        "proof_scope": {
            "scope": "hypothesis",
            "candidate_selected_or_fitted_by_tool": False,
            "family_count_prior_used": False,
            "integer_or_tenth_prior_used": False,
            "runtime_dispatch_cross_validated": manifest is not None,
            "passing_manifest_is_gauge_proof": False,
            "passing_manifest_is_family_count_proof": False,
            "unknown_runtime_keys_fail_closed": True,
            "independent_post_lift_raster_validation_required": True,
        },
        "errors": errors,
    }
    return report, manifest


def _atomic_write(path: Path, payload: str) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(payload, encoding="utf-8")
    temporary.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("lineage_report", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--audit", type=Path)
    args = parser.parse_args()
    try:
        lineage = json.loads(args.lineage_report.read_text(encoding="utf-8"))
        candidate = json.loads(args.candidate.read_text(encoding="utf-8"))
        report, manifest = build_visibility_runtime_manifest(lineage, candidate)
        audit_payload = json.dumps(report, indent=2, sort_keys=True) + "\n"
        if args.audit:
            _atomic_write(args.audit, audit_payload)
        else:
            print(audit_payload, end="")
        if manifest is not None:
            _atomic_write(args.manifest, manifest)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"visibility_runtime_manifest: {exc}", file=sys.stderr)
        return 2
    return 0 if manifest is not None else 2


if __name__ == "__main__":
    raise SystemExit(main())
