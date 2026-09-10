#!/usr/bin/env python3
"""Build an explicitly partial presentation-depth preview manifest.

This tool is intentionally separate from visibility_runtime_manifest.py.  The
stable generator accepts only a complete caller-authored gauge candidate; this
preview instead derives a minimal positive-integer order embedding from edges
which recur in both discovery and holdout.  It omits unresolved/context-only
runtime identities and labels the result as non-certified WIP evidence.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any


AUDIT_SCHEMA = "ps1-visibility-wip-preview-manifest-audit/v1"
MANIFEST_SCHEMA = "ps1-visibility-runtime-manifest/v1"
DISPATCH_REPORT_SCHEMA = "ps1-visibility-runtime-dispatch-refinement/v1"
DISPATCH_SCHEMA = "ps1-visibility-runtime-dispatch-map/v3"
CONTEXT_REPORT_SCHEMA = "ps1-visibility-runtime-context-refinement/v1"
CONTEXT_SCHEMA = "ps1-visibility-runtime-context-dispatch-map/v1"
CONSTRAINT_SCHEMA = "ps1-visibility-fragment-stack-constraints/v1"
HEX64 = re.compile(r"[0-9a-f]{16}")
SHA256 = re.compile(r"[0-9a-f]{64}")


def _exact_bool(value: Any, context: str) -> bool:
    if not isinstance(value, bool):
        raise ValueError(f"{context}: expected a boolean")
    return value


def _plain_int(value: Any, minimum: int, maximum: int, context: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{context}: expected an integer")
    if value < minimum or value > maximum:
        raise ValueError(f"{context}: outside {minimum}..{maximum}")
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


def _require_flags(record: dict[str, Any], checks: tuple[tuple[str, bool], ...], context: str) -> None:
    for field, expected in checks:
        actual = _exact_bool(record.get(field), f"{context} {field}")
        if actual != expected:
            raise ValueError(f"{context} {field} is {actual}, expected {expected}")


def _strict_bound(constraint: Any, index: int) -> tuple[str, str, int, int]:
    context = f"constraint {index}"
    if not isinstance(constraint, dict):
        raise ValueError(f"{context}: expected an object")
    nearer = _hex(
        constraint.get("nearer_ot_component_sha256"), SHA256, f"{context} nearer"
    )
    farther = _hex(
        constraint.get("farther_depth_component_sha256"),
        SHA256,
        f"{context} farther",
    )
    if nearer == farther:
        raise ValueError(f"{context}: self relation")
    bound = constraint.get("conservative_lower_bound")
    if not isinstance(bound, dict):
        raise ValueError(f"{context}: missing conservative lower bound")
    numerator = _plain_int(bound.get("numerator"), 1, 2**63 - 1, f"{context} numerator")
    denominator = _plain_int(
        bound.get("denominator"), 1, 2**63 - 1, f"{context} denominator"
    )
    return nearer, farther, numerator, denominator


def build_wip_preview_manifest(
    dispatch_report: Any, context_report: Any, constraint_report: Any
) -> tuple[dict[str, Any], str | None]:
    audit: dict[str, Any] = {
        "schema": AUDIT_SCHEMA,
        "input_valid": False,
        "manifest_ready": False,
        "errors": [],
        "proof_scope": {
            "presentation_preview_only": True,
            "gauge_certified": False,
            "persistent_object_identity_certified": False,
            "cross_validated_constraints_only": True,
            "observed_depth_used_only_for_strict_order_bounds": True,
            "family_count_prior_used": False,
            "integer_or_tenth_physical_prior_used": False,
            "integer_values_are_preview_normalization_not_physical_scale": True,
            "frame_local_context_entries_omitted": True,
            "unknown_runtime_keys_retain_hybrid": True,
            "independent_raster_holdout_still_required": True,
        },
    }
    try:
        if not isinstance(dispatch_report, dict):
            raise ValueError("dispatch refinement: expected an object")
        if dispatch_report.get("schema") != DISPATCH_REPORT_SCHEMA:
            raise ValueError("dispatch refinement: unsupported schema")
        if not _exact_bool(dispatch_report.get("valid"), "dispatch refinement valid"):
            raise ValueError("dispatch refinement is invalid")
        dispatch = dispatch_report.get("runtime_dispatch")
        if not isinstance(dispatch, dict) or dispatch.get("schema") != DISPATCH_SCHEMA:
            raise ValueError("dispatch refinement has no v3 runtime map")
        _require_flags(
            dispatch,
            (
                ("ready_for_runtime_lookup", True),
                ("compact_model_fingerprints_cross_audited_against_full_dag", True),
                ("uses_screen_position", False),
                ("uses_colour", False),
                ("uses_observed_depth", False),
                ("uses_numeric_transform_values", False),
                ("uses_family_count_prior", False),
                ("uses_integer_or_tenth_prior", False),
            ),
            "runtime dispatch",
        )
        if _plain_int(
            dispatch.get("compact_model_fingerprint_schema"),
            0,
            2**31 - 1,
            "runtime fingerprint schema",
        ) != 1:
            raise ValueError("runtime fingerprint schema is not 1")

        if not isinstance(context_report, dict):
            raise ValueError("context refinement: expected an object")
        if context_report.get("schema") != CONTEXT_REPORT_SCHEMA:
            raise ValueError("context refinement: unsupported schema")
        if not _exact_bool(context_report.get("valid"), "context refinement valid"):
            raise ValueError("context refinement is invalid")
        context_dispatch = context_report.get("runtime_context_dispatch")
        if not isinstance(context_dispatch, dict) or context_dispatch.get("schema") != CONTEXT_SCHEMA:
            raise ValueError("context refinement has no runtime context map")
        _require_flags(
            context_dispatch,
            (
                ("ready_for_frame_local_runtime_lookup", True),
                ("requires_exact_static_v3_anchor", True),
                ("uses_screen_position", False),
                ("uses_colour", False),
                ("uses_observed_depth", False),
                ("uses_numeric_transform_values", False),
                ("uses_family_count_prior", False),
                ("uses_integer_or_tenth_prior", False),
            ),
            "runtime context dispatch",
        )

        if not isinstance(constraint_report, dict):
            raise ValueError("constraint report: expected an object")
        if constraint_report.get("schema") != CONSTRAINT_SCHEMA:
            raise ValueError("constraint report: unsupported schema")
        if not _exact_bool(constraint_report.get("valid"), "constraint report valid"):
            raise ValueError("constraint report is invalid")
        graph = constraint_report.get("constraint_graph")
        if not isinstance(graph, dict):
            raise ValueError("constraint report has no graph")
        if not _exact_bool(graph.get("constraint_graph_acyclic"), "constraint graph acyclic"):
            raise ValueError("source constraint graph is cyclic")
        if _plain_int(
            graph.get("self_component_conflict_records"),
            0,
            2**63 - 1,
            "self-component conflicts",
        ) != 0:
            raise ValueError("source graph contains self-component conflicts")
        constraints = graph.get("constraints")
        if not isinstance(constraints, list):
            raise ValueError("constraint graph constraints must be a list")
        cross_validated = []
        holdout_only = 0
        for index, constraint in enumerate(constraints):
            if not isinstance(constraint, dict):
                raise ValueError(f"constraint {index}: expected an object")
            if _exact_bool(constraint.get("cross_validated"), f"constraint {index} cross-validated"):
                cross_validated.append(_strict_bound(constraint, index))
            else:
                holdout_only += 1
        if not cross_validated:
            raise ValueError("no cross-validated constraints")
        reported_cross = _plain_int(
            graph.get("cross_validated_constraints"),
            0,
            2**31 - 1,
            "reported cross-validated constraints",
        )
        if reported_cross != len(cross_validated):
            raise ValueError("cross-validated constraint count mismatch")

        nodes = {value for edge in cross_validated for value in edge[:2]}
        adjacency: dict[str, set[str]] = defaultdict(set)
        indegree = {node: 0 for node in nodes}
        outgoing: dict[str, list[tuple[str, int, int]]] = defaultdict(list)
        for nearer, farther, numerator, denominator in cross_validated:
            outgoing[nearer].append((farther, numerator, denominator))
            if farther not in adjacency[nearer]:
                adjacency[nearer].add(farther)
                indegree[farther] += 1
        ready = sorted(node for node, degree in indegree.items() if degree == 0)
        topological: list[str] = []
        while ready:
            node = ready.pop(0)
            topological.append(node)
            for farther in sorted(adjacency[node]):
                indegree[farther] -= 1
                if indegree[farther] == 0:
                    ready.append(farther)
                    ready.sort()
        if len(topological) != len(nodes):
            raise ValueError("cross-validated subset is cyclic")

        multipliers = {node: 1 for node in nodes}
        for nearer in topological:
            for farther, numerator, denominator in outgoing[nearer]:
                required = (numerator * multipliers[nearer]) // denominator + 1
                multipliers[farther] = max(multipliers[farther], required)
        for nearer, farther, numerator, denominator in cross_validated:
            if multipliers[farther] * denominator <= multipliers[nearer] * numerator:
                raise ValueError("derived preview does not strictly satisfy every edge")

        projection_values = context_dispatch.get("component_projection")
        if not isinstance(projection_values, list):
            raise ValueError("context component projection must be a list")
        projection: dict[str, str] = {}
        for index, value in enumerate(projection_values):
            if not isinstance(value, dict):
                raise ValueError(f"component projection {index}: expected an object")
            static = _hex(value.get("static_component_sha256"), SHA256, f"projection {index} static")
            target = _hex(value.get("target_component_sha256"), SHA256, f"projection {index} target")
            if static in projection:
                raise ValueError("duplicate static component projection")
            projection[static] = target

        entries_value = dispatch.get("entries")
        if not isinstance(entries_value, list):
            raise ValueError("runtime dispatch entries must be a list")
        expected_entries = _plain_int(
            dispatch.get("entry_count"), 0, 2**31 - 1, "runtime entry count"
        )
        if expected_entries != len(entries_value):
            raise ValueError("runtime dispatch entry count mismatch")
        canonical_entries = [
            _canonical_dispatch_entry(entry, index)
            for index, entry in enumerate(entries_value)
        ]
        runtime_keys = {(entry[1], entry[2], entry[3]) for entry in canonical_entries}
        if len(runtime_keys) != len(canonical_entries):
            raise ValueError("runtime dispatch contains duplicate keys")
        missing_projection = sorted(
            {entry[0] for entry in canonical_entries if entry[0] not in projection}
        )
        if missing_projection:
            raise ValueError("runtime components are missing from context projection")

        selected = [
            (projection[entry[0]], entry[1], entry[2], entry[3])
            for entry in canonical_entries
            if projection[entry[0]] in nodes
        ]
        selected.sort()
        referenced = {entry[0] for entry in selected}
        missing_runtime_nodes = sorted(nodes - referenced)
        if missing_runtime_nodes:
            raise ValueError("cross-validated components lack a static runtime entry")

        lines = [
            f"schema {MANIFEST_SCHEMA}",
            "fingerprint_schema 1",
            "scope hypothesis",
            "family_count_prior_used 0",
            "integer_or_tenth_prior_used 0",
        ]
        for component in sorted(nodes):
            lines.append(f"component {component} {multipliers[component]} 1")
        for component, transform, triangle, vertices in selected:
            fields = ["entry", component, transform, str(triangle)]
            for index_value, source_present, source, flow_source, flow_program, offset in vertices:
                fields.extend(
                    (
                        str(index_value),
                        "1" if source_present else "0",
                        source,
                        flow_source,
                        flow_program,
                        str(offset),
                    )
                )
            lines.append(" ".join(fields))
        lines.append(f"end {len(nodes)} {len(selected)}")
        manifest = "\n".join(lines) + "\n"

        audit.update(
            {
                "input_valid": True,
                "manifest_ready": True,
                "algorithm": {
                    "name": "topological-minimal-positive-integer-order-embedding",
                    "rule": "far=max(far,floor(bound*near)+1)",
                    "source_nodes_start_at": 1,
                },
                "coverage": {
                    "cross_validated_constraints_used": len(cross_validated),
                    "single_validation_set_constraints_omitted": holdout_only,
                    "participating_target_components": len(nodes),
                    "static_runtime_entries_total": len(canonical_entries),
                    "static_runtime_entries_selected": len(selected),
                    "static_runtime_entries_fallback": len(canonical_entries) - len(selected),
                    "frame_local_context_entries_omitted": _plain_int(
                        context_dispatch.get("entry_count"),
                        0,
                        2**31 - 1,
                        "context entry count",
                    ),
                },
                "presentation_multipliers": [
                    {
                        "component_sha256": component,
                        "numerator": multipliers[component],
                        "denominator": 1,
                    }
                    for component in sorted(nodes)
                ],
                "manifest": {
                    "schema": MANIFEST_SCHEMA,
                    "components": len(nodes),
                    "entries": len(selected),
                    "sha256": hashlib.sha256(manifest.encode("utf-8")).hexdigest(),
                },
            }
        )
        return audit, manifest
    except ValueError as error:
        audit["errors"].append(str(error))
        return audit, None


def _load(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _source_file_record(path: Path) -> dict[str, Any]:
    record: dict[str, Any] = {"path": str(path), "sha256": None}
    try:
        record["sha256"] = _sha256(path)
    except OSError:
        # The main diagnostic already records the actionable load error. Keep
        # the audit writable and explicit rather than masking it with a second
        # exception while collecting provenance.
        pass
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dispatch_refinement", type=Path)
    parser.add_argument("context_refinement", type=Path)
    parser.add_argument("constraints", type=Path)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--audit", type=Path, required=True)
    args = parser.parse_args()

    try:
        reports = (
            _load(args.dispatch_refinement),
            _load(args.context_refinement),
            _load(args.constraints),
        )
        audit, manifest = build_wip_preview_manifest(*reports)
    except (OSError, json.JSONDecodeError) as error:
        audit = {
            "schema": AUDIT_SCHEMA,
            "input_valid": False,
            "manifest_ready": False,
            "errors": [str(error)],
        }
        manifest = None

    audit["source_files"] = {
        "dispatch_refinement": _source_file_record(args.dispatch_refinement),
        "context_refinement": _source_file_record(args.context_refinement),
        "constraints": _source_file_record(args.constraints),
    }
    args.audit.parent.mkdir(parents=True, exist_ok=True)
    args.audit.write_text(json.dumps(audit, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if manifest is None:
        return 1
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(manifest, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
