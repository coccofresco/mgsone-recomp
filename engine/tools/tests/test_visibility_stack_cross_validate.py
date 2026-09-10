import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from visibility_stack_cross_validate import (
    cross_validate_visibility_stack_reports,
)
from visibility_lineage_trace import _signature_id


COMPONENT = "2" * 64
MAPPED = "3" * 64


RUNTIME_ENTRY = {
    "transform_lineage_hex": "0000000000000042",
    "triangle_index": 0,
    "vertices": [
        {
            "index": vertex,
            "model_source_present": False,
            "model_source_hex": "0000000000000000",
            "model_flow_source_hex": f"{100 + vertex:016x}",
            "model_flow_program_hex": f"{200 + vertex:016x}",
            "gp0_node_offset": 8 + vertex * 12,
        }
        for vertex in range(3)
    ],
}
RUNTIME_KEY = (
    "visibility-runtime-dispatch/v2",
    0x42,
    0,
    tuple(
        (vertex, False, 0, 100 + vertex, 200 + vertex, 8 + vertex * 12)
        for vertex in range(3)
    ),
)
ENDPOINT = _signature_id(RUNTIME_KEY)


def relation(mapped_vertex=1):
    return {
        "endpoint_vertex": 0,
        "mapped_vertex": mapped_vertex,
        "mapped_component": COMPONENT,
        "mapped_dispatch_sha256": MAPPED,
    }


def report(relations, *, topology="a" * 64):
    return {
        "schema": "ps1-visibility-fragment-stack-audit/v1",
        "valid": True,
        "audited_frames": 2,
        "constraint_coverage": {"all_captures_complete": True},
        "topology_partition": {"source_sha256": topology},
        "proof_scope": {
            "fixed_layer_limit_used": False,
            "family_count_inferred": False,
            "topology_component_count_inferred_as_family_count": False,
            "topology_components_promoted_to_persistent_objects": False,
            "integer_or_tenth_prior_used": False,
            "multiplier_fitted_or_recommended": False,
            "projection_anchor_report_promoted_to_attachment": False,
        },
        "unmapped_endpoint_projection_anchors": [
            {
                "endpoint_dispatch_sha256": ENDPOINT,
                "runtime_dispatch_entry": RUNTIME_ENTRY,
                "frames": [10, 11],
                "owner_occurrences": len(relations),
                "sample_memberships": 7,
                "roles": {"ot": 7},
                "relation_sets": [
                    {"relations": value, "owner_occurrences": 1}
                    for value in relations
                ],
                "within_frame_duplicate": False,
                "attachment_certified": False,
            }
        ],
    }


class VisibilityStackCrossValidationTests(unittest.TestCase):
    def test_accepts_only_the_same_exact_projection_relation_in_both_sets(self):
        discovery = report([[relation()]])
        holdout = report([[relation()]])

        result = cross_validate_visibility_stack_reports(discovery, holdout)

        self.assertTrue(result["valid"])
        self.assertEqual(
            result["endpoint_counts"]["stable_exact_projection_relations"], 1
        )
        self.assertTrue(result["ready_to_extend_runtime_dispatch"])
        self.assertEqual(
            result["stable_endpoint_relations"][0]["mapped_components"],
            [COMPONENT],
        )
        self.assertFalse(result["proof_scope"]["family_count_inferred"])

    def test_changed_vertex_relation_remains_variable(self):
        discovery = report([[relation(1)]])
        holdout = report([[relation(2)]])

        result = cross_validate_visibility_stack_reports(discovery, holdout)

        self.assertEqual(
            result["endpoint_counts"]["stable_exact_projection_relations"], 0
        )
        self.assertEqual(
            result["endpoint_counts"]["variable_or_multi_relation"], 1
        )
        self.assertFalse(result["ready_to_extend_runtime_dispatch"])

    def test_rejects_different_topology_sources(self):
        with self.assertRaisesRegex(ValueError, "different topology sources"):
            cross_validate_visibility_stack_reports(
                report([[relation()]]), report([[relation()]], topology="b" * 64)
            )


if __name__ == "__main__":
    unittest.main()
