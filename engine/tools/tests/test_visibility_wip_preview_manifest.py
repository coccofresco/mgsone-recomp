import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from visibility_wip_preview_manifest import build_wip_preview_manifest


NEAR = "1" * 64
MID = "2" * 64
FAR = "3" * 64
UNVALIDATED = "4" * 64


def dispatch_entry(component, transform, flow):
    return {
        "component_sha256": component,
        "transform_lineage_hex": f"{transform:016x}",
        "triangle_index": 0,
        "vertices": [
            {
                "index": index,
                "model_source_present": False,
                "model_source_hex": "0" * 16,
                "model_flow_source_hex": f"{flow + index:016x}",
                "model_flow_program_hex": f"{flow + 10 + index:016x}",
                "gp0_node_offset": 8 * (index + 1),
            }
            for index in range(3)
        ],
    }


def dispatch_report():
    entries = [
        dispatch_entry(NEAR, 1, 100),
        dispatch_entry(MID, 2, 200),
        dispatch_entry(FAR, 3, 300),
        dispatch_entry(UNVALIDATED, 4, 400),
    ]
    return {
        "schema": "ps1-visibility-runtime-dispatch-refinement/v1",
        "valid": True,
        "runtime_dispatch": {
            "schema": "ps1-visibility-runtime-dispatch-map/v3",
            "ready_for_runtime_lookup": True,
            "compact_model_fingerprint_schema": 1,
            "compact_model_fingerprints_cross_audited_against_full_dag": True,
            "uses_screen_position": False,
            "uses_colour": False,
            "uses_observed_depth": False,
            "uses_numeric_transform_values": False,
            "uses_family_count_prior": False,
            "uses_integer_or_tenth_prior": False,
            "entries": entries,
            "entry_count": len(entries),
        },
    }


def context_report():
    components = (NEAR, MID, FAR, UNVALIDATED)
    return {
        "schema": "ps1-visibility-runtime-context-refinement/v1",
        "valid": True,
        "runtime_context_dispatch": {
            "schema": "ps1-visibility-runtime-context-dispatch-map/v1",
            "ready_for_frame_local_runtime_lookup": True,
            "requires_exact_static_v3_anchor": True,
            "uses_screen_position": False,
            "uses_colour": False,
            "uses_observed_depth": False,
            "uses_numeric_transform_values": False,
            "uses_family_count_prior": False,
            "uses_integer_or_tenth_prior": False,
            "entry_count": 7,
            "component_projection": [
                {
                    "static_component_sha256": component,
                    "target_component_sha256": component,
                }
                for component in components
            ],
        },
    }


def edge(nearer, farther, numerator, denominator=1, cross_validated=True):
    return {
        "nearer_ot_component_sha256": nearer,
        "farther_depth_component_sha256": farther,
        "conservative_lower_bound": {
            "numerator": numerator,
            "denominator": denominator,
        },
        "cross_validated": cross_validated,
    }


def constraint_report():
    return {
        "schema": "ps1-visibility-fragment-stack-constraints/v1",
        "valid": True,
        "constraint_graph": {
            "constraint_graph_acyclic": True,
            "self_component_conflict_records": 0,
            "cross_validated_constraints": 2,
            "constraints": [
                edge(NEAR, MID, 1),
                edge(MID, FAR, 2),
                edge(NEAR, UNVALIDATED, 99, cross_validated=False),
            ],
        },
    }


class VisibilityWipPreviewManifestTest(unittest.TestCase):
    def test_builds_partial_order_embedding_and_omits_unvalidated_keys(self):
        audit, manifest = build_wip_preview_manifest(
            dispatch_report(), context_report(), constraint_report()
        )

        self.assertTrue(audit["input_valid"], audit["errors"])
        self.assertTrue(audit["manifest_ready"])
        self.assertIsNotNone(manifest)
        self.assertIn(f"component {NEAR} 1 1\n", manifest)
        self.assertIn(f"component {MID} 2 1\n", manifest)
        self.assertIn(f"component {FAR} 5 1\n", manifest)
        self.assertNotIn(UNVALIDATED, manifest)
        self.assertEqual(audit["coverage"]["static_runtime_entries_selected"], 3)
        self.assertEqual(audit["coverage"]["static_runtime_entries_fallback"], 1)
        self.assertEqual(audit["coverage"]["frame_local_context_entries_omitted"], 7)
        self.assertFalse(audit["proof_scope"]["gauge_certified"])
        self.assertTrue(audit["proof_scope"]["unknown_runtime_keys_retain_hybrid"])

    def test_rejects_prior_contamination_and_cross_validated_cycles(self):
        contaminated = dispatch_report()
        contaminated["runtime_dispatch"]["uses_integer_or_tenth_prior"] = True
        audit, manifest = build_wip_preview_manifest(
            contaminated, context_report(), constraint_report()
        )
        self.assertFalse(audit["input_valid"])
        self.assertIsNone(manifest)

        cyclic = constraint_report()
        cyclic["constraint_graph"]["constraints"].append(edge(FAR, NEAR, 1))
        cyclic["constraint_graph"]["cross_validated_constraints"] = 3
        audit, manifest = build_wip_preview_manifest(
            dispatch_report(), context_report(), cyclic
        )
        self.assertFalse(audit["input_valid"])
        self.assertIsNone(manifest)
        self.assertIn("cyclic", " ".join(audit["errors"]))


if __name__ == "__main__":
    unittest.main()
