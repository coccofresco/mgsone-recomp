#!/usr/bin/env python3
"""Validate exhaustive Vulkan fragment stacks and label every occluder pair.

The collector is complete only when its GPU append counter did not overflow and
the CPU audit reconciled the final OT owner with the accepted fragment stack.
This tool joins each resulting physical-occluder/desired-OT pair to the compact
intrinsic owner key and, optionally, to a cross-validated partial projection
topology.  It never assumes a layer/family count and never fits or rounds a
presentation multiplier.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import math
import struct
import sys
from pathlib import Path
from typing import Any

from visibility_candidate_trace import _parse_topology_dispatch
from visibility_lineage_trace import (
    _owner_fields,
    _runtime_dispatch_key,
    _runtime_dispatch_key_report_entry,
    _signature_id,
    _validate_owner,
)


TRACE_SCHEMA = "ps1trace/v1"
REPORT_SCHEMA = "ps1-visibility-fragment-stack-audit/v1"
AUDIT_EVENT = "replay.visibility_stack_audit"
OCCLUDER_EVENT = "replay.visibility_stack_occluder"
OWNER_EVENT = "replay.visibility_owner"
CATALOG_EVENT = "replay.visibility_catalog"
STACK_SCHEMA = 1
COMPARED_STATUS = 0
OVERFLOW_STATUS = 4


def _integer(record: dict[str, Any], field: str, context: str) -> int:
    value = record.get(field)
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{context}: missing/non-integer field {field!r}")
    return value


def _depth(bits: int, context: str) -> float:
    if bits < 0 or bits > 0xFFFFFFFF:
        raise ValueError(f"{context}: depth bits are outside uint32")
    value = struct.unpack("<f", struct.pack("<I", bits))[0]
    if not math.isfinite(value) or not 0.0 < value < 1.0:
        raise ValueError(f"{context}: depth is not a finite physical D32 value")
    return value


def _relaxed_owner_errors(owner: dict[str, Any], context: str) -> list[str]:
    """Retain structural checks while allowing an unmapped topology endpoint."""

    errors = _validate_owner(owner, context)
    if owner.get("comparable") == 1 and owner.get("valid_topology") == 0:
        errors = [
            error
            for error in errors
            if not error.endswith(
                "a selected owner is not comparable/topology-valid"
            )
        ]
    return errors


def analyze_visibility_stack_records(
    records: list[dict[str, Any]], topology_lineage: Any | None = None
) -> dict[str, Any]:
    if topology_lineage is None:
        topology_dispatch = None
        topology_report = None
    else:
        topology_dispatch, topology_report = _parse_topology_dispatch(
            topology_lineage
        )
        topology_report = {
            **topology_report,
            "source_sha256": hashlib.sha256(
                json.dumps(
                    topology_lineage, separators=(",", ":"), sort_keys=True
                ).encode("utf-8")
            ).hexdigest(),
        }

    errors: list[str] = []
    warnings: list[str] = []
    audits: dict[int, dict[str, Any]] = {}
    occluders: dict[int, list[dict[str, Any]]] = collections.defaultdict(list)
    catalogs: dict[int, dict[str, Any]] = {}
    owners: dict[tuple[int, int], dict[str, Any]] = {}

    for record_index, record in enumerate(records):
        event = record.get("event")
        if event not in {AUDIT_EVENT, OCCLUDER_EVENT, OWNER_EVENT, CATALOG_EVENT}:
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
                errors.append(f"{context}: duplicate stack audit frame {frame}")
            else:
                audits[frame] = record
        elif event == OCCLUDER_EVENT:
            occluders[frame].append(record)
        elif event == CATALOG_EVENT:
            if frame in catalogs:
                errors.append(f"{context}: duplicate owner catalog frame {frame}")
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
            owner_errors = _relaxed_owner_errors(parsed, context)
            if owner_errors:
                errors.extend(owner_errors)
                continue
            owners[key] = parsed

    if not audits:
        errors.append(f"trace has no {AUDIT_EVENT} records")

    owner_assignments: dict[tuple[int, int], dict[str, Any]] = {}
    topology_counts = collections.Counter()
    unknown_topology_keys: set[str] = set()
    incomplete_dispatch_keys: set[str] = set()
    for owner_key, owner in owners.items():
        runtime_key = _runtime_dispatch_key(owner)
        dispatch_sha256 = _signature_id(runtime_key) if runtime_key else None
        if runtime_key is None:
            dispatch_status = "incomplete-key"
            incomplete_dispatch_keys.add(f"{owner_key[0]}:{owner_key[1]}")
        else:
            dispatch_status = "complete-key"

        if topology_dispatch is None:
            topology_status = "not-requested"
            topology_component = None
        elif owner["valid_topology"] != 1 or runtime_key is None:
            topology_status = "incomplete-key"
            topology_component = None
        else:
            topology_component = topology_dispatch.get(runtime_key)
            if topology_component is None:
                topology_status = "unmapped-key"
                if dispatch_sha256 is not None:
                    unknown_topology_keys.add(dispatch_sha256)
            else:
                topology_status = "mapped"
        topology_counts[topology_status] += 1
        owner_assignments[owner_key] = {
            "dispatch_status": dispatch_status,
            "dispatch_sha256": dispatch_sha256,
            "runtime_dispatch_entry": (
                _runtime_dispatch_key_report_entry(runtime_key)
                if runtime_key is not None
                else None
            ),
            "topology_status": topology_status,
            "topology_component": topology_component,
        }

    def assignment(frame: int, token: int) -> dict[str, Any] | None:
        return owner_assignments.get((frame, token))

    frame_reports: list[dict[str, Any]] = []
    totals = collections.Counter()
    dispatch_pairs = collections.Counter()
    topology_pairs = collections.Counter()
    incomplete_endpoint_samples = 0
    unmapped_topology_endpoint_samples = 0
    same_topology_component_samples = 0
    incomplete_endpoint_memberships: collections.Counter[tuple[int, int]] = (
        collections.Counter()
    )
    unmapped_topology_endpoint_memberships: collections.Counter[
        tuple[int, int]
    ] = collections.Counter()
    unmapped_topology_endpoint_roles: dict[
        tuple[int, int], collections.Counter[str]
    ] = collections.defaultdict(collections.Counter)

    audit_fields = (
        "status",
        "complete",
        "owners",
        "atlas_x",
        "atlas_y",
        "width",
        "height",
        "attempted_records",
        "record_capacity",
        "overflow",
        "retained_records",
        "stale_records",
        "boundary_records",
        "duplicate_owner_records",
        "inconsistent_duplicate_depth_records",
        "ot_evidence_samples",
        "complete_owned_samples",
        "missing_ot_fragment_samples",
        "ot_depth_mismatch_samples",
        "invalid_owner_samples",
        "invalid_depth_samples",
        "occluded_ot_samples",
        "maximum_distinct_owners_per_sample",
        "occluder_pairs",
    )
    for frame in sorted(audits):
        audit = audits[frame]
        context = f"frame {frame}"
        try:
            if _integer(audit, "stack_schema", context) != STACK_SCHEMA:
                errors.append(f"{context}: unsupported stack schema")
            values = {
                field: _integer(audit, field, context) for field in audit_fields
            }
        except ValueError as exc:
            errors.append(str(exc))
            continue
        if any(value < 0 for value in values.values()):
            errors.append(f"{context}: negative stack field")
            continue
        if values["complete"] not in (0, 1) or values["overflow"] not in (0, 1):
            errors.append(f"{context}: complete/overflow is not boolean")
        if values["width"] == 0 or values["height"] == 0:
            errors.append(f"{context}: empty stack region")
        pixels = values["width"] * values["height"]
        if values["record_capacity"] == 0:
            errors.append(f"{context}: zero record capacity")
        if values["owners"] == 0:
            errors.append(f"{context}: empty owner catalog")
        if values["maximum_distinct_owners_per_sample"] > values["owners"]:
            errors.append(f"{context}: distinct-owner maximum exceeds catalog")
        for field in (
            "ot_evidence_samples",
            "complete_owned_samples",
            "missing_ot_fragment_samples",
            "ot_depth_mismatch_samples",
            "invalid_owner_samples",
            "invalid_depth_samples",
            "occluded_ot_samples",
        ):
            if values[field] > pixels:
                errors.append(f"{context}: {field} exceeds region samples")
        if values["complete_owned_samples"] > values["ot_evidence_samples"]:
            errors.append(f"{context}: complete OT samples exceed evidence")
        if values["occluded_ot_samples"] > values["complete_owned_samples"]:
            errors.append(f"{context}: occluded samples exceed complete samples")
        if (
            values["stale_records"] + values["boundary_records"]
            > values["retained_records"]
        ):
            errors.append(f"{context}: stale/boundary records exceed retained")

        overflow = values["overflow"] == 1
        if overflow:
            if (
                values["status"] != OVERFLOW_STATUS
                or values["complete"] != 0
                or values["attempted_records"] <= values["record_capacity"]
                or values["retained_records"] != values["record_capacity"]
            ):
                errors.append(f"{context}: malformed overflow result")
            errors.append(
                f"{context}: fragment stack overflowed; replay with at least "
                f"{values['attempted_records']} records"
            )
        else:
            if values["attempted_records"] > values["record_capacity"]:
                errors.append(f"{context}: attempted records exceed capacity")
            if values["retained_records"] != values["attempted_records"]:
                errors.append(f"{context}: retained/attempted mismatch")
            if values["status"] != COMPARED_STATUS or values["complete"] != 1:
                errors.append(f"{context}: stack capture is not complete")
            for field in (
                "inconsistent_duplicate_depth_records",
                "missing_ot_fragment_samples",
                "ot_depth_mismatch_samples",
                "invalid_owner_samples",
                "invalid_depth_samples",
            ):
                if values[field] != 0:
                    errors.append(f"{context}: complete capture has nonzero {field}")

        catalog = catalogs.get(frame)
        frame_owner_records = sum(1 for owner_frame, _ in owners if owner_frame == frame)
        if catalog is None:
            errors.append(f"{context}: missing complete owner catalog")
        else:
            try:
                catalog_schema = _integer(catalog, "catalog_schema", context)
                owner_schema = _integer(catalog, "owner_schema", context)
                catalog_owners = _integer(catalog, "owners", context)
                eligible = _integer(catalog, "eligible_owners", context)
                emitted = _integer(catalog, "emitted_owners", context)
            except ValueError as exc:
                errors.append(str(exc))
            else:
                if catalog_schema != 1 or owner_schema < 2:
                    errors.append(f"{context}: unsupported catalog schema")
                if catalog_owners != values["owners"]:
                    errors.append(f"{context}: stack/catalog owner count mismatch")
                if emitted != frame_owner_records:
                    errors.append(f"{context}: catalog/owner record mismatch")
                if not 0 <= eligible <= emitted <= catalog_owners:
                    errors.append(f"{context}: invalid catalog counts")

        frame_pairs = occluders.get(frame, [])
        if len(frame_pairs) != values["occluder_pairs"]:
            errors.append(
                f"{context}: {len(frame_pairs)} occluder records, expected "
                f"{values['occluder_pairs']}"
            )
        pair_tokens: set[tuple[int, int]] = set()
        pair_sample_sum = 0
        pair_preview: list[dict[str, Any]] = []
        for pair_index, pair in enumerate(frame_pairs):
            pair_context = f"{context} occluder {pair_index}"
            try:
                if _integer(pair, "stack_schema", pair_context) != STACK_SCHEMA:
                    raise ValueError(f"{pair_context}: unsupported stack schema")
                ot_owner = _integer(pair, "ot_owner", pair_context)
                physical_owner = _integer(pair, "physical_owner", pair_context)
                samples = _integer(pair, "samples", pair_context)
                first_x = _integer(pair, "first_atlas_x", pair_context)
                first_y = _integer(pair, "first_atlas_y", pair_context)
                depths = {
                    name: _depth(_integer(pair, name, pair_context), pair_context)
                    for name in (
                        "minimum_ot_depth_bits",
                        "maximum_ot_depth_bits",
                        "minimum_physical_depth_bits",
                        "maximum_physical_depth_bits",
                    )
                }
            except ValueError as exc:
                errors.append(str(exc))
                continue
            token_pair = (ot_owner, physical_owner)
            if token_pair in pair_tokens:
                errors.append(f"{pair_context}: duplicate directed owner pair")
            pair_tokens.add(token_pair)
            if (
                ot_owner <= 0
                or physical_owner <= 0
                or ot_owner > values["owners"]
                or physical_owner > values["owners"]
                or ot_owner == physical_owner
            ):
                errors.append(f"{pair_context}: invalid owner token pair")
            if samples <= 0:
                errors.append(f"{pair_context}: non-positive sample count")
                continue
            if not (
                values["atlas_x"]
                <= first_x
                < values["atlas_x"] + values["width"]
                and values["atlas_y"]
                <= first_y
                < values["atlas_y"] + values["height"]
            ):
                errors.append(f"{pair_context}: first sample outside region")
            if depths["minimum_ot_depth_bits"] > depths["maximum_ot_depth_bits"]:
                errors.append(f"{pair_context}: inverted OT depth extrema")
            if (
                depths["minimum_physical_depth_bits"]
                > depths["maximum_physical_depth_bits"]
            ):
                errors.append(f"{pair_context}: inverted physical depth extrema")

            ot_assignment = assignment(frame, ot_owner)
            physical_assignment = assignment(frame, physical_owner)
            ot_record = owners.get((frame, ot_owner))
            physical_record = owners.get((frame, physical_owner))
            if (
                ot_assignment is None
                or physical_assignment is None
                or ot_record is None
                or physical_record is None
            ):
                errors.append(f"{pair_context}: endpoint lacks owner record")
                incomplete_endpoint_samples += samples
                continue
            ot_order = (ot_record["source_ordinal"], ot_record["triangle_index"])
            physical_order = (
                physical_record["source_ordinal"],
                physical_record["triangle_index"],
            )
            if ot_order <= physical_order:
                errors.append(f"{pair_context}: desired OT owner is not later")
            dispatch_key = (
                ot_assignment["dispatch_sha256"]
                or ot_assignment["dispatch_status"],
                physical_assignment["dispatch_sha256"]
                or physical_assignment["dispatch_status"],
            )
            dispatch_pairs[dispatch_key] += samples
            for token, endpoint_assignment in (
                (ot_owner, ot_assignment),
                (physical_owner, physical_assignment),
            ):
                if endpoint_assignment["dispatch_status"] != "complete-key":
                    incomplete_endpoint_samples += samples
                    incomplete_endpoint_memberships[(frame, token)] += samples

            topology_value = None
            if topology_dispatch is not None:
                ot_topology = (
                    ot_assignment["topology_component"]
                    or ot_assignment["topology_status"]
                )
                physical_topology = (
                    physical_assignment["topology_component"]
                    or physical_assignment["topology_status"]
                )
                topology_pairs[(ot_topology, physical_topology)] += samples
                topology_value = {
                    "ot": ot_topology,
                    "physical": physical_topology,
                }
                for role, token, endpoint_assignment in (
                    ("ot", ot_owner, ot_assignment),
                    ("physical", physical_owner, physical_assignment),
                ):
                    if endpoint_assignment["topology_status"] != "mapped":
                        unmapped_topology_endpoint_samples += samples
                        unmapped_topology_endpoint_memberships[
                            (frame, token)
                        ] += samples
                        unmapped_topology_endpoint_roles[(frame, token)][
                            role
                        ] += samples
                if (
                    ot_assignment["topology_status"] == "mapped"
                    and physical_assignment["topology_status"] == "mapped"
                    and ot_topology == physical_topology
                ):
                    same_topology_component_samples += samples

            pair_sample_sum += samples
            if len(pair_preview) < 32:
                pair_preview.append(
                    {
                        "owners": {"ot": ot_owner, "physical": physical_owner},
                        "samples": samples,
                        "first_atlas": [first_x, first_y],
                        "dispatch_sha256": {
                            "ot": ot_assignment["dispatch_sha256"],
                            "physical": physical_assignment["dispatch_sha256"],
                        },
                        "topology_components": topology_value,
                        "depth_extrema": depths,
                    }
                )
        if values["occluded_ot_samples"] == 0 and pair_sample_sum != 0:
            errors.append(f"{context}: pairs exist without occluded samples")
        if values["occluded_ot_samples"] > 0 and pair_sample_sum < values[
            "occluded_ot_samples"
        ]:
            errors.append(f"{context}: pair samples do not cover occluded samples")

        frame_reports.append(
            {
                "frame": frame,
                **values,
                "occluder_sample_memberships": pair_sample_sum,
                "occluder_preview": pair_preview,
            }
        )
        for field in (
            "owners",
            "attempted_records",
            "retained_records",
            "stale_records",
            "boundary_records",
            "duplicate_owner_records",
            "inconsistent_duplicate_depth_records",
            "ot_evidence_samples",
            "complete_owned_samples",
            "missing_ot_fragment_samples",
            "ot_depth_mismatch_samples",
            "invalid_owner_samples",
            "invalid_depth_samples",
            "occluded_ot_samples",
            "occluder_pairs",
        ):
            totals[field] += values[field]
        totals["maximum_record_capacity"] = max(
            totals["maximum_record_capacity"], values["record_capacity"]
        )
        totals["maximum_distinct_owners_per_sample"] = max(
            totals["maximum_distinct_owners_per_sample"],
            values["maximum_distinct_owners_per_sample"],
        )

    orphan_occluders = sorted(set(occluders) - set(audits))
    if orphan_occluders:
        errors.append(f"occluder records without audits in frames {orphan_occluders}")
    orphan_catalogs = sorted(set(catalogs) - set(audits))
    if orphan_catalogs:
        warnings.append(f"catalog records without stack audits in frames {orphan_catalogs}")

    dispatch_summary = [
        {
            "ot_dispatch_sha256": key[0],
            "physical_dispatch_sha256": key[1],
            "samples": samples,
        }
        for key, samples in sorted(
            dispatch_pairs.items(), key=lambda item: (-item[1], item[0])
        )
    ]
    topology_summary = [
        {
            "ot_topology_component": key[0],
            "physical_topology_component": key[1],
            "samples": samples,
        }
        for key, samples in sorted(
            topology_pairs.items(), key=lambda item: (-item[1], item[0])
        )
    ]
    # Exact projection IDs are process-local, but equality inside one frame is
    # an intrinsic incidence relation. Report which already-mapped components
    # share a projected vertex with each unmapped pair endpoint. This is a
    # capture-planning witness only: no endpoint is attached until the same
    # relation is independently cross-validated.
    projection_components: dict[tuple[int, int], set[str]] = (
        collections.defaultdict(set)
    )
    projection_relations: dict[
        tuple[int, int], set[tuple[int, str, str]]
    ] = collections.defaultdict(set)
    if topology_dispatch is not None:
        for owner_key, owner in owners.items():
            owner_assignment = owner_assignments.get(owner_key)
            if (
                owner_assignment is None
                or owner_assignment["topology_status"] != "mapped"
            ):
                continue
            component = owner_assignment["topology_component"]
            for vertex in owner["vertices"]:
                if vertex["evidence"] & 1:
                    projection_key = (owner_key[0], vertex["projection_id"])
                    projection_components[projection_key].add(component)
                    projection_relations[projection_key].add(
                        (
                            vertex["index"],
                            component,
                            owner_assignment["dispatch_sha256"],
                        )
                    )

    anchor_groups: dict[str, dict[str, Any]] = {}
    for owner_key, sample_memberships in sorted(
        unmapped_topology_endpoint_memberships.items()
    ):
        owner = owners.get(owner_key)
        owner_assignment = owner_assignments.get(owner_key)
        if owner is None or owner_assignment is None:
            continue
        dispatch_id = (
            owner_assignment["dispatch_sha256"]
            or owner_assignment["dispatch_status"]
        )
        anchors: set[str] = set()
        relations: set[tuple[int, int, str, str]] = set()
        for vertex in owner["vertices"]:
            if vertex["evidence"] & 1:
                projection_key = (owner_key[0], vertex["projection_id"])
                anchors.update(projection_components.get(projection_key, set()))
                for mapped_vertex, component, mapped_dispatch in (
                    projection_relations.get(projection_key, set())
                ):
                    relations.add(
                        (
                            vertex["index"],
                            mapped_vertex,
                            component,
                            mapped_dispatch,
                        )
                    )
        group = anchor_groups.setdefault(
            dispatch_id,
            {
                "frames": set(),
                "owner_occurrences": 0,
                "sample_memberships": 0,
                "roles": collections.Counter(),
                "anchor_sets": collections.Counter(),
                "relation_sets": collections.Counter(),
                "runtime_dispatch_entries": {},
                "frame_occurrences": collections.Counter(),
            },
        )
        group["frames"].add(owner_key[0])
        group["owner_occurrences"] += 1
        group["sample_memberships"] += sample_memberships
        group["roles"].update(unmapped_topology_endpoint_roles[owner_key])
        group["anchor_sets"][tuple(sorted(anchors))] += 1
        group["relation_sets"][tuple(sorted(relations))] += 1
        runtime_entry = owner_assignment["runtime_dispatch_entry"]
        if runtime_entry is not None:
            canonical_entry = json.dumps(
                runtime_entry, separators=(",", ":"), sort_keys=True
            )
            group["runtime_dispatch_entries"].setdefault(
                canonical_entry, runtime_entry
            )
        group["frame_occurrences"][owner_key[0]] += 1

    anchor_summary = []
    for dispatch_id, group in sorted(
        anchor_groups.items(),
        key=lambda item: (-item[1]["sample_memberships"], item[0]),
    ):
        anchor_sets = [
            {"components": list(components), "owner_occurrences": count}
            for components, count in sorted(group["anchor_sets"].items())
        ]
        relation_sets = [
            {
                "relations": [
                    {
                        "endpoint_vertex": relation[0],
                        "mapped_vertex": relation[1],
                        "mapped_component": relation[2],
                        "mapped_dispatch_sha256": relation[3],
                    }
                    for relation in relations
                ],
                "owner_occurrences": count,
            }
            for relations, count in sorted(group["relation_sets"].items())
        ]
        frames = sorted(group["frames"])
        unique_nonempty_anchor = (
            len(group["anchor_sets"]) == 1
            and bool(next(iter(group["anchor_sets"]), ()))
        )
        runtime_entries = list(group["runtime_dispatch_entries"].values())
        if len(runtime_entries) != 1:
            errors.append(
                f"unmapped endpoint {dispatch_id}: expected one exact runtime "
                f"dispatch entry, observed {len(runtime_entries)}"
            )
        anchor_summary.append(
            {
                "endpoint_dispatch_sha256": dispatch_id,
                "runtime_dispatch_entry": (
                    runtime_entries[0] if len(runtime_entries) == 1 else None
                ),
                "frames": frames,
                "owner_occurrences": group["owner_occurrences"],
                "sample_memberships": group["sample_memberships"],
                "roles": dict(sorted(group["roles"].items())),
                "anchor_sets": anchor_sets,
                "relation_sets": relation_sets,
                "within_frame_duplicate": any(
                    count > 1 for count in group["frame_occurrences"].values()
                ),
                "repeats_across_frames": len(frames) > 1,
                "one_consistent_nonempty_anchor_set": unique_nonempty_anchor,
                "attachment_certified": False,
            }
        )
    complete_frames = sum(frame["complete"] == 1 for frame in frame_reports)
    overflow_frames = sum(frame["overflow"] == 1 for frame in frame_reports)
    return {
        "schema": REPORT_SCHEMA,
        "valid": not errors,
        "topology_partition": topology_report,
        "audited_frames": len(frame_reports),
        "complete_frames": complete_frames,
        "overflow_frames": overflow_frames,
        "frames": frame_reports,
        "totals": dict(sorted(totals.items())),
        "owner_dispatch": {
            "owner_occurrences": len(owner_assignments),
            "complete_keys": sum(
                value["dispatch_status"] == "complete-key"
                for value in owner_assignments.values()
            ),
            "incomplete_keys": len(incomplete_dispatch_keys),
        },
        "owner_topology_dispatch": (
            {
                "owner_occurrences": len(owner_assignments),
                "mapped": topology_counts["mapped"],
                "unmapped_keys": topology_counts["unmapped-key"],
                "incomplete_keys": topology_counts["incomplete-key"],
                "unknown_distinct_dispatch_keys": len(unknown_topology_keys),
                "unknown_dispatch_sha256_preview": sorted(unknown_topology_keys)[:64],
            }
            if topology_dispatch is not None
            else None
        ),
        "occluder_dispatch_pairs": dispatch_summary,
        "occluder_topology_pairs": topology_summary,
        "unmapped_endpoint_projection_anchors": anchor_summary,
        "constraint_coverage": {
            "all_captures_complete": bool(frame_reports)
            and complete_frames == len(frame_reports),
            "all_pair_endpoints_have_complete_dispatch_keys": (
                incomplete_endpoint_samples == 0
            ),
            "all_pair_endpoints_topology_mapped": (
                topology_dispatch is not None
                and unmapped_topology_endpoint_samples == 0
            ),
            "incomplete_endpoint_sample_memberships": incomplete_endpoint_samples,
            "unmapped_topology_endpoint_sample_memberships": (
                unmapped_topology_endpoint_samples
            ),
            "same_topology_component_sample_memberships": (
                same_topology_component_samples
            ),
            "ready_for_constraint_refinement": (
                not errors
                and topology_dispatch is not None
                and unmapped_topology_endpoint_samples == 0
            ),
        },
        "proof_scope": {
            "all_accepted_fragments_required": True,
            "overflow_is_incomplete": True,
            "fixed_layer_limit_used": False,
            "family_count_inferred": False,
            "topology_component_count_inferred_as_family_count": False,
            "topology_components_promoted_to_persistent_objects": False,
            "integer_or_tenth_prior_used": False,
            "multiplier_fitted_or_recommended": False,
            "projection_anchor_report_promoted_to_attachment": False,
            "runtime_dispatch_key_uses_intrinsic_fields_only": True,
        },
        "errors": errors,
        "warnings": warnings,
    }


def analyze_visibility_stack_trace(
    trace_path: Path, topology_lineage_path: Path | None = None
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
    return analyze_visibility_stack_records(records, topology_lineage)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--topology-lineage", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        report = analyze_visibility_stack_trace(
            args.trace, args.topology_lineage
        )
    except (OSError, ValueError) as exc:
        print(f"visibility_stack_trace: {exc}", file=sys.stderr)
        return 2
    payload = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(payload, encoding="utf-8")
    else:
        print(payload, end="")
    return 0 if report["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
