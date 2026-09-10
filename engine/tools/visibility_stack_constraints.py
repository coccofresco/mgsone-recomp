#!/usr/bin/env python3
"""Derive exhaustive presentation-depth constraints from Vulkan stacks.

The input dispatch is the independently cross-validated v3 refinement: base
projection-incidence component hashes remain immutable anchor labels and only
the separately proved attachments extend runtime lookup.  Every accepted
physical occluder is then mapped without a layer/family cap.  Discovery and
holdout remain separate, unresolved endpoints and one-set edges remain
explicit, and cycles fail the candidate-testing gate.

Depth is used only after intrinsic dispatch.  The current stack schema stores
independent extrema per directed owner pair, so its exact IEEE-754 rational is
a conservative sufficient bound for every represented sample, not a fitted
factor or a claim that the extrema came from the same sample.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import math
import re
import struct
import sys
from fractions import Fraction
from pathlib import Path
from typing import Any

from visibility_candidate_trace import _parse_topology_dispatch
from visibility_lineage_trace import (
    _owner_fields,
    _runtime_dispatch_key,
    _runtime_dispatch_key_report_entry,
    _signature_id,
)
from visibility_stack_cross_validate import _parse_runtime_dispatch_entry
from visibility_stack_trace import (
    AUDIT_EVENT,
    OCCLUDER_EVENT,
    OWNER_EVENT,
    analyze_visibility_stack_records,
)


REPORT_SCHEMA = "ps1-visibility-fragment-stack-constraints/v1"
REFINEMENT_SCHEMA = "ps1-visibility-runtime-dispatch-refinement/v1"
DISPATCH_SCHEMA = "ps1-visibility-runtime-dispatch-map/v3"
CONTEXT_REPORT_SCHEMA = "ps1-visibility-runtime-context-refinement/v1"
CONTEXT_DISPATCH_SCHEMA = "ps1-visibility-runtime-context-dispatch-map/v1"
CONTEXT_SIGNATURE_SCHEMA = "visibility-runtime-context-dispatch/v1"
CONTEXT_RELATION_SCHEMA = "exact-static-projection-incidence/v1"
CONTEXT_QUOTIENT_SCHEMA = "projection-incidence-context-quotient/v1"
TRACE_SCHEMA = "ps1trace/v1"
SHA256 = re.compile(r"[0-9a-f]{64}")


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


def _sha(value: Any, context: str) -> str:
    if not isinstance(value, str) or SHA256.fullmatch(value) is None:
        raise ValueError(f"{context}: invalid SHA-256")
    return value


def _fraction_report(value: Fraction) -> dict[str, Any]:
    return {
        "numerator": value.numerator,
        "denominator": value.denominator,
        "decimal_preview": format(float(value), ".12g"),
    }


def _depth_fraction(bits: Any, context: str) -> Fraction:
    raw = _plain_int(bits, 0, 0xFFFFFFFF, context)
    value = struct.unpack("<f", struct.pack("<I", raw))[0]
    if not math.isfinite(value) or not 0.0 < value < 1.0:
        raise ValueError(f"{context}: not a finite physical D32 value")
    return Fraction(value)


def _parse_refined_dispatch(
    base_lineage: Any, refinement: Any
) -> tuple[dict[tuple[Any, ...], str], dict[str, Any]]:
    base_dispatch, base_summary = _parse_topology_dispatch(base_lineage)
    if not isinstance(refinement, dict) or refinement.get(
        "schema"
    ) != REFINEMENT_SCHEMA:
        raise ValueError("invalid runtime-dispatch refinement schema")
    if refinement.get("valid") is not True or refinement.get("errors") != []:
        raise ValueError("runtime-dispatch refinement is not valid")

    base_sha = _canonical_sha256(base_lineage)
    if refinement.get("base_topology_source_sha256") != base_sha:
        raise ValueError("runtime-dispatch refinement targets a different topology")
    if refinement.get("base_runtime_dispatch") != base_summary:
        raise ValueError("runtime-dispatch refinement base summary is inconsistent")

    proof = refinement.get("proof_scope")
    required_true = (
        "base_component_hashes_are_immutable_anchor_labels",
        "exact_projection_relation_cross_validated",
        "compact_fingerprint_full_dag_cross_validated",
        "independent_post_dispatch_raster_validation_required",
    )
    required_false = (
        "attachments_recompute_base_component_hashes",
        "family_count_inferred",
        "integer_or_tenth_prior_used",
        "multiplier_fitted_or_recommended",
        "attachments_are_persistent_object_identity",
    )
    if (
        not isinstance(proof, dict)
        or any(proof.get(field) is not True for field in required_true)
        or any(proof.get(field) is not False for field in required_false)
    ):
        raise ValueError("runtime-dispatch refinement proof scope is contaminated")

    fingerprint = refinement.get("fingerprint_evidence")
    if (
        not isinstance(fingerprint, dict)
        or fingerprint.get("all_required_fingerprints_cross_validated") is not True
        or fingerprint.get("cross_capture_dag_mismatches") != 0
        or _plain_int(
            fingerprint.get("capture_count"),
            2,
            2**31 - 1,
            "fingerprint capture count",
        )
        < 2
    ):
        raise ValueError("runtime-dispatch fingerprint evidence is incomplete")

    runtime = refinement.get("runtime_dispatch")
    if not isinstance(runtime, dict) or runtime.get("schema") != DISPATCH_SCHEMA:
        raise ValueError("refined runtime dispatch is not v3")
    if runtime.get("target") != base_summary["target"]:
        raise ValueError("refined runtime dispatch has a different target")
    runtime_true = (
        "compact_model_fingerprints_cross_audited_against_full_dag",
        "ready_for_runtime_lookup",
    )
    runtime_false = (
        "anchor_component_hashes_recomputed",
        "attachment_membership_is_part_of_anchor_component_hash",
        "uses_screen_position",
        "uses_colour",
        "uses_observed_depth",
        "uses_numeric_transform_values",
        "uses_family_count_prior",
        "uses_integer_or_tenth_prior",
        "persistent_object_identity_certified",
    )
    if (
        any(runtime.get(field) is not True for field in runtime_true)
        or any(runtime.get(field) is not False for field in runtime_false)
        or runtime.get("compact_model_fingerprint_schema") != 1
    ):
        raise ValueError("refined runtime dispatch is incomplete or contaminated")

    entries = runtime.get("entries")
    if not isinstance(entries, list) or not entries:
        raise ValueError("refined runtime dispatch has no entries")
    expected_entries = _plain_int(
        runtime.get("entry_count"), 1, 2**31 - 1, "refined entry count"
    )
    base_entries = _plain_int(
        runtime.get("base_entries"), 1, 2**31 - 1, "refined base entry count"
    )
    attachment_entries = _plain_int(
        runtime.get("cross_validated_attachment_entries"),
        1,
        2**31 - 1,
        "refined attachment entry count",
    )
    expected_components = _plain_int(
        runtime.get("components"), 1, 2**31 - 1, "refined component count"
    )
    if (
        expected_entries != len(entries)
        or base_entries != len(base_dispatch)
        or expected_entries != base_entries + attachment_entries
        or expected_components != base_summary["components"]
    ):
        raise ValueError("refined runtime dispatch counts are inconsistent")

    refined: dict[tuple[Any, ...], str] = {}
    for entry_index, entry in enumerate(entries):
        context = f"refined runtime entry {entry_index}"
        if not isinstance(entry, dict):
            raise ValueError(f"{context}: expected an object")
        component = _sha(entry.get("component_sha256"), f"{context} component")
        key, _ = _parse_runtime_dispatch_entry(entry, context)
        if key in refined:
            raise ValueError(f"{context}: duplicate runtime key")
        refined[key] = component

    for key, component in base_dispatch.items():
        if refined.get(key) != component:
            raise ValueError("refined runtime dispatch changes or omits a base entry")
    if len(set(refined.values())) != expected_components:
        raise ValueError("refined runtime dispatch component count is inconsistent")

    raw_attachments = refinement.get("attachments")
    attachment_count = _plain_int(
        refinement.get("attachment_count"),
        1,
        2**31 - 1,
        "refinement attachment count",
    )
    if (
        not isinstance(raw_attachments, list)
        or attachment_count != len(raw_attachments)
        or attachment_count != attachment_entries
    ):
        raise ValueError("refinement attachment counts are inconsistent")

    attachments: dict[tuple[Any, ...], str] = {}
    attachment_digests: set[str] = set()
    for index, attachment in enumerate(raw_attachments):
        context = f"attachment {index}"
        if not isinstance(attachment, dict):
            raise ValueError(f"{context}: expected an object")
        key, _ = _parse_runtime_dispatch_entry(
            attachment.get("runtime_dispatch_entry"), context
        )
        endpoint = _sha(
            attachment.get("endpoint_dispatch_sha256"), f"{context} endpoint"
        )
        component = _sha(
            attachment.get("anchor_component_sha256"), f"{context} component"
        )
        _sha(
            attachment.get("complete_triangle_sha256"),
            f"{context} complete triangle",
        )
        _sha(
            attachment.get("attachment_proof_sha256"), f"{context} proof"
        )
        if endpoint != _signature_id(key):
            raise ValueError(f"{context}: endpoint/runtime-key mismatch")
        if endpoint in attachment_digests or key in attachments:
            raise ValueError(f"{context}: duplicate attachment")
        if key in base_dispatch:
            raise ValueError(f"{context}: attachment duplicates a base entry")
        if refined.get(key) != component:
            raise ValueError(f"{context}: attachment/refined component mismatch")
        if attachment.get("base_component_hash_includes_attachment") is not False:
            raise ValueError(f"{context}: attachment contaminates its base hash")
        _plain_int(
            attachment.get("exact_projection_relations"),
            1,
            2**31 - 1,
            f"{context} projection relations",
        )
        _plain_int(
            attachment.get("heavy_captures"),
            2,
            2**31 - 1,
            f"{context} heavy captures",
        )
        attachment_digests.add(endpoint)
        attachments[key] = component

    additions = set(refined) - set(base_dispatch)
    if additions != set(attachments):
        raise ValueError("refined runtime additions are not exactly the attachments")
    return refined, {
        "base_topology_source_sha256": base_sha,
        "refinement_source_sha256": _canonical_sha256(refinement),
        "base_entries": len(base_dispatch),
        "refined_entries": len(refined),
        "attachment_entries": len(attachments),
        "components": expected_components,
        "base_dispatch": base_dispatch,
        "attachment_keys": set(attachments),
    }


def _parse_context_dispatch(
    base_lineage: Any,
    static_refinement: Any,
    context_refinement: Any,
    static_dispatch: dict[tuple[Any, ...], str],
    static_summary: dict[str, Any],
) -> tuple[dict[tuple[Any, ...], str], dict[str, Any]]:
    if not isinstance(context_refinement, dict) or context_refinement.get(
        "schema"
    ) != CONTEXT_REPORT_SCHEMA:
        raise ValueError("invalid runtime-context refinement schema")
    if (
        context_refinement.get("valid") is not True
        or context_refinement.get("errors") != []
    ):
        raise ValueError("runtime-context refinement is not valid")
    if context_refinement.get(
        "base_static_refinement_source_sha256"
    ) != _canonical_sha256(static_refinement):
        raise ValueError("runtime-context refinement targets a different v3 map")
    if context_refinement.get("base_static_dispatch") != static_summary:
        raise ValueError("runtime-context base dispatch summary is inconsistent")

    proof = context_refinement.get("proof_scope")
    required_true = (
        "exact_context_requires_discovery_and_holdout",
        "only_static_v3_anchors_may_define_context",
        "multi_anchor_context_requires_discovery_and_holdout",
        "multi_anchor_context_defines_topology_quotient_only",
        "base_component_hashes_are_immutable_anchor_labels",
        "compact_fingerprint_full_dag_cross_validated",
        "context_is_frame_local_occurrence_evidence",
        "projection_id_equality_used_as_exact_incidence",
        "independent_post_dispatch_raster_validation_required",
    )
    required_false = (
        "context_promoted_to_persistent_runtime_key",
        "process_local_projection_id_value_persisted",
        "screen_position_used",
        "numeric_depth_used",
        "numeric_transform_used",
        "family_count_inferred",
        "integer_or_tenth_prior_used",
        "topology_components_promoted_to_persistent_objects",
        "quotient_components_promoted_to_families",
        "multiplier_fitted_or_recommended",
        "presentation_changed_by_this_report",
    )
    if (
        not isinstance(proof, dict)
        or any(proof.get(field) is not True for field in required_true)
        or any(proof.get(field) is not False for field in required_false)
    ):
        raise ValueError("runtime-context refinement proof scope is contaminated")

    fingerprint = context_refinement.get("fingerprint_evidence")
    if (
        not isinstance(fingerprint, dict)
        or fingerprint.get("all_required_fingerprints_cross_validated") is not True
        or fingerprint.get("cross_capture_dag_mismatches") != 0
        or _plain_int(
            fingerprint.get("capture_count"),
            2,
            2**31 - 1,
            "context fingerprint capture count",
        )
        < 2
    ):
        raise ValueError("runtime-context fingerprint evidence is incomplete")

    runtime = context_refinement.get("runtime_context_dispatch")
    if (
        not isinstance(runtime, dict)
        or runtime.get("schema") != CONTEXT_DISPATCH_SCHEMA
        or runtime.get("context_signature_schema") != CONTEXT_SIGNATURE_SCHEMA
        or runtime.get("relation_schema") != CONTEXT_RELATION_SCHEMA
        or runtime.get("quotient_schema") != CONTEXT_QUOTIENT_SCHEMA
    ):
        raise ValueError("runtime-context dispatch schema is unsupported")
    runtime_true = (
        "requires_exact_static_v3_anchor",
        "uses_projection_id_equality_within_frame",
        "ready_for_frame_local_runtime_lookup",
    )
    runtime_false = (
        "uses_process_local_projection_id_values",
        "uses_screen_position",
        "uses_colour",
        "uses_observed_depth",
        "uses_numeric_transform_values",
        "uses_family_count_prior",
        "uses_integer_or_tenth_prior",
        "persistent_object_identity_certified",
        "quotient_component_is_family",
        "base_component_hashes_recomputed",
    )
    if (
        any(runtime.get(field) is not True for field in runtime_true)
        or any(runtime.get(field) is not False for field in runtime_false)
        or runtime.get("base_static_dispatch_entries") != len(static_dispatch)
    ):
        raise ValueError("runtime-context dispatch is incomplete or contaminated")

    static_components = set(static_dispatch.values())
    if runtime.get("base_static_components") != len(static_components):
        raise ValueError("runtime-context base component count is inconsistent")
    raw_projection = runtime.get("component_projection")
    if not isinstance(raw_projection, list) or len(raw_projection) != len(
        static_components
    ):
        raise ValueError("runtime-context component projection is incomplete")
    component_projection: dict[str, str] = {}
    for index, projection in enumerate(raw_projection):
        context = f"component projection {index}"
        if not isinstance(projection, dict):
            raise ValueError(f"{context}: expected an object")
        source = _sha(
            projection.get("static_component_sha256"), f"{context} source"
        )
        target = _sha(
            projection.get("target_component_sha256"), f"{context} target"
        )
        if source not in static_components or source in component_projection:
            raise ValueError(f"{context}: unknown/duplicate static component")
        component_projection[source] = target
    if set(component_projection) != static_components:
        raise ValueError("runtime-context component projection misses static labels")

    raw_quotient_groups = runtime.get("quotient_groups")
    if not isinstance(raw_quotient_groups, list):
        raise ValueError("runtime-context quotient groups are missing")
    merged_members: set[str] = set()
    for index, group in enumerate(raw_quotient_groups):
        context = f"quotient group {index}"
        if not isinstance(group, dict):
            raise ValueError(f"{context}: expected an object")
        raw_members = group.get("static_anchor_components")
        if not isinstance(raw_members, list) or len(raw_members) < 2:
            raise ValueError(f"{context}: expected at least two anchors")
        members = tuple(_sha(value, f"{context} member") for value in raw_members)
        if tuple(sorted(set(members))) != members:
            raise ValueError(f"{context}: members are not unique/sorted")
        if any(member not in static_components for member in members):
            raise ValueError(f"{context}: unknown static component")
        if merged_members & set(members):
            raise ValueError(f"{context}: overlapping quotient groups")
        merged_members.update(members)
        expected_target = _signature_id((CONTEXT_QUOTIENT_SCHEMA, members))
        target = _sha(
            group.get("quotient_component_sha256"), f"{context} target"
        )
        if target != expected_target or any(
            component_projection[member] != target for member in members
        ):
            raise ValueError(f"{context}: quotient hash/projection mismatch")
    for component in static_components - merged_members:
        if component_projection[component] != component:
            raise ValueError("singleton static component changed in quotient")
    if (
        runtime.get("cross_validated_quotient_groups")
        != len(raw_quotient_groups)
        or runtime.get("quotient_components")
        != len(set(component_projection.values()))
    ):
        raise ValueError("runtime-context quotient counts are inconsistent")

    entries = runtime.get("entries")
    expected_entries = _plain_int(
        runtime.get("entry_count"), 1, 2**31 - 1, "runtime-context entry count"
    )
    if not isinstance(entries, list) or len(entries) != expected_entries:
        raise ValueError("runtime-context entry count is inconsistent")

    static_by_sha: dict[str, tuple[tuple[Any, ...], str]] = {}
    for key, component in static_dispatch.items():
        digest = _signature_id(key)
        previous = static_by_sha.setdefault(digest, (key, component))
        if previous != (key, component):
            raise ValueError("SHA-256 collision in static runtime dispatch")

    contexts: dict[tuple[Any, ...], str] = {}
    context_by_sha: dict[str, tuple[Any, ...]] = {}
    endpoint_keys: set[tuple[Any, ...]] = set()
    for entry_index, entry in enumerate(entries):
        context = f"runtime-context entry {entry_index}"
        if not isinstance(entry, dict):
            raise ValueError(f"{context}: expected an object")
        runtime_key, _ = _parse_runtime_dispatch_entry(
            entry.get("runtime_dispatch_entry"), context
        )
        endpoint = _sha(
            entry.get("endpoint_dispatch_sha256"), f"{context} endpoint"
        )
        component = _sha(
            entry.get("target_component_sha256"), f"{context} component"
        )
        raw_anchor_components = entry.get("static_anchor_components")
        if not isinstance(raw_anchor_components, list) or not raw_anchor_components:
            raise ValueError(f"{context}: static anchor components are missing")
        anchor_components = tuple(
            _sha(value, f"{context} static anchor")
            for value in raw_anchor_components
        )
        if tuple(sorted(set(anchor_components))) != anchor_components:
            raise ValueError(f"{context}: static anchors are not unique/sorted")
        if any(value not in static_components for value in anchor_components):
            raise ValueError(f"{context}: unknown static anchor component")
        if {component_projection[value] for value in anchor_components} != {
            component
        }:
            raise ValueError(f"{context}: anchors do not resolve to target quotient")
        _sha(
            entry.get("complete_triangle_sha256"), f"{context} complete triangle"
        )
        _sha(
            entry.get("context_attachment_proof_sha256"), f"{context} proof"
        )
        if endpoint != _signature_id(runtime_key):
            raise ValueError(f"{context}: endpoint/runtime-key mismatch")
        if runtime_key in static_dispatch:
            raise ValueError(f"{context}: contextual key is already static")
        raw_relations = entry.get("exact_static_projection_relations")
        expected_relations = _plain_int(
            entry.get("exact_projection_relations"),
            1,
            2**31 - 1,
            f"{context} relation count",
        )
        if not isinstance(raw_relations, list) or len(raw_relations) != expected_relations:
            raise ValueError(f"{context}: relation count is inconsistent")
        relations = []
        for relation_index, relation in enumerate(raw_relations):
            relation_context = f"{context} relation {relation_index}"
            if not isinstance(relation, dict):
                raise ValueError(f"{relation_context}: expected an object")
            mapped_sha = _sha(
                relation.get("mapped_dispatch_sha256"),
                f"{relation_context} mapped dispatch",
            )
            mapped = static_by_sha.get(mapped_sha)
            if mapped is None:
                raise ValueError(f"{relation_context}: anchor is not static")
            mapped_component = _sha(
                relation.get("mapped_component"),
                f"{relation_context} mapped component",
            )
            if mapped_component != mapped[1] or component_projection[
                mapped_component
            ] != component:
                raise ValueError(f"{relation_context}: anchor component mismatch")
            relations.append(
                (
                    _plain_int(
                        relation.get("endpoint_vertex"),
                        0,
                        3,
                        f"{relation_context} endpoint vertex",
                    ),
                    _plain_int(
                        relation.get("mapped_vertex"),
                        0,
                        3,
                        f"{relation_context} mapped vertex",
                    ),
                    mapped_component,
                    mapped_sha,
                )
            )
        canonical_relations = tuple(sorted(set(relations)))
        if len(canonical_relations) != len(relations):
            raise ValueError(f"{context}: duplicate exact relation")
        if tuple(sorted({relation[2] for relation in canonical_relations})) != (
            anchor_components
        ):
            raise ValueError(f"{context}: relation/static-anchor set mismatch")
        expected_kind = (
            "single-static-anchor"
            if len(anchor_components) == 1
            else "cross-validated-topology-merge"
        )
        if entry.get("context_kind") != expected_kind:
            raise ValueError(f"{context}: incorrect contextual kind")
        signature = (
            CONTEXT_SIGNATURE_SCHEMA,
            runtime_key,
            (CONTEXT_RELATION_SCHEMA, canonical_relations),
        )
        digest = _signature_id(signature)
        if entry.get("context_dispatch_sha256") != digest:
            raise ValueError(f"{context}: contextual signature mismatch")
        previous_signature = context_by_sha.setdefault(digest, signature)
        if previous_signature != signature:
            raise ValueError("runtime-context SHA-256 collision")
        if signature in contexts:
            raise ValueError(f"{context}: duplicate contextual key")
        if entry.get("frame_local_occurrence_attachment") is not True or entry.get(
            "persistent_runtime_key_promotion"
        ) is not False:
            raise ValueError(f"{context}: invalid occurrence/persistence scope")
        contexts[signature] = component
        endpoint_keys.add(runtime_key)

    expected_endpoint_keys = _plain_int(
        runtime.get("endpoint_runtime_keys"),
        1,
        2**31 - 1,
        "runtime-context endpoint keys",
    )
    expected_components = _plain_int(
        runtime.get("target_components"),
        1,
        2**31 - 1,
        "runtime-context target components",
    )
    if (
        len(endpoint_keys) != expected_endpoint_keys
        or len(set(contexts.values())) != expected_components
        or fingerprint.get("endpoint_runtime_keys") != expected_endpoint_keys
    ):
        raise ValueError("runtime-context key/component counts are inconsistent")

    cross = context_refinement.get("context_cross_validation")
    if not isinstance(cross, dict):
        raise ValueError("runtime-context cross-validation summary is missing")
    single_contexts = _plain_int(
        cross.get("stable_exact_single_component_contexts"),
        0,
        2**31 - 1,
        "single-component context count",
    )
    merge_context_count = _plain_int(
        cross.get("stable_exact_merge_contexts"),
        0,
        2**31 - 1,
        "merge-context count",
    )
    if (
        cross.get("stable_exact_nonempty_contexts") != len(contexts)
        or single_contexts + merge_context_count != len(contexts)
        or cross.get("stable_endpoint_runtime_keys") != len(endpoint_keys)
        or cross.get("within_frame_duplicate_contexts") != 0
        or cross.get("multi_component_contexts") != merge_context_count
    ):
        raise ValueError("runtime-context cross-validation summary is inconsistent")

    capture_expectations = {}
    for label in ("discovery", "holdout"):
        capture = context_refinement.get(label)
        if not isinstance(capture, dict):
            raise ValueError(f"runtime-context refinement lacks {label} coverage")
        capture_expectations[label] = {
            "stack_report_sha256": _sha(
                capture.get("stack_report_sha256"), f"{label} stack report"
            ),
            "admitted_context_sample_memberships": _plain_int(
                capture.get("admitted_context_sample_memberships"),
                1,
                2**63 - 1,
                f"{label} admitted context samples",
            ),
            "residual_unknown_endpoint_sample_memberships": _plain_int(
                capture.get("residual_unknown_endpoint_sample_memberships"),
                0,
                2**63 - 1,
                f"{label} residual context samples",
            ),
        }
    return contexts, {
        "schema": runtime["schema"],
        "source_sha256": _canonical_sha256(context_refinement),
        "entries": len(contexts),
        "endpoint_runtime_keys": len(endpoint_keys),
        "target_components": len(set(contexts.values())),
        "base_static_components": len(static_components),
        "quotient_components": len(set(component_projection.values())),
        "quotient_groups": len(raw_quotient_groups),
        "frame_local_occurrence_evidence": True,
        "persistent_runtime_key_promotion": False,
        "family_count_prior_used": False,
        "integer_or_tenth_prior_used": False,
        "component_projection": component_projection,
        "capture_expectations": capture_expectations,
    }


def _cyclic_strong_components(
    adjacency: dict[str, set[str]], self_conflicts: set[str]
) -> list[list[str]]:
    """Return deterministic cyclic SCCs; self conflicts are explicit cycles."""

    nodes = sorted(
        set(adjacency)
        | {target for targets in adjacency.values() for target in targets}
        | self_conflicts
    )
    index = 0
    indices: dict[str, int] = {}
    lowlinks: dict[str, int] = {}
    stack: list[str] = []
    on_stack: set[str] = set()
    components: list[list[str]] = []

    def visit(node: str) -> None:
        nonlocal index
        indices[node] = index
        lowlinks[node] = index
        index += 1
        stack.append(node)
        on_stack.add(node)
        for target in sorted(adjacency.get(node, ())):
            if target not in indices:
                visit(target)
                lowlinks[node] = min(lowlinks[node], lowlinks[target])
            elif target in on_stack:
                lowlinks[node] = min(lowlinks[node], indices[target])
        if lowlinks[node] != indices[node]:
            return
        component = []
        while True:
            value = stack.pop()
            on_stack.remove(value)
            component.append(value)
            if value == node:
                break
        component.sort()
        if len(component) > 1 or component[0] in self_conflicts:
            components.append(component)

    for node in nodes:
        if node not in indices:
            visit(node)
    return sorted(components)


def _capture_constraints(
    records: list[dict[str, Any]],
    label: str,
    base_lineage: Any,
    refined_dispatch: dict[tuple[Any, ...], str],
    base_dispatch: dict[tuple[Any, ...], str],
    attachment_keys: set[tuple[Any, ...]],
    context_dispatch: dict[tuple[Any, ...], str] | None = None,
    component_projection: dict[str, str] | None = None,
    context_expectation: dict[str, Any] | None = None,
) -> dict[str, Any]:
    stack_report = analyze_visibility_stack_records(records, base_lineage)
    if stack_report.get("valid") is not True:
        details = "; ".join(stack_report.get("errors", [])[:3])
        raise ValueError(f"{label}: invalid fragment stack: {details}")
    coverage = stack_report.get("constraint_coverage")
    if (
        not isinstance(coverage, dict)
        or coverage.get("all_captures_complete") is not True
        or coverage.get("all_pair_endpoints_have_complete_dispatch_keys") is not True
        or stack_report.get("overflow_frames") != 0
    ):
        raise ValueError(f"{label}: fragment stack is incomplete")

    stack_report_sha = _canonical_sha256(stack_report)
    if (
        context_expectation is not None
        and context_expectation["stack_report_sha256"] != stack_report_sha
    ):
        raise ValueError(f"{label}: runtime-context stack source mismatch")

    owners: dict[tuple[int, int], dict[str, Any]] = {}
    owner_entries: dict[tuple[int, int], dict[str, Any]] = {}
    pairs: list[tuple[int, int, dict[str, Any]]] = []
    audits: set[int] = set()
    for record_index, record in enumerate(records):
        event = record.get("event")
        if event not in {AUDIT_EVENT, OCCLUDER_EVENT, OWNER_EVENT}:
            continue
        context = f"{label} record {record_index}"
        if record.get("schema") != TRACE_SCHEMA:
            raise ValueError(f"{context}: invalid trace schema")
        frame = _plain_int(record.get("frame"), 0, 2**63 - 1, f"{context} frame")
        if event == AUDIT_EVENT:
            audits.add(frame)
        elif event == OWNER_EVENT:
            token = _plain_int(
                record.get("owner"), 1, 2**32 - 1, f"{context} owner"
            )
            owner = _owner_fields(record, context)
            key = _runtime_dispatch_key(owner)
            if key is None:
                # The base stack validator proves that no referenced endpoint
                # has this state; retain a clear error if its contract changes.
                continue
            owner_key = (frame, token)
            if owner_key in owners:
                raise ValueError(f"{context}: duplicate owner token")
            owners[owner_key] = {
                "runtime_key": key,
                "owner": owner,
                "dispatch_sha256": _signature_id(key),
                "static_component": refined_dispatch.get(key),
            }
            owner_entries[owner_key] = _runtime_dispatch_key_report_entry(key)
        else:
            pairs.append((frame, record_index, record))

    projection_users: dict[
        tuple[int, int], list[tuple[tuple[int, int], int]]
    ] = collections.defaultdict(list)
    for owner_key, owner_record in owners.items():
        for vertex in owner_record["owner"]["vertices"]:
            if vertex["evidence"] & 1:
                projection_users[(owner_key[0], vertex["projection_id"])].append(
                    (owner_key, vertex["index"])
                )

    owner_resolution: dict[tuple[int, int], dict[str, Any]] = {}
    for owner_key, owner_record in owners.items():
        static_component = owner_record["static_component"]
        if static_component is not None:
            effective_static_component = (
                component_projection.get(static_component, static_component)
                if component_projection is not None
                else static_component
            )
            owner_resolution[owner_key] = {
                "static_component": static_component,
                "effective_component": effective_static_component,
                "context_signature": None,
                "context_dispatch_sha256": None,
                "status": "static-v3",
            }
            continue
        relations: set[tuple[int, int, str, str]] = set()
        for vertex in owner_record["owner"]["vertices"]:
            if not vertex["evidence"] & 1:
                continue
            for mapped_owner_key, mapped_vertex in projection_users.get(
                (owner_key[0], vertex["projection_id"]), ()
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
        signature = (
            CONTEXT_SIGNATURE_SCHEMA,
            owner_record["runtime_key"],
            (CONTEXT_RELATION_SCHEMA, canonical_relations),
        )
        component = (
            context_dispatch.get(signature)
            if context_dispatch is not None
            else None
        )
        owner_resolution[owner_key] = {
            "static_component": None,
            "effective_component": component,
            "context_signature": signature,
            "context_dispatch_sha256": _signature_id(signature),
            "status": (
                "frame-local-context"
                if component is not None
                else "unmapped-context"
                if context_dispatch is not None
                else "context-not-requested"
            ),
        }

    constraints: dict[tuple[str, str], dict[str, Any]] = {}
    unresolved: dict[str, dict[str, Any]] = {}
    total_records = 0
    total_samples = 0
    base_fully_mapped_records = 0
    base_fully_mapped_samples = 0
    refined_fully_mapped_records = 0
    refined_fully_mapped_samples = 0
    base_unmapped_endpoint_records = 0
    base_unmapped_endpoint_samples = 0
    refined_unmapped_endpoint_records = 0
    refined_unmapped_endpoint_samples = 0
    context_fully_mapped_records = 0
    context_fully_mapped_samples = 0
    context_unmapped_endpoint_records = 0
    context_unmapped_endpoint_samples = 0
    unresolved_pair_records = 0
    unresolved_pair_samples = 0
    self_component_records = 0
    self_component_samples = 0
    non_strict_records = 0
    non_strict_samples = 0
    attachment_endpoint_records = 0
    attachment_endpoint_samples = 0
    attachment_keys_used: set[tuple[Any, ...]] = set()
    context_endpoint_records = 0
    context_endpoint_samples = 0
    context_keys_used: set[tuple[Any, ...]] = set()

    for frame, record_index, pair in pairs:
        context = f"{label} record {record_index}"
        ot_token = _plain_int(
            pair.get("ot_owner"), 1, 2**32 - 1, f"{context} OT owner"
        )
        physical_token = _plain_int(
            pair.get("physical_owner"),
            1,
            2**32 - 1,
            f"{context} physical owner",
        )
        samples = _plain_int(
            pair.get("samples"), 1, 2**63 - 1, f"{context} samples"
        )
        endpoints = (
            ("ot", ot_token, owners.get((frame, ot_token))),
            ("physical", physical_token, owners.get((frame, physical_token))),
        )
        if any(owner_record is None for _, _, owner_record in endpoints):
            raise ValueError(f"{context}: endpoint lacks a complete runtime key")

        total_records += 1
        total_samples += samples
        endpoint_keys = [
            owner_record["runtime_key"]
            for _, _, owner_record in endpoints
            if owner_record is not None
        ]
        base_components = [base_dispatch.get(key) for key in endpoint_keys]
        refined_components = [refined_dispatch.get(key) for key in endpoint_keys]
        effective_components = [
            owner_resolution[(frame, token)]["effective_component"]
            for _, token, _ in endpoints
        ]
        if all(component is not None for component in base_components):
            base_fully_mapped_records += 1
            base_fully_mapped_samples += samples
        if all(component is not None for component in refined_components):
            refined_fully_mapped_records += 1
            refined_fully_mapped_samples += samples
        if all(component is not None for component in effective_components):
            context_fully_mapped_records += 1
            context_fully_mapped_samples += samples
        else:
            unresolved_pair_records += 1
            unresolved_pair_samples += samples

        for endpoint_index, (role, token, owner_record) in enumerate(endpoints):
            assert owner_record is not None
            key = owner_record["runtime_key"]
            resolution = owner_resolution[(frame, token)]
            if base_components[endpoint_index] is None:
                base_unmapped_endpoint_records += 1
                base_unmapped_endpoint_samples += samples
            if key in attachment_keys:
                attachment_endpoint_records += 1
                attachment_endpoint_samples += samples
                attachment_keys_used.add(key)
            if refined_components[endpoint_index] is not None:
                continue
            refined_unmapped_endpoint_records += 1
            refined_unmapped_endpoint_samples += samples
            if resolution["status"] == "frame-local-context":
                context_endpoint_records += 1
                context_endpoint_samples += samples
                context_keys_used.add(resolution["context_signature"])
            if effective_components[endpoint_index] is not None:
                continue
            context_unmapped_endpoint_records += 1
            context_unmapped_endpoint_samples += samples
            digest = _signature_id(key)
            value = unresolved.setdefault(
                digest,
                {
                    "runtime_key": key,
                    "runtime_dispatch_entry": owner_entries[(frame, token)],
                    "roles": collections.Counter(),
                    "frames": set(),
                    "owners": set(),
                    "context_dispatch_sha256": set(),
                },
            )
            if value["runtime_key"] != key:
                raise ValueError(f"{context}: runtime-key SHA-256 collision")
            value["roles"][role] += samples
            value["frames"].add(frame)
            value["owners"].add((frame, token))
            value["context_dispatch_sha256"].add(
                resolution["context_dispatch_sha256"]
            )

        if not all(component is not None for component in effective_components):
            continue
        ot_component, physical_component = effective_components
        assert ot_component is not None and physical_component is not None
        maximum_ot = _depth_fraction(
            pair.get("maximum_ot_depth_bits"), f"{context} maximum OT depth"
        )
        minimum_physical = _depth_fraction(
            pair.get("minimum_physical_depth_bits"),
            f"{context} minimum physical depth",
        )
        ratio = maximum_ot / minimum_physical
        if ratio <= 1:
            non_strict_records += 1
            non_strict_samples += samples
            continue
        if ot_component == physical_component:
            self_component_records += 1
            self_component_samples += samples
            continue

        key = (ot_component, physical_component)
        constraint = constraints.setdefault(
            key,
            {
                "records": 0,
                "samples": 0,
                "frames": set(),
                "maximum_ratio": Fraction(0, 1),
                "maximum_ratio_witness": None,
            },
        )
        constraint["records"] += 1
        constraint["samples"] += samples
        constraint["frames"].add(frame)
        if ratio > constraint["maximum_ratio"]:
            constraint["maximum_ratio"] = ratio
            constraint["maximum_ratio_witness"] = {
                "validation_set": label,
                "frame": frame,
                "ot_owner": ot_token,
                "physical_owner": physical_token,
                "samples_in_owner_pair": samples,
                "maximum_ot_depth_bits": pair["maximum_ot_depth_bits"],
                "minimum_physical_depth_bits": pair[
                    "minimum_physical_depth_bits"
                ],
                "extrema_are_same_sample_witness": False,
            }

    expected_base_unmapped = coverage.get(
        "unmapped_topology_endpoint_sample_memberships"
    )
    if expected_base_unmapped != base_unmapped_endpoint_samples:
        raise ValueError(
            f"{label}: base topology coverage does not reconcile with raw pairs"
        )
    expected_pair_records = stack_report.get("totals", {}).get("occluder_pairs")
    if expected_pair_records != total_records:
        raise ValueError(f"{label}: pair record count does not reconcile")
    if context_expectation is not None:
        if (
            context_expectation["admitted_context_sample_memberships"]
            != context_endpoint_samples
            or context_expectation[
                "residual_unknown_endpoint_sample_memberships"
            ]
            != context_unmapped_endpoint_samples
        ):
            raise ValueError(
                f"{label}: runtime-context coverage does not reconcile"
            )

    unresolved_report = []
    for digest, value in sorted(
        unresolved.items(),
        key=lambda item: (-sum(item[1]["roles"].values()), item[0]),
    ):
        unresolved_report.append(
            {
                "endpoint_dispatch_sha256": digest,
                "runtime_dispatch_entry": value["runtime_dispatch_entry"],
                "owner_occurrences": len(value["owners"]),
                "sample_memberships": sum(value["roles"].values()),
                "roles": dict(sorted(value["roles"].items())),
                "frames": sorted(value["frames"]),
                "unresolved_context_dispatch_sha256": sorted(
                    value["context_dispatch_sha256"]
                ),
                "intrinsic_attachment_required": True,
            }
        )

    totals = stack_report.get("totals", {})
    occluded_samples = totals.get("occluded_ot_samples", 0)
    return {
        "label": label,
        "audited_frames": stack_report["audited_frames"],
        "frames": sorted(audits),
        "attempted_fragment_records": totals.get("attempted_records", 0),
        "complete_owned_samples": totals.get("complete_owned_samples", 0),
        "occluded_ot_samples": occluded_samples,
        "occluder_pair_records": total_records,
        "occluder_sample_memberships": total_samples,
        "latent_extra_occluder_memberships": total_samples - occluded_samples,
        "base_fully_mapped_pair_records": base_fully_mapped_records,
        "base_fully_mapped_pair_sample_memberships": base_fully_mapped_samples,
        "refined_fully_mapped_pair_records": refined_fully_mapped_records,
        "refined_fully_mapped_pair_sample_memberships": refined_fully_mapped_samples,
        "context_fully_mapped_pair_records": context_fully_mapped_records,
        "context_fully_mapped_pair_sample_memberships": context_fully_mapped_samples,
        "unresolved_pair_records": unresolved_pair_records,
        "unresolved_pair_sample_memberships": unresolved_pair_samples,
        "base_unmapped_endpoint_record_memberships": (
            base_unmapped_endpoint_records
        ),
        "base_unmapped_endpoint_sample_memberships": (
            base_unmapped_endpoint_samples
        ),
        "refined_unmapped_endpoint_record_memberships": (
            refined_unmapped_endpoint_records
        ),
        "refined_unmapped_endpoint_sample_memberships": (
            refined_unmapped_endpoint_samples
        ),
        "context_unmapped_endpoint_record_memberships": (
            context_unmapped_endpoint_records
        ),
        "context_unmapped_endpoint_sample_memberships": (
            context_unmapped_endpoint_samples
        ),
        "attachment_mapping_gain_endpoint_sample_memberships": (
            base_unmapped_endpoint_samples - refined_unmapped_endpoint_samples
        ),
        "attachment_endpoint_record_memberships": attachment_endpoint_records,
        "attachment_endpoint_sample_memberships": attachment_endpoint_samples,
        "attachment_runtime_keys_used": len(attachment_keys_used),
        "context_mapping_gain_endpoint_sample_memberships": (
            refined_unmapped_endpoint_samples - context_unmapped_endpoint_samples
        ),
        "context_endpoint_record_memberships": context_endpoint_records,
        "context_endpoint_sample_memberships": context_endpoint_samples,
        "context_dispatch_entries_used": len(context_keys_used),
        "self_component_conflict_records": self_component_records,
        "self_component_conflict_samples": self_component_samples,
        "non_strict_depth_bound_records": non_strict_records,
        "non_strict_depth_bound_samples": non_strict_samples,
        "unresolved_distinct_runtime_keys": len(unresolved_report),
        "unresolved_endpoints": unresolved_report,
        "constraints": constraints,
        "stack_report_sha256": stack_report_sha,
    }


def analyze_visibility_stack_constraints(
    base_lineage: Any,
    refinement: Any,
    discovery_records: list[dict[str, Any]],
    holdout_records: list[dict[str, Any]],
    context_refinement: Any | None = None,
) -> dict[str, Any]:
    refined_dispatch, dispatch_summary = _parse_refined_dispatch(
        base_lineage, refinement
    )
    base_dispatch = dispatch_summary.pop("base_dispatch")
    attachment_keys = dispatch_summary.pop("attachment_keys")
    if context_refinement is None:
        context_dispatch = None
        component_projection = None
        context_summary = None
        context_expectations = {"discovery": None, "holdout": None}
    else:
        context_dispatch, context_summary = _parse_context_dispatch(
            base_lineage,
            refinement,
            context_refinement,
            refined_dispatch,
            dispatch_summary,
        )
        context_expectations = context_summary.pop("capture_expectations")
        component_projection = context_summary.pop("component_projection")
    discovery = _capture_constraints(
        discovery_records,
        "discovery",
        base_lineage,
        refined_dispatch,
        base_dispatch,
        attachment_keys,
        context_dispatch,
        component_projection,
        context_expectations["discovery"],
    )
    holdout = _capture_constraints(
        holdout_records,
        "holdout",
        base_lineage,
        refined_dispatch,
        base_dispatch,
        attachment_keys,
        context_dispatch,
        component_projection,
        context_expectations["holdout"],
    )

    constraint_reports = []
    adjacency: dict[str, set[str]] = collections.defaultdict(set)
    all_keys = sorted(set(discovery["constraints"]) | set(holdout["constraints"]))
    single_set_constraints = []
    for nearer, farther in all_keys:
        left = discovery["constraints"].get((nearer, farther))
        right = holdout["constraints"].get((nearer, farther))
        values = [value for value in (left, right) if value is not None]
        validation_sets = [
            label
            for label, value in (("discovery", left), ("holdout", right))
            if value is not None
        ]
        cross_validated = len(values) == 2
        if not cross_validated:
            single_set_constraints.append(
                {
                    "nearer_ot_component_sha256": nearer,
                    "farther_depth_component_sha256": farther,
                    "validation_set": validation_sets[0],
                }
            )
        maximum = max(values, key=lambda value: value["maximum_ratio"])
        maximum_ratio = maximum["maximum_ratio"]
        adjacency[nearer].add(farther)
        constraint_reports.append(
            {
                "nearer_ot_component_sha256": nearer,
                "farther_depth_component_sha256": farther,
                "records": sum(value["records"] for value in values),
                "samples": sum(value["samples"] for value in values),
                "validation_sets": validation_sets,
                "cross_validated": cross_validated,
                "required_relation": (
                    "farther_multiplier / nearer_multiplier > "
                    "conservative_lower_bound"
                ),
                "conservative_lower_bound": _fraction_report(maximum_ratio),
                "discovery_conservative_lower_bound": (
                    _fraction_report(left["maximum_ratio"])
                    if left is not None
                    else None
                ),
                "holdout_conservative_lower_bound": (
                    _fraction_report(right["maximum_ratio"])
                    if right is not None
                    else None
                ),
                "maximum_bound_witness": maximum["maximum_ratio_witness"],
                "frames": {
                    "discovery": sorted(left["frames"]) if left else [],
                    "holdout": sorted(right["frames"]) if right else [],
                },
                "gauge_certified": False,
            }
        )

    # The exact component labels of self-conflicts are intentionally not
    # collapsed into a synthetic node. Any such record independently blocks
    # readiness; SCCs below describe cycles among distinct mapped components.
    cyclic_components = _cyclic_strong_components(adjacency, set())
    self_records = (
        discovery["self_component_conflict_records"]
        + holdout["self_component_conflict_records"]
    )
    graph_acyclic = not cyclic_components and self_records == 0
    all_cross_validated = bool(constraint_reports) and not single_set_constraints
    all_endpoints_mapped = (
        discovery["context_unmapped_endpoint_sample_memberships"] == 0
        and holdout["context_unmapped_endpoint_sample_memberships"] == 0
    )
    all_pairs_mapped = (
        discovery["unresolved_pair_sample_memberships"] == 0
        and holdout["unresolved_pair_sample_memberships"] == 0
    )
    all_bounds_strict = (
        discovery["non_strict_depth_bound_records"] == 0
        and holdout["non_strict_depth_bound_records"] == 0
    )
    ready = (
        all_endpoints_mapped
        and all_pairs_mapped
        and self_records == 0
        and all_bounds_strict
        and all_cross_validated
        and graph_acyclic
    )

    def public_capture(value: dict[str, Any]) -> dict[str, Any]:
        return {key: item for key, item in value.items() if key != "constraints"}

    graph_nodes = sorted(
        set(adjacency)
        | {target for targets in adjacency.values() for target in targets}
    )
    return {
        "schema": REPORT_SCHEMA,
        "valid": True,
        "dispatch": dispatch_summary,
        "context_dispatch": context_summary,
        "discovery": public_capture(discovery),
        "holdout": public_capture(holdout),
        "coverage": {
            "all_pair_endpoints_intrinsically_mapped": all_endpoints_mapped,
            "all_occluder_pair_memberships_mapped": all_pairs_mapped,
            "base_unmapped_endpoint_sample_memberships": (
                discovery["base_unmapped_endpoint_sample_memberships"]
                + holdout["base_unmapped_endpoint_sample_memberships"]
            ),
            "refined_unmapped_endpoint_sample_memberships": (
                discovery["refined_unmapped_endpoint_sample_memberships"]
                + holdout["refined_unmapped_endpoint_sample_memberships"]
            ),
            "context_unmapped_endpoint_sample_memberships": (
                discovery["context_unmapped_endpoint_sample_memberships"]
                + holdout["context_unmapped_endpoint_sample_memberships"]
            ),
            "attachment_mapping_gain_endpoint_sample_memberships": (
                discovery["attachment_mapping_gain_endpoint_sample_memberships"]
                + holdout["attachment_mapping_gain_endpoint_sample_memberships"]
            ),
            "context_mapping_gain_endpoint_sample_memberships": (
                discovery["context_mapping_gain_endpoint_sample_memberships"]
                + holdout["context_mapping_gain_endpoint_sample_memberships"]
            ),
            "presentation_changing_unresolved_endpoints_fall_back_to_hybrid_ot": (
                True
            ),
        },
        "constraint_graph": {
            "component_nodes": len(graph_nodes),
            "directed_component_constraints": len(constraint_reports),
            "cross_validated_constraints": sum(
                constraint["cross_validated"] for constraint in constraint_reports
            ),
            "single_validation_set_constraints": len(single_set_constraints),
            "single_validation_set_constraint_details": single_set_constraints,
            "self_component_conflict_records": self_records,
            "self_component_conflict_samples": (
                discovery["self_component_conflict_samples"]
                + holdout["self_component_conflict_samples"]
            ),
            "cyclic_strong_component_count": len(cyclic_components),
            "cyclic_strong_components": cyclic_components,
            "constraint_graph_acyclic": graph_acyclic,
            "all_component_constraints_cross_validated": all_cross_validated,
            "constraints": constraint_reports,
            "constraint_system_ready_for_candidate_testing": ready,
            "visibility_constraints_certify_gauge": False,
            "gauge_solution_unique": False,
            "scale_factor_certified": False,
        },
        "proof_scope": {
            "all_accepted_triangle_fragments_required": True,
            "overflow_invalidates_capture": True,
            "fixed_layer_limit_used": False,
            "maximum_observed_owner_count_used_as_limit": False,
            "family_count_inferred": False,
            "topology_component_count_inferred_as_family_count": False,
            "topology_components_promoted_to_persistent_objects": False,
            "integer_or_tenth_prior_used": False,
            "priors_used_to_select_or_reject_evidence": False,
            "screen_position_used_for_identity": False,
            "observed_depth_used_for_identity": False,
            "observed_depth_used_only_for_constraint_bounds": True,
            "runtime_dispatch_key_uses_intrinsic_fields_only": True,
            "frame_local_context_lookup_used": context_dispatch is not None,
            "frame_local_context_requires_exact_static_projection_incidence": (
                context_dispatch is not None
            ),
            "frame_local_context_promoted_to_persistent_identity": False,
            "independent_pair_extrema_form_conservative_bound": True,
            "same_sample_maximum_ratio_claimed": False,
            "multiplier_fitted_or_recommended": False,
            "presentation_changed_by_this_report": False,
            "independent_post_dispatch_raster_validation_required": True,
        },
        "blockers": {
            "unresolved_endpoint_sample_memberships": (
                discovery["context_unmapped_endpoint_sample_memberships"]
                + holdout["context_unmapped_endpoint_sample_memberships"]
            ),
            "unresolved_pair_sample_memberships": (
                discovery["unresolved_pair_sample_memberships"]
                + holdout["unresolved_pair_sample_memberships"]
            ),
            "single_validation_set_constraints": len(single_set_constraints),
            "self_component_conflict_records": self_records,
            "non_strict_depth_bound_records": (
                discovery["non_strict_depth_bound_records"]
                + holdout["non_strict_depth_bound_records"]
            ),
            "cyclic_strong_components": len(cyclic_components),
        },
    }


def _load_jsonl(path: Path) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    records = []
    with path.open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                value = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{line_number}: invalid JSON") from exc
            if not isinstance(value, dict):
                raise ValueError(f"{path}:{line_number}: record is not an object")
            records.append(value)
    return records, {
        "path": str(path),
        "bytes": path.stat().st_size,
        "sha256": _file_sha256(path),
        "records": len(records),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base_lineage", type=Path)
    parser.add_argument("dispatch_refinement", type=Path)
    parser.add_argument("discovery_stack", type=Path)
    parser.add_argument("holdout_stack", type=Path)
    parser.add_argument("--context-refinement", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        base = json.loads(args.base_lineage.read_text(encoding="utf-8"))
        refinement = json.loads(
            args.dispatch_refinement.read_text(encoding="utf-8")
        )
        discovery, discovery_source = _load_jsonl(args.discovery_stack)
        holdout, holdout_source = _load_jsonl(args.holdout_stack)
        context_refinement = (
            json.loads(args.context_refinement.read_text(encoding="utf-8"))
            if args.context_refinement is not None
            else None
        )
        report = analyze_visibility_stack_constraints(
            base, refinement, discovery, holdout, context_refinement
        )
        report["source_files"] = {
            "base_lineage": {
                "path": str(args.base_lineage),
                "bytes": args.base_lineage.stat().st_size,
                "sha256": _file_sha256(args.base_lineage),
            },
            "dispatch_refinement": {
                "path": str(args.dispatch_refinement),
                "bytes": args.dispatch_refinement.stat().st_size,
                "sha256": _file_sha256(args.dispatch_refinement),
            },
            "discovery_stack": discovery_source,
            "holdout_stack": holdout_source,
        }
        if args.context_refinement is not None:
            report["source_files"]["context_refinement"] = {
                "path": str(args.context_refinement),
                "bytes": args.context_refinement.stat().st_size,
                "sha256": _file_sha256(args.context_refinement),
            }
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"visibility_stack_constraints: {exc}", file=sys.stderr)
        return 2
    payload = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(payload, encoding="utf-8")
    else:
        print(payload, end="")
    return 0 if report["constraint_graph"][
        "constraint_system_ready_for_candidate_testing"
    ] else 1


if __name__ == "__main__":
    raise SystemExit(main())
