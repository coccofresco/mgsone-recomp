#!/usr/bin/env python3
"""Audit third-pass Vulkan outcomes against an intrinsic runtime manifest.

The report joins every non-passing OT/strict/lifted owner triplet to the exact
v2 construction-lineage dispatch used at runtime. It identifies incomplete and
unmapped keys, retained strict winners, third-winner intrusions and regressions
without changing, fitting or recommending a multiplier.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import re
import struct
import sys
from pathlib import Path
from typing import Any

from visibility_lineage_trace import (
    _owner_fields,
    _runtime_dispatch_key,
    _signature_id,
    _validate_owner,
)


TRACE_SCHEMA = "ps1trace/v1"
REPORT_SCHEMA = "ps1-visibility-candidate-outcomes/v1"
MANIFEST_SCHEMA = "ps1-visibility-runtime-manifest/v1"
LINEAGE_SCHEMA = "ps1-visibility-lineage-audit/v1"
DISPATCH_SCHEMA = "ps1-visibility-runtime-dispatch-map/v2"
TOPOLOGY_TARGET = "projection-incidence-component/v1"
HEX64 = re.compile(r"[0-9a-f]{16}")
SHA256 = re.compile(r"[0-9a-f]{64}")
AUDIT_EVENT = "replay.visibility_candidate_audit"
OUTCOME_EVENT = "replay.visibility_candidate_outcome"
OWNER_EVENT = "replay.visibility_owner"
CATALOG_EVENT = "replay.visibility_catalog"

CLASS_NAMES = {
    0: "agreement-preserved",
    1: "conflict-resolved-to-ot",
    2: "conflict-retained-by-strict",
    3: "conflict-changed-winner",
    4: "baseline-coverage-divergence",
    5: "candidate-coverage-divergence",
    6: "candidate-regression",
    7: "indeterminate",
    8: "inconsistent-evidence",
}


def _integer(record: dict[str, Any], field: str, context: str) -> int:
    value = record.get(field)
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{context}: missing/non-integer field {field!r}")
    return value


def _plain_integer(value: Any, minimum: int, maximum: int, context: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{context}: expected an integer")
    if value < minimum or value > maximum:
        raise ValueError(f"{context}: outside {minimum}..{maximum}")
    return value


def _lower_hex(value: Any, pattern: re.Pattern[str], context: str) -> str:
    if not isinstance(value, str) or pattern.fullmatch(value) is None:
        raise ValueError(f"{context}: invalid lowercase hexadecimal value")
    return value


def _parse_manifest(text: str) -> tuple[dict[tuple[Any, ...], str], dict[str, Any]]:
    components: dict[str, tuple[int, int]] = {}
    dispatch: dict[tuple[Any, ...], str] = {}
    headers: dict[str, str] = {}
    footer: tuple[int, int] | None = None
    ended = False
    for line_number, raw_line in enumerate(text.splitlines(), 1):
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        if ended:
            raise ValueError(f"manifest line {line_number}: content follows end")
        words = line.split()
        directive = words[0]
        if directive in {
            "schema",
            "fingerprint_schema",
            "scope",
            "family_count_prior_used",
            "integer_or_tenth_prior_used",
        }:
            if len(words) != 2 or directive in headers:
                raise ValueError(f"manifest line {line_number}: invalid header")
            headers[directive] = words[1]
        elif directive == "component":
            if len(words) != 4 or len(words[1]) != 64:
                raise ValueError(f"manifest line {line_number}: invalid component")
            component = words[1]
            if component in components:
                raise ValueError(f"manifest line {line_number}: duplicate component")
            try:
                numerator, denominator = int(words[2]), int(words[3])
            except ValueError as exc:
                raise ValueError(
                    f"manifest line {line_number}: invalid component rational"
                ) from exc
            if numerator <= 0 or denominator <= 0:
                raise ValueError(
                    f"manifest line {line_number}: non-positive component rational"
                )
            components[component] = (numerator, denominator)
        elif directive == "entry":
            if len(words) != 22:
                raise ValueError(f"manifest line {line_number}: invalid entry width")
            component = words[1]
            if component not in components:
                raise ValueError(
                    f"manifest line {line_number}: unknown entry component"
                )
            try:
                transform = int(words[2], 16)
                triangle = int(words[3])
                vertices = []
                cursor = 4
                for _ in range(3):
                    index = int(words[cursor])
                    source_present_value = int(words[cursor + 1])
                    source = int(words[cursor + 2], 16)
                    flow_source = int(words[cursor + 3], 16)
                    flow_program = int(words[cursor + 4], 16)
                    offset = int(words[cursor + 5])
                    cursor += 6
                    if source_present_value not in (0, 1):
                        raise ValueError("non-boolean source-present")
                    vertices.append(
                        (
                            index,
                            bool(source_present_value),
                            source,
                            flow_source,
                            flow_program,
                            offset,
                        )
                    )
            except ValueError as exc:
                raise ValueError(f"manifest line {line_number}: invalid entry") from exc
            key = (
                "visibility-runtime-dispatch/v2",
                transform,
                triangle,
                tuple(vertices),
            )
            if key in dispatch:
                raise ValueError(f"manifest line {line_number}: duplicate entry")
            dispatch[key] = component
        elif directive == "end":
            if len(words) != 3:
                raise ValueError(f"manifest line {line_number}: invalid end")
            try:
                footer = (int(words[1]), int(words[2]))
            except ValueError as exc:
                raise ValueError(f"manifest line {line_number}: invalid counts") from exc
            ended = True
        else:
            raise ValueError(f"manifest line {line_number}: unknown directive")

    expected_headers = {
        "schema": MANIFEST_SCHEMA,
        "fingerprint_schema": "1",
        "scope": "hypothesis",
        "family_count_prior_used": "0",
        "integer_or_tenth_prior_used": "0",
    }
    if headers != expected_headers:
        raise ValueError("manifest headers are incomplete, unsupported or contaminated")
    if footer != (len(components), len(dispatch)) or not components or not dispatch:
        raise ValueError("manifest footer/counts are incomplete")
    if set(dispatch.values()) != set(components):
        raise ValueError("manifest does not dispatch every component")
    return dispatch, {
        "schema": headers["schema"],
        "fingerprint_schema": 1,
        "scope": headers["scope"],
        "components": len(components),
        "entries": len(dispatch),
        "sha256": hashlib.sha256(text.encode("utf-8")).hexdigest(),
        "multipliers": [
            {
                "component_sha256": component,
                "numerator": rational[0],
                "denominator": rational[1],
            }
            for component, rational in sorted(components.items())
        ],
    }


def _parse_topology_dispatch(
    lineage: Any,
) -> tuple[dict[tuple[Any, ...], str], dict[str, Any]]:
    if not isinstance(lineage, dict):
        raise ValueError("lineage report must be an object")
    if lineage.get("schema") != LINEAGE_SCHEMA or lineage.get("valid") is not True:
        raise ValueError("lineage report is invalid or unsupported")
    priors = lineage.get("advisory_priors")
    if not isinstance(priors, dict):
        raise ValueError("lineage report has no explicit advisory-prior scope")
    for field in (
        "family_count_is_correctness_limit",
        "integer_or_tenth_scale_is_correctness_limit",
        "integer_or_tenth_scale_used_as_filter",
        "priors_used_to_select_or_reject_evidence",
    ):
        if priors.get(field) is not False:
            raise ValueError(f"lineage advisory prior {field} is contaminated")

    topology = lineage.get("projection_incidence_topology")
    if not isinstance(topology, dict):
        raise ValueError("lineage report has no projection-incidence topology")
    for field in (
        "component_count_is_object_count",
        "component_count_is_family_count",
        "component_count_compared_to_family_prior",
        "persistent_object_identity_certified",
    ):
        if topology.get(field) is not False:
            raise ValueError(f"topology field {field} is unsupported or contaminated")
    dispatch = topology.get("runtime_dispatch")
    if not isinstance(dispatch, dict):
        raise ValueError("topology has no runtime dispatch map")
    if dispatch.get("schema") != DISPATCH_SCHEMA:
        raise ValueError("topology runtime dispatch schema is not v2")
    if dispatch.get("target") != TOPOLOGY_TARGET:
        raise ValueError("runtime dispatch does not target projection topology")
    checks = {
        "candidate_dispatch_ready": True,
        "compact_model_fingerprints_cross_audited_against_full_dag": True,
        "all_reported_component_signatures_dispatched": True,
        "uses_screen_position": False,
        "uses_colour": False,
        "uses_observed_depth": False,
        "uses_numeric_transform_values": False,
        "uses_family_count_prior": False,
        "uses_integer_or_tenth_prior": False,
        "persistent_object_identity_certified": False,
    }
    for field, expected in checks.items():
        if dispatch.get(field) is not expected:
            raise ValueError(
                f"topology runtime dispatch {field} is unsupported or contaminated"
            )
    if dispatch.get("compact_model_fingerprint_schema") != 1:
        raise ValueError("topology runtime fingerprint schema is not 1")

    entries = dispatch.get("entries")
    if not isinstance(entries, list) or not entries:
        raise ValueError("topology runtime dispatch has no entries")
    expected_entries = _plain_integer(
        dispatch.get("entry_count"), 1, 2**31 - 1, "topology dispatch entries"
    )
    expected_components = _plain_integer(
        dispatch.get("components"), 1, 2**31 - 1, "topology dispatch components"
    )
    if expected_entries != len(entries):
        raise ValueError("topology runtime dispatch entry count is inconsistent")

    result: dict[tuple[Any, ...], str] = {}
    for entry_index, entry in enumerate(entries):
        context = f"topology dispatch entry {entry_index}"
        if not isinstance(entry, dict):
            raise ValueError(f"{context}: expected an object")
        component = _lower_hex(
            entry.get("component_sha256"), SHA256, f"{context} component"
        )
        transform = int(
            _lower_hex(
                entry.get("transform_lineage_hex"),
                HEX64,
                f"{context} transform lineage",
            ),
            16,
        )
        triangle = _plain_integer(
            entry.get("triangle_index"), 0, 255, f"{context} triangle"
        )
        vertices = entry.get("vertices")
        if not isinstance(vertices, list) or len(vertices) != 3:
            raise ValueError(f"{context}: vertices must contain three records")
        canonical_vertices = []
        seen_indices = set()
        for vertex_index, vertex in enumerate(vertices):
            vertex_context = f"{context} vertex {vertex_index}"
            if not isinstance(vertex, dict):
                raise ValueError(f"{vertex_context}: expected an object")
            index = _plain_integer(
                vertex.get("index"), 0, 3, f"{vertex_context} index"
            )
            if index in seen_indices:
                raise ValueError(f"{context}: repeated vertex index")
            seen_indices.add(index)
            source_present = vertex.get("model_source_present")
            if not isinstance(source_present, bool):
                raise ValueError(f"{vertex_context}: invalid source-present flag")
            source = int(
                _lower_hex(
                    vertex.get("model_source_hex"),
                    HEX64,
                    f"{vertex_context} model source",
                ),
                16,
            )
            flow_source = int(
                _lower_hex(
                    vertex.get("model_flow_source_hex"),
                    HEX64,
                    f"{vertex_context} model-flow source",
                ),
                16,
            )
            flow_program = int(
                _lower_hex(
                    vertex.get("model_flow_program_hex"),
                    HEX64,
                    f"{vertex_context} model-flow program",
                ),
                16,
            )
            offset = _plain_integer(
                vertex.get("gp0_node_offset"),
                0,
                0x1FFFFF,
                f"{vertex_context} GP0-node offset",
            )
            if source_present != (source != 0):
                raise ValueError(f"{vertex_context}: source-presence mismatch")
            if flow_source == 0 or flow_program == 0:
                raise ValueError(f"{vertex_context}: zero model-flow fingerprint")
            canonical_vertices.append(
                (
                    index,
                    source_present,
                    source,
                    flow_source,
                    flow_program,
                    offset,
                )
            )
        key = (
            "visibility-runtime-dispatch/v2",
            transform,
            triangle,
            tuple(canonical_vertices),
        )
        if key in result:
            raise ValueError(f"{context}: duplicate runtime key")
        result[key] = component
    if len(set(result.values())) != expected_components:
        raise ValueError("topology runtime dispatch component count is inconsistent")
    return result, {
        "schema": dispatch["schema"],
        "target": dispatch["target"],
        "entries": len(result),
        "components": expected_components,
        "stable_partial_components_are_objects": False,
        "stable_partial_components_are_families": False,
        "family_count_prior_used": False,
        "integer_or_tenth_prior_used": False,
    }


def _float_bits(bits: int) -> str:
    if bits < 0 or bits > 0xFFFFFFFF:
        raise ValueError("depth bit pattern is outside uint32")
    return format(struct.unpack("<f", struct.pack("<I", bits))[0], ".9g")


def analyze_candidate_records(
    records: list[dict[str, Any]],
    manifest_text: str,
    topology_lineage: Any | None = None,
) -> dict[str, Any]:
    manifest_dispatch, manifest_report = _parse_manifest(manifest_text)
    if topology_lineage is None:
        topology_dispatch = None
        topology_report = None
    else:
        topology_dispatch, topology_report = _parse_topology_dispatch(
            topology_lineage
        )
    errors: list[str] = []
    audits: dict[int, dict[str, Any]] = {}
    outcomes: dict[int, list[dict[str, Any]]] = collections.defaultdict(list)
    catalogs: dict[int, dict[str, Any]] = {}
    owners: dict[tuple[int, int], dict[str, Any]] = {}

    for record_index, record in enumerate(records):
        event = record.get("event")
        if event not in {AUDIT_EVENT, OUTCOME_EVENT, OWNER_EVENT, CATALOG_EVENT}:
            continue
        context = f"record {record_index}"
        if record.get("schema") != TRACE_SCHEMA:
            errors.append(f"{context}: invalid trace schema")
            continue
        try:
            frame = _integer(record, "frame", context)
        except ValueError as exc:
            errors.append(str(exc))
            continue
        if event == AUDIT_EVENT:
            if frame in audits:
                errors.append(f"{context}: duplicate candidate audit frame {frame}")
            else:
                audits[frame] = record
        elif event == OUTCOME_EVENT:
            outcomes[frame].append(record)
        elif event == CATALOG_EVENT:
            if frame in catalogs:
                errors.append(f"{context}: duplicate catalog frame {frame}")
            else:
                catalogs[frame] = record
        else:
            try:
                token = _integer(record, "owner", context)
                parsed = _owner_fields(record, context)
            except ValueError as exc:
                errors.append(str(exc))
                continue
            key = (frame, token)
            if token <= 0 or key in owners:
                errors.append(f"{context}: invalid/duplicate owner token")
                continue
            owner_errors = _validate_owner(parsed, context)
            if owner_errors:
                errors.extend(owner_errors)
                continue
            owners[key] = parsed

    if not audits:
        errors.append("trace has no candidate audit records")

    owner_dispatch: dict[tuple[int, int], dict[str, Any]] = {}
    dispatch_counts = collections.Counter()
    topology_dispatch_counts = collections.Counter()
    unknown_keys: dict[str, tuple[Any, ...]] = {}
    unknown_topology_keys: dict[str, tuple[Any, ...]] = {}
    for owner_key, owner in owners.items():
        dispatch_key = _runtime_dispatch_key(owner)
        if dispatch_key is None:
            status = "incomplete-key"
            component = None
            dispatch_id = None
            topology_status = "incomplete-key"
            topology_component = None
        else:
            dispatch_id = _signature_id(dispatch_key)
            component = manifest_dispatch.get(dispatch_key)
            if component is None:
                status = "unmapped-key"
                unknown_keys.setdefault(dispatch_id, dispatch_key)
            else:
                status = "mapped"
            if topology_dispatch is None:
                topology_status = "not-requested"
                topology_component = None
            else:
                topology_component = topology_dispatch.get(dispatch_key)
                if topology_component is None:
                    topology_status = "unmapped-key"
                    unknown_topology_keys.setdefault(dispatch_id, dispatch_key)
                else:
                    topology_status = "mapped"
        dispatch_counts[status] += 1
        topology_dispatch_counts[topology_status] += 1
        owner_dispatch[owner_key] = {
            "status": status,
            "component": component,
            "dispatch_sha256": dispatch_id,
            "topology_status": topology_status,
            "topology_component": topology_component,
        }

    def owner_assignment(
        frame: int, token: int
    ) -> tuple[str, str, str, str, str]:
        if token == 0:
            return (
                "background",
                "background",
                "background",
                "background",
                "background",
            )
        value = owner_dispatch.get((frame, token))
        if value is None:
            return (
                "missing-owner-record",
                "missing-owner-record",
                "missing-owner-record",
                "missing-owner-record",
                "missing-owner-record",
            )
        return (
            value["component"] or value["status"],
            value["status"],
            value["dispatch_sha256"] or value["status"],
            value["topology_component"] or value["topology_status"],
            value["topology_status"],
        )

    frame_reports = []
    aggregate = collections.Counter()
    component_outcomes = collections.Counter()
    topology_outcomes = collections.Counter()
    dispatch_outcomes = collections.Counter()
    generations = set()
    for frame in sorted(audits):
        audit = audits[frame]
        context = f"frame {frame}"
        try:
            if _integer(audit, "candidate_schema", context) != 2:
                errors.append(f"{context}: candidate schema is not 2")
            if _integer(audit, "candidate_mode", context) != 2:
                errors.append(f"{context}: trace is not manifest mode")
            if _integer(audit, "identity_control", context) != 0:
                errors.append(f"{context}: manifest trace claims identity control")
            generations.add(_integer(audit, "manifest_generation", context))
            values = {
                field: _integer(audit, field, context)
                for field in (
                    "status",
                    "raster_gate_pass",
                    "samples",
                    "baseline_agreement",
                    "preserved_agreement",
                    "baseline_conflicts",
                    "resolved_to_ot",
                    "retained_by_strict",
                    "changed_winner",
                    "baseline_coverage_divergence",
                    "candidate_coverage_divergence",
                    "regressions",
                    "indeterminate",
                    "inconsistent",
                )
            }
        except ValueError as exc:
            errors.append(str(exc))
            continue
        if values["status"] != 0:
            errors.append(f"{context}: candidate status is not compared")
        if values["baseline_agreement"] != (
            values["preserved_agreement"] + values["regressions"]
        ):
            errors.append(f"{context}: agreement partition mismatch")
        if values["baseline_conflicts"] != (
            values["resolved_to_ot"]
            + values["retained_by_strict"]
            + values["changed_winner"]
        ):
            errors.append(f"{context}: conflict partition mismatch")
        classified = (
            values["baseline_agreement"]
            + values["baseline_conflicts"]
            + values["baseline_coverage_divergence"]
            + values["candidate_coverage_divergence"]
            + values["indeterminate"]
            + values["inconsistent"]
        )
        if classified != values["samples"]:
            errors.append(f"{context}: sample partition mismatch")

        outcome_class_counts = collections.Counter()
        outcome_preview = []
        for outcome_index, outcome in enumerate(outcomes.get(frame, [])):
            outcome_context = f"{context} outcome {outcome_index}"
            try:
                if _integer(outcome, "outcome_schema", outcome_context) != 1:
                    raise ValueError(f"{outcome_context}: unsupported outcome schema")
                if _integer(outcome, "candidate_mode", outcome_context) != 2:
                    raise ValueError(f"{outcome_context}: not manifest mode")
                classification = _integer(
                    outcome, "classification", outcome_context
                )
                if classification not in CLASS_NAMES or classification in (0, 1):
                    raise ValueError(f"{outcome_context}: invalid failure class")
                samples = _integer(outcome, "samples", outcome_context)
                if samples <= 0:
                    raise ValueError(f"{outcome_context}: non-positive samples")
                tokens = tuple(
                    _integer(outcome, field, outcome_context)
                    for field in ("ot_owner", "strict_owner", "lifted_owner")
                )
                first_x = _integer(outcome, "first_atlas_x", outcome_context)
                first_y = _integer(outcome, "first_atlas_y", outcome_context)
                depth_fields = {
                    field: _float_bits(_integer(outcome, field, outcome_context))
                    for field in (
                        "minimum_ot_depth_bits",
                        "maximum_ot_depth_bits",
                        "minimum_strict_depth_bits",
                        "maximum_strict_depth_bits",
                        "minimum_lifted_depth_bits",
                        "maximum_lifted_depth_bits",
                    )
                }
            except ValueError as exc:
                errors.append(str(exc))
                continue
            assignments = [owner_assignment(frame, token) for token in tokens]
            if any(
                status == "missing-owner-record"
                for _, status, _, _, _ in assignments
            ):
                errors.append(f"{outcome_context}: owner token lacks catalog record")
            class_name = CLASS_NAMES[classification]
            outcome_class_counts[class_name] += samples
            component_key = (
                class_name,
                assignments[0][0],
                assignments[1][0],
                assignments[2][0],
            )
            component_outcomes[component_key] += samples
            if topology_dispatch is not None:
                topology_key = (
                    class_name,
                    assignments[0][3],
                    assignments[1][3],
                    assignments[2][3],
                )
                topology_outcomes[topology_key] += samples
            dispatch_key = (
                class_name,
                assignments[0][2],
                assignments[1][2],
                assignments[2][2],
            )
            dispatch_outcomes[dispatch_key] += samples
            if len(outcome_preview) < 32:
                outcome_preview.append(
                    {
                        "classification": class_name,
                        "samples": samples,
                        "owners": {
                            "ot": tokens[0],
                            "strict": tokens[1],
                            "lifted": tokens[2],
                        },
                        "components": {
                            "ot": assignments[0][0],
                            "strict": assignments[1][0],
                            "lifted": assignments[2][0],
                        },
                        "topology_components": (
                            {
                                "ot": assignments[0][3],
                                "strict": assignments[1][3],
                                "lifted": assignments[2][3],
                            }
                            if topology_dispatch is not None
                            else None
                        ),
                        "dispatch_sha256": {
                            "ot": assignments[0][2],
                            "strict": assignments[1][2],
                            "lifted": assignments[2][2],
                        },
                        "first_atlas": [first_x, first_y],
                        "depth_extrema": depth_fields,
                    }
                )

        expected_outcome_counts = {
            "conflict-retained-by-strict": values["retained_by_strict"],
            "conflict-changed-winner": values["changed_winner"],
            "baseline-coverage-divergence": values[
                "baseline_coverage_divergence"
            ],
            "candidate-coverage-divergence": values[
                "candidate_coverage_divergence"
            ],
            "candidate-regression": values["regressions"],
            "indeterminate": values["indeterminate"],
            "inconsistent-evidence": values["inconsistent"],
        }
        if dict(outcome_class_counts) != {
            key: value for key, value in expected_outcome_counts.items() if value
        }:
            errors.append(f"{context}: outcome aggregates do not match audit")

        catalog = catalogs.get(frame)
        if catalog is None:
            errors.append(f"{context}: missing complete owner catalog")
        else:
            try:
                emitted = _integer(catalog, "emitted_owners", context)
                owner_schema = _integer(catalog, "owner_schema", context)
                frame_owner_records = sum(1 for key in owners if key[0] == frame)
                if owner_schema < 2 or emitted != frame_owner_records:
                    errors.append(f"{context}: catalog/owner record mismatch")
            except ValueError as exc:
                errors.append(str(exc))

        aggregate.update(values)
        frame_reports.append(
            {
                "frame": frame,
                **values,
                "outcome_groups": len(outcomes.get(frame, [])),
                "outcome_preview": outcome_preview,
            }
        )

    extra_outcome_frames = sorted(set(outcomes) - set(audits))
    if extra_outcome_frames:
        errors.append(f"outcomes without audits in frames {extra_outcome_frames}")
    if len(generations) != 1:
        errors.append("candidate frames do not use exactly one manifest generation")

    component_summary = [
        {
            "classification": key[0],
            "ot_component": key[1],
            "strict_component": key[2],
            "lifted_component": key[3],
            "samples": samples,
        }
        for key, samples in sorted(
            component_outcomes.items(), key=lambda item: (-item[1], item[0])
        )
    ]
    topology_summary = [
        {
            "classification": key[0],
            "ot_topology_component": key[1],
            "strict_topology_component": key[2],
            "lifted_topology_component": key[3],
            "samples": samples,
        }
        for key, samples in sorted(
            topology_outcomes.items(), key=lambda item: (-item[1], item[0])
        )
    ]
    dispatch_summary = [
        {
            "classification": key[0],
            "ot_dispatch_sha256": key[1],
            "strict_dispatch_sha256": key[2],
            "lifted_dispatch_sha256": key[3],
            "samples": samples,
        }
        for key, samples in sorted(
            dispatch_outcomes.items(), key=lambda item: (-item[1], item[0])
        )
    ]
    return {
        "schema": REPORT_SCHEMA,
        "valid": not errors,
        "manifest": manifest_report,
        "topology_partition": topology_report,
        "manifest_generation": next(iter(generations)) if len(generations) == 1 else None,
        "frames": frame_reports,
        "audited_frames": len(frame_reports),
        "raster_gate_pass_all_frames": bool(frame_reports)
        and all(frame["raster_gate_pass"] == 1 for frame in frame_reports),
        "totals": dict(sorted(aggregate.items())),
        "owner_dispatch": {
            "owner_occurrences": len(owner_dispatch),
            "mapped": dispatch_counts["mapped"],
            "unmapped_keys": dispatch_counts["unmapped-key"],
            "incomplete_keys": dispatch_counts["incomplete-key"],
            "unknown_distinct_dispatch_keys": len(unknown_keys),
            "unknown_dispatch_sha256_preview": sorted(unknown_keys)[:64],
        },
        "owner_topology_dispatch": (
            {
                "owner_occurrences": len(owner_dispatch),
                "mapped": topology_dispatch_counts["mapped"],
                "unmapped_keys": topology_dispatch_counts["unmapped-key"],
                "incomplete_keys": topology_dispatch_counts["incomplete-key"],
                "unknown_distinct_dispatch_keys": len(unknown_topology_keys),
                "unknown_dispatch_sha256_preview": sorted(
                    unknown_topology_keys
                )[:64],
            }
            if topology_dispatch is not None
            else None
        ),
        "failure_component_triplets": component_summary,
        "failure_topology_triplets": topology_summary,
        "failure_dispatch_triplets": dispatch_summary,
        "proof_scope": {
            "multipliers_fitted_or_changed": False,
            "family_count_inferred": False,
            "topology_component_count_inferred_as_family_count": False,
            "topology_components_promoted_to_persistent_objects": False,
            "integer_or_tenth_prior_used": False,
            "numeric_transform_values_used_for_dispatch": False,
            "report_is_candidate_rejection_evidence_only": True,
        },
        "errors": errors,
    }


def analyze_candidate_trace(
    trace_path: Path,
    manifest_path: Path,
    topology_lineage_path: Path | None = None,
) -> dict[str, Any]:
    records = []
    with trace_path.open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise ValueError(f"{trace_path}:{line_number}: invalid JSON") from exc
    topology_lineage = (
        json.loads(topology_lineage_path.read_text(encoding="utf-8"))
        if topology_lineage_path is not None
        else None
    )
    return analyze_candidate_records(
        records,
        manifest_path.read_text(encoding="utf-8"),
        topology_lineage,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument(
        "--topology-lineage",
        type=Path,
        help=(
            "optional cross-validated lineage report used only to label "
            "failure triplets by stable partial projection topology"
        ),
    )
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        report = analyze_candidate_trace(
            args.trace, args.manifest, args.topology_lineage
        )
    except (OSError, ValueError) as exc:
        print(f"visibility_candidate_trace: {exc}", file=sys.stderr)
        return 2
    payload = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(payload, encoding="utf-8")
    else:
        print(payload, end="")
    return 0 if report["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
