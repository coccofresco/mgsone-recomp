#!/usr/bin/env python3
"""Cross-validate exact frame-local topology contexts for stack endpoints.

Static v3 dispatch remains the only source of anchor labels.  For each
otherwise-unmapped owner occurrence, this tool records the exact equality of
its GTE projection IDs with vertices of already-static owners in the same
frame.  Process-local IDs are discarded; the contextual key contains the
endpoint's intrinsic runtime key and the canonical vertex-to-static-key
incidence relation only.

A context is admitted only when that exact nonempty relation occurs in both
discovery and holdout, is not duplicated in a frame, and every compact endpoint
fingerprint reproduces the same complete MIPS DAG in at least two
already-trusted heavy captures.  A single-anchor context inherits that static
label.  A recurrent multi-anchor context proves only a separately hashed
topology quotient over immutable static labels; it is not an object or family
merge.  This is a per-frame lookup refinement, not persistent identity, family
inference or factor fitting.
"""

from __future__ import annotations

import argparse
import collections
import json
import sys
from pathlib import Path
from typing import Any

from visibility_lineage_trace import (
    _owner_fields,
    _runtime_dispatch_key,
    _runtime_dispatch_key_report_entry,
    _signature_id,
)
from visibility_stack_constraints import (
    _canonical_sha256,
    _load_jsonl,
    _parse_refined_dispatch,
    _plain_int,
)
from visibility_stack_dispatch_refine import (
    _complete_triangle_signature,
    _extract_fingerprint_evidence,
)
from visibility_stack_trace import (
    CATALOG_EVENT,
    OCCLUDER_EVENT,
    OWNER_EVENT,
    TRACE_SCHEMA,
    analyze_visibility_stack_records,
)


REPORT_SCHEMA = "ps1-visibility-runtime-context-refinement/v1"
DISPATCH_SCHEMA = "ps1-visibility-runtime-context-dispatch-map/v1"
CONTEXT_SIGNATURE_SCHEMA = "visibility-runtime-context-dispatch/v1"
RELATION_SCHEMA = "exact-static-projection-incidence/v1"
QUOTIENT_SCHEMA = "projection-incidence-context-quotient/v1"


class _UnionFind:
    def __init__(self) -> None:
        self.parent: dict[str, str] = {}

    def find(self, value: str) -> str:
        self.parent.setdefault(value, value)
        if self.parent[value] != value:
            self.parent[value] = self.find(self.parent[value])
        return self.parent[value]

    def union(self, left: str, right: str) -> None:
        left_root = self.find(left)
        right_root = self.find(right)
        if left_root == right_root:
            return
        if left_root < right_root:
            self.parent[right_root] = left_root
        else:
            self.parent[left_root] = right_root


def _context_signature(
    runtime_key: tuple[Any, ...],
    relations: tuple[tuple[int, int, str, str], ...],
) -> tuple[Any, ...]:
    return (
        CONTEXT_SIGNATURE_SCHEMA,
        runtime_key,
        (RELATION_SCHEMA, relations),
    )


