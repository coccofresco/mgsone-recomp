#!/usr/bin/env python3
"""Audit recurrent intrinsic lineage candidates in visibility conflicts.

The dual-owner renderer emits one ``replay.visibility_owner`` record for each
triangle owner which participates in a certain OT/depth conflict. Its explicit
full-catalog mode also emits every comparable topology-valid Projected3D owner
plus a completeness record. This tool validates those raw payloads, joins the
conflict subset back to its metadata, and measures exact categorical recurrence
across caller-labelled discovery and holdout captures.

Recurrence is deliberately not promoted to object identity. Transform and
computed-model fields currently include versioned runtime digests, while guest
RAM and DMA addresses can name reused staging slots. The report therefore
answers which intrinsic evidence is present, recurrent, unique, or ambiguous;
for unambiguous recurrent triangles it also compares exact shared-projection
incidence and the frame-local complete-affine equality partition without
retaining numeric process-local IDs or matrices as persistent identity. It
never clusters pixels, treats absent conflict geometry as separation evidence,
caps the result at four, rounds a ratio to tenths, or fits a depth scale.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import math
import struct
import sys
from fractions import Fraction
from pathlib import Path
from typing import Any, Iterable

from depth_capture_compare import compare_captures, read_trace
from depth_family_trace import (
    AFFINE_FIELDS,
    MODEL_VALUE_FLOW_PROVENANCE,
    canonical_affine,
    model_value_flow_lineage,
    runtime_model_fingerprints,
    unsigned64,
)
from visibility_audit_trace import (
    AUDIT_EVENT,
    CONFLICT_EVENT,
    TRACE_SCHEMA,
    analyze_visibility_trace,
)


REPORT_SCHEMA = "ps1-visibility-lineage-audit/v1"
OWNER_EVENT = "replay.visibility_owner"
CATALOG_EVENT = "replay.visibility_catalog"
OWNER_SCHEMA = 2
LEGACY_OWNER_SCHEMA = 1
CATALOG_SCHEMA = 1

CATALOG_FIELDS = (
    "catalog_schema",
    "owner_schema",
    "owners",
    "eligible_owners",
    "emitted_owners",
    "conflict_referenced_owners",
)

PROJECTION_EXACT = 1 << 0
TRANSFORM_EXACT = 1 << 1
MODEL_SOURCE_EXACT = 1 << 2
GP0_SOURCE_EXACT = 1 << 3
DMA_TRAVERSAL_EXACT = 1 << 4
MODEL_FLOW_EXACT = 1 << 5
KNOWN_EVIDENCE = (
    PROJECTION_EXACT
    | TRANSFORM_EXACT
    | MODEL_SOURCE_EXACT
    | GP0_SOURCE_EXACT
    | DMA_TRAVERSAL_EXACT
    | MODEL_FLOW_EXACT
)

OWNER_SCALAR_FIELDS = (
    "owner_schema",
    "owner",
    "packet_sequence",
    "submission_id",
    "source_ordinal",
    "frame_index",
    "operation_index",
    "triangle_index",
    "route",
    "opaque",
    "fully_precise",
    "authentic_depth",
    "comparable",
    "valid_topology",
    "complete_evidence",
    "coherent_projection_context",
    "coherent_transform_lineage",
    "coherent_dma_traversal",
)

VERTEX_FIELDS = (
    "index",
    "evidence",
    "projection_id",
    "projection_context",
    "transform_lineage",
    "model_source",
    "model_flow_source",
    "model_flow_program",
    "gp0_source",
    "dma_list",
    "dma_node",
    "dma_ordinal",
)


def _integer(record: dict[str, Any], field: str, context: str) -> int:
    value = record.get(field)
    if not isinstance(value, int):
        raise ValueError(f"{context}: missing/non-integer field {field!r}")
    return value


def _owner_fields(record: dict[str, Any], context: str) -> dict[str, Any]:
    owner = {field: _integer(record, field, context) for field in OWNER_SCALAR_FIELDS}
    owner_schema = owner["owner_schema"]
    if owner_schema >= 2:
        owner["coherent_transform_value"] = _integer(
            record, "coherent_transform_value", context
        )
    else:
        owner["coherent_transform_value"] = None
    vertices = []
    for vertex_index in range(3):
        prefix = f"v{vertex_index}_"
        vertex = {
            field: _integer(record, prefix + field, context)
            for field in VERTEX_FIELDS
        }
        vertex["transform_value"] = (
            _integer(record, prefix + "transform_value", context)
            if owner_schema >= 2
            else None
        )
        vertices.append(vertex)
    owner["vertices"] = vertices
    return owner


def _catalog_fields(record: dict[str, Any], context: str) -> dict[str, int]:
    return {field: _integer(record, field, context) for field in CATALOG_FIELDS}


def _validate_owner(owner: dict[str, Any], context: str) -> list[str]:
    errors: list[str] = []
    if owner["owner_schema"] not in (LEGACY_OWNER_SCHEMA, OWNER_SCHEMA):
        errors.append(
            f"{context}: owner schema {owner['owner_schema']}, expected "
            f"{LEGACY_OWNER_SCHEMA} or {OWNER_SCHEMA}"
        )
    for field in (
        "opaque",
        "fully_precise",
        "authentic_depth",
        "comparable",
        "valid_topology",
        "coherent_projection_context",
        "coherent_transform_lineage",
        "coherent_dma_traversal",
    ):
        if owner[field] not in (0, 1):
            errors.append(f"{context}: {field} is not boolean")
    if owner["owner_schema"] >= 2 and owner["coherent_transform_value"] not in (
        0,
        1,
    ):
        errors.append(f"{context}: coherent_transform_value is not boolean")

    comparable = (
        owner["route"] == 0
        and owner["opaque"] == 1
        and owner["fully_precise"] == 1
        and owner["authentic_depth"] == 1
    )
    if owner["comparable"] != int(comparable):
        errors.append(f"{context}: comparable flag disagrees with owner state")
    if not comparable or owner["valid_topology"] != 1:
        errors.append(
            f"{context}: a selected owner is not comparable/topology-valid"
        )

    vertices = owner["vertices"]
    indices = [vertex["index"] for vertex in vertices]
    if any(index < 0 or index > 3 for index in indices) or len(set(indices)) != 3:
        errors.append(f"{context}: invalid triangle vertex indices {indices}")

    evidence_intersection = KNOWN_EVIDENCE
    for vertex_index, vertex in enumerate(vertices):
        vertex_context = f"{context} vertex {vertex_index}"
        evidence = vertex["evidence"]
        if evidence < 0 or evidence & ~KNOWN_EVIDENCE:
            errors.append(f"{vertex_context}: unknown provenance evidence bits")
            continue
        evidence_intersection &= evidence

        projection = bool(evidence & PROJECTION_EXACT)
        transform = bool(evidence & TRANSFORM_EXACT)
        model_source = bool(evidence & MODEL_SOURCE_EXACT)
        gp0_source = bool(evidence & GP0_SOURCE_EXACT)
        dma = bool(evidence & DMA_TRAVERSAL_EXACT)
        model_flow = bool(evidence & MODEL_FLOW_EXACT)
        if not projection and (
            vertex["projection_id"] != 0
            or vertex["projection_context"] != 0
            or (
                owner["owner_schema"] >= 2
                and vertex["transform_value"] != 0
            )
            or transform
            or model_source
            or model_flow
        ):
            errors.append(f"{vertex_context}: lineage lacks projection evidence")
        if projection and vertex["projection_id"] == 0:
            errors.append(f"{vertex_context}: zero exact projection id")
        if not transform and vertex["transform_lineage"] != 0:
            errors.append(f"{vertex_context}: transform value lacks evidence")
        if model_source != (vertex["model_source"] != 0):
            errors.append(f"{vertex_context}: model source disagrees with evidence")
        if not model_flow and (
            vertex["model_flow_source"] != 0
            or vertex["model_flow_program"] != 0
        ):
            errors.append(f"{vertex_context}: model-flow value lacks evidence")
        if gp0_source != (vertex["gp0_source"] != 0):
            errors.append(f"{vertex_context}: GP0 source disagrees with evidence")
        dma_values = (
            vertex["dma_list"],
            vertex["dma_node"],
            vertex["dma_ordinal"],
        )
        if not dma and any(value != 0 for value in dma_values):
            errors.append(f"{vertex_context}: DMA value lacks evidence")
        if dma and (
            vertex["dma_list"] == 0
            or vertex["dma_node"] == 0
            or not gp0_source
        ):
            errors.append(f"{vertex_context}: incomplete exact DMA provenance")

    if owner["complete_evidence"] != evidence_intersection:
        errors.append(f"{context}: complete-evidence intersection mismatch")

    projection_coherent = bool(evidence_intersection & PROJECTION_EXACT) and len(
        {vertex["projection_context"] for vertex in vertices}
    ) == 1
    transform_coherent = bool(evidence_intersection & TRANSFORM_EXACT) and len(
        {vertex["transform_lineage"] for vertex in vertices}
    ) == 1
    dma_coherent = bool(evidence_intersection & DMA_TRAVERSAL_EXACT) and len(
        {
            (
                vertex["dma_list"],
                vertex["dma_node"],
                vertex["dma_ordinal"],
            )
            for vertex in vertices
        }
    ) == 1
    if owner["coherent_projection_context"] != int(projection_coherent):
        errors.append(f"{context}: projection-context coherence mismatch")
    if owner["coherent_transform_lineage"] != int(transform_coherent):
        errors.append(f"{context}: transform-lineage coherence mismatch")
    if owner["owner_schema"] >= 2:
        transform_value_coherent = bool(evidence_intersection & PROJECTION_EXACT) and len(
            {vertex["transform_value"] for vertex in vertices}
        ) == 1
        if owner["coherent_transform_value"] != int(transform_value_coherent):
            errors.append(f"{context}: transform-value coherence mismatch")
    if owner["coherent_dma_traversal"] != int(dma_coherent):
        errors.append(f"{context}: DMA-traversal coherence mismatch")
    return errors


def _transform_signature(owner: dict[str, Any]) -> tuple[Any, ...] | None:
    if owner["coherent_transform_lineage"] != 1:
        return None
    return ("transform-digest/v1", owner["vertices"][0]["transform_lineage"])


def _geometry_signature(owner: dict[str, Any]) -> tuple[Any, ...] | None:
    geometry = []
    for vertex in owner["vertices"]:
        evidence = vertex["evidence"]
        direct = bool(evidence & MODEL_SOURCE_EXACT)
        verified_flow = vertex.get("verified_model_flow")
        computed = bool(evidence & MODEL_FLOW_EXACT) or verified_flow is not None
        if not direct and not computed:
            return None
        if verified_flow is not None:
            computed_lineage = (
                "sha256-payload/v1",
                verified_flow["source_digest"],
                verified_flow["program_digest"],
            )
        elif evidence & MODEL_FLOW_EXACT:
            computed_lineage = (
                "runtime-digest64/v1",
                vertex["model_flow_source"],
                vertex["model_flow_program"],
            )
        else:
            computed_lineage = None
        geometry.append(
            (
                vertex["index"],
                vertex["model_source"] if direct else None,
                computed_lineage,
            )
        )
    return ("ordered-model-lineage/v1", tuple(geometry))


def _submission_signatures(
    owner: dict[str, Any],
) -> tuple[tuple[Any, ...] | None, tuple[Any, ...] | None]:
    if owner["coherent_dma_traversal"] != 1:
        return None, None
    words = []
    packet_structure = []
    for vertex in owner["vertices"]:
        evidence = vertex["evidence"]
        if not evidence & GP0_SOURCE_EXACT or not evidence & DMA_TRAVERSAL_EXACT:
            return None, None
        words.append(
            (
                vertex["index"],
                vertex["gp0_source"],
                vertex["dma_list"],
                vertex["dma_node"],
                vertex["dma_ordinal"],
            )
        )
        # GP0 addresses may use KSEG0 while DMA nodes are physical. Normalize
        # both through the exact 2 MiB PS1 RAM mirror, then retain the operand's
        # structural offset inside its proven node. Absolute list/node address
        # and traversal ordinal remain in submission_exact: alternating OT
        # buffers and changing depth buckets are transport facts, not stable
        # object identity.
        word_address = (vertex["gp0_source"] - 1) & 0x1FFFFF
        node_address = (vertex["dma_node"] - 1) & 0x1FFFFF
        packet_structure.append(
            (vertex["index"], (word_address - node_address) & 0x1FFFFF)
        )
    exact = ("ordered-dma-lineage/v1", owner["triangle_index"], tuple(words))
    structural = (
        "node-relative-packet-lineage/v1",
        owner["triangle_index"],
        tuple(packet_structure),
    )
    return exact, structural


def _signatures(owner: dict[str, Any]) -> dict[str, tuple[Any, ...] | None]:
    transform = _transform_signature(owner)
    geometry = _geometry_signature(owner)
    submission_exact, submission_structure = _submission_signatures(owner)
    complete = None
    if (
        transform is not None
        and geometry is not None
        and submission_structure is not None
    ):
        complete = (
            "complete-triangle-lineage/v2",
            transform,
            geometry,
            submission_structure,
        )
    return {
        "transform": transform,
        "geometry": geometry,
        "submission_exact": submission_exact,
        "submission_structure": submission_structure,
        "complete_triangle": complete,
    }


def _signature_id(signature: tuple[Any, ...]) -> str:
    payload = json.dumps(signature, separators=(",", ":"), sort_keys=False)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _runtime_dispatch_key(owner: dict[str, Any]) -> tuple[Any, ...] | None:
    """Return the compact-runtime observable complete categorical signature.

    The compact MIPS tracker emits the same versioned 64-bit construction
    fingerprints as the full DAG without retaining its nodes. Offline heavy
    captures audit those values against both the full DAG and SHA-256 payloads.
    A key may dispatch only when discovery and holdout map it to exactly one
    relation component. It is never promoted to persistent object identity.
    """

    if (
        owner["coherent_transform_lineage"] != 1
        or owner["coherent_dma_traversal"] != 1
    ):
        return None
    vertices = []
    for vertex in owner["vertices"]:
        evidence = vertex["evidence"]
        if not evidence & GP0_SOURCE_EXACT or not evidence & DMA_TRAVERSAL_EXACT:
            return None
        verified_flow = vertex.get("verified_model_flow")
        if evidence & MODEL_FLOW_EXACT:
            model_flow_source = unsigned64(vertex["model_flow_source"])
            model_flow_program = unsigned64(vertex["model_flow_program"])
        elif verified_flow is not None:
            model_flow_source = unsigned64(verified_flow["runtime_source"])
            model_flow_program = unsigned64(verified_flow["runtime_program"])
        else:
            return None
        model_source_present = bool(evidence & MODEL_SOURCE_EXACT)
        word_address = (vertex["gp0_source"] - 1) & 0x1FFFFF
        node_address = (vertex["dma_node"] - 1) & 0x1FFFFF
        vertices.append(
            (
                vertex["index"],
                model_source_present,
                unsigned64(vertex["model_source"])
                if model_source_present
                else 0,
                model_flow_source,
                model_flow_program,
                (word_address - node_address) & 0x1FFFFF,
            )
        )
    return (
        "visibility-runtime-dispatch/v2",
        unsigned64(owner["vertices"][0]["transform_lineage"]),
        owner["triangle_index"],
        tuple(vertices),
    )


def _runtime_dispatch_key_report_entry(
    key: tuple[Any, ...],
) -> dict[str, Any]:
    _, transform_lineage, triangle_index, vertices = key
    return {
        "transform_lineage_hex": f"{transform_lineage:016x}",
        "triangle_index": triangle_index,
        "vertices": [
            {
                "index": index,
                "model_source_present": model_source_present,
                "model_source_hex": f"{model_source:016x}",
                "model_flow_source_hex": f"{model_flow_source:016x}",
                "model_flow_program_hex": f"{model_flow_program:016x}",
                "gp0_node_offset": gp0_node_offset,
            }
            for (
                index,
                model_source_present,
                model_source,
                model_flow_source,
                model_flow_program,
                gp0_node_offset,
            ) in vertices
        ],
    }


def _runtime_dispatch_report_entry(
    key: tuple[Any, ...], component: str
) -> dict[str, Any]:
    return {
        "component_sha256": component,
        **_runtime_dispatch_key_report_entry(key),
    }


def _summarize_runtime_dispatch(
    occurrences: list[dict[str, Any]],
    component_membership: dict[str, str],
    reported_components: set[str],
    *,
    target: str,
    basis: str,
) -> dict[str, Any]:
    """Cross-validate compact runtime keys for an intrinsic partition.

    The partition is supplied by a separate relation audit.  This helper only
    asks whether the compact runtime evidence can reproduce that partition in
    both validation sets without an ambiguous key; it neither creates nor
    merges components.
    """

    reported_component_signatures = {
        signature
        for signature, component in component_membership.items()
        if component in reported_components
    }
    dispatch_observations: dict[
        tuple[Any, ...], dict[str, set[str]]
    ] = collections.defaultdict(
        lambda: {
            "components": set(),
            "signatures": set(),
            "validation_sets": set(),
        }
    )
    complete_signatures_with_dispatch = set()
    for occurrence in occurrences:
        signature = occurrence["signatures"]["complete_triangle"]
        dispatch_key = occurrence.get("runtime_dispatch_key")
        if signature is None or dispatch_key is None:
            continue
        signature_id = _signature_id(signature)
        component = component_membership.get(signature_id)
        if component not in reported_components:
            continue
        observation = dispatch_observations[dispatch_key]
        observation["components"].add(component)
        observation["signatures"].add(signature_id)
        observation["validation_sets"].add(occurrence["validation_set"])
        complete_signatures_with_dispatch.add(signature_id)

    dispatch_collisions = []
    dispatch_entries = []
    unassessed_dispatch_keys = 0
    dispatched_signatures = set()
    dispatched_components = set()
    for key, observation in sorted(
        dispatch_observations.items(), key=lambda item: repr(item[0])
    ):
        if len(observation["components"]) != 1:
            dispatch_collisions.append(
                {
                    "dispatch_sha256": _signature_id(key),
                    "component_sha256": sorted(observation["components"]),
                    "triangle_sha256": sorted(observation["signatures"]),
                }
            )
            continue
        if observation["validation_sets"] != {"discovery", "holdout"}:
            unassessed_dispatch_keys += 1
            continue
        component = next(iter(observation["components"]))
        dispatch_entries.append(_runtime_dispatch_report_entry(key, component))
        dispatched_signatures.update(observation["signatures"])
        dispatched_components.add(component)
    dispatch_entries.sort(
        key=lambda item: (
            item["component_sha256"],
            item["transform_lineage_hex"],
            item["triangle_index"],
            json.dumps(item["vertices"], separators=(",", ":")),
        )
    )

    return {
        "schema": "ps1-visibility-runtime-dispatch-map/v2",
        "target": target,
        "basis": basis,
        "entries": dispatch_entries,
        "entry_count": len(dispatch_entries),
        "components": len(dispatched_components),
        "complete_triangle_signatures_with_observable_key": len(
            complete_signatures_with_dispatch
        ),
        "complete_triangle_signatures_dispatched": len(dispatched_signatures),
        "ambiguous_key_collisions": len(dispatch_collisions),
        "ambiguous_key_collision_preview": dispatch_collisions[:32],
        "unassessed_single_validation_set_keys": unassessed_dispatch_keys,
        "reported_component_triangle_signatures": len(
            reported_component_signatures
        ),
        "all_reported_component_signatures_dispatched": (
            dispatched_signatures == reported_component_signatures
        ),
        "compact_model_fingerprint_schema": 1,
        "compact_model_fingerprints_cross_audited_against_full_dag": True,
        "uses_screen_position": False,
        "uses_colour": False,
        "uses_observed_depth": False,
        "uses_numeric_transform_values": False,
        "uses_family_count_prior": False,
        "uses_integer_or_tenth_prior": False,
        "persistent_object_identity_certified": False,
        "candidate_dispatch_ready": bool(dispatch_entries)
        and not dispatch_collisions
        and unassessed_dispatch_keys == 0
        and dispatched_signatures == reported_component_signatures
        and dispatched_components == reported_components,
    }


def _attach_instrumented_model_lineage(
    light_path: Path,
    instrumented_path: Path,
    owner_records: dict[tuple[int, int], tuple[dict[str, Any], int]],
    selected: set[tuple[int, int]],
) -> dict[str, Any]:
    equivalence = compare_captures(light_path, instrumented_path)
    errors: list[str] = []
    warnings: list[str] = []
    stream_proved = (
        equivalence["stream_equal"]
        and not equivalence["validation_errors"]["oracle"]
        and not equivalence["validation_errors"]["instrumented"]
    )
    if not stream_proved:
        errors.append(
            f"{instrumented_path}: instrumented stream is not equivalent to "
            f"timing oracle {light_path}"
        )
        return {
            "equivalence": equivalence,
            "owner_vertices": 0,
            "projection_window_vertices": 0,
            "verified_model_flow_vertices": 0,
            "verified_model_flow_owners": 0,
            "verified_affine_vertices": 0,
            "verified_affine_owners": 0,
            "payloads": {"source": {}, "program": {}},
            "errors": errors,
            "warnings": warnings,
        }

    light_records, _ = read_trace(light_path)
    instrumented_records, _ = read_trace(instrumented_path)
    projection_pairs: dict[int, tuple[dict[str, Any], dict[str, Any]]] = {}
    gp0_pairs: dict[
        tuple[int, int, int, int, int, int],
        tuple[dict[str, Any], dict[str, Any]],
    ] = {}
    for index, (light, instrumented) in enumerate(
        zip(light_records, instrumented_records)
    ):
        if light["event"] != instrumented["event"]:
            errors.append(f"canonical record {index}: event mismatch after equivalence")
            continue
        if light["event"] == "gte.projection":
            projection_id = int(light.get("projection_id", 0))
            if projection_id in projection_pairs:
                errors.append(f"duplicate light projection id {projection_id}")
            else:
                projection_pairs[projection_id] = (light, instrumented)
        elif light["event"] == "gpu.gp0_word":
            if int(light.get("projection_id", 0)) == 0:
                continue
            key = (
                int(light.get("submission_id", 0)),
                int(light.get("projection_id", 0)),
                int(light.get("word_address", -1)),
                int(light.get("dma_list_start", -1)),
                int(light.get("dma_node_address", -1)),
                int(light.get("dma_traversal_ordinal", -1)),
            )
            if key in gp0_pairs:
                errors.append(f"duplicate light GP0 transport key {key}")
            else:
                gp0_pairs[key] = (light, instrumented)

    expressions: dict[int, dict[str, Any]] = {}
    with instrumented_path.open(encoding="utf-8") as stream:
        for lineno, line in enumerate(stream, 1):
            if not line.strip():
                continue
            record = json.loads(line)
            if record.get("event") != "mips.value_expr":
                continue
            expression_id = int(record.get("expression_id", 0))
            if expression_id == 0 or expression_id in expressions:
                errors.append(
                    f"{instrumented_path}:{lineno}: duplicate/zero expression id "
                    f"{expression_id}"
                )
            else:
                expressions[expression_id] = record

    projection_flows: dict[int, dict[str, Any]] = {}
    payloads: dict[str, dict[str, str]] = {"source": {}, "program": {}}
    incomplete_model_projections = 0
    for light_id, (light, instrumented) in projection_pairs.items():
        flow = model_value_flow_lineage(instrumented, expressions)
        runtime = runtime_model_fingerprints(instrumented, expressions)
        runtime_complete = int(
            instrumented.get("model_fingerprint_complete", 0)
        ) == 1
        runtime_source = unsigned64(
            int(instrumented.get("model_source_fingerprint", 0))
        )
        runtime_program = unsigned64(
            int(instrumented.get("model_program_fingerprint", 0))
        )
        if not runtime_complete:
            incomplete_model_projections += 1
            if runtime_source != 0 or runtime_program != 0:
                errors.append(
                    f"{instrumented_path}: incomplete projection "
                    f"{instrumented.get('projection_id')} carries model fingerprints"
                )
            continue
        if flow is None or runtime is None:
            errors.append(
                f"{instrumented_path}: projection {instrumented.get('projection_id')} "
                "claims model evidence without a complete DAG"
            )
            continue
        if (
            runtime_source != runtime[0]
            or runtime_program != runtime[1]
        ):
            errors.append(
                f"{instrumented_path}: projection {instrumented.get('projection_id')} "
                "runtime model fingerprint disagrees with complete DAG"
            )
            continue
        for kind in ("source", "program"):
            digest = flow[f"{kind}_digest"]
            payload = json.dumps(
                flow[f"{kind}_payload"], separators=(",", ":")
            )
            previous = payloads[kind].setdefault(digest, payload)
            if previous != payload:
                errors.append(
                    f"{instrumented_path}: {kind} SHA-256 model payload collision"
                )
        projection_flows[light_id] = {
            "light": light,
            "instrumented": instrumented,
            "source_digest": flow["source_digest"],
            "program_digest": flow["program_digest"],
            "runtime_source": runtime_source,
            "runtime_program": runtime_program,
        }

    owner_vertices = 0
    projection_window_vertices = 0
    joined_vertices = 0
    joined_owners = 0
    affine_vertices = 0
    affine_owners = 0
    for key in selected:
        pair = owner_records.get(key)
        if pair is None:
            continue
        owner, lineno = pair
        owner_joined = True
        owner_affine_joined = True
        for vertex_index, vertex in enumerate(owner["vertices"]):
            owner_vertices += 1
            projection_id = vertex["projection_id"]
            if projection_id not in projection_pairs:
                owner_joined = False
                owner_affine_joined = False
                continue
            projection_window_vertices += 1
            light_projection, heavy_projection = projection_pairs[projection_id]
            context = (
                f"{light_path}:{lineno} owner {owner['owner']} vertex {vertex_index}"
            )
            if vertex["projection_context"] != int(
                light_projection.get("projection_context", 0)
            ):
                errors.append(f"{context}: projection-context join mismatch")
                owner_joined = False
                owner_affine_joined = False
                continue
            if vertex["transform_lineage"] != int(
                light_projection.get("transform_lineage", 0)
            ):
                errors.append(f"{context}: transform-lineage join mismatch")
                owner_joined = False
                owner_affine_joined = False
                continue
            if all(
                isinstance(light_projection.get(field), int)
                for field in AFFINE_FIELDS
            ):
                vertex["verified_affine"] = tuple(
                    int(light_projection[field]) for field in AFFINE_FIELDS
                )
                affine_vertices += 1
            else:
                owner_affine_joined = False

            flow = projection_flows.get(projection_id)
            if flow is None:
                owner_joined = False
                continue
            transport_key = (
                owner["submission_id"],
                projection_id,
                vertex["gp0_source"] - 1,
                vertex["dma_list"] - 1,
                vertex["dma_node"] - 1,
                vertex["dma_ordinal"],
            )
            gp0_pair = gp0_pairs.get(transport_key)
            if gp0_pair is None:
                errors.append(f"{context}: no exact GP0/DMA transport join")
                owner_joined = False
                continue
            _, heavy_word = gp0_pair
            if int(heavy_word.get("projection_id", 0)) != int(
                heavy_projection.get("projection_id", 0)
            ):
                errors.append(f"{context}: heavy projection/GP0 join mismatch")
                owner_joined = False
                continue
            if (
                int(heavy_word.get("intrinsic_provenance", 0))
                & MODEL_VALUE_FLOW_PROVENANCE
            ) == 0:
                errors.append(f"{context}: heavy GP0 word lacks model-flow evidence")
                owner_joined = False
                continue
            if unsigned64(int(heavy_word.get("model_source_fingerprint", 0))) != flow[
                "runtime_source"
            ] or unsigned64(
                int(heavy_word.get("model_program_fingerprint", 0))
            ) != flow["runtime_program"]:
                errors.append(f"{context}: heavy GTE-to-GP0 fingerprint mismatch")
                owner_joined = False
                continue
            vertex["verified_model_flow"] = {
                "source_digest": flow["source_digest"],
                "program_digest": flow["program_digest"],
                "runtime_source": flow["runtime_source"],
                "runtime_program": flow["runtime_program"],
            }
            joined_vertices += 1
        if owner_joined:
            joined_owners += 1
        if owner_affine_joined:
            affine_owners += 1

    if joined_vertices == 0:
        warnings.append(
            f"{light_path}: equivalent heavy trace joins no selected owner vertices"
        )
    return {
        "equivalence": equivalence,
        "owner_vertices": owner_vertices,
        "projection_window_vertices": projection_window_vertices,
        "verified_model_flow_vertices": joined_vertices,
        "verified_model_flow_owners": joined_owners,
        "verified_affine_vertices": affine_vertices,
        "verified_affine_owners": affine_owners,
        "captured_projections": len(projection_pairs),
        "verified_model_flow_projections": len(projection_flows),
        "incomplete_model_projections": incomplete_model_projections,
        "payloads": payloads,
        "errors": errors,
        "warnings": warnings,
    }


def _load_capture(
    path: Path,
    validation_set: str,
    capture_index: int,
    instrumented_path: Path | None = None,
) -> dict[str, Any]:
    summary = analyze_visibility_trace(path)
    audits: dict[int, dict[str, Any]] = {}
    conflicts: dict[int, list[dict[str, Any]]] = collections.defaultdict(list)
    catalogs: dict[int, tuple[dict[str, int], int]] = {}
    owner_records: dict[tuple[int, int], tuple[dict[str, Any], int]] = {}
    errors = list(summary["errors"])
    warnings = list(summary["warnings"])

    with path.open(encoding="utf-8") as stream:
        for lineno, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{lineno}: invalid JSON: {exc}") from exc
            event = record.get("event")
            if event not in {
                AUDIT_EVENT,
                CONFLICT_EVENT,
                CATALOG_EVENT,
                OWNER_EVENT,
            }:
                continue
            if record.get("schema") != TRACE_SCHEMA:
                errors.append(
                    f"{path}:{lineno}: schema {record.get('schema')!r}, "
                    f"expected {TRACE_SCHEMA!r}"
                )
                continue
            try:
                frame = _integer(record, "frame", f"{path}:{lineno}")
            except ValueError as exc:
                errors.append(str(exc))
                continue
            if event == AUDIT_EVENT:
                audits[frame] = record
            elif event == CONFLICT_EVENT:
                conflicts[frame].append(record)
            elif event == CATALOG_EVENT:
                context = f"{path}:{lineno} frame {frame}"
                try:
                    catalog = _catalog_fields(record, context)
                except ValueError as exc:
                    errors.append(str(exc))
                    continue
                if frame in catalogs:
                    errors.append(f"{context}: duplicate {CATALOG_EVENT} record")
                    continue
                catalogs[frame] = (catalog, lineno)
            else:
                context = f"{path}:{lineno} frame {frame}"
                try:
                    owner = _owner_fields(record, context)
                except ValueError as exc:
                    errors.append(str(exc))
                    continue
                token = owner["owner"]
                key = (frame, token)
                if key in owner_records:
                    errors.append(f"{context}: duplicate owner token {token}")
                    continue
                errors.extend(_validate_owner(owner, context))
                owner_records[key] = (owner, lineno)

    occurrences: list[dict[str, Any]] = []
    owner_schemas_by_frame: dict[int, set[int]] = collections.defaultdict(set)
    for (frame, _), (owner, _) in owner_records.items():
        owner_schemas_by_frame[frame].add(owner["owner_schema"])
    for frame, schemas in sorted(owner_schemas_by_frame.items()):
        if len(schemas) != 1:
            errors.append(
                f"{path} frame {frame}: mixed owner schemas {sorted(schemas)}"
            )

    referenced: set[tuple[int, int]] = set()
    metadata = (
        ("packet_sequence", "packet_sequence"),
        ("submission_id", "submission_id"),
        ("source_ordinal", "source_ordinal"),
        ("frame_index", "frame_index"),
        ("operation_index", "operation_index"),
        ("triangle_index", "triangle_index"),
    )
    per_owner_roles: dict[tuple[int, int], set[str]] = collections.defaultdict(set)
    per_owner_samples: collections.Counter[tuple[int, int]] = collections.Counter()
    for frame, frame_conflicts in conflicts.items():
        audit = audits.get(frame)
        catalog_size = int(audit.get("owners", 0)) if audit is not None else 0
        for conflict_index, conflict in enumerate(frame_conflicts):
            for role, prefix in (("ot", "ot"), ("depth", "depth")):
                context = f"{path} frame {frame} conflict {conflict_index} {role}"
                try:
                    token = _integer(conflict, f"{prefix}_owner", context)
                    samples = _integer(conflict, "samples", context)
                except ValueError as exc:
                    errors.append(str(exc))
                    continue
                key = (frame, token)
                referenced.add(key)
                per_owner_roles[key].add(role)
                if samples > 0:
                    per_owner_samples[key] += samples
                pair = owner_records.get(key)
                if pair is None:
                    errors.append(f"{context}: missing {OWNER_EVENT} record")
                    continue
                owner, _ = pair
                if token <= 0 or token > catalog_size:
                    errors.append(f"{context}: owner token outside audit catalog")
                for owner_field, conflict_suffix in metadata:
                    conflict_field = f"{prefix}_{conflict_suffix}"
                    try:
                        conflict_value = _integer(conflict, conflict_field, context)
                    except ValueError as exc:
                        errors.append(str(exc))
                        continue
                    if owner[owner_field] != conflict_value:
                        errors.append(
                            f"{context}: {conflict_field} disagrees with owner payload"
                        )

    catalog_frames: set[int] = set()
    catalog_owner_records = 0
    for frame, (catalog, lineno) in catalogs.items():
        context = f"{path}:{lineno} frame {frame} catalog"
        catalog_valid = True

        def catalog_error(message: str) -> None:
            nonlocal catalog_valid
            catalog_valid = False
            errors.append(f"{context}: {message}")

        if catalog["catalog_schema"] != CATALOG_SCHEMA:
            catalog_error(
                f"catalog schema {catalog['catalog_schema']}, expected "
                f"{CATALOG_SCHEMA}"
            )
        if catalog["owner_schema"] not in (LEGACY_OWNER_SCHEMA, OWNER_SCHEMA):
            catalog_error(
                f"owner schema {catalog['owner_schema']}, expected "
                f"{LEGACY_OWNER_SCHEMA} or {OWNER_SCHEMA}"
            )
        frame_owner_schemas = owner_schemas_by_frame.get(frame, set())
        if frame_owner_schemas and frame_owner_schemas != {
            catalog["owner_schema"]
        }:
            catalog_error(
                "owner schema disagrees with visibility owner records"
            )
        audit = audits.get(frame)
        if audit is None:
            catalog_error(f"missing {AUDIT_EVENT} record")
            catalog_size = 0
        else:
            try:
                catalog_size = _integer(audit, "owners", context)
            except ValueError as exc:
                catalog_error(str(exc))
                catalog_size = 0
        if catalog["owners"] != catalog_size:
            catalog_error("owner count disagrees with visibility audit")

        frame_owner_keys = {
            key for key in owner_records if key[0] == frame
        }
        frame_referenced = {key for key in referenced if key[0] == frame}
        if catalog["emitted_owners"] != len(frame_owner_keys):
            catalog_error("emitted-owner count disagrees with owner records")
        if catalog["eligible_owners"] != len(frame_owner_keys):
            catalog_error("eligible-owner catalog is incomplete or over-complete")
        if catalog["conflict_referenced_owners"] != len(frame_referenced):
            catalog_error("conflict-owner count disagrees with conflict records")
        for field in (
            "owners",
            "eligible_owners",
            "emitted_owners",
            "conflict_referenced_owners",
        ):
            if catalog[field] < 0 or catalog[field] > catalog_size:
                catalog_error(f"{field} is outside the owner catalog")
        for _, token in frame_owner_keys:
            if token <= 0 or token > catalog_size:
                catalog_error(f"owner token {token} is outside the owner catalog")
        if catalog_valid:
            catalog_frames.add(frame)
            catalog_owner_records += len(frame_owner_keys)

    selected = set(referenced)
    selected.update(
        key for key in owner_records if key[0] in catalog_frames
    )
    raw_owner_conflict_join_valid = not errors
    instrumentation = None
    if instrumented_path is not None:
        instrumentation = _attach_instrumented_model_lineage(
            path, instrumented_path, owner_records, selected
        )
        errors.extend(instrumentation["errors"])
        warnings.extend(instrumentation["warnings"])

    for key, (owner, lineno) in owner_records.items():
        frame, token = key
        if key not in selected:
            warnings.append(
                f"{path}:{lineno} frame {frame}: owner {token} is neither "
                "conflict-referenced nor covered by a complete catalog"
            )
            continue
        affine_values = [
            vertex.get("verified_affine") for vertex in owner["vertices"]
        ]
        coherent_affine = (
            affine_values[0]
            if affine_values[0] is not None
            and affine_values[0] == affine_values[1] == affine_values[2]
            else None
        )
        occurrences.append(
            {
                "capture": capture_index,
                "path": str(path),
                "validation_set": validation_set,
                "frame": frame,
                "token": token,
                "catalog_complete": frame in catalog_frames,
                "conflict_referenced": key in referenced,
                "roles": sorted(per_owner_roles[key]),
                "conflict_samples": per_owner_samples[key],
                # Projection IDs are process-local and therefore never enter
                # a persistent signature. Equality inside one frame is still
                # exact causal topology: two submitted triangle vertices can
                # share an ID only when they came from the same GTE projection
                # event. The cross-validation audit below compares only that
                # equality relation, not the numeric IDs.
                "topology_vertices": tuple(
                    (vertex["index"], vertex["projection_id"])
                    for vertex in owner["vertices"]
                ),
                "numeric_affine": coherent_affine,
                "signatures": _signatures(owner),
                "runtime_dispatch_key": _runtime_dispatch_key(owner),
                "transform_value_fingerprint": (
                    unsigned64(owner["vertices"][0]["transform_value"])
                    if owner["owner_schema"] >= 2
                    and owner["coherent_transform_value"] == 1
                    else None
                ),
            }
        )

    occurrence_by_key = {
        (occurrence["frame"], occurrence["token"]): occurrence
        for occurrence in occurrences
    }
    conflict_edges = []
    for frame, frame_conflicts in conflicts.items():
        for conflict_index, conflict in enumerate(frame_conflicts):
            context = f"{path} frame {frame} conflict {conflict_index}"
            try:
                ot_token = _integer(conflict, "ot_owner", context)
                depth_token = _integer(conflict, "depth_owner", context)
                samples = _integer(conflict, "samples", context)
            except ValueError:
                # The validation error was already retained above and by the
                # visibility audit. Never manufacture a partial edge.
                continue
            ot_occurrence = occurrence_by_key.get((frame, ot_token))
            depth_occurrence = occurrence_by_key.get((frame, depth_token))
            if ot_occurrence is None or depth_occurrence is None:
                continue
            conflict_edges.append(
                {
                    "capture": capture_index,
                    "validation_set": validation_set,
                    "frame": frame,
                    "samples": samples,
                    "depth_bounds_bits": (
                        tuple(
                            conflict[field]
                            for field in (
                                "min_ot_depth_bits",
                                "max_ot_depth_bits",
                                "min_strict_depth_bits",
                                "max_strict_depth_bits",
                            )
                        )
                        if all(
                            isinstance(conflict.get(field), int)
                            and 0 <= conflict[field] <= 0xFFFFFFFF
                            for field in (
                                "min_ot_depth_bits",
                                "max_ot_depth_bits",
                                "min_strict_depth_bits",
                                "max_strict_depth_bits",
                            )
                        )
                        else None
                    ),
                    "ot": ot_occurrence,
                    "depth": depth_occurrence,
                }
            )

    return {
        "path": str(path),
        "validation_set": validation_set,
        "audit": summary,
        "owner_records": len(owner_records),
        "referenced_owner_records": len(referenced & set(owner_records)),
        "selected_owner_records": len(selected & set(owner_records)),
        "complete_catalog_frames": len(catalog_frames),
        "complete_catalog_owner_records": catalog_owner_records,
        "raw_conflict_records": sum(len(values) for values in conflicts.values()),
        "raw_conflict_samples": sum(
            int(conflict.get("samples", 0))
            for values in conflicts.values()
            for conflict in values
            if isinstance(conflict.get("samples"), int)
        ),
        "raw_conflicts_by_frame": {
            frame: {
                "records": len(values),
                "samples": sum(
                    int(conflict.get("samples", 0))
                    for conflict in values
                    if isinstance(conflict.get("samples"), int)
                ),
            }
            for frame, values in conflicts.items()
        },
        "instrumented_path": (
            str(instrumented_path) if instrumented_path is not None else None
        ),
        "raw_owner_conflict_join_valid": raw_owner_conflict_join_valid,
        "instrumentation": instrumentation,
        "occurrences": occurrences,
        "conflict_edges": conflict_edges,
        "errors": errors,
        "warnings": warnings,
    }


def _summarize_level(
    occurrences: Iterable[dict[str, Any]], level: str
) -> dict[str, Any]:
    groups: dict[tuple[Any, ...], list[dict[str, Any]]] = collections.defaultdict(list)
    total = 0
    for occurrence in occurrences:
        signature = occurrence["signatures"][level]
        if signature is None:
            continue
        total += 1
        groups[signature].append(occurrence)

    recurrent = 0
    cross_validation = 0
    ambiguous = 0
    cross_validation_unambiguous = 0
    previews = []
    digest_payloads: dict[str, tuple[Any, ...]] = {}
    digest_collisions = []
    for signature, values in groups.items():
        signature_id = _signature_id(signature)
        previous = digest_payloads.setdefault(signature_id, signature)
        if previous != signature:
            digest_collisions.append(signature_id)
        frame_counts = collections.Counter(
            (value["capture"], value["frame"]) for value in values
        )
        sets = {value["validation_set"] for value in values}
        is_recurrent = len(frame_counts) >= 2
        is_cross_validation = sets == {"discovery", "holdout"}
        is_ambiguous = any(count > 1 for count in frame_counts.values())
        recurrent += is_recurrent
        cross_validation += is_cross_validation
        ambiguous += is_ambiguous
        cross_validation_unambiguous += is_cross_validation and not is_ambiguous
        if level == "complete_triangle" and is_cross_validation:
            previews.append(
                {
                    "signature_sha256": signature_id,
                    "occurrences": len(values),
                    "discovery_frames": sorted(
                        {
                            (value["capture"], value["frame"])
                            for value in values
                            if value["validation_set"] == "discovery"
                        }
                    ),
                    "holdout_frames": sorted(
                        {
                            (value["capture"], value["frame"])
                            for value in values
                            if value["validation_set"] == "holdout"
                        }
                    ),
                    "roles": sorted(
                        {role for value in values for role in value["roles"]}
                    ),
                    "conflict_samples": sum(
                        value["conflict_samples"] for value in values
                    ),
                    "ambiguous_within_frame": is_ambiguous,
                    "persistent_object_identity_certified": False,
                }
            )
    previews.sort(key=lambda item: (-item["conflict_samples"], item["signature_sha256"]))
    return {
        "owner_occurrences_with_complete_level": total,
        "distinct_signatures": len(groups),
        "recurrent_signatures": recurrent,
        "cross_validation_signatures": cross_validation,
        "ambiguous_within_frame_signatures": ambiguous,
        "cross_validation_unambiguous_candidates": cross_validation_unambiguous,
        "signature_digest_collisions": sorted(set(digest_collisions)),
        "cross_validation_preview": previews[:32],
    }


def _summarize_conflict_endpoint_coverage(
    captures: Iterable[dict[str, Any]],
) -> dict[str, Any]:
    """Measure which exact evidence exists on both sides of OT/Z conflicts.

    A frame-local affine difference is useful for locating a discontinuity, but
    it is never promoted to a family or persistent identity. In particular, a
    fully identified OT endpoint must not hide an incomplete strict-depth
    endpoint: both are required before a directed conflict can become a later
    post-lift constraint.
    """

    captures = list(captures)
    edges = [edge for capture in captures for edge in capture["conflict_edges"]]
    levels = (
        "transform",
        "geometry",
        "submission_exact",
        "submission_structure",
        "complete_triangle",
    )
    raw_records = sum(capture["raw_conflict_records"] for capture in captures)
    raw_samples = sum(capture["raw_conflict_samples"] for capture in captures)
    joined_samples = sum(edge["samples"] for edge in edges)
    affine_records = 0
    affine_samples = 0
    same_records = 0
    same_samples = 0
    different_records = 0
    different_samples = 0
    affine_validation_sets: set[str] = set()
    affine_frames: set[tuple[int, int]] = set()
    endpoint_levels = {
        level: collections.Counter() for level in levels
    }
    complete_pair_signatures: set[tuple[str, str]] = set()
    complete_pair_validation_sets: dict[tuple[str, str], set[str]] = (
        collections.defaultdict(set)
    )
    frame_coverage: dict[tuple[int, int], dict[str, Any]] = {}
    for capture_index, capture in enumerate(captures):
        for frame, raw in capture["raw_conflicts_by_frame"].items():
            frame_coverage[(capture_index, frame)] = {
                "capture": capture_index,
                "validation_set": capture["validation_set"],
                "frame": frame,
                "raw_records": raw["records"],
                "raw_samples": raw["samples"],
                "joined_records": 0,
                "joined_samples": 0,
                "complete_triangle_both_records": 0,
                "complete_triangle_both_samples": 0,
                "both_numeric_affine_records": 0,
                "both_numeric_affine_samples": 0,
                "same_numeric_affine_records": 0,
                "different_numeric_affine_records": 0,
            }

    for edge in edges:
        ot = edge["ot"]
        depth = edge["depth"]
        samples = edge["samples"]
        frame_stats = frame_coverage[(edge["capture"], edge["frame"])]
        frame_stats["joined_records"] += 1
        frame_stats["joined_samples"] += samples
        for level in levels:
            ot_complete = ot["signatures"][level] is not None
            depth_complete = depth["signatures"][level] is not None
            endpoint_levels[level]["ot_records"] += ot_complete
            endpoint_levels[level]["ot_samples"] += samples if ot_complete else 0
            endpoint_levels[level]["depth_records"] += depth_complete
            endpoint_levels[level]["depth_samples"] += (
                samples if depth_complete else 0
            )
            endpoint_levels[level]["both_records"] += (
                ot_complete and depth_complete
            )
            endpoint_levels[level]["both_samples"] += (
                samples if ot_complete and depth_complete else 0
            )

        ot_complete_signature = ot["signatures"]["complete_triangle"]
        depth_complete_signature = depth["signatures"]["complete_triangle"]
        if ot_complete_signature is not None and depth_complete_signature is not None:
            frame_stats["complete_triangle_both_records"] += 1
            frame_stats["complete_triangle_both_samples"] += samples
            directed_pair = (
                _signature_id(ot_complete_signature),
                _signature_id(depth_complete_signature),
            )
            complete_pair_signatures.add(directed_pair)
            complete_pair_validation_sets[directed_pair].add(
                edge["validation_set"]
            )

        ot_affine = ot.get("numeric_affine")
        depth_affine = depth.get("numeric_affine")
        if ot_affine is None or depth_affine is None:
            continue
        frame_stats["both_numeric_affine_records"] += 1
        frame_stats["both_numeric_affine_samples"] += samples
        affine_records += 1
        affine_samples += samples
        affine_validation_sets.add(edge["validation_set"])
        affine_frames.add((edge["capture"], edge["frame"]))
        if ot_affine == depth_affine:
            frame_stats["same_numeric_affine_records"] += 1
            same_records += 1
            same_samples += samples
        else:
            frame_stats["different_numeric_affine_records"] += 1
            different_records += 1
            different_samples += samples

    level_report = {
        level: dict(sorted(counts.items()))
        for level, counts in endpoint_levels.items()
    }
    recurrent_complete_pairs = sum(
        sets == {"discovery", "holdout"}
        for sets in complete_pair_validation_sets.values()
    )
    frame_reports = []
    ready_frames = []
    for key in sorted(frame_coverage):
        values = frame_coverage[key]
        raw_frame_records = values["raw_records"]
        raw_frame_samples = values["raw_samples"]
        all_joined = (
            values["joined_records"] == raw_frame_records
            and values["joined_samples"] == raw_frame_samples
        )
        all_complete = (
            raw_frame_records > 0
            and values["complete_triangle_both_records"] == raw_frame_records
            and values["complete_triangle_both_samples"] == raw_frame_samples
        )
        all_affine = (
            raw_frame_records > 0
            and values["both_numeric_affine_records"] == raw_frame_records
            and values["both_numeric_affine_samples"] == raw_frame_samples
        )
        frame_report = {
            **values,
            "all_raw_conflicts_have_joined_endpoints": all_joined,
            "all_conflict_endpoints_have_complete_triangle_identity": all_complete,
            "all_conflict_endpoints_have_numeric_affines": all_affine,
            "all_assessed_conflicts_cross_frame_local_affines": (
                values["both_numeric_affine_records"] > 0
                and values["same_numeric_affine_records"] == 0
            ),
            "complete_endpoint_constraint_input_ready": (
                all_joined and all_complete and all_affine
            ),
        }
        frame_reports.append(frame_report)
        if frame_report["complete_endpoint_constraint_input_ready"]:
            ready_frames.append(frame_report)
    ready_validation_sets = {
        frame["validation_set"] for frame in ready_frames
    }
    complete_both = endpoint_levels["complete_triangle"]["both_records"]
    return {
        "raw_directed_conflict_records": raw_records,
        "raw_conflict_samples": raw_samples,
        "joined_endpoint_records": len(edges),
        "joined_endpoint_samples": joined_samples,
        "all_raw_conflicts_have_joined_endpoints": (
            len(edges) == raw_records and joined_samples == raw_samples
        ),
        "both_numeric_affine_records": affine_records,
        "both_numeric_affine_samples": affine_samples,
        "both_numeric_affine_frames": len(affine_frames),
        "both_numeric_affine_seen_in_discovery_and_holdout": (
            affine_validation_sets == {"discovery", "holdout"}
        ),
        "same_frame_local_affine_records": same_records,
        "same_frame_local_affine_samples": same_samples,
        "different_frame_local_affine_records": different_records,
        "different_frame_local_affine_samples": different_samples,
        "all_assessed_conflicts_cross_frame_local_affines": (
            affine_records > 0 and same_records == 0
        ),
        "endpoint_level_coverage": level_report,
        "distinct_directed_complete_triangle_pairs": len(complete_pair_signatures),
        "cross_validation_directed_complete_triangle_pairs": (
            recurrent_complete_pairs
        ),
        "frames_with_raw_conflicts": len(frame_reports),
        "complete_endpoint_constraint_frames": len(ready_frames),
        "complete_endpoint_constraint_records": sum(
            frame["raw_records"] for frame in ready_frames
        ),
        "complete_endpoint_constraint_samples": sum(
            frame["raw_samples"] for frame in ready_frames
        ),
        "complete_endpoint_constraint_seen_in_discovery_and_holdout": (
            ready_validation_sets == {"discovery", "holdout"}
        ),
        "complete_endpoint_constraint_frame_preview": ready_frames[:32],
        "frame_endpoint_coverage": frame_reports,
        "frame_local_constraint_inputs_ready": len(ready_frames) > 0,
        "all_conflict_endpoints_have_complete_triangle_identity": (
            raw_records > 0 and complete_both == raw_records
        ),
        "frame_local_affine_relation_is_family_proof": False,
        "numeric_affines_used_as_persistent_identity": False,
        "post_lift_constraints_ready": False,
    }


def _frame_projection_incidence(
    values: list[dict[str, Any]],
) -> dict[tuple[str, str], tuple[tuple[int, int], ...]]:
    projections: dict[int, list[tuple[str, int]]] = collections.defaultdict(list)
    for occurrence in values:
        signature = occurrence["signatures"]["complete_triangle"]
        if signature is None:
            continue
        signature_id = _signature_id(signature)
        for vertex_index, projection_id in occurrence["topology_vertices"]:
            projections[projection_id].append((signature_id, vertex_index))

    relations: dict[tuple[str, str], set[tuple[int, int]]] = (
        collections.defaultdict(set)
    )
    for projection_uses in projections.values():
        for left_index, (left_signature, left_vertex) in enumerate(projection_uses):
            for right_signature, right_vertex in projection_uses[left_index + 1 :]:
                if left_signature == right_signature:
                    continue
                if left_signature < right_signature:
                    key = (left_signature, right_signature)
                    relation = (left_vertex, right_vertex)
                else:
                    key = (right_signature, left_signature)
                    relation = (right_vertex, left_vertex)
                relations[key].add(relation)
    return {key: tuple(sorted(value)) for key, value in relations.items()}


def _summarize_projection_topology(
    occurrences: list[dict[str, Any]],
) -> dict[str, Any]:
    by_frame: dict[tuple[int, int], list[dict[str, Any]]] = (
        collections.defaultdict(list)
    )
    signature_frames: dict[str, set[tuple[int, int]]] = collections.defaultdict(set)
    signature_sets: dict[str, set[str]] = collections.defaultdict(set)
    ambiguous_signatures: set[str] = set()
    for occurrence in occurrences:
        signature = occurrence["signatures"]["complete_triangle"]
        if signature is None:
            continue
        signature_id = _signature_id(signature)
        frame = (occurrence["capture"], occurrence["frame"])
        by_frame[frame].append(occurrence)
        signature_frames[signature_id].add(frame)
        signature_sets[signature_id].add(occurrence["validation_set"])

    frame_signatures: dict[tuple[int, int], dict[str, dict[str, Any]]] = {}
    frame_relations: dict[
        tuple[int, int], dict[tuple[str, str], tuple[tuple[int, int], ...]]
    ] = {}
    for frame, values in by_frame.items():
        grouped: dict[str, list[dict[str, Any]]] = collections.defaultdict(list)
        for occurrence in values:
            signature = occurrence["signatures"]["complete_triangle"]
            grouped[_signature_id(signature)].append(occurrence)
        ambiguous_signatures.update(
            signature_id for signature_id, group in grouped.items() if len(group) != 1
        )
        unique = {
            signature_id: group[0]
            for signature_id, group in grouped.items()
            if len(group) == 1
        }
        frame_signatures[frame] = unique
        frame_relations[frame] = _frame_projection_incidence(list(unique.values()))

    mapped = sorted(
        signature_id
        for signature_id, sets in signature_sets.items()
        if sets == {"discovery", "holdout"}
        and signature_id not in ambiguous_signatures
    )
    mapped_set = set(mapped)
    mapped_non_conflict = {
        _signature_id(occurrence["signatures"]["complete_triangle"])
        for occurrence in occurrences
        if occurrence["signatures"]["complete_triangle"] is not None
        and not occurrence.get("conflict_referenced", True)
        and _signature_id(occurrence["signatures"]["complete_triangle"])
        in mapped_set
    }
    observed_adjacencies = {
        key
        for relations in frame_relations.values()
        for key in relations
        if key[0] in mapped_set and key[1] in mapped_set
    }
    stable_edges: dict[tuple[str, str], tuple[tuple[int, int], ...]] = {}
    contradictions = []
    unassessed = 0
    for key in sorted(observed_adjacencies):
        left, right = key
        observations: dict[str, set[tuple[tuple[int, int], ...]]] = {
            "discovery": set(),
            "holdout": set(),
        }
        for frame, unique in frame_signatures.items():
            if left not in unique or right not in unique:
                continue
            validation_set = unique[left]["validation_set"]
            observations[validation_set].add(frame_relations[frame].get(key, ()))
        if not observations["discovery"] or not observations["holdout"]:
            unassessed += 1
            continue
        combined = observations["discovery"] | observations["holdout"]
        if (
            len(observations["discovery"]) == 1
            and len(observations["holdout"]) == 1
            and len(combined) == 1
        ):
            relation = next(iter(combined))
            if relation:
                stable_edges[key] = relation
            continue
        contradictions.append(
            {
                "left_triangle_sha256": left,
                "right_triangle_sha256": right,
                "discovery_relations": [
                    list(relation)
                    for relation in sorted(observations["discovery"])
                ],
                "holdout_relations": [
                    list(relation)
                    for relation in sorted(observations["holdout"])
                ],
            }
        )

    parent = {signature_id: signature_id for signature_id in mapped}

    def root(signature_id: str) -> str:
        while parent[signature_id] != signature_id:
            parent[signature_id] = parent[parent[signature_id]]
            signature_id = parent[signature_id]
        return signature_id

    def union(left: str, right: str) -> None:
        left_root = root(left)
        right_root = root(right)
        if left_root != right_root:
            parent[right_root] = left_root

    for left, right in stable_edges:
        union(left, right)
    components: dict[str, list[str]] = collections.defaultdict(list)
    for signature_id in mapped:
        components[root(signature_id)].append(signature_id)

    component_records = []
    component_payloads: dict[str, tuple[Any, ...]] = {}
    component_digest_collisions = []
    component_membership: dict[str, str] = {}
    for nodes in components.values():
        nodes.sort()
        node_set = set(nodes)
        edges = tuple(
            (left, right, relation)
            for (left, right), relation in sorted(stable_edges.items())
            if left in node_set and right in node_set
        )
        payload = ("projection-incidence-component/v1", tuple(nodes), edges)
        digest = _signature_id(payload)
        previous = component_payloads.setdefault(digest, payload)
        if previous != payload:
            component_digest_collisions.append(digest)
        for signature_id in nodes:
            component_membership[signature_id] = digest
        component_records.append(
            {
                "component_sha256": digest,
                "triangle_count": len(nodes),
                "stable_triangle_adjacencies": len(edges),
                "stable_shared_vertex_relations": sum(
                    len(relation) for _, _, relation in edges
                ),
                "triangle_signature_preview": nodes[:8],
                "complete_object_topology": False,
                "persistent_object_identity_certified": False,
            }
        )
    component_records.sort(
        key=lambda item: (-item["triangle_count"], item["component_sha256"])
    )
    reported_components = {
        record["component_sha256"] for record in component_records
    }
    frame_topology_component_membership: dict[
        tuple[int, int], dict[str, str]
    ] = {}
    frame_attachable_occurrences = 0
    frame_anchor_ambiguities = []
    for frame, unique in sorted(frame_signatures.items()):
        candidates: dict[str, set[str]] = collections.defaultdict(set)
        for left, right in frame_relations[frame]:
            left_component = component_membership.get(left)
            right_component = component_membership.get(right)
            if left_component is None and right_component is not None:
                candidates[left].add(right_component)
            if right_component is None and left_component is not None:
                candidates[right].add(left_component)
        resolved = {}
        for signature_id, values in sorted(candidates.items()):
            if len(values) == 1:
                resolved[signature_id] = next(iter(values))
                frame_attachable_occurrences += 1
            else:
                frame_anchor_ambiguities.append(
                    {
                        "capture": frame[0],
                        "frame": frame[1],
                        "triangle_sha256": signature_id,
                        "candidate_components": sorted(values),
                    }
                )
        frame_topology_component_membership[frame] = resolved
    runtime_dispatch = _summarize_runtime_dispatch(
        occurrences,
        component_membership,
        reported_components,
        target="projection-incidence-component/v1",
        basis=(
            "cross-validated complete triangle lineage reproducing stable "
            "partial projection-incidence components"
        ),
    )
    assessed_adjacencies = len(observed_adjacencies) - unassessed
    return {
        "basis": "equality of exact frame-local GTE projection IDs",
        "numeric_projection_ids_used_as_persistent_identity": False,
        "mapped_cross_validation_triangle_signatures": len(mapped),
        "mapped_triangle_signatures_with_non_conflict_occurrence": len(
            mapped_non_conflict
        ),
        "ambiguous_triangle_signatures_excluded": len(ambiguous_signatures),
        "frames_with_complete_triangles": len(by_frame),
        "observed_triangle_adjacencies": len(observed_adjacencies),
        "assessed_observed_adjacencies": assessed_adjacencies,
        "unassessed_observed_adjacencies": unassessed,
        "stable_triangle_adjacencies": len(stable_edges),
        "stable_shared_vertex_relations": sum(
            len(relation) for relation in stable_edges.values()
        ),
        "nonincidence_used_as_separation_evidence": False,
        "contradictory_or_variable_relations": len(contradictions),
        "contradiction_preview": contradictions[:32],
        "stable_partial_components": len(component_records),
        "stable_partial_component_sizes": [
            item["triangle_count"] for item in component_records
        ],
        "component_preview": component_records[:32],
        "component_digest_collisions": sorted(set(component_digest_collisions)),
        "topology_consistent_across_assessed_pairs": not contradictions,
        "non_conflict_geometry_included": bool(mapped_non_conflict),
        "components_cover_all_eligible_catalog_geometry": False,
        # Seeing at least one mapped catalog-only occurrence proves that the
        # analysis is not conflict-gated. It cannot prove coverage of all
        # non-conflicting geometry: ineligible routes and incomplete intrinsic
        # signatures are deliberately absent from the graph.
        "components_cover_non_conflicting_geometry": False,
        "frame_local_projection_anchor_occurrences": (
            frame_attachable_occurrences
        ),
        "frame_local_projection_anchor_ambiguities": len(
            frame_anchor_ambiguities
        ),
        "frame_local_projection_anchor_ambiguity_preview": (
            frame_anchor_ambiguities[:32]
        ),
        "frame_local_projection_anchor_used_as_persistent_identity": False,
        "component_count_is_object_count": False,
        "component_count_is_family_count": False,
        "component_count_compared_to_family_prior": False,
        "persistent_object_identity_certified": False,
        "runtime_dispatch": runtime_dispatch,
        # Consumed internally by finer visibility diagnostics and removed
        # before serialization.  These are partial topology components, not
        # certified objects or presentation families.
        "_triangle_component_membership": component_membership,
        "_frame_topology_component_membership": (
            frame_topology_component_membership
        ),
    }


def _summarize_numeric_affine_cohorts(
    occurrences: list[dict[str, Any]],
) -> dict[str, Any]:
    """Cross-validate exact affine equality without promoting values to IDs.

    The numeric GTE matrix is allowed to change at every semantic checkpoint.
    Only its frame-local equality partition is compared: if two recurrent,
    unambiguous triangles use the same exact 12-component affine in discovery,
    they must retain that relation in holdout before it becomes a stable edge.
    Missing co-occurrence is never interpreted as a different-transform edge.
    """

    by_frame: dict[tuple[int, int], list[dict[str, Any]]] = (
        collections.defaultdict(list)
    )
    signature_sets: dict[str, set[str]] = collections.defaultdict(set)
    affine_signature_sets: dict[str, set[str]] = collections.defaultdict(set)
    ambiguous_signatures: set[str] = set()
    complete_occurrences = 0
    coherent_affine_occurrences = 0
    for occurrence in occurrences:
        signature = occurrence["signatures"]["complete_triangle"]
        if signature is None:
            continue
        complete_occurrences += 1
        signature_id = _signature_id(signature)
        signature_sets[signature_id].add(occurrence["validation_set"])
        frame = (occurrence["capture"], occurrence["frame"])
        by_frame[frame].append(occurrence)
        if occurrence.get("numeric_affine") is not None:
            coherent_affine_occurrences += 1
            affine_signature_sets[signature_id].add(occurrence["validation_set"])

    frame_signatures: dict[tuple[int, int], dict[str, dict[str, Any]]] = {}
    frame_reports = []
    proportional_group_preview = []
    proportional_groups = 0
    proportional_pairs = 0
    zero_affine_transforms = 0
    for frame, values in sorted(by_frame.items()):
        grouped: dict[str, list[dict[str, Any]]] = collections.defaultdict(list)
        for occurrence in values:
            signature = occurrence["signatures"]["complete_triangle"]
            grouped[_signature_id(signature)].append(occurrence)
        ambiguous_signatures.update(
            signature_id
            for signature_id, group in grouped.items()
            if len(group) != 1
        )
        unique = {
            signature_id: group[0]
            for signature_id, group in grouped.items()
            if len(group) == 1 and group[0].get("numeric_affine") is not None
        }
        frame_signatures[frame] = unique

        vectors = sorted(
            {tuple(value["numeric_affine"]) for value in unique.values()}
        )
        affine_partition_sizes = sorted(
            collections.Counter(
                tuple(value["numeric_affine"]) for value in unique.values()
            ).values(),
            reverse=True,
        )
        canonical_groups: dict[tuple[int, ...], list[int]] = (
            collections.defaultdict(list)
        )
        frame_zero = 0
        for vector in vectors:
            try:
                canonical, divisor = canonical_affine(vector)
            except ValueError:
                frame_zero += 1
                continue
            canonical_groups[canonical].append(divisor)
        zero_affine_transforms += frame_zero

        frame_proportional_groups = 0
        frame_proportional_pairs = 0
        for divisors in canonical_groups.values():
            divisors = sorted(set(divisors))
            if len(divisors) < 2:
                continue
            frame_proportional_groups += 1
            frame_proportional_pairs += len(divisors) * (len(divisors) - 1) // 2
            if len(proportional_group_preview) < 32:
                anchor = divisors[0]
                ratios = []
                for divisor in divisors:
                    common = math.gcd(divisor, anchor)
                    numerator = divisor // common
                    denominator = anchor // common
                    ratios.append(
                        {
                            "numerator": numerator,
                            "denominator": denominator,
                            "is_integer_or_tenth": (
                                numerator * 10 % denominator == 0
                            ),
                        }
                    )
                sample = next(iter(unique.values()), None)
                proportional_group_preview.append(
                    {
                        "capture": frame[0],
                        "frame": frame[1],
                        "validation_set": (
                            sample["validation_set"] if sample is not None else None
                        ),
                        "members": len(divisors),
                        "exact_ratios_from_smallest_divisor": ratios,
                    }
                )
        proportional_groups += frame_proportional_groups
        proportional_pairs += frame_proportional_pairs
        sample = next(iter(unique.values()), None)
        frame_reports.append(
            {
                "capture": frame[0],
                "frame": frame[1],
                "validation_set": (
                    sample["validation_set"]
                    if sample is not None
                    else values[0]["validation_set"]
                ),
                "complete_triangle_occurrences": len(values),
                "unambiguous_coherent_affine_occurrences": len(unique),
                "distinct_exact_affines": len(vectors),
                "exact_affine_partition_sizes": affine_partition_sizes,
                "distinct_nonzero_canonical_affines": len(canonical_groups),
                "zero_affine_transforms": frame_zero,
                "exact_full_affine_proportional_groups": (
                    frame_proportional_groups
                ),
            }
        )

    mapped = sorted(
        signature_id
        for signature_id, sets in signature_sets.items()
        if sets == {"discovery", "holdout"}
        and signature_id not in ambiguous_signatures
    )
    mapped_affine = sorted(
        signature_id
        for signature_id in mapped
        if affine_signature_sets[signature_id] == {"discovery", "holdout"}
    )
    mapped_affine_set = set(mapped_affine)

    pair_observations: dict[
        tuple[str, str], dict[str, set[bool]]
    ] = collections.defaultdict(
        lambda: {"discovery": set(), "holdout": set()}
    )
    for unique in frame_signatures.values():
        values = [
            (signature_id, occurrence)
            for signature_id, occurrence in sorted(unique.items())
            if signature_id in mapped_affine_set
        ]
        for left_index, (left_id, left) in enumerate(values):
            for right_id, right in values[left_index + 1 :]:
                validation_set = left["validation_set"]
                pair_observations[(left_id, right_id)][validation_set].add(
                    left["numeric_affine"] == right["numeric_affine"]
                )

    stable_same = set()
    stable_different = set()
    rejected_relations = []
    unassessed = 0
    cross_validation_assessed = 0
    for key, observations in sorted(pair_observations.items()):
        has_discovery = bool(observations["discovery"])
        has_holdout = bool(observations["holdout"])
        if has_discovery and has_holdout:
            cross_validation_assessed += 1
        else:
            unassessed += 1
        combined = observations["discovery"] | observations["holdout"]
        if any(len(values) > 1 for values in observations.values()) or (
            has_discovery and has_holdout and len(combined) > 1
        ):
            rejected_relations.append(
                {
                    "left_triangle_sha256": key[0],
                    "right_triangle_sha256": key[1],
                    "discovery_relations": sorted(
                        "same" if value else "different"
                        for value in observations["discovery"]
                    ),
                    "holdout_relations": sorted(
                        "same" if value else "different"
                        for value in observations["holdout"]
                    ),
                }
            )
            continue
        if not (has_discovery and has_holdout):
            continue
        if next(iter(combined)):
            stable_same.add(key)
        else:
            stable_different.add(key)

    parent = {signature_id: signature_id for signature_id in mapped_affine}

    def root(signature_id: str) -> str:
        while parent[signature_id] != signature_id:
            parent[signature_id] = parent[parent[signature_id]]
            signature_id = parent[signature_id]
        return signature_id

    def union(left: str, right: str) -> None:
        left_root = root(left)
        right_root = root(right)
        if left_root != right_root:
            parent[right_root] = left_root

    for left, right in stable_same:
        union(left, right)

    component_nodes: dict[str, list[str]] = collections.defaultdict(list)
    for signature_id in mapped_affine:
        component_nodes[root(signature_id)].append(signature_id)
    component_records = []
    component_payloads: dict[str, tuple[Any, ...]] = {}
    component_digest_collisions = []
    component_membership: dict[str, str] = {}
    mapped_in_components = 0
    transitive_conflicts = 0
    for nodes in component_nodes.values():
        nodes.sort()
        node_set = set(nodes)
        same_edges = tuple(
            edge
            for edge in sorted(stable_same)
            if edge[0] in node_set and edge[1] in node_set
        )
        different_edges = tuple(
            edge
            for edge in sorted(stable_different)
            if edge[0] in node_set and edge[1] in node_set
        )
        transitive_conflicts += len(different_edges)
        payload = (
            "exact-affine-equality-component/v1",
            tuple(nodes),
            same_edges,
            different_edges,
        )
        digest = _signature_id(payload)
        previous = component_payloads.setdefault(digest, payload)
        if previous != payload:
            component_digest_collisions.append(digest)
        for signature_id in nodes:
            component_membership[signature_id] = digest
        if len(nodes) < 2:
            continue
        mapped_in_components += len(nodes)
        component_records.append(
            {
                "component_sha256": digest,
                "triangle_count": len(nodes),
                "stable_same_affine_relations": len(same_edges),
                "stable_different_relations_inside_component": len(
                    different_edges
                ),
                "triangle_signature_preview": nodes[:8],
                "temporally_consistent": not different_edges,
                "complete_affine_cohort": False,
                "persistent_object_identity_certified": False,
                "presentation_family_certified": False,
            }
        )
    component_records.sort(
        key=lambda item: (-item["triangle_count"], item["component_sha256"])
    )

    frame_affine_component_membership: dict[
        tuple[int, int], dict[tuple[int, ...], str]
    ] = {}
    frame_anchor_occurrences = 0
    frame_attachable_unmapped_occurrences = 0
    frame_anchor_ambiguities = []
    for frame, unique in sorted(frame_signatures.items()):
        affine_components: dict[tuple[int, ...], set[str]] = (
            collections.defaultdict(set)
        )
        component_affines: dict[str, set[tuple[int, ...]]] = (
            collections.defaultdict(set)
        )
        for signature_id, occurrence in unique.items():
            component = component_membership.get(signature_id)
            if component is None:
                continue
            affine = tuple(occurrence["numeric_affine"])
            affine_components[affine].add(component)
            component_affines[component].add(affine)
            frame_anchor_occurrences += 1

        invalid_components = {
            component
            for component, affines in component_affines.items()
            if len(affines) != 1
        }
        resolved = {}
        for affine, components in affine_components.items():
            if len(components) != 1 or components & invalid_components:
                frame_anchor_ambiguities.append(
                    {
                        "capture": frame[0],
                        "frame": frame[1],
                        "candidate_components": len(components),
                        "component_has_multiple_frame_affines": bool(
                            components & invalid_components
                        ),
                    }
                )
                continue
            resolved[affine] = next(iter(components))
        frame_affine_component_membership[frame] = resolved
        for signature_id, occurrence in unique.items():
            if signature_id in component_membership:
                continue
            if tuple(occurrence["numeric_affine"]) in resolved:
                frame_attachable_unmapped_occurrences += 1

    reported_components = {
        record["component_sha256"] for record in component_records
    }
    runtime_dispatch = _summarize_runtime_dispatch(
        occurrences,
        component_membership,
        reported_components,
        target="exact-affine-equality-component/v1",
        basis=(
            "cross-validated complete triangle lineage using compact runtime "
            "Merkle fingerprints audited against the full DAG"
        ),
    )

    return {
        "basis": (
            "frame-local equality of all 12 exact GTE affine components, "
            "cross-validated as a relation rather than a numeric value"
        ),
        "numeric_affine_values_used_as_persistent_identity": False,
        "complete_triangle_occurrences": complete_occurrences,
        "coherent_affine_occurrences": coherent_affine_occurrences,
        "mapped_cross_validation_triangle_signatures": len(mapped),
        "mapped_signatures_with_affine_in_both_validation_sets": len(
            mapped_affine
        ),
        "ambiguous_triangle_signatures_excluded": len(ambiguous_signatures),
        "frames_with_complete_triangles": len(by_frame),
        "frame_partitions": frame_reports,
        "observed_pair_relations": len(pair_observations),
        "cross_validation_assessed_pair_relations": cross_validation_assessed,
        "unassessed_cross_validation_pair_relations": unassessed,
        "stable_same_affine_relations": len(stable_same),
        "stable_different_affine_relations": len(stable_different),
        "rejected_variable_or_contradictory_relations": len(
            rejected_relations
        ),
        "rejected_relation_preview": rejected_relations[:32],
        "missing_relation_used_as_separation_evidence": False,
        "same_relation_components": len(component_records),
        "same_relation_component_sizes": [
            item["triangle_count"] for item in component_records
        ],
        "mapped_signatures_in_same_relation_components": mapped_in_components,
        "mapped_signatures_without_stable_same_relation": (
            len(mapped_affine) - mapped_in_components
        ),
        "component_preview": component_records[:32],
        "component_digest_collisions": sorted(set(component_digest_collisions)),
        "stable_different_relations_inside_same_components": transitive_conflicts,
        "relations_consistent_across_assessed_pairs": (
            not rejected_relations and transitive_conflicts == 0
        ),
        "exact_full_affine_proportional_groups": proportional_groups,
        "exact_full_affine_proportional_pairs": proportional_pairs,
        "zero_affine_transforms": zero_affine_transforms,
        "proportional_group_preview": proportional_group_preview,
        "integer_or_tenth_ratio_used_as_filter": False,
        "component_count_is_object_count": False,
        "component_count_is_family_count": False,
        "component_count_compared_to_family_prior": False,
        "exact_proportionality_is_scale_certification": False,
        "persistent_object_identity_certified": False,
        "presentation_family_certified": False,
        "scale_factor_certified": False,
        "frame_local_anchor_occurrences": frame_anchor_occurrences,
        "frame_local_anchored_affines": sum(
            len(values) for values in frame_affine_component_membership.values()
        ),
        "frame_local_attachable_unmapped_occurrences": (
            frame_attachable_unmapped_occurrences
        ),
        "frame_local_anchor_ambiguities": len(frame_anchor_ambiguities),
        "frame_local_anchor_ambiguity_preview": frame_anchor_ambiguities[:32],
        "frame_local_affine_anchor_used_as_persistent_identity": False,
        "runtime_dispatch": runtime_dispatch,
        # Consumed internally by the visibility-constraint audit and removed
        # before JSON serialization. The digest names a cross-validated
        # equality-relation component, never a certified object or family.
        "_triangle_component_membership": component_membership,
        "_frame_affine_component_membership": (
            frame_affine_component_membership
        ),
    }


def _positive_float_fraction(bits: int) -> Fraction | None:
    value = struct.unpack("<f", struct.pack("<I", bits))[0]
    if not math.isfinite(value) or value <= 0.0:
        return None
    return Fraction(value)


def _fraction_report(value: Fraction) -> dict[str, Any]:
    return {
        "numerator": value.numerator,
        "denominator": value.denominator,
        "decimal_preview": format(float(value), ".12g"),
    }


def _summarize_visibility_gauge_constraints(
    captures: list[dict[str, Any]],
    endpoint_coverage: dict[str, Any],
    triangle_components: dict[str, str],
    frame_affine_components: dict[
        tuple[int, int], dict[tuple[int, ...], str]
    ],
    *,
    component_basis: str = "cross-validated affine-relation components",
    allow_frame_affine_attachment: bool = True,
    frame_signature_components: dict[
        tuple[int, int], dict[str, str]
    ] | None = None,
) -> dict[str, Any]:
    """Build exact necessary gauge inequalities without selecting a factor.

    For a confirmed OT/Z conflict the OT owner is later and its stored D32 W
    is larger than the strict-depth winner. A screen-preserving homogeneous
    multiplier therefore has to satisfy

        farther_multiplier / nearer_multiplier > ot_W / strict_W.

    Per-pair extrema make the reported bound conservative for every accepted
    sample in that record. These inequalities can reject a candidate or expose
    cycles, but they cannot choose a unique gauge and are never identity proof.
    """

    ready_keys = {
        (frame["capture"], frame["frame"])
        for frame in endpoint_coverage["frame_endpoint_coverage"]
        if frame["complete_endpoint_constraint_input_ready"]
    }
    ready_records = endpoint_coverage["complete_endpoint_constraint_records"]
    ready_samples = endpoint_coverage["complete_endpoint_constraint_samples"]
    mapped_records = 0
    mapped_samples = 0
    direct_signature_mapped_endpoints = 0
    frame_anchor_mapped_endpoints = 0
    frame_signature_anchor_mapped_endpoints = 0
    frame_affine_anchor_mapped_endpoints = 0
    unmapped_endpoints = 0
    interval_records = 0
    interval_samples = 0
    non_physical_interval_records = 0
    non_strict_interval_records = 0
    self_component_records = 0
    self_component_samples = 0
    constraints: dict[tuple[str, str], dict[str, Any]] = {}

    for capture in captures:
        for edge in capture["conflict_edges"]:
            if (edge["capture"], edge["frame"]) not in ready_keys:
                continue
            ot_signature = edge["ot"]["signatures"]["complete_triangle"]
            depth_signature = edge["depth"]["signatures"]["complete_triangle"]
            if ot_signature is None or depth_signature is None:
                continue
            frame_affine_map = (
                frame_affine_components.get(
                    (edge["capture"], edge["frame"]), {}
                )
                if allow_frame_affine_attachment
                else {}
            )
            frame_signature_map = (
                frame_signature_components.get(
                    (edge["capture"], edge["frame"]), {}
                )
                if frame_signature_components is not None
                else {}
            )

            def resolve_component(endpoint: dict[str, Any], signature: Any) -> str | None:
                nonlocal direct_signature_mapped_endpoints
                nonlocal frame_anchor_mapped_endpoints
                nonlocal frame_signature_anchor_mapped_endpoints
                nonlocal frame_affine_anchor_mapped_endpoints
                nonlocal unmapped_endpoints
                signature_id = _signature_id(signature)
                component = triangle_components.get(signature_id)
                if component is not None:
                    direct_signature_mapped_endpoints += 1
                    return component
                component = frame_signature_map.get(signature_id)
                if component is not None:
                    frame_anchor_mapped_endpoints += 1
                    frame_signature_anchor_mapped_endpoints += 1
                    return component
                affine = endpoint.get("numeric_affine")
                component = (
                    frame_affine_map.get(tuple(affine))
                    if affine is not None
                    else None
                )
                if component is not None:
                    frame_anchor_mapped_endpoints += 1
                    frame_affine_anchor_mapped_endpoints += 1
                    return component
                unmapped_endpoints += 1
                return None

            ot_component = resolve_component(edge["ot"], ot_signature)
            depth_component = resolve_component(edge["depth"], depth_signature)
            if ot_component is None or depth_component is None:
                continue
            mapped_records += 1
            mapped_samples += edge["samples"]

            bounds = edge.get("depth_bounds_bits")
            if bounds is None:
                continue
            _, max_ot_bits, min_strict_bits, _ = bounds
            maximum_ot = _positive_float_fraction(max_ot_bits)
            minimum_strict = _positive_float_fraction(min_strict_bits)
            if maximum_ot is None or minimum_strict is None:
                non_physical_interval_records += 1
                continue
            ratio = maximum_ot / minimum_strict
            interval_records += 1
            interval_samples += edge["samples"]
            if ratio <= 1:
                non_strict_interval_records += 1
                continue
            if ot_component == depth_component:
                self_component_records += 1
                self_component_samples += edge["samples"]
                continue

            key = (ot_component, depth_component)
            constraint = constraints.setdefault(
                key,
                {
                    "records": 0,
                    "samples": 0,
                    "frames": set(),
                    "validation_sets": set(),
                    "maximum_ratio": Fraction(0, 1),
                    "validation_maximum_ratio": {},
                },
            )
            constraint["records"] += 1
            constraint["samples"] += edge["samples"]
            constraint["frames"].add((edge["capture"], edge["frame"]))
            validation_set = edge["validation_set"]
            constraint["validation_sets"].add(validation_set)
            constraint["maximum_ratio"] = max(
                constraint["maximum_ratio"], ratio
            )
            previous = constraint["validation_maximum_ratio"].get(
                validation_set, Fraction(0, 1)
            )
            constraint["validation_maximum_ratio"][validation_set] = max(
                previous, ratio
            )

    constraint_reports = []
    adjacency: dict[str, set[str]] = collections.defaultdict(set)
    all_cross_validated = bool(constraints)
    for (nearer, farther), constraint in sorted(constraints.items()):
        adjacency[nearer].add(farther)
        validation_sets = constraint["validation_sets"]
        cross_validated = validation_sets == {"discovery", "holdout"}
        all_cross_validated &= cross_validated
        constraint_reports.append(
            {
                "nearer_ot_component_sha256": nearer,
                "farther_depth_component_sha256": farther,
                "records": constraint["records"],
                "samples": constraint["samples"],
                "frames": [
                    {"capture": capture, "frame": frame}
                    for capture, frame in sorted(constraint["frames"])
                ],
                "validation_sets": sorted(validation_sets),
                "cross_validated": cross_validated,
                "required_relation": (
                    "farther_multiplier / nearer_multiplier > lower_bound"
                ),
                "strict_lower_bound": _fraction_report(
                    constraint["maximum_ratio"]
                ),
                "discovery_strict_lower_bound": (
                    _fraction_report(
                        constraint["validation_maximum_ratio"]["discovery"]
                    )
                    if "discovery" in constraint["validation_maximum_ratio"]
                    else None
                ),
                "holdout_strict_lower_bound": (
                    _fraction_report(
                        constraint["validation_maximum_ratio"]["holdout"]
                    )
                    if "holdout" in constraint["validation_maximum_ratio"]
                    else None
                ),
                "gauge_certified": False,
            }
        )

    visit_state: dict[str, int] = {}
    visit_stack: list[str] = []
    cycle_preview: list[list[str]] = []

    def visit(node: str) -> None:
        visit_state[node] = 1
        visit_stack.append(node)
        for target in sorted(adjacency.get(node, ())):
            state = visit_state.get(target, 0)
            if state == 0:
                visit(target)
            elif state == 1 and len(cycle_preview) < 32:
                start = visit_stack.index(target)
                cycle_preview.append(visit_stack[start:] + [target])
        visit_stack.pop()
        visit_state[node] = 2

    graph_nodes = sorted(
        set(adjacency)
        | {node for values in adjacency.values() for node in values}
    )
    for node in graph_nodes:
        if visit_state.get(node, 0) == 0:
            visit(node)

    complete_for_ready_frames = (
        ready_records > 0
        and mapped_records == ready_records
        and mapped_samples == ready_samples
        and interval_records == ready_records
        and interval_samples == ready_samples
        and non_physical_interval_records == 0
        and non_strict_interval_records == 0
        and self_component_records == 0
    )
    graph_acyclic = not cycle_preview and self_component_records == 0
    return {
        "basis": (
            "confirmed per-sample OT order plus conservative exact IEEE-754 "
            f"D32 W bounds between {component_basis}"
        ),
        "ready_frame_records": ready_records,
        "ready_frame_samples": ready_samples,
        "stable_component_mapped_records": mapped_records,
        "stable_component_mapped_samples": mapped_samples,
        "direct_signature_mapped_endpoints": direct_signature_mapped_endpoints,
        "frame_anchor_mapped_endpoints": frame_anchor_mapped_endpoints,
        "frame_signature_anchor_mapped_endpoints": (
            frame_signature_anchor_mapped_endpoints
        ),
        "frame_affine_anchor_mapped_endpoints": (
            frame_affine_anchor_mapped_endpoints
        ),
        "unmapped_endpoints": unmapped_endpoints,
        "depth_interval_records": interval_records,
        "depth_interval_samples": interval_samples,
        "non_physical_depth_interval_records": non_physical_interval_records,
        "non_strict_depth_interval_records": non_strict_interval_records,
        "self_component_conflict_records": self_component_records,
        "self_component_conflict_samples": self_component_samples,
        "constraint_components": len(graph_nodes),
        "directed_component_constraints": len(constraint_reports),
        "constraints": constraint_reports,
        "constraint_preview": constraint_reports[:32],
        "all_component_constraints_cross_validated": all_cross_validated,
        "constraint_cycle_count": len(cycle_preview),
        "constraint_cycle_preview": cycle_preview,
        "constraint_graph_acyclic": graph_acyclic,
        "constraint_system_complete_for_ready_frames": complete_for_ready_frames,
        "constraint_system_ready_for_candidate_testing": (
            complete_for_ready_frames and graph_acyclic and all_cross_validated
        ),
        "screen_position_used_for_identity": False,
        "observed_depth_used_for_identity": False,
        "observed_depth_used_for_constraint_bounds": interval_records > 0,
        "numeric_affine_used_only_for_frame_local_anchor_attachment": (
            allow_frame_affine_attachment
        ),
        "numeric_affine_used_as_persistent_identity": False,
        "visibility_constraints_certify_gauge": False,
        "gauge_solution_unique": False,
        "integer_or_tenth_prior_used_to_select_factor": False,
        "family_count_prior_used_to_limit_components": False,
        "scale_factor_certified": False,
    }


def analyze_visibility_lineages(
    discovery: Iterable[Path],
    holdout: Iterable[Path],
    instrumented: dict[Path, Path] | None = None,
) -> dict[str, Any]:
    discovery_paths = list(discovery)
    holdout_paths = list(holdout)
    if not discovery_paths or not holdout_paths:
        raise ValueError("at least one discovery and one holdout trace are required")
    normalized_discovery = {path.resolve() for path in discovery_paths}
    normalized_holdout = {path.resolve() for path in holdout_paths}
    overlap = normalized_discovery & normalized_holdout
    if overlap:
        raise ValueError(
            "discovery and holdout inputs must be disjoint: "
            + ", ".join(str(path) for path in sorted(overlap))
        )

    instrumented_by_light = {
        light.resolve(): heavy for light, heavy in (instrumented or {}).items()
    }
    all_inputs = normalized_discovery | normalized_holdout
    unknown_instrumented = set(instrumented_by_light) - all_inputs
    if unknown_instrumented:
        raise ValueError(
            "instrumented traces reference unknown light inputs: "
            + ", ".join(str(path) for path in sorted(unknown_instrumented))
        )

    captures = []
    for index, path in enumerate(discovery_paths + holdout_paths):
        validation_set = "discovery" if index < len(discovery_paths) else "holdout"
        captures.append(
            _load_capture(
                path,
                validation_set,
                index,
                instrumented_by_light.get(path.resolve()),
            )
        )

    occurrences = [
        occurrence
        for capture in captures
        for occurrence in capture["occurrences"]
    ]
    levels = {
        level: _summarize_level(occurrences, level)
        for level in (
            "transform",
            "geometry",
            "submission_exact",
            "submission_structure",
            "complete_triangle",
        )
    }
    conflict_endpoint_coverage = _summarize_conflict_endpoint_coverage(captures)
    projection_topology = _summarize_projection_topology(occurrences)
    projection_components = projection_topology.pop(
        "_triangle_component_membership"
    )
    frame_projection_components = projection_topology.pop(
        "_frame_topology_component_membership"
    )
    numeric_affine_cohorts = _summarize_numeric_affine_cohorts(occurrences)
    triangle_components = numeric_affine_cohorts.pop(
        "_triangle_component_membership"
    )
    frame_affine_components = numeric_affine_cohorts.pop(
        "_frame_affine_component_membership"
    )
    visibility_gauge_constraints = _summarize_visibility_gauge_constraints(
        captures,
        conflict_endpoint_coverage,
        triangle_components,
        frame_affine_components,
    )
    projection_topology_visibility_constraints = (
        _summarize_visibility_gauge_constraints(
            captures,
            conflict_endpoint_coverage,
            projection_components,
            {},
            component_basis=(
                "cross-validated stable partial projection-incidence components"
            ),
            allow_frame_affine_attachment=False,
            frame_signature_components=frame_projection_components,
        )
    )
    errors = [error for capture in captures for error in capture["errors"]]
    warnings = [warning for capture in captures for warning in capture["warnings"]]
    digest_collisions = [
        collision
        for report in levels.values()
        for collision in report["signature_digest_collisions"]
    ]
    if digest_collisions:
        errors.append("SHA-256 report-label collision over distinct raw signatures")
    if projection_topology["component_digest_collisions"]:
        errors.append("SHA-256 topology-component label collision")
    if numeric_affine_cohorts["component_digest_collisions"]:
        errors.append("SHA-256 affine-component label collision")

    model_payloads: dict[str, dict[str, str]] = {"source": {}, "program": {}}
    for capture in captures:
        instrumentation_report = capture["instrumentation"]
        if instrumentation_report is None:
            continue
        for kind in ("source", "program"):
            for digest, payload in instrumentation_report["payloads"][kind].items():
                previous = model_payloads[kind].setdefault(digest, payload)
                if previous != payload:
                    errors.append(
                        f"cross-capture {kind} SHA-256 model payload collision"
                    )

    complete_candidates = levels["complete_triangle"][
        "cross_validation_unambiguous_candidates"
    ]
    conflict_referenced_occurrences = sum(
        occurrence.get("conflict_referenced", True)
        for occurrence in occurrences
    )
    return {
        "schema": REPORT_SCHEMA,
        "valid": not errors,
        "captures": [
            {
                "path": capture["path"],
                "validation_set": capture["validation_set"],
                "audit_valid": capture["audit"]["valid"],
                "audited_frames": capture["audit"]["audited_frames"],
                "conflict_frames": capture["audit"]["conflict_frames"],
                "owner_records": capture["owner_records"],
                "referenced_owner_records": capture["referenced_owner_records"],
                "selected_owner_records": capture["selected_owner_records"],
                "complete_catalog_frames": capture["complete_catalog_frames"],
                "complete_catalog_owner_records": capture[
                    "complete_catalog_owner_records"
                ],
                "raw_conflict_records": capture["raw_conflict_records"],
                "raw_conflict_samples": capture["raw_conflict_samples"],
                "joined_conflict_endpoint_records": len(capture["conflict_edges"]),
                "instrumented_path": capture["instrumented_path"],
                "instrumented_stream_equal": (
                    capture["instrumentation"]["equivalence"]["stream_equal"]
                    if capture["instrumentation"] is not None
                    else None
                ),
                "instrumented_all_projections_complete": (
                    capture["instrumentation"]["equivalence"][
                        "instrumented_value_flow_complete"
                    ]
                    if capture["instrumentation"] is not None
                    else None
                ),
                "projection_window_owner_vertices": (
                    capture["instrumentation"]["projection_window_vertices"]
                    if capture["instrumentation"] is not None
                    else 0
                ),
                "verified_model_flow_owner_vertices": (
                    capture["instrumentation"]["verified_model_flow_vertices"]
                    if capture["instrumentation"] is not None
                    else 0
                ),
                "verified_model_flow_owners": (
                    capture["instrumentation"]["verified_model_flow_owners"]
                    if capture["instrumentation"] is not None
                    else 0
                ),
                "verified_affine_owner_vertices": (
                    capture["instrumentation"].get("verified_affine_vertices", 0)
                    if capture["instrumentation"] is not None
                    else 0
                ),
                "verified_affine_owners": (
                    capture["instrumentation"].get("verified_affine_owners", 0)
                    if capture["instrumentation"] is not None
                    else 0
                ),
                "verified_model_flow_projections": (
                    capture["instrumentation"].get(
                        "verified_model_flow_projections", 0
                    )
                    if capture["instrumentation"] is not None
                    else 0
                ),
                "incomplete_model_projections": (
                    capture["instrumentation"].get(
                        "incomplete_model_projections", 0
                    )
                    if capture["instrumentation"] is not None
                    else 0
                ),
            }
            for capture in captures
        ],
        "owner_occurrences": len(occurrences),
        "conflict_referenced_owner_occurrences": conflict_referenced_occurrences,
        "catalog_only_owner_occurrences": (
            len(occurrences) - conflict_referenced_occurrences
        ),
        "levels": levels,
        "conflict_endpoint_coverage": conflict_endpoint_coverage,
        "visibility_gauge_constraints": visibility_gauge_constraints,
        "projection_topology_visibility_constraints": (
            projection_topology_visibility_constraints
        ),
        "projection_incidence_topology": projection_topology,
        "numeric_affine_cohorts": numeric_affine_cohorts,
        "advisory_priors": {
            "likely_maximum_family_count": 4,
            "family_prior_certainty": "uncertain",
            "complete_cross_validation_triangle_candidates": complete_candidates,
            "triangle_candidates_are_family_count": False,
            "triangle_candidate_count_compared_to_family_prior": False,
            "topology_component_count_compared_to_family_prior": False,
            "affine_component_count_compared_to_family_prior": False,
            "family_count_assessed": False,
            "family_count_is_correctness_limit": False,
            "integer_or_tenth_scale_is_correctness_limit": False,
            "integer_or_tenth_scale_used_as_filter": False,
            "priors_used_to_select_or_reject_evidence": False,
            "scale_was_analyzed": False,
        },
        "inference_scope": {
            "raw_owner_conflict_join_validated": all(
                capture["raw_owner_conflict_join_valid"] for capture in captures
            ),
            "complete_eligible_owner_catalog_measured": any(
                capture["complete_catalog_frames"] > 0 for capture in captures
            ),
            "all_audited_frames_have_complete_eligible_owner_catalog": all(
                capture["complete_catalog_frames"]
                == capture["audit"]["audited_frames"]
                for capture in captures
            ),
            "categorical_recurrence_measured": True,
            "projection_incidence_topology_measured": True,
            "numeric_affine_equality_relations_measured": (
                numeric_affine_cohorts["coherent_affine_occurrences"] > 0
            ),
            "conflict_endpoint_completeness_measured": True,
            "visibility_gauge_constraints_measured": (
                visibility_gauge_constraints["depth_interval_records"] > 0
            ),
            "projection_topology_visibility_constraints_measured": (
                projection_topology_visibility_constraints[
                    "depth_interval_records"
                ]
                > 0
            ),
            "constraint_system_ready_for_candidate_testing": (
                visibility_gauge_constraints[
                    "constraint_system_ready_for_candidate_testing"
                ]
            ),
            "all_conflict_endpoints_have_complete_triangle_identity": (
                conflict_endpoint_coverage[
                    "all_conflict_endpoints_have_complete_triangle_identity"
                ]
            ),
            "exact_full_affine_proportionality_measured": (
                numeric_affine_cohorts["coherent_affine_occurrences"] > 0
            ),
            "complete_object_topology_inferred": False,
            "screen_position_used_for_identity": False,
            "colour_used_for_identity": False,
            "observed_depth_used_for_identity": False,
            "family_count_inferred": False,
            "scale_factor_inferred": False,
            "persistent_object_identity_inferred": False,
            "remaining_identity_risks": [
                "transform lineage is presently a 64-bit runtime digest",
                "unpaired captures retain only a 64-bit computed-model digest pair",
                "guest model/GP0/DMA addresses may name reused staging slots",
                "categorical recurrence alone does not prove semantic lifetime",
                "captures without a complete eligible-owner catalog can fragment "
                "a larger connected mesh",
                "complete eligible-owner catalogs still exclude ineligible routes "
                "and owners without a complete intrinsic signature",
            ],
        },
        "errors": errors,
        "warnings": warnings,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--discovery", type=Path, action="append", required=True)
    parser.add_argument("--holdout", type=Path, action="append", required=True)
    parser.add_argument(
        "--instrumented",
        type=Path,
        nargs=2,
        action="append",
        metavar=("LIGHT_TRACE", "HEAVY_TRACE"),
        help=(
            "pair a light visibility trace with an equivalent MIPS value-flow "
            "replay; may be repeated"
        ),
    )
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        instrumented = {}
        for light, heavy in args.instrumented or []:
            if light in instrumented:
                raise ValueError(f"duplicate instrumented pairing for {light}")
            instrumented[light] = heavy
        report = analyze_visibility_lineages(
            args.discovery, args.holdout, instrumented
        )
    except (OSError, ValueError) as exc:
        print(f"visibility_lineage_trace: {exc}", file=sys.stderr)
        return 2
    payload = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(payload, encoding="utf-8")
    else:
        print(payload, end="")
    return 0 if report["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
