import sys
import unittest
from pathlib import Path


TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))

from visibility_gauge_candidate import validate_visibility_gauge_candidate


NEAR_A = "a" * 64
NEAR_B = "b" * 64
FAR = "f" * 64


def constraint(nearer, farther, numerator, denominator=1):
    return {
        "nearer_ot_component_sha256": nearer,
        "farther_depth_component_sha256": farther,
        "strict_lower_bound": {
            "numerator": numerator,
            "denominator": denominator,
        },
    }


def lineage_report():
    constraints = [
        constraint(NEAR_A, FAR, 29),
        constraint(NEAR_B, FAR, 30),
    ]
    return {
        "schema": "ps1-visibility-lineage-audit/v1",
        "valid": True,
        "visibility_gauge_constraints": {
            "constraint_system_ready_for_candidate_testing": True,
            "constraint_graph_acyclic": True,
            "directed_component_constraints": len(constraints),
            "constraints": constraints,
        },
    }


def candidate(far_numerator=31, include_near_b=True):
    multipliers = [
        {
            "component_sha256": NEAR_A,
            "numerator": 1,
            "denominator": 1,
        },
        {
            "component_sha256": FAR,
            "numerator": far_numerator,
            "denominator": 1,
        },
    ]
    if include_near_b:
        multipliers.append(
            {
                "component_sha256": NEAR_B,
                "numerator": 1,
                "denominator": 1,
            }
        )
    return {
        "schema": "ps1-visibility-gauge-candidate/v1",
        "presentation_multipliers": multipliers,
    }


class VisibilityGaugeCandidateTest(unittest.TestCase):
    def test_accepts_explicit_candidate_with_exact_strict_margin(self):
        report = validate_visibility_gauge_candidate(
            lineage_report(), candidate(31)
        )

        self.assertTrue(report["input_valid"], report["errors"])
        self.assertTrue(report["candidate_satisfies_all_measured_constraints"])
        self.assertEqual(report["constraints"], 2)
        self.assertTrue(
            all(check["strict_inequality_satisfied"] for check in report["checks"])
        )
        self.assertFalse(
            report["proof_scope"]["candidate_selected_or_fitted_by_tool"]
        )
        self.assertFalse(report["proof_scope"]["passing_candidate_is_gauge_proof"])
        self.assertTrue(
            report["proof_scope"]["independent_post_lift_raster_validation_required"]
        )

    def test_rejects_equality_at_strict_bound(self):
        report = validate_visibility_gauge_candidate(
            lineage_report(), candidate(30)
        )

        self.assertTrue(report["input_valid"], report["errors"])
        self.assertFalse(report["candidate_satisfies_all_measured_constraints"])
        self.assertEqual(
            [check["strict_inequality_satisfied"] for check in report["checks"]],
            [True, False],
        )

    def test_rejects_missing_component_assignment(self):
        report = validate_visibility_gauge_candidate(
            lineage_report(), candidate(31, include_near_b=False)
        )

        self.assertFalse(report["input_valid"])
        self.assertFalse(report["candidate_satisfies_all_measured_constraints"])
        self.assertTrue(
            any("missing components" in error for error in report["errors"])
        )

    def test_rejects_truncated_constraint_report(self):
        lineage = lineage_report()
        lineage["visibility_gauge_constraints"]["constraints"] = lineage[
            "visibility_gauge_constraints"
        ]["constraints"][:1]

        report = validate_visibility_gauge_candidate(lineage, candidate(31))

        self.assertFalse(report["input_valid"])
        self.assertTrue(
            any("constraint list count" in error for error in report["errors"])
        )


if __name__ == "__main__":
    unittest.main()