def _parse_context_capture(
    records: list[dict[str, Any]],
    label: str,
    base_lineage: Any,
    static_dispatch: dict[tuple[Any, ...], str],
) -> dict[str, Any]:
    stack_report = analyze_visibility_stack_records(records, base_lineage)
    coverage = stack_report.get("constraint_coverage")
    if stack_report.get("valid") is not True:
        details = "; ".join(stack_report.get("errors", [])[:3])
        raise ValueError(f"{label}: invalid fragment stack: {details}")
    if (
        not isinstance(coverage, dict)
        or coverage.get("all_captures_complete") is not True
        or coverage.get("all_pair_endpoints_have_complete_dispatch_keys") is not True
        or stack_report.get("overflow_frames") != 0
    ):
        raise ValueError(f"{label}: fragment stack is incomplete")

    owners: dict[tuple[int, int], dict[str, Any]] = {}
    projection_users: dict[
        tuple[int, int], list[tuple[tuple[int, int], int]]
    ] = collections.defaultdict(list)
    memberships: collections.Counter[tuple[int, int]] = collections.Counter()
    roles: dict[tuple[int, int], collections.Counter[str]] = (
        collections.defaultdict(collections.Counter)
    )
    pair_records = 0
    pair_samples = 0

    for record_index, record in enumerate(records):
        event = record.get("event")
        if event not in {OWNER_EVENT, OCCLUDER_EVENT, CATALOG_EVENT}:
            continue
        context = f"{label} record {record_index}"
        if record.get("schema") != TRACE_SCHEMA:
            raise ValueError(f"{context}: invalid trace schema")
        frame = _plain_int(record.get("frame"), 0, 2**63 - 1, f"{context} frame")
        if event == OWNER_EVENT:
            token = _plain_int(
                record.get("owner"), 1, 2**32 - 1, f"{context} owner"
            )
            owner = _owner_fields(record, context)
            runtime_key = _runtime_dispatch_key(owner)
            owner_key = (frame, token)
            if runtime_key is None:
                continue
            if owner_key in owners:
                raise ValueError(f"{context}: duplicate owner token")
            owners[owner_key] = {
                "owner": owner,
                "runtime_key": runtime_key,
                "dispatch_sha256": _signature_id(runtime_key),
                "static_component": static_dispatch.get(runtime_key),
            }
            for vertex in owner["vertices"]:
                if vertex["evidence"] & 1:
                    projection_users[(frame, vertex["projection_id"])].append(
                        (owner_key, vertex["index"])
                    )
        elif event == OCCLUDER_EVENT:
            ot = _plain_int(
                record.get("ot_owner"), 1, 2**32 - 1, f"{context} OT owner"
            )
            physical = _plain_int(
                record.get("physical_owner"),
                1,
                2**32 - 1,
                f"{context} physical owner",
            )
            samples = _plain_int(
                record.get("samples"), 1, 2**63 - 1, f"{context} samples"
            )
            pair_records += 1
            pair_samples += samples
            for role, token in (("ot", ot), ("physical", physical)):
                memberships[(frame, token)] += samples
                roles[(frame, token)][role] += samples

    raw_groups: dict[tuple[Any, ...], dict[str, Any]] = {}
    context_signatures_by_sha: dict[str, tuple[Any, ...]] = {}
    frame_context_occurrences: collections.Counter[
        tuple[int, tuple[Any, ...]]
    ] = collections.Counter()
    static_endpoint_samples = 0
    static_endpoint_owner_occurrences = 0
    for owner_key, samples in sorted(memberships.items()):
        owner_record = owners.get(owner_key)
        if owner_record is None:
            raise ValueError(f"{label}: pair endpoint {owner_key} lacks a runtime key")
        if owner_record["static_component"] is not None:
            static_endpoint_samples += samples
            static_endpoint_owner_occurrences += 1
            continue

        relations: set[tuple[int, int, str, str]] = set()
        for vertex in owner_record["owner"]["vertices"]:
            if not vertex["evidence"] & 1:
                continue
            projection_key = (owner_key[0], vertex["projection_id"])
            for mapped_owner_key, mapped_vertex in projection_users.get(
                projection_key, ()
            ):
                if mapped_owner_key == owner_key:
                    continue
                mapped = owners[mapped_owner_key]
                component = mapped["static_component"]
                if component is None:
                    continue
                relations.add(
                    (
                        vertex["index"],
                        mapped_vertex,
                        component,
                        mapped["dispatch_sha256"],
                    )
                )
        canonical_relations = tuple(sorted(relations))
        runtime_key = owner_record["runtime_key"]
        context_signature = _context_signature(runtime_key, canonical_relations)
        context_sha = _signature_id(context_signature)
        previous_signature = context_signatures_by_sha.setdefault(
            context_sha, context_signature
        )
        if previous_signature != context_signature:
            raise ValueError(f"{label}: contextual SHA-256 collision")
        group = raw_groups.setdefault(
            context_signature,
            {
                "context_dispatch_sha256": context_sha,
                "endpoint_dispatch_sha256": owner_record["dispatch_sha256"],
                "runtime_key": runtime_key,
                "runtime_dispatch_entry": _runtime_dispatch_key_report_entry(
                    runtime_key
                ),
                "relations": canonical_relations,
                "frames": set(),
                "owner_occurrences": 0,
                "sample_memberships": 0,
                "roles": collections.Counter(),
            },
        )
        group["frames"].add(owner_key[0])
        group["owner_occurrences"] += 1
        group["sample_memberships"] += samples
        group["roles"].update(roles[owner_key])
        frame_context_occurrences[(owner_key[0], context_signature)] += 1

    groups: dict[tuple[Any, ...], dict[str, Any]] = {}
    for context_signature, group in raw_groups.items():
        frames = sorted(group["frames"])
        groups[context_signature] = {
            **group,
            "frames": frames,
            "roles": dict(sorted(group["roles"].items())),
            "within_frame_duplicate": any(
                frame_context_occurrences[(frame, context_signature)] > 1
                for frame in frames
            ),
            "anchor_components": sorted(
                {relation[2] for relation in group["relations"]}
            ),
        }

    unknown_endpoint_samples = sum(memberships.values()) - static_endpoint_samples
    totals = stack_report.get("totals", {})
    if pair_records != totals.get("occluder_pairs"):
        raise ValueError(f"{label}: raw/context pair record count mismatch")
    if sum(memberships.values()) != pair_samples * 2:
        raise ValueError(f"{label}: endpoint membership accounting mismatch")
    return {
        "groups": groups,
        "audited_frames": stack_report["audited_frames"],
        "frames": [frame["frame"] for frame in stack_report["frames"]],
        "pair_records": pair_records,
        "pair_sample_memberships": pair_samples,
        "endpoint_sample_memberships": pair_samples * 2,
        "static_endpoint_owner_occurrences": static_endpoint_owner_occurrences,
        "static_endpoint_sample_memberships": static_endpoint_samples,
        "unknown_endpoint_sample_memberships": unknown_endpoint_samples,
        "unknown_context_variants": len(groups),
        "unknown_endpoint_runtime_keys": len(
            {group["endpoint_dispatch_sha256"] for group in groups.values()}
        ),
        "stack_report_sha256": _canonical_sha256(stack_report),
    }


