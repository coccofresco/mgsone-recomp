#!/usr/bin/env python3
"""Cross-validate intrinsic projection anchors from two fragment-stack sets.

Only an identical, non-empty shared-projection relation set observed in both
discovery and holdout is admitted as a stable partial-topology edge. Variable,
missing, duplicated and one-set endpoints remain explicit. The result refines
an incidence graph; it does not infer objects, family count or depth factors.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

from visibility_lineage_trace import _signature_id


INPUT_SCHEMA = "ps1-visibility-fragment-stack-audit/v1"
REPORT_SCHEMA = "ps1-visibility-fragment-stack-cross-validation/v1"
SHA256 = re.compile(r"[0-9a-f]{64}")
HEX64 = re.compile(r"[0-9a-f]{16}")


def _integer(value: Any, minimum: int, maximum: int, context: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{context}: expected an integer")
    if value < minimum or value > maximum:
        raise ValueError(f"{context}: outside {minimum}..{maximum}")
    return value


def _sha(value: Any, context: str) -> str:
    if not isinstance(value, str) or SHA256.fullmatch(value) is None:
        raise ValueError(f"{context}: invalid SHA-256")
    return value


def _hex64(value: Any, context: str) -> int:
    if not isinstance(value, str) or HEX64.fullmatch(value) is None:
        raise ValueError(f"{context}: invalid 64-bit lowercase hexadecimal value")
    return int(value, 16)


def _parse_runtime_dispatch_entry(
    value: Any, context: str
) -> tuple[tuple[Any, ...], dict[str, Any]]:
    if not isinstance(value, dict):
        raise ValueError(f"{context}: missing runtime dispatch entry")
    transform = _hex64(
        value.get("transform_lineage_hex"), f"{context} transform lineage"
    )
    triangle = _integer(
        value.get("triangle_index"), 0, 255, f"{context} triangle index"
    )
    raw_vertices = value.get("vertices")
    if not isinstance(raw_vertices, list) or len(raw_vertices) != 3:
        raise ValueError(f"{context}: runtime entry needs exactly three vertices")
    vertices = []
    canonical_vertices = []
    seen_indices = set()
    for vertex_number, raw_vertex in enumerate(raw_vertices):
        vertex_context = f"{context} vertex {vertex_number}"
        if not isinstance(raw_vertex, dict):
            raise ValueError(f"{vertex_context}: expected an object")
        index = _integer(raw_vertex.get("index"), 0, 3, f"{vertex_context} index")
        if index in seen_indices:
            raise ValueError(f"{context}: repeated runtime vertex index")
        seen_indices.add(index)
        source_present = raw_vertex.get("model_source_present")
        if not isinstance(source_present, bool):
            raise ValueError(f"{vertex_context}: invalid source-present flag")
        source = _hex64(
            raw_vertex.get("model_source_hex"), f"{vertex_context} model source"
        )
        flow_source = _hex64(
            raw_vertex.get("model_flow_source_hex"),
            f"{vertex_context} model-flow source",
        )
        flow_program = _hex64(
            raw_vertex.get("model_flow_program_hex"),
            f"{vertex_context} model-flow program",
        )
        offset = _integer(
            raw_vertex.get("gp0_node_offset"),
            0,
            0x1FFFFF,
            f"{vertex_context} GP0-node offset",
        )
        if source_present != (source != 0):
            raise ValueError(f"{vertex_context}: model-source presence mismatch")
        if flow_source == 0 or flow_program == 0:
            raise ValueError(f"{vertex_context}: zero model-flow fingerprint")
        vertices.append(
            (index, source_present, source, flow_source, flow_program, offset)
        )
        canonical_vertices.append(
            {
                "index": index,
                "model_source_present": source_present,
                "model_source_hex": f"{source:016x}",
                "model_flow_source_hex": f"{flow_source:016x}",
                "model_flow_program_hex": f"{flow_program:016x}",
                "gp0_node_offset": offset,
            }
        )
    key = (
        "visibility-runtime-dispatch/v2",
        transform,
        triangle,
        tuple(vertices),
    )
    return key, {
        "transform_lineage_hex": f"{transform:016x}",
        "triangle_index": triangle,
        "vertices": canonical_vertices,
    }


def _parse_report(report: Any, label: str) -> tuple[dict[str, Any], dict[str, Any]]:
    if not isinstance(report, dict) or report.get("schema") != INPUT_SCHEMA:
        raise ValueError(f"{label}: invalid stack report schema")
    if report.get("valid") is not True:
        raise ValueError(f"{label}: stack report is not valid")
    coverage = report.get("constraint_coverage")
    if not isinstance(coverage, dict) or coverage.get("all_captures_complete") is not True:
        raise ValueError(f"{label}: stack captures are incomplete")
    proof = report.get("proof_scope")
    required_false = (
        "fixed_layer_limit_used",
        "family_count_inferred",
        "topology_component_count_inferred_as_family_count",
        "topology_components_promoted_to_persistent_objects",
        "integer_or_tenth_prior_used",
        "multiplier_fitted_or_recommended",
        "projection_anchor_report_promoted_to_attachment",
    )
    if not isinstance(proof, dict) or any(
        proof.get(field) is not False for field in required_false
    ):
        raise ValueError(f"{label}: proof scope is contaminated")
    topology = report.get("topology_partition")
    if not isinstance(topology, dict):
        raise ValueError(f"{label}: missing topology partition")
    _sha(topology.get("source_sha256"), f"{label} topology source")

    raw_anchors = report.get("unmapped_endpoint_projection_anchors")
    if not isinstance(raw_anchors, list):
        raise ValueError(f"{label}: missing endpoint anchors")
    anchors: dict[str, dict[str, Any]] = {}
    for item_index, item in enumerate(raw_anchors):
        context = f"{label} endpoint {item_index}"
        if not isinstance(item, dict):
            raise ValueError(f"{context}: expected an object")
        endpoint = _sha(item.get("endpoint_dispatch_sha256"), context)
        if endpoint in anchors:
            raise ValueError(f"{context}: duplicate endpoint")
        runtime_key, runtime_entry = _parse_runtime_dispatch_entry(
            item.get("runtime_dispatch_entry"), context
        )
        if _signature_id(runtime_key) != endpoint:
            raise ValueError(f"{context}: runtime entry disagrees with endpoint SHA-256")
        if item.get("attachment_certified") is not False:
            raise ValueError(f"{context}: input already promotes an attachment")
        duplicate = item.get("within_frame_duplicate")
        if not isinstance(duplicate, bool):
            raise ValueError(f"{context}: invalid duplicate flag")
        frames = item.get("frames")
        if not isinstance(frames, list) or not frames:
            raise ValueError(f"{context}: empty frame list")
        parsed_frames = tuple(
            _integer(frame, 0, 2**63 - 1, f"{context} frame") for frame in frames
        )
        if tuple(sorted(set(parsed_frames))) != parsed_frames:
            raise ValueError(f"{context}: frames are not unique/sorted")
        sample_memberships = _integer(
            item.get("sample_memberships"), 1, 2**63 - 1, f"{context} samples"
        )
        roles = item.get("roles")
        if not isinstance(roles, dict) or not roles:
            raise ValueError(f"{context}: empty roles")
        parsed_roles = {
            role: _integer(value, 1, 2**63 - 1, f"{context} role {role}")
            for role, value in roles.items()
            if role in {"ot", "physical"}
        }
        if parsed_roles != roles or sum(parsed_roles.values()) != sample_memberships:
            raise ValueError(f"{context}: invalid role partition")

        raw_sets = item.get("relation_sets")
        if not isinstance(raw_sets, list) or not raw_sets:
            raise ValueError(f"{context}: missing relation sets")
        relation_sets: set[tuple[tuple[int, int, str, str], ...]] = set()
        relation_occurrences = 0
        for set_index, relation_set in enumerate(raw_sets):
            set_context = f"{context} relation set {set_index}"
            if not isinstance(relation_set, dict):
                raise ValueError(f"{set_context}: expected an object")
            occurrences = _integer(
                relation_set.get("owner_occurrences"),
                1,
                2**31 - 1,
                f"{set_context} occurrences",
            )
            relation_occurrences += occurrences
            raw_relations = relation_set.get("relations")
            if not isinstance(raw_relations, list):
                raise ValueError(f"{set_context}: relations must be a list")
            relations = []
            for relation_index, relation in enumerate(raw_relations):
                relation_context = f"{set_context} relation {relation_index}"
                if not isinstance(relation, dict):
                    raise ValueError(f"{relation_context}: expected an object")
                relations.append(
                    (
                        _integer(
                            relation.get("endpoint_vertex"),
                            0,
                            3,
                            f"{relation_context} endpoint vertex",
                        ),
                        _integer(
                            relation.get("mapped_vertex"),
                            0,
                            3,
                            f"{relation_context} mapped vertex",
                        ),
                        _sha(
                            relation.get("mapped_component"),
                            f"{relation_context} component",
                        ),
                        _sha(
                            relation.get("mapped_dispatch_sha256"),
                            f"{relation_context} dispatch",
                        ),
                    )
                )
            canonical = tuple(sorted(set(relations)))
            if len(canonical) != len(relations) or canonical in relation_sets:
                raise ValueError(f"{set_context}: duplicate relation/set")
            relation_sets.add(canonical)
        owner_occurrences = _integer(
            item.get("owner_occurrences"),
            1,
            2**31 - 1,
            f"{context} owner occurrences",
        )
        if relation_occurrences != owner_occurrences:
            raise ValueError(f"{context}: relation-set occurrence mismatch")
        anchors[endpoint] = {
            "frames": parsed_frames,
            "sample_memberships": sample_memberships,
            "roles": parsed_roles,
            "within_frame_duplicate": duplicate,
            "relation_sets": relation_sets,
            "runtime_dispatch_key": runtime_key,
            "runtime_dispatch_entry": runtime_entry,
        }
    return anchors, topology


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


def cross_validate_visibility_stack_reports(
    discovery: Any, holdout: Any
) -> dict[str, Any]:
    discovery_anchors, discovery_topology = _parse_report(discovery, "discovery")
    holdout_anchors, holdout_topology = _parse_report(holdout, "holdout")
    if discovery_topology["source_sha256"] != holdout_topology["source_sha256"]:
        raise ValueError("discovery/holdout use different topology sources")

    all_endpoints = sorted(set(discovery_anchors) | set(holdout_anchors))
    stable: list[dict[str, Any]] = []
    variable: list[dict[str, Any]] = []
    discovery_only: list[str] = []
    holdout_only: list[str] = []
    empty_relation: list[str] = []
    duplicated: list[str] = []
    union = _UnionFind()

    for endpoint in all_endpoints:
        left = discovery_anchors.get(endpoint)
        right = holdout_anchors.get(endpoint)
        if left is None:
            holdout_only.append(endpoint)
            continue
        if right is None:
            discovery_only.append(endpoint)
            continue
        if left["within_frame_duplicate"] or right["within_frame_duplicate"]:
            duplicated.append(endpoint)
            continue
        if left["runtime_dispatch_key"] != right["runtime_dispatch_key"]:
            raise ValueError(
                f"endpoint {endpoint}: discovery/holdout runtime keys disagree"
            )
        if len(left["relation_sets"]) != 1 or len(right["relation_sets"]) != 1:
            variable.append(
                {
                    "endpoint_dispatch_sha256": endpoint,
                    "discovery_relation_sets": len(left["relation_sets"]),
                    "holdout_relation_sets": len(right["relation_sets"]),
                    "same_relation_sets": left["relation_sets"]
                    == right["relation_sets"],
                }
            )
            continue
        left_relations = next(iter(left["relation_sets"]))
        right_relations = next(iter(right["relation_sets"]))
        if not left_relations or not right_relations:
            empty_relation.append(endpoint)
            continue
        if left_relations != right_relations:
            variable.append(
                {
                    "endpoint_dispatch_sha256": endpoint,
                    "discovery_relation_sets": 1,
                    "holdout_relation_sets": 1,
                    "same_relation_sets": False,
                }
            )
            continue
        components = sorted({relation[2] for relation in left_relations})
        endpoint_node = f"endpoint:{endpoint}"
        for component in components:
            union.union(endpoint_node, f"component:{component}")
        stable.append(
            {
                "endpoint_dispatch_sha256": endpoint,
                "runtime_dispatch_entry": left["runtime_dispatch_entry"],
                "mapped_components": components,
                "relations": [
                    {
                        "endpoint_vertex": relation[0],
                        "mapped_vertex": relation[1],
                        "mapped_component": relation[2],
                        "mapped_dispatch_sha256": relation[3],
                    }
                    for relation in left_relations
                ],
                "discovery_frames": list(left["frames"]),
                "holdout_frames": list(right["frames"]),
                "discovery_sample_memberships": left["sample_memberships"],
                "holdout_sample_memberships": right["sample_memberships"],
            }
        )

    groups: dict[str, dict[str, list[str]]] = {}
    for node in sorted(union.parent):
        root = union.find(node)
        group = groups.setdefault(root, {"components": [], "endpoints": []})
        kind, value = node.split(":", 1)
        group[f"{kind}s"].append(value)
    merge_groups = [
        {
            "components": sorted(group["components"]),
            "endpoint_dispatch_sha256": sorted(group["endpoints"]),
        }
        for group in groups.values()
        if group["components"] and group["endpoints"]
    ]
    merge_groups.sort(
        key=lambda group: (group["components"], group["endpoint_dispatch_sha256"])
    )

    return {
        "schema": REPORT_SCHEMA,
        "valid": True,
        "topology_source_sha256": discovery_topology["source_sha256"],
        "discovery_audited_frames": discovery["audited_frames"],
        "holdout_audited_frames": holdout["audited_frames"],
        "endpoint_counts": {
            "discovery": len(discovery_anchors),
            "holdout": len(holdout_anchors),
            "observed_in_both": len(set(discovery_anchors) & set(holdout_anchors)),
            "stable_exact_projection_relations": len(stable),
            "variable_or_multi_relation": len(variable),
            "empty_relation": len(empty_relation),
            "within_frame_duplicate": len(duplicated),
            "discovery_only": len(discovery_only),
            "holdout_only": len(holdout_only),
        },
        "stable_endpoint_relations": stable,
        "refined_partial_topology_merge_groups": merge_groups,
        "variable_endpoints": variable,
        "empty_relation_endpoints": empty_relation,
        "within_frame_duplicate_endpoints": duplicated,
        "discovery_only_endpoints": discovery_only,
        "holdout_only_endpoints": holdout_only,
        "ready_to_extend_runtime_dispatch": bool(stable),
        "proof_scope": {
            "relation_requires_discovery_and_holdout": True,
            "exact_shared_projection_relation_required": True,
            "screen_position_used": False,
            "numeric_depth_used_for_topology": False,
            "numeric_transform_used_for_topology": False,
            "family_count_inferred": False,
            "integer_or_tenth_prior_used": False,
            "topology_components_promoted_to_persistent_objects": False,
            "multiplier_fitted_or_recommended": False,
            "runtime_dispatch_entry_exactly_cross_validated": True,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("discovery_report", type=Path)
    parser.add_argument("holdout_report", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        discovery = json.loads(args.discovery_report.read_text(encoding="utf-8"))
        holdout = json.loads(args.holdout_report.read_text(encoding="utf-8"))
        report = cross_validate_visibility_stack_reports(discovery, holdout)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"visibility_stack_cross_validate: {exc}", file=sys.stderr)
        return 2
    payload = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(payload, encoding="utf-8")
    else:
        print(payload, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
