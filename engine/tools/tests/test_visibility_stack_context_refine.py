import copy
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

TESTS = pathlib.Path(__file__).resolve().parent
TOOLS = TESTS.parent
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TESTS))

from test_visibility_stack_constraints import (
    COMPONENT_A,
    base_lineage,
    occluder,
    owner,
    refinement,
    stack_records,
)
from visibility_stack_context_refine import refine_visibility_stack_contexts
from visibility_stack_constraints import analyze_visibility_stack_constraints


def contextual_records(frame, *, anchor_token=1, projection_bias=0):
    records = stack_records(frame)
    unknown = owner(frame, 4, 55)
    unknown["source_ordinal"] = 2
    anchor = next(
        value
        for value in records
        if value.get("event") == "replay.visibility_owner"
        and value["owner"] == anchor_token
    )
    anchor["v1_projection_id"] += projection_bias
    unknown["v0_projection_id"] = anchor["v1_projection_id"]
    pair = occluder(frame, 4, 1, 0.7, 0.15)
    audit = next(
        value
        for value in records
        if value.get("event") == "replay.visibility_stack_audit"
    )
    audit["owners"] = 4
    audit["maximum_distinct_owners_per_sample"] = 4
    audit["occluder_pairs"] += 1
    catalog = next(
        value
        for value in records
        if value.get("event") == "replay.visibility_catalog"
    )
    catalog["owners"] = 4
    catalog["eligible_owners"] = 4
    catalog["emitted_owners"] = 4
    records.insert(2, pair)
    records.append(unknown)
    return records


def configure_heavy(base, directory):
    paths = [directory / "heavy-a.jsonl", directory / "heavy-b.jsonl"]
    for path in paths:
        path.touch()
    base["captures"] = [
        {
            "instrumented_path": str(path),
            "instrumented_stream_equal": True,
            "instrumented_all_projections_complete": True,
        }
        for path in paths
    ]
    return paths


def matching_full_dags(path, targets):
    del path
    return (
        {pair: ("a" * 64, "b" * 64) for pair in targets},
        {
            "path": "mock",
            "sha256": "c" * 64,
            "expressions": 6,
            "projections": 3,
            "matched_projections": len(targets),
            "matched_compact_fingerprints": len(targets),
            "all_targets_present": True,
            "compact_fingerprints_recomputed_from_full_dag": True,
        },
    )