def refine_visibility_stack_contexts(
    base_lineage: Any,
    static_refinement: Any,
    discovery_records: list[dict[str, Any]],
    holdout_records: list[dict[str, Any]],
    fingerprint_evidence: list[Path],
) -> dict[str, Any]:
    static_dispatch, static_summary = _parse_refined_dispatch(
        base_lineage, static_refinement
    )
    base_dispatch = static_summary.pop("base_dispatch")
    attachment_keys = static_summary.pop("attachment_keys")
    del base_dispatch, attachment_keys
    discovery = _parse_context_capture(
        discovery_records, "discovery", base_lineage, static_dispatch
    )
    holdout = _parse_context_capture(
        holdout_records, "holdout", base_lineage, static_dispatch
    )

    admitted: list[tuple[tuple[Any, ...], dict[str, Any], dict[str, Any]]] = []
    discovery_only = []
    holdout_only = []
    empty = []
    merge_contexts = []
    duplicated = []
    all_contexts = sorted(
        set(discovery["groups"]) | set(holdout["groups"]),
        key=_signature_id,
    )
    for signature in all_contexts:
        left = discovery["groups"].get(signature)
        right = holdout["groups"].get(signature)
        digest = _signature_id(signature)
        if left is None:
            holdout_only.append(digest)
            continue
        if right is None:
            discovery_only.append(digest)
            continue
        if left["runtime_key"] != right["runtime_key"]:
            raise ValueError(f"context {digest}: discovery/holdout key mismatch")
        if left["within_frame_duplicate"] or right["within_frame_duplicate"]:
            duplicated.append(digest)
            continue
        if not left["relations"]:
            empty.append(digest)
            continue
        if left["anchor_components"] != right["anchor_components"]:
            raise ValueError(f"context {digest}: anchor component mismatch")
        if len(left["anchor_components"]) > 1:
            merge_contexts.append(
                {
                    "context_dispatch_sha256": digest,
                    "anchor_components": left["anchor_components"],
                }
            )
        admitted.append((signature, left, right))

    if not admitted:
        raise ValueError("no exact contextual attachment survives cross-validation")

    union = _UnionFind()
    static_components = sorted(set(static_dispatch.values()))
    for component in static_components:
        union.find(component)
    for item in merge_contexts:
        components = item["anchor_components"]
        for component in components[1:]:
            union.union(components[0], component)
    groups: dict[str, list[str]] = collections.defaultdict(list)
    for component in static_components:
        groups[union.find(component)].append(component)
    component_projection: dict[str, str] = {}
    quotient_groups = []
    for members in sorted((sorted(values) for values in groups.values())):
        target = (
            members[0]
            if len(members) == 1
            else _signature_id((QUOTIENT_SCHEMA, tuple(members)))
        )
        for member in members:
            component_projection[member] = target
        if len(members) > 1:
            quotient_groups.append(
                {
                    "quotient_component_sha256": target,
                    "static_anchor_components": members,
                }
            )

    if len(fingerprint_evidence) < 2:
        raise ValueError("at least two heavy fingerprint-evidence captures are required")
    resolved_paths = [path.resolve() for path in fingerprint_evidence]
    if len(set(resolved_paths)) != len(resolved_paths):
        raise ValueError("fingerprint-evidence captures must be distinct")
    trusted_heavy = {
        Path(capture["instrumented_path"]).resolve()
        for capture in base_lineage.get("captures", [])
        if isinstance(capture, dict)
        and isinstance(capture.get("instrumented_path"), str)
        and capture.get("instrumented_stream_equal") is True
        and capture.get("instrumented_all_projections_complete") is True
    }
    if any(path not in trusted_heavy for path in resolved_paths):
        raise ValueError(
            "every heavy fingerprint capture must have an equivalent complete "
            "pair in the base lineage report"
        )

    runtime_keys: dict[str, tuple[Any, ...]] = {}
    for _, left, _ in admitted:
        endpoint = left["endpoint_dispatch_sha256"]
        previous = runtime_keys.setdefault(endpoint, left["runtime_key"])
        if previous != left["runtime_key"]:
            raise ValueError("endpoint runtime-key SHA-256 collision")
    target_pairs = {
        (vertex[3], vertex[4])
        for runtime_key in runtime_keys.values()
        for vertex in runtime_key[3]
    }
    capture_maps = []
    capture_reports = []
    for path in fingerprint_evidence:
        mapping, report = _extract_fingerprint_evidence(path, target_pairs)
        capture_maps.append(mapping)
        capture_reports.append(report)
    fingerprints: dict[tuple[int, int], tuple[str, str]] = {}
    for pair in sorted(target_pairs):
        values = {mapping[pair] for mapping in capture_maps}
        if len(values) != 1:
            raise ValueError(
                "heavy captures disagree on a compact fingerprint's full DAG"
            )
        fingerprints[pair] = next(iter(values))

    complete_triangle_sha = {
        endpoint: _signature_id(
            _complete_triangle_signature(runtime_key, fingerprints)
        )
        for endpoint, runtime_key in runtime_keys.items()
    }
    entries = []
    discovery_admitted_samples = 0
    holdout_admitted_samples = 0
    for signature, left, right in admitted:
        anchor_components = left["anchor_components"]
        projected_components = {
            component_projection[component] for component in anchor_components
        }
        if len(projected_components) != 1:
            raise ValueError("admitted context does not resolve in the quotient")
        component = next(iter(projected_components))
        relations = left["relations"]
        context_sha = _signature_id(signature)
        proof_payload = (
            "visibility-runtime-context-attachment/v1",
            context_sha,
            complete_triangle_sha[left["endpoint_dispatch_sha256"]],
            component,
            tuple(anchor_components),
            relations,
            tuple(
                (pair, fingerprints[pair])
                for pair in sorted(
                    {
                        (vertex[3], vertex[4])
                        for vertex in left["runtime_key"][3]
                    }
                )
            ),
        )
        discovery_admitted_samples += left["sample_memberships"]
        holdout_admitted_samples += right["sample_memberships"]
        entries.append(
            {
                "context_dispatch_sha256": context_sha,
                "endpoint_dispatch_sha256": left["endpoint_dispatch_sha256"],
                "complete_triangle_sha256": complete_triangle_sha[
                    left["endpoint_dispatch_sha256"]
                ],
                "target_component_sha256": component,
                "static_anchor_components": anchor_components,
                "context_attachment_proof_sha256": _signature_id(proof_payload),
                "runtime_dispatch_entry": left["runtime_dispatch_entry"],
                "exact_projection_relations": len(relations),
                "context_kind": (
                    "single-static-anchor"
                    if len(anchor_components) == 1
                    else "cross-validated-topology-merge"
                ),
                "exact_static_projection_relations": [
                    {
                        "endpoint_vertex": relation[0],
                        "mapped_vertex": relation[1],
                        "mapped_component": relation[2],
                        "mapped_dispatch_sha256": relation[3],
                    }
                    for relation in relations
                ],
                "discovery_frames": left["frames"],
                "holdout_frames": right["frames"],
                "discovery_owner_occurrences": left["owner_occurrences"],
                "holdout_owner_occurrences": right["owner_occurrences"],
                "discovery_sample_memberships": left["sample_memberships"],
                "holdout_sample_memberships": right["sample_memberships"],
                "frame_local_occurrence_attachment": True,
                "persistent_runtime_key_promotion": False,
            }
        )
    entries.sort(key=lambda entry: entry["context_dispatch_sha256"])

    def capture_report(value: dict[str, Any], admitted: int) -> dict[str, Any]:
        return {
            key: item
            for key, item in value.items()
            if key != "groups"
        } | {
            "admitted_context_sample_memberships": admitted,
            "residual_unknown_endpoint_sample_memberships": (
                value["unknown_endpoint_sample_memberships"] - admitted
            ),
        }

    return {
        "schema": REPORT_SCHEMA,
        "valid": True,
        "base_static_dispatch": static_summary,
        "base_static_refinement_source_sha256": _canonical_sha256(
            static_refinement
        ),
        "discovery": capture_report(discovery, discovery_admitted_samples),
        "holdout": capture_report(holdout, holdout_admitted_samples),
        "context_cross_validation": {
            "discovery_variants": len(discovery["groups"]),
            "holdout_variants": len(holdout["groups"]),
            "observed_in_both": len(
                set(discovery["groups"]) & set(holdout["groups"])
            ),
            "stable_exact_nonempty_contexts": len(entries),
            "stable_exact_single_component_contexts": (
                len(entries) - len(merge_contexts)
            ),
            "stable_exact_merge_contexts": len(merge_contexts),
            "stable_endpoint_runtime_keys": len(runtime_keys),
            "empty_contexts": len(empty),
            "multi_component_contexts": len(merge_contexts),
            "within_frame_duplicate_contexts": len(duplicated),
            "discovery_only_contexts": len(discovery_only),
            "holdout_only_contexts": len(holdout_only),
            "empty_context_sha256": empty,
            "multi_component_context_details": merge_contexts,
            "within_frame_duplicate_context_sha256": duplicated,
            "discovery_only_context_sha256": discovery_only,
            "holdout_only_context_sha256": holdout_only,
        },
        "fingerprint_evidence": {
            "captures": capture_reports,
            "capture_count": len(capture_reports),
            "required_compact_fingerprints": len(target_pairs),
            "endpoint_runtime_keys": len(runtime_keys),
            "cross_capture_dag_mismatches": 0,
            "all_required_fingerprints_cross_validated": True,
        },
        "runtime_context_dispatch": {
            "schema": DISPATCH_SCHEMA,
            "context_signature_schema": CONTEXT_SIGNATURE_SCHEMA,
            "relation_schema": RELATION_SCHEMA,
            "base_static_dispatch_entries": static_summary["refined_entries"],
            "entries": entries,
            "entry_count": len(entries),
            "endpoint_runtime_keys": len(runtime_keys),
            "target_components": len(
                {entry["target_component_sha256"] for entry in entries}
            ),
            "quotient_schema": QUOTIENT_SCHEMA,
            "base_static_components": len(static_components),
            "quotient_components": len(set(component_projection.values())),
            "cross_validated_quotient_groups": len(quotient_groups),
            "component_projection": [
                {
                    "static_component_sha256": component,
                    "target_component_sha256": component_projection[component],
                }
                for component in static_components
            ],
            "quotient_groups": quotient_groups,
            "requires_exact_static_v3_anchor": True,
            "uses_process_local_projection_id_values": False,
            "uses_projection_id_equality_within_frame": True,
            "uses_screen_position": False,
            "uses_colour": False,
            "uses_observed_depth": False,
            "uses_numeric_transform_values": False,
            "uses_family_count_prior": False,
            "uses_integer_or_tenth_prior": False,
            "persistent_object_identity_certified": False,
            "quotient_component_is_family": False,
            "base_component_hashes_recomputed": False,
            "ready_for_frame_local_runtime_lookup": True,
        },
        "proof_scope": {
            "exact_context_requires_discovery_and_holdout": True,
            "only_static_v3_anchors_may_define_context": True,
            "multi_anchor_context_requires_discovery_and_holdout": True,
            "multi_anchor_context_defines_topology_quotient_only": True,
            "base_component_hashes_are_immutable_anchor_labels": True,
            "compact_fingerprint_full_dag_cross_validated": True,
            "context_is_frame_local_occurrence_evidence": True,
            "context_promoted_to_persistent_runtime_key": False,
            "process_local_projection_id_value_persisted": False,
            "projection_id_equality_used_as_exact_incidence": True,
            "screen_position_used": False,
            "numeric_depth_used": False,
            "numeric_transform_used": False,
            "family_count_inferred": False,
            "integer_or_tenth_prior_used": False,
            "topology_components_promoted_to_persistent_objects": False,
            "quotient_components_promoted_to_families": False,
            "multiplier_fitted_or_recommended": False,
            "presentation_changed_by_this_report": False,
            "independent_post_dispatch_raster_validation_required": True,
        },
        "errors": [],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base_lineage", type=Path)
    parser.add_argument("static_dispatch_refinement", type=Path)
    parser.add_argument("discovery_stack", type=Path)
    parser.add_argument("holdout_stack", type=Path)
    parser.add_argument(
        "--fingerprint-evidence", type=Path, action="append", required=True
    )
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        base = json.loads(args.base_lineage.read_text(encoding="utf-8"))
        static_refinement = json.loads(
            args.static_dispatch_refinement.read_text(encoding="utf-8")
        )
        discovery, discovery_source = _load_jsonl(args.discovery_stack)
        holdout, holdout_source = _load_jsonl(args.holdout_stack)
        report = refine_visibility_stack_contexts(
            base,
            static_refinement,
            discovery,
            holdout,
            args.fingerprint_evidence,
        )
        report["source_files"] = {
            "base_lineage": str(args.base_lineage),
            "static_dispatch_refinement": str(args.static_dispatch_refinement),
            "discovery_stack": discovery_source,
            "holdout_stack": holdout_source,
        }
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"visibility_stack_context_refine: {exc}", file=sys.stderr)
        return 2
    payload = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(payload, encoding="utf-8")
    else:
        print(payload, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
