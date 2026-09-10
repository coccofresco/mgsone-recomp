import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from visibility_runtime_manifest import build_visibility_runtime_manifest


NEAR = "1" * 64
FAR = "2" * 64


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


def lineage():
    entries = [dispatch_entry(NEAR, 1, 100), dispatch_entry(FAR, 2, 200)]
    return {
        "schema": "ps1-visibility-lineage-audit/v1",
        "valid": True,
        "visibility_gauge_constraints": {
            "constraint_system_ready_for_candidate_testing": True,
            "constraint_graph_acyclic": True,
            "directed_component_constraints": 1,
            "constraints": [
                {
                    "nearer_ot_component_sha256": NEAR,
                    "farther_depth_component_sha256": FAR,
                    "strict_lower_bound": {"numerator": 30, "denominator": 1},
                }
            ],
        },
        "numeric_affine_cohorts": {
            "runtime_dispatch": {
                "schema": "ps1-visibility-runtime-dispatch-map/v2",
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
                "entries": entries,
                "entry_count": len(entries),
            }
        },
    }


def candidate(far=31):
    return {
        "schema": "ps1-visibility-gauge-candidate/v1",
        "presentation_multipliers": [
            {"component_sha256": NEAR, "numerator": 1, "denominator": 1},
            {"component_sha256": FAR, "numerator": far, "denominator": 1},
        ],
    }


class VisibilityRuntimeManifestTest(unittest.TestCase):
    def test_serializes_only_an_explicit_cross_validated_hypothesis(self):
        report, manifest = build_visibility_runtime_manifest(lineage(), candidate())

        self.assertTrue(report["input_valid"], report["errors"])
        self.assertTrue(report["manifest_ready"])
        self.assertIsNotNone(manifest)
        self.assertIn("scope hypothesis\n", manifest)
        self.assertIn("family_count_prior_used 0\n", manifest)
        self.assertIn(f"component {FAR} 31 1\n", manifest)
        self.assertEqual(manifest.count("\nentry ") + manifest.startswith("entry "), 2)
        self.assertTrue(report["proof_scope"]["unknown_runtime_keys_fail_closed"])
        self.assertFalse(report["proof_scope"]["passing_manifest_is_gauge_proof"])

    def test_rejects_a_candidate_that_only_meets_not_exceeds_the_bound(self):
        report, manifest = build_visibility_runtime_manifest(lineage(), candidate(30))

        self.assertFalse(report["input_valid"])
        self.assertIsNone(manifest)
        self.assertIn("does not satisfy", " ".join(report["errors"]))

    def test_rejects_any_prior_contamination_or_incomplete_dispatch(self):
        contaminated = lineage()
        contaminated["numeric_affine_cohorts"]["runtime_dispatch"][
            "uses_family_count_prior"
        ] = True
        report, manifest = build_visibility_runtime_manifest(contaminated, candidate())
        self.assertFalse(report["input_valid"])
        self.assertIsNone(manifest)

        incomplete = lineage()
        dispatch = incomplete["numeric_affine_cohorts"]["runtime_dispatch"]
        dispatch["entries"].pop()
        dispatch["entry_count"] = 1
        report, manifest = build_visibility_runtime_manifest(incomplete, candidate())
        self.assertFalse(report["input_valid"])
        self.assertIsNone(manifest)
        self.assertIn("missing candidate components", " ".join(report["errors"]))

    def test_rejects_duplicate_or_value_dependent_dispatch_fields(self):
        duplicate = lineage()
        dispatch = duplicate["numeric_affine_cohorts"]["runtime_dispatch"]
        dispatch["entries"][1] = dict(dispatch["entries"][0])
        dispatch["entry_count"] = 2
        report, manifest = build_visibility_runtime_manifest(duplicate, candidate())
        self.assertFalse(report["input_valid"])
        self.assertIsNone(manifest)
        self.assertIn("duplicate keys", " ".join(report["errors"]))

        dependent = lineage()
        dependent["numeric_affine_cohorts"]["runtime_dispatch"][
            "uses_observed_depth"
        ] = True
        report, manifest = build_visibility_runtime_manifest(dependent, candidate())
        self.assertFalse(report["input_valid"])
        self.assertIsNone(manifest)


if __name__ == "__main__":
    unittest.main()
