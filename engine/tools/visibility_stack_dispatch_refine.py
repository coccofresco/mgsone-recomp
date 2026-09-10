#!/usr/bin/env python3
"""Build an intrinsic runtime-dispatch attachment set from fragment stacks.

The base topology component hashes remain immutable anchor labels. New runtime
keys are attached only when discovery and holdout observed the same exact
shared-projection relation and at least two already-equivalent heavy captures
map every compact 64-bit model fingerprint to the same complete SHA-256 DAG
payload. The tool neither guesses a family count nor fits a depth multiplier.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any

from depth_family_trace import (
    model_value_flow_lineage,
    runtime_model_fingerprints,
    unsigned64,
)
from visibility_candidate_trace import _parse_topology_dispatch
from visibility_lineage_trace import _signature_id
from visibility_stack_cross_validate import _parse_runtime_dispatch_entry


TRACE_SCHEMA = "ps1trace/v1"
CROSS_SCHEMA = "ps1-visibility-fragment-stack-cross-validation/v1"
REPORT_SCHEMA = "ps1-visibility-runtime-dispatch-refinement/v1"
DISPATCH_SCHEMA = "ps1-visibility-runtime-dispatch-map/v3"
FINGERPRINT_SCHEMA = 1


def _canonical_sha256(value: Any) -> str:
    payload = json.dumps(value, separators=(",", ":"), sort_keys=True)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _plain_int(value: Any, minimum: int, maximum: int, context: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{context}: expected an integer")
    if value < minimum or value > maximum:
        raise ValueError(f"{context}: outside {minimum}..{maximum}")
    return value


def _extract_fingerprint_evidence(
    path: Path, targets: set[tuple[int, int]]
) -> tuple[dict[tuple[int, int], tuple[str, str]], dict[str, Any]]:
    expressions: dict[int, dict[str, Any]] = {}
    projections: list[dict[str, Any]] = []
    relevant_records = 0
    with path.open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{line_number}: invalid JSON: {exc}") from exc
            event = record.get("event")
            if event not in {"mips.value_expr", "gte.projection"}:
                continue
            relevant_records += 1
            if record.get("schema") != TRACE_SCHEMA:
                raise ValueError(f"{path}:{line_number}: invalid trace schema")
            if event == "mips.value_expr":
                expression_id = _plain_int(
                    record.get("expression_id"),
                    1,
                    2**63 - 1,
                    f"{path}:{line_number} expression id",
                )
                if expression_id in expressions:
                    raise ValueError(
                        f"{path}:{line_number}: duplicate expression {expression_id}"
                    )
                expressions[expression_id] = record
            else:
                projections.append(record)
    if relevant_records == 0 or not expressions or not projections:
        raise ValueError(f"{path}: no complete value-flow projection stream")

    observed: dict[tuple[int, int], set[tuple[str, str]]] = {}
    matched_projections = 0
    for projection_index, projection in enumerate(projections):
        source = unsigned64(int(projection.get("model_source_fingerprint", 0)))
        program = unsigned64(int(projection.get("model_program_fingerprint", 0)))
        compact = (source, program)
        if compact not in targets:
            continue
        context = f"{path} projection {projection_index}"
        for field in ("model_fingerprint_complete", "value_flow_complete"):
            if projection.get(field) != 1:
                raise ValueError(f"{context}: {field} is not complete")
        # New traces publish the compact/full agreement directly. Older heavy
        # traces predate those two fields; recomputing the compact pair from
        # the complete DAG below is the stronger equivalent check.
        for field in ("model_compact_complete", "model_compact_matches_full"):
            if field in projection and projection.get(field) != 1:
                raise ValueError(f"{context}: {field} is not complete")
        if projection.get("model_fingerprint_schema") != FINGERPRINT_SCHEMA:
            raise ValueError(f"{context}: unsupported fingerprint schema")
        recomputed = runtime_model_fingerprints(projection, expressions)
        lineage = model_value_flow_lineage(projection, expressions)
        if recomputed != compact or lineage is None:
            raise ValueError(f"{context}: compact fingerprint/full DAG mismatch")
        observed.setdefault(compact, set()).add(
            (lineage["source_digest"], lineage["program_digest"])
        )
        matched_projections += 1

    missing = sorted(targets - set(observed))
    if missing:
        raise ValueError(
            f"{path}: heavy trace misses {len(missing)} required compact fingerprints"
        )
    ambiguous = sorted(key for key, values in observed.items() if len(values) != 1)
    if ambiguous:
        raise ValueError(
            f"{path}: {len(ambiguous)} compact fingerprints map to multiple DAGs"
        )
    return (
        {key: next(iter(values)) for key, values in observed.items()},
        {
            "path": str(path),
            "sha256": _file_sha256(path),
            "expressions": len(expressions),
            "projections": len(projections),
            "matched_projections": matched_projections,
            "matched_compact_fingerprints": len(observed),
            "all_targets_present": True,
            "compact_fingerprints_recomputed_from_full_dag": True,
        },
    )


def _complete_triangle_signature(
    runtime_key: tuple[Any, ...],
    fingerprints: dict[tuple[int, int], tuple[str, str]],
) -> tuple[Any, ...]:
    _, transform, triangle, vertices = runtime_key
    geometry = []
    packet = []
    for (
        index,
        source_present,
        source,
        flow_source,
        flow_program,
        offset,
    ) in vertices:
        digests = fingerprints[(flow_source, flow_program)]
        geometry.append(
            (
                index,
                source if source_present else None,
                ("sha256-payload/v1", digests[0], digests[1]),
            )
        )
        packet.append((index, offset))
    return (
        "complete-triangle-lineage/v2",
        ("transform-digest/v1", transform),
        ("ordered-model-lineage/v1", tuple(geometry)),
        ("node-relative-packet-lineage/v1", triangle, tuple(packet)),
    )


def refine_visibility_stack_dispatch(
    base_lineage: Any,
    cross_validation: Any,
    fingerprint_evidence: list[Path],
) -> dict[str, Any]:
    base_dispatch, base_summary = _parse_topology_dispatch(base_lineage)
    if not isinstance(cross_validation, dict) or cross_validation.get(
        "schema"
    ) != CROSS_SCHEMA:
        raise ValueError("invalid fragment-stack cross-validation schema")
    if cross_validation.get("valid") is not True or cross_validation.get(
        "ready_to_extend_runtime_dispatch"
    ) is not True:
        raise ValueError("fragment-stack cross-validation is not ready")
    expected_source = _canonical_sha256(base_lineage)
    if cross_validation.get("topology_source_sha256") != expected_source:
        raise ValueError("cross-validation does not target the supplied topology")
    proof = cross_validation.get("proof_scope")
    required_false = (
        "screen_position_used",
        "numeric_depth_used_for_topology",
        "numeric_transform_used_for_topology",
        "family_count_inferred",
        "integer_or_tenth_prior_used",
        "topology_components_promoted_to_persistent_objects",
        "multiplier_fitted_or_recommended",
    )
    if (
        not isinstance(proof, dict)
        or proof.get("relation_requires_discovery_and_holdout") is not True
        or proof.get("exact_shared_projection_relation_required") is not True
        or proof.get("runtime_dispatch_entry_exactly_cross_validated") is not True
        or any(proof.get(field) is not False for field in required_false)
    ):
        raise ValueError("fragment-stack proof scope is incomplete or contaminated")

    raw_stable = cross_validation.get("stable_endpoint_relations")
    if not isinstance(raw_stable, list) or not raw_stable:
        raise ValueError("cross-validation contains no stable endpoints")
    stable = []
    target_pairs: set[tuple[int, int]] = set()
    for index, item in enumerate(raw_stable):
        context = f"stable endpoint {index}"
        if not isinstance(item, dict):
            raise ValueError(f"{context}: expected an object")
        runtime_key, runtime_entry = _parse_runtime_dispatch_entry(
            item.get("runtime_dispatch_entry"), context
        )
        endpoint = item.get("endpoint_dispatch_sha256")
        if endpoint != _signature_id(runtime_key):
            raise ValueError(f"{context}: endpoint/runtime-key mismatch")
        components = item.get("mapped_components")
        if not isinstance(components, list) or len(components) != 1:
            raise ValueError(
                f"{context}: attachment must target exactly one base component"
            )
        component = components[0]
        relations = item.get("relations")
        if not isinstance(relations, list) or not relations:
            raise ValueError(f"{context}: missing exact projection relations")
        stable.append((endpoint, runtime_key, runtime_entry, component, relations))
        for vertex in runtime_key[3]:
            target_pairs.add((vertex[3], vertex[4]))

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

    base_by_sha: dict[str, tuple[tuple[Any, ...], str]] = {}
    for key, component in base_dispatch.items():
        digest = _signature_id(key)
        if digest in base_by_sha and base_by_sha[digest][0] != key:
            raise ValueError("SHA-256 collision in base runtime dispatch")
        base_by_sha[digest] = (key, component)

    attachments = []
    attachment_entries = []
    seen_endpoints = set()
    for endpoint, runtime_key, runtime_entry, component, relations in stable:
        if endpoint in seen_endpoints:
            raise ValueError(f"duplicate stable endpoint {endpoint}")
        seen_endpoints.add(endpoint)
        if runtime_key in base_dispatch:
            raise ValueError(f"stable endpoint {endpoint} already exists in base dispatch")
        canonical_relations = []
        for relation_index, relation in enumerate(relations):
            context = f"endpoint {endpoint} relation {relation_index}"
            if not isinstance(relation, dict):
                raise ValueError(f"{context}: expected an object")
            mapped_sha = relation.get("mapped_dispatch_sha256")
            mapped = base_by_sha.get(mapped_sha)
            if mapped is None:
                raise ValueError(f"{context}: mapped dispatch is absent from base")
            if relation.get("mapped_component") != component or mapped[1] != component:
                raise ValueError(f"{context}: relation/component mismatch")
            canonical_relations.append(
                (
                    _plain_int(
                        relation.get("endpoint_vertex"), 0, 3, f"{context} endpoint"
                    ),
                    _plain_int(
                        relation.get("mapped_vertex"), 0, 3, f"{context} mapped"
                    ),
                    component,
                    mapped_sha,
                )
            )
        if len(set(canonical_relations)) != len(canonical_relations):
            raise ValueError(f"endpoint {endpoint}: duplicate exact relation")
        triangle_signature = _complete_triangle_signature(runtime_key, fingerprints)
        triangle_sha = _signature_id(triangle_signature)
        proof_payload = (
            "visibility-runtime-dispatch-attachment/v1",
            endpoint,
            triangle_sha,
            component,
            tuple(sorted(canonical_relations)),
            tuple(
                (pair, fingerprints[pair])
                for pair in sorted(
                    {(vertex[3], vertex[4]) for vertex in runtime_key[3]}
                )
            ),
        )
        attachment = {
            "endpoint_dispatch_sha256": endpoint,
            "complete_triangle_sha256": triangle_sha,
            "anchor_component_sha256": component,
            "attachment_proof_sha256": _signature_id(proof_payload),
            "exact_projection_relations": len(canonical_relations),
            "heavy_captures": len(capture_maps),
            "runtime_dispatch_entry": runtime_entry,
            "base_component_hash_includes_attachment": False,
        }
        attachments.append(attachment)
        attachment_entries.append(
            {"component_sha256": component, **runtime_entry}
        )

    topology = base_lineage["projection_incidence_topology"]
    base_runtime = topology["runtime_dispatch"]
    base_entries = base_runtime["entries"]
    combined_entries = [*base_entries, *attachment_entries]
    combined_entries.sort(
        key=lambda item: (
            item["component_sha256"],
            item["transform_lineage_hex"],
            item["triangle_index"],
            json.dumps(item["vertices"], separators=(",", ":"), sort_keys=True),
        )
    )
    attachments.sort(key=lambda item: item["endpoint_dispatch_sha256"])
    return {
        "schema": REPORT_SCHEMA,
        "valid": True,
        "base_topology_source_sha256": expected_source,
        "base_runtime_dispatch": base_summary,
        "fingerprint_evidence": {
            "captures": capture_reports,
            "capture_count": len(capture_reports),
            "required_compact_fingerprints": len(target_pairs),
            "cross_capture_dag_mismatches": 0,
            "all_required_fingerprints_cross_validated": True,
        },
        "attachments": attachments,
        "attachment_count": len(attachments),
        "runtime_dispatch": {
            "schema": DISPATCH_SCHEMA,
            "target": base_runtime["target"],
            "basis": (
                "base v2 partial topology plus discovery/holdout exact "
                "projection-incidence attachments with two-capture full-DAG audit"
            ),
            "entries": combined_entries,
            "entry_count": len(combined_entries),
            "components": base_summary["components"],
            "base_entries": len(base_entries),
            "cross_validated_attachment_entries": len(attachment_entries),
            "compact_model_fingerprint_schema": FINGERPRINT_SCHEMA,
            "compact_model_fingerprints_cross_audited_against_full_dag": True,
            "ready_for_runtime_lookup": bool(attachment_entries),
            "anchor_component_hashes_recomputed": False,
            "attachment_membership_is_part_of_anchor_component_hash": False,
            "uses_screen_position": False,
            "uses_colour": False,
            "uses_observed_depth": False,
            "uses_numeric_transform_values": False,
            "uses_family_count_prior": False,
            "uses_integer_or_tenth_prior": False,
            "persistent_object_identity_certified": False,
        },
        "proof_scope": {
            "base_component_hashes_are_immutable_anchor_labels": True,
            "attachments_recompute_base_component_hashes": False,
            "exact_projection_relation_cross_validated": True,
            "compact_fingerprint_full_dag_cross_validated": True,
            "family_count_inferred": False,
            "integer_or_tenth_prior_used": False,
            "multiplier_fitted_or_recommended": False,
            "attachments_are_persistent_object_identity": False,
            "independent_post_dispatch_raster_validation_required": True,
        },
        "errors": [],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base_lineage", type=Path)
    parser.add_argument("stack_cross_validation", type=Path)
    parser.add_argument(
        "--fingerprint-evidence", type=Path, action="append", required=True
    )
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        base = json.loads(args.base_lineage.read_text(encoding="utf-8"))
        cross = json.loads(args.stack_cross_validation.read_text(encoding="utf-8"))
        report = refine_visibility_stack_dispatch(
            base, cross, args.fingerprint_evidence
        )
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"visibility_stack_dispatch_refine: {exc}", file=sys.stderr)
        return 2
    payload = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(payload, encoding="utf-8")
    else:
        print(payload, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