class VisibilityStackContextRefineTests(unittest.TestCase):
    def fixture(self, directory):
        base = base_lineage()
        discovery = contextual_records(7)
        holdout = contextual_records(8)
        attachment_owner = next(
            value
            for value in discovery
            if value.get("event") == "replay.visibility_owner"
            and value["owner"] == 2
        )
        static = refinement(base, attachment_owner)
        paths = configure_heavy(base, directory)
        # Adding trusted-capture metadata changes the canonical base hash stored
        # by the already-created static refinement.
        static["base_topology_source_sha256"] = __import__(
            "visibility_stack_constraints"
        )._canonical_sha256(base)
        return base, static, discovery, holdout, paths

    def test_admits_exact_context_without_promoting_the_runtime_key(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = self.fixture(pathlib.Path(temporary))
            with mock.patch(
                "visibility_stack_context_refine._extract_fingerprint_evidence",
                side_effect=matching_full_dags,
            ):
                report = refine_visibility_stack_contexts(*fixture)

        self.assertTrue(report["valid"])
        self.assertEqual(report["runtime_context_dispatch"]["entry_count"], 1)
        self.assertEqual(
            report["runtime_context_dispatch"]["endpoint_runtime_keys"], 1
        )
        self.assertEqual(
            report["runtime_context_dispatch"]["entries"][0][
                "target_component_sha256"
            ],
            COMPONENT_A,
        )
        self.assertEqual(
            report["discovery"]["admitted_context_sample_memberships"], 1
        )
        self.assertFalse(
            report["runtime_context_dispatch"]["entries"][0][
                "persistent_runtime_key_promotion"
            ]
        )
        self.assertFalse(report["proof_scope"]["family_count_inferred"])

    def test_process_local_projection_values_are_not_part_of_context(self):
        with tempfile.TemporaryDirectory() as temporary:
            base, static, discovery, _, paths = self.fixture(
                pathlib.Path(temporary)
            )
            holdout = contextual_records(8, projection_bias=100000)
            with mock.patch(
                "visibility_stack_context_refine._extract_fingerprint_evidence",
                side_effect=matching_full_dags,
            ):
                report = refine_visibility_stack_contexts(
                    base, static, discovery, holdout, paths
                )

        self.assertEqual(report["runtime_context_dispatch"]["entry_count"], 1)
        self.assertFalse(
            report["runtime_context_dispatch"][
                "uses_process_local_projection_id_values"
            ]
        )

    def test_constraint_graph_applies_only_the_exact_occurrence_context(self):
        with tempfile.TemporaryDirectory() as temporary:
            base, static, discovery, holdout, paths = self.fixture(
                pathlib.Path(temporary)
            )
            with mock.patch(
                "visibility_stack_context_refine._extract_fingerprint_evidence",
                side_effect=matching_full_dags,
            ):
                context = refine_visibility_stack_contexts(
                    base, static, discovery, holdout, paths
                )

            report = analyze_visibility_stack_constraints(
                base, static, discovery, holdout, context
            )

        self.assertEqual(
            report["coverage"][
                "context_mapping_gain_endpoint_sample_memberships"
            ],
            2,
        )
        self.assertEqual(
            report["coverage"][
                "context_unmapped_endpoint_sample_memberships"
            ],
            0,
        )
        self.assertTrue(
            report["constraint_graph"][
                "constraint_system_ready_for_candidate_testing"
            ]
        )
        self.assertTrue(report["proof_scope"]["frame_local_context_lookup_used"])

    def test_different_anchor_relation_is_not_cross_validated(self):
        with tempfile.TemporaryDirectory() as temporary:
            base, static, discovery, _, paths = self.fixture(
                pathlib.Path(temporary)
            )
            holdout = contextual_records(8, anchor_token=3)
            with self.assertRaisesRegex(ValueError, "no exact contextual"):
                refine_visibility_stack_contexts(
                    base, static, discovery, holdout, paths
                )

    def test_multi_component_context_forms_a_separate_topology_quotient(self):
        with tempfile.TemporaryDirectory() as temporary:
            base, static, discovery, holdout, paths = self.fixture(
                pathlib.Path(temporary)
            )
            for records in (discovery, holdout):
                unknown = records[-1]
                anchor = next(
                    value
                    for value in records
                    if value.get("event") == "replay.visibility_owner"
                    and value["owner"] == 3
                )
                unknown["v1_projection_id"] = anchor["v1_projection_id"]
            with mock.patch(
                "visibility_stack_context_refine._extract_fingerprint_evidence",
                side_effect=matching_full_dags,
            ):
                report = refine_visibility_stack_contexts(
                    base, static, discovery, holdout, paths
                )

        self.assertEqual(
            report["context_cross_validation"]["stable_exact_merge_contexts"],
            1,
        )
        self.assertEqual(
            report["runtime_context_dispatch"][
                "cross_validated_quotient_groups"
            ],
            1,
        )
        entry = report["runtime_context_dispatch"]["entries"][0]
        self.assertEqual(entry["context_kind"], "cross-validated-topology-merge")
        self.assertEqual(len(entry["static_anchor_components"]), 2)
        self.assertNotIn(
            entry["target_component_sha256"], entry["static_anchor_components"]
        )
        self.assertFalse(
            report["runtime_context_dispatch"]["quotient_component_is_family"]
        )

    def test_full_dag_disagreement_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            base, static, discovery, holdout, paths = self.fixture(
                pathlib.Path(temporary)
            )

            def disagreeing(path, targets):
                source = "a" * 64 if path == paths[0] else "d" * 64
                return (
                    {pair: (source, "b" * 64) for pair in targets},
                    {"path": str(path)},
                )

            with mock.patch(
                "visibility_stack_context_refine._extract_fingerprint_evidence",
                side_effect=disagreeing,
            ):
                with self.assertRaisesRegex(ValueError, "disagree"):
                    refine_visibility_stack_contexts(
                        base, static, discovery, holdout, paths
                    )

    def test_constraint_consumer_rejects_context_prior_contamination(self):
        with tempfile.TemporaryDirectory() as temporary:
            base, static, discovery, holdout, paths = self.fixture(
                pathlib.Path(temporary)
            )
            with mock.patch(
                "visibility_stack_context_refine._extract_fingerprint_evidence",
                side_effect=matching_full_dags,
            ):
                context = refine_visibility_stack_contexts(
                    base, static, discovery, holdout, paths
                )
            contaminated = copy.deepcopy(context)
            contaminated["runtime_context_dispatch"][
                "uses_family_count_prior"
            ] = True

            with self.assertRaisesRegex(ValueError, "contaminated"):
                analyze_visibility_stack_constraints(
                    base, static, discovery, holdout, contaminated
                )


if __name__ == "__main__":
    unittest.main()
