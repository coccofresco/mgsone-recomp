import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from depth_family_trace import runtime_model_fingerprints
from visibility_lineage_trace import (
    _runtime_dispatch_key_report_entry,
    _signature_id,
)
from visibility_stack_dispatch_refine import (
    _canonical_sha256,
    refine_visibility_stack_dispatch,
)


COMPONENT = "a" * 64


def expression(expression_id, value):
    return {
        "schema": "ps1trace/v1",
        "source": "test",
        "seq": expression_id,
        "frame": 10,
        "host_us": 0,
        "event": "mips.value_expr",
        "expression_id": expression_id,
        "op": 1,
        "pc": 0x80001000 + expression_id * 4,
        "raw": 0x24020000 | value,
        "value": value,
        "lhs": 0,
        "rhs": 0,
        "immediate": 0,
        "address": -1,
        "width": 0,
        "signed": 0,
        "complete": 1,
    }


def heavy_records(frame):
    expressions = {}
    records = []
    projections = []
    pairs = []
    for vertex in range(3):
        xy_id = vertex * 2 + 1
        z_id = xy_id + 1
        xy = expression(xy_id, 10 + vertex)
        z = expression(z_id, 20 + vertex)
        expressions[xy_id] = xy
        expressions[z_id] = z
        records.extend((xy, z))
        projection = {
            "schema": "ps1trace/v1",
            "source": "test",
            "seq": 100 + vertex,
            "frame": frame,
            "host_us": 0,
            "event": "gte.projection",
            "projection_id": 1000 + vertex + frame,
            "vxy_expr": xy_id,
            "vz_expr": z_id,
            "model_fingerprint_complete": 1,
            "value_flow_complete": 1,
            "model_fingerprint_schema": 1,
        }
        pair = runtime_model_fingerprints(projection, expressions)
        projection["model_source_fingerprint"] = pair[0]
        projection["model_program_fingerprint"] = pair[1]
        pairs.append(pair)
        projections.append(projection)
    records.extend(projections)
    return records, pairs


def runtime_key(transform, pairs):
    return (
        "visibility-runtime-dispatch/v2",
        transform,
        0,
        tuple(
            (vertex, False, 0, pair[0], pair[1], 8 + vertex * 12)
            for vertex, pair in enumerate(pairs)
        ),
    )


def base_lineage(base_key, heavy_paths):
    entry = {"component_sha256": COMPONENT, **_runtime_dispatch_key_report_entry(base_key)}
    return {
        "schema": "ps1-visibility-lineage-audit/v1",
        "valid": True,
        "captures": [
            {
                "instrumented_path": str(path),
                "instrumented_stream_equal": True,
                "instrumented_all_projections_complete": True,
            }
            for path in heavy_paths
        ],
        "advisory_priors": {
            "family_count_is_correctness_limit": False,
            "integer_or_tenth_scale_is_correctness_limit": False,
            "integer_or_tenth_scale_used_as_filter": False,
            "priors_used_to_select_or_reject_evidence": False,
        },
        "projection_incidence_topology": {
            "component_count_is_object_count": False,
            "component_count_is_family_count": False,
            "component_count_compared_to_family_prior": False,
            "persistent_object_identity_certified": False,
            "runtime_dispatch": {
                "schema": "ps1-visibility-runtime-dispatch-map/v2",
                "target": "projection-incidence-component/v1",
                "entries": [entry],
                "entry_count": 1,
                "components": 1,
                "candidate_dispatch_ready": True,
                "compact_model_fingerprint_schema": 1,
                "compact_model_fingerprints_cross_audited_against_full_dag": True,
                "all_reported_component_signatures_dispatched": True,
                "uses_screen_position": False,
                "uses_colour": False,
                "uses_observed_depth": False,
                "uses_numeric_transform_values": False,
                "uses_family_count_prior": False,
                "uses_integer_or_tenth_prior": False,
                "persistent_object_identity_certified": False,
            },
        },
    }


def cross_report(base, base_key, endpoint_key):
    endpoint = _signature_id(endpoint_key)
    return {
        "schema": "ps1-visibility-fragment-stack-cross-validation/v1",
        "valid": True,
        "ready_to_extend_runtime_dispatch": True,
        "topology_source_sha256": _canonical_sha256(base),
        "stable_endpoint_relations": [
            {
                "endpoint_dispatch_sha256": endpoint,
                "runtime_dispatch_entry": _runtime_dispatch_key_report_entry(
                    endpoint_key
                ),
                "mapped_components": [COMPONENT],
                "relations": [
                    {
                        "endpoint_vertex": 0,
                        "mapped_vertex": 1,
                        "mapped_component": COMPONENT,
                        "mapped_dispatch_sha256": _signature_id(base_key),
                    }
                ],
            }
        ],
        "proof_scope": {
            "relation_requires_discovery_and_holdout": True,
            "exact_shared_projection_relation_required": True,
            "runtime_dispatch_entry_exactly_cross_validated": True,
            "screen_position_used": False,
            "numeric_depth_used_for_topology": False,
            "numeric_transform_used_for_topology": False,
            "family_count_inferred": False,
            "integer_or_tenth_prior_used": False,
            "topology_components_promoted_to_persistent_objects": False,
            "multiplier_fitted_or_recommended": False,
        },
    }


class VisibilityStackDispatchRefineTests(unittest.TestCase):
    def fixture(self, directory):
        records_a, pairs = heavy_records(10)
        records_b, pairs_b = heavy_records(11)
        self.assertEqual(pairs, pairs_b)
        paths = [directory / "heavy-a.jsonl", directory / "heavy-b.jsonl"]
        for path, records in zip(paths, (records_a, records_b)):
            path.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )
        base_key = runtime_key(0x22, [(301, 401), (302, 402), (303, 403)])
        endpoint_key = runtime_key(0x44, pairs)
        base = base_lineage(base_key, paths)
        return paths, base, cross_report(base, base_key, endpoint_key)

    def test_builds_separate_cross_validated_attachments_without_rehashing_base(self):
        with tempfile.TemporaryDirectory() as temporary:
            paths, base, cross = self.fixture(pathlib.Path(temporary))

            report = refine_visibility_stack_dispatch(base, cross, paths)

        self.assertTrue(report["valid"])
        self.assertEqual(report["attachment_count"], 1)
        self.assertEqual(report["runtime_dispatch"]["entry_count"], 2)
        self.assertEqual(
            report["runtime_dispatch"]["schema"],
            "ps1-visibility-runtime-dispatch-map/v3",
        )
        self.assertFalse(
            report["attachments"][0]["base_component_hash_includes_attachment"]
        )
        self.assertFalse(report["proof_scope"]["family_count_inferred"])
        self.assertFalse(report["proof_scope"]["integer_or_tenth_prior_used"])

    def test_requires_two_independent_heavy_captures(self):
        with tempfile.TemporaryDirectory() as temporary:
            paths, base, cross = self.fixture(pathlib.Path(temporary))
            with self.assertRaisesRegex(ValueError, "at least two"):
                refine_visibility_stack_dispatch(base, cross, paths[:1])

    def test_rejects_heavy_capture_not_cross_audited_by_base(self):
        with tempfile.TemporaryDirectory() as temporary:
            paths, base, cross = self.fixture(pathlib.Path(temporary))
            base["captures"] = base["captures"][:1]
            cross["topology_source_sha256"] = _canonical_sha256(base)
            with self.assertRaisesRegex(ValueError, "equivalent complete pair"):
                refine_visibility_stack_dispatch(base, cross, paths)


if __name__ == "__main__":
    unittest.main()
