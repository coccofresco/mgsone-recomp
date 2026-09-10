import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))

from depth_family_trace import (  # noqa: E402
    analyze,
    canonical_affine,
    read_trace,
    runtime_model_fingerprints,
)


def record(sequence, event, **fields):
    return {
        "schema": "ps1trace/v1",
        "source": "test",
        "seq": sequence,
        "frame": 10,
        "host_us": sequence,
        "event": event,
        **fields,
    }


def projection(sequence, projection_id, multiplier, transform_hash):
    base = (1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 2, 3)
    affine = {
        field: value * multiplier
        for field, value in zip(
            ("r00", "r01", "r02", "r10", "r11", "r12",
             "r20", "r21", "r22", "tx", "ty", "tz"),
            base,
        )
    }
    return record(
        sequence,
        "gte.projection",
        projection_id=projection_id,
        transform_hash=transform_hash,
        transform_lineage=500,
        lineage_complete=1,
        model_source=0x80030000 + projection_id * 8,
        transform_source=0x80020000,
        **affine,
    )


def expression(sequence, expression_id, op, value, lhs=0, rhs=0,
               immediate=0, complete=1, address=-1, width=0, signed=0,
               pc=0x8004213C, **fields):
    return record(
        sequence,
        "mips.value_expr",
        expression_id=expression_id,
        op=op,
        pc=pc,
        raw=0,
        value=value,
        lhs=lhs,
        rhs=rhs,
        immediate=immediate,
        address=address,
        width=width,
        signed=signed,
        complete=complete,
        **fields,
    )


class DepthFamilyTraceTest(unittest.TestCase):
    def test_joins_projection_dma_provenance_and_finds_only_exact_ratio(self):
        records = [
            projection(1, 1, 2, 101),
            projection(2, 3, 3, 102),
            record(3, "gpu.dma_node", submission_id=7, node_address=0x1000,
                   words=3, traversal_ordinal=0),
            record(4, "gpu.gp0_word", submission_id=7, word_address=0x1004,
                   projection_id=1),
            record(5, "gpu.gp0_word", submission_id=7, word_address=0x1008,
                   projection_id=3),
            record(6, "gpu.gp0_word", submission_id=7, word_address=0x100C,
                   projection_id=2),
        ]

        report = analyze(records)

        self.assertEqual(report["counts"]["submitted_projection_words"], 3)
        self.assertEqual(report["counts"]["joined_projection_words"], 2)
        self.assertEqual(report["counts"]["dma_joined_projection_words"], 2)
        self.assertEqual(
            report["counts"]["source_complete_projection_words"], 2
        )
        self.assertEqual(report["transform_lineages"][0]["projection_count"], 2)
        self.assertEqual(report["transform_lineages"][0]["submitted_uses"], 2)
        self.assertEqual(report["integrity"]["missing_projection_ids"], [2])
        self.assertEqual(report["counts"]["exact_affine_gauge_groups"], 1)
        members = report["exact_affine_gauge_groups"][0]["members"]
        self.assertEqual(
            [member["relative_gauge_tenths"] for member in members], [10, 15]
        )
        prior = report["prior_assessment"]
        self.assertFalse(prior["family_count_is_correctness_limit"])
        self.assertFalse(prior["tenths_are_correctness_requirement"])
        self.assertFalse(prior["exact_affine_group_count_is_family_count"])
        self.assertEqual(prior["largest_candidate_group_member_count"], 2)
        self.assertTrue(
            prior["all_reported_relative_gauges_expressible_in_tenths"]
        )

    def test_non_tenth_and_fifth_exact_candidates_are_advisory_only(self):
        records = [
            projection(index, index, multiplier, 100 + index)
            for index, multiplier in enumerate((4, 5, 6, 7, 9), 1)
        ]

        report = analyze(records)

        self.assertEqual(report["counts"]["exact_affine_gauge_groups"], 1)
        prior = report["prior_assessment"]
        self.assertEqual(prior["largest_candidate_group_member_count"], 5)
        self.assertTrue(prior["candidate_group_exceeds_likely_family_count"])
        self.assertFalse(
            prior["all_reported_relative_gauges_expressible_in_tenths"]
        )
        self.assertIn("advisory priors", report["activation_note"])

    def test_reports_exact_fixed_point_scale_without_certifying_family(self):
        projected = projection(6, 1, 1, 101)
        projected.update(
            value_flow_complete=0,
            tr0_expr=5,
            rt0_expr=999,
        )
        records = [
            expression(1, 1, 2, 100, address=0x80020000, width=4),
            expression(2, 2, 1, 0xC0, immediate=0xC0),
            expression(3, 3, 12, 19200, lhs=1, rhs=2),
            expression(4, 4, 7, 75, lhs=3, immediate=8),
            expression(5, 5, 25, 75, lhs=4, immediate=5),
            projected,
        ]

        report = analyze(records)

        self.assertEqual(report["counts"]["mips_value_expressions"], 5)
        self.assertEqual(report["counts"]["value_flow_rooted_projections"], 1)
        self.assertEqual(report["counts"]["complete_value_flow_projections"], 0)
        self.assertEqual(
            report["integrity"]["missing_projection_expression_roots"],
            [999],
        )
        self.assertEqual(len(report["fixed_point_scale_candidates"]), 1)
        candidate = report["fixed_point_scale_candidates"][0]
        self.assertEqual(
            (candidate["factor_numerator"], candidate["factor_denominator"]),
            (3, 4),
        )
        self.assertEqual(candidate["root_slots"], ["tr0"])
        self.assertFalse(
            candidate["single_expression_reaches_all_transform_word_roots"]
        )
        self.assertFalse(candidate["multiplication_wrapped"])
        self.assertEqual(candidate["discarded_remainder"], 0)
        coverage = report["fixed_point_factor_coverage"][0]
        self.assertEqual(coverage["root_slots"], ["tr0"])
        self.assertEqual(coverage["projection_ids"], [1])
        self.assertFalse(coverage["covers_all_transform_word_roots"])
        self.assertFalse(coverage["word_root_coverage_is_gauge_proof"])

    def test_routes_packed_scale_to_exact_rotation_components(self):
        projected = projection(26, 1, 1, 101)
        projected.update(rt1_expr=13, rt2_expr=25)
        records = [
            expression(1, 1, 2, 7, address=0x80020000, width=4),
            expression(2, 2, 1, 0xC0, immediate=0xC0),
            expression(3, 3, 2, 100, address=0x80020004, width=4),
            expression(4, 4, 12, 19200, lhs=2, rhs=3),
            expression(5, 5, 7, 75, lhs=4, immediate=8),
            expression(6, 6, 1, 0, immediate=0),
            expression(7, 7, 4, 0xFFFFFFB5, lhs=6, rhs=5),
            expression(8, 8, 5, 0xFFB50000, lhs=7, immediate=16),
            expression(9, 9, 1, 0xFFFF, immediate=0xFFFF),
            expression(10, 10, 8, 7, lhs=1, rhs=9),
            expression(11, 11, 9, 0xFFB50007, lhs=10, rhs=8),
            expression(12, 12, 25, 0xFFB50007, lhs=11, immediate=1),
            # Keep IDs aligned with the projection roots while making two
            # independent 3/4 instances for RT22 and RT23.
            expression(13, 13, 25, 0xFFB50007, lhs=11, immediate=1),
            expression(14, 14, 2, 80, address=0x80020008, width=4),
            expression(15, 15, 12, 15360, lhs=2, rhs=14),
            expression(16, 16, 7, 60, lhs=15, immediate=8),
            expression(17, 17, 4, 0xFFFFFFC4, lhs=6, rhs=16),
            expression(18, 18, 8, 0xFFC4, lhs=17, rhs=9),
            expression(19, 19, 2, 40, address=0x8002000C, width=4),
            expression(20, 20, 12, 7680, lhs=2, rhs=19),
            expression(21, 21, 7, 30, lhs=20, immediate=8),
            expression(22, 22, 4, 0xFFFFFFE2, lhs=6, rhs=21),
            expression(23, 23, 5, 0xFFE20000, lhs=22, immediate=16),
            expression(24, 24, 9, 0xFFE2FFC4, lhs=18, rhs=23),
            expression(25, 25, 25, 0xFFE2FFC4, lhs=24, immediate=2),
            projected,
        ]

        report = analyze(records)

        self.assertEqual(report["counts"]["exact_component_scale_routes"], 3)
        self.assertEqual(len(report["fixed_point_component_coverage"]), 1)
        coverage = report["fixed_point_component_coverage"][0]
        self.assertEqual(
            (coverage["factor_numerator"], coverage["factor_denominator"]),
            (3, 4),
        )
        self.assertEqual(coverage["components"], ["r10", "r11", "r12"])
        self.assertEqual(
            coverage["post_scale_signs"],
            {"r10": -1, "r11": -1, "r12": -1},
        )
        self.assertTrue(coverage["all_routes_complete"])
        self.assertFalse(coverage["covers_all_rotation_components"])
        self.assertFalse(coverage["covers_full_affine_transform"])
        self.assertFalse(coverage["component_routing_is_gauge_proof"])
        self.assertEqual(report["component_routing_failures"], [])

    def test_overlapping_packed_sources_fail_closed(self):
        projected = projection(8, 1, 1, 101)
        projected.update(tr0_expr=7)
        report = analyze([
            expression(1, 1, 2, 100, address=0x80020000, width=4),
            expression(2, 2, 1, 0xC0, immediate=0xC0),
            expression(3, 3, 12, 19200, lhs=1, rhs=2),
            expression(4, 4, 7, 75, lhs=3, immediate=8),
            expression(5, 5, 2, 1, address=0x80020004, width=4),
            expression(6, 6, 9, 75, lhs=4, rhs=5),
            expression(7, 7, 25, 75, lhs=6, immediate=5),
            projected,
        ])

        self.assertEqual(report["counts"]["exact_component_scale_routes"], 0)
        self.assertEqual(report["fixed_point_component_coverage"], [])
        self.assertEqual(len(report["component_routing_failures"]), 1)
        failure = report["component_routing_failures"][0]
        self.assertEqual(failure["component"], "tx")
        self.assertEqual(failure["reason"], "overlapping-bit-sources")

    def test_variable_shift_is_not_a_fixed_factor(self):
        projected = projection(7, 1, 1, 101)
        projected.update(tr0_expr=6)
        report = analyze([
            expression(1, 1, 2, 100, address=0x80020000, width=4),
            expression(2, 2, 1, 0xC0, immediate=0xC0),
            expression(3, 3, 12, 19200, lhs=1, rhs=2),
            expression(4, 4, 1, 8, immediate=8),
            # RHS marks SRAV even though immediate records this sample's 8.
            expression(5, 5, 7, 75, lhs=3, rhs=4, immediate=8),
            expression(6, 6, 25, 75, lhs=5, immediate=5),
            projected,
        ])

        self.assertEqual(report["fixed_point_scale_candidates"], [])
        self.assertEqual(report["fixed_point_component_coverage"], [])

    def test_reports_incomplete_boundary_instead_of_inventing_source(self):
        projected = projection(2, 1, 1, 101)
        projected.update(value_flow_complete=0, rt0_expr=1)
        report = analyze([
            expression(1, 1, 0, 0x1000, immediate=4, complete=0,
                       address=0x200, pc=0x8004103C),
            projected,
        ])

        self.assertEqual(report["counts"]["complete_value_flow_projections"], 0)
        self.assertEqual(len(report["value_flow_boundaries"]), 1)
        boundary = report["value_flow_boundaries"][0]
        self.assertEqual(boundary["root_slot"], "rt0")
        self.assertEqual(boundary["boundary_name"], "existing-gte-state")
        self.assertEqual(boundary["projection_count"], 1)
        self.assertEqual(boundary["projection_first"], 1)

    def test_model_dag_lineage_ignores_samples_but_not_exact_sources(self):
        first = projection(9, 1, 1, 101)
        first.update(vxy_expr=2, vz_expr=4, value_flow_complete=1)
        same_source = projection(18, 2, 1, 101)
        same_source.update(vxy_expr=12, vz_expr=14, value_flow_complete=1)
        moved_source = projection(27, 3, 1, 101)
        moved_source.update(vxy_expr=22, vz_expr=24, value_flow_complete=1)
        incomplete = projection(30, 4, 1, 101)
        incomplete.update(vxy_expr=31, vz_expr=32, value_flow_complete=0)
        records = [
            expression(1, 1, 2, 0x00020001, address=0x80030000,
                       width=4, pc=0x80041000),
            expression(2, 2, 24, 0x00020001, lhs=1, immediate=0,
                       pc=0x80042000),
            expression(3, 3, 2, 3, address=0x80030004, width=4,
                       pc=0x80041004),
            expression(4, 4, 24, 3, lhs=3, immediate=1,
                       pc=0x80042004),
            # Different sampled values and IDs, identical program and source.
            expression(10, 11, 2, 0x00090008, address=0x80030000,
                       width=4, pc=0x80041000),
            expression(11, 12, 24, 0x00090008, lhs=11, immediate=0,
                       pc=0x80042000),
            expression(12, 13, 2, 10, address=0x80030004, width=4,
                       pc=0x80041004),
            expression(13, 14, 24, 10, lhs=13, immediate=1,
                       pc=0x80042004),
            # Same exact instruction path, different physical model source.
            expression(19, 21, 2, 0x00050004, address=0x80030100,
                       width=4, pc=0x80041000),
            expression(20, 22, 24, 0x00050004, lhs=21, immediate=0,
                       pc=0x80042000),
            expression(21, 23, 2, 6, address=0x80030104, width=4,
                       pc=0x80041004),
            expression(22, 24, 24, 6, lhs=23, immediate=1,
                       pc=0x80042004),
            expression(28, 31, 0, 0, immediate=2, complete=0),
            expression(29, 32, 0, 0, immediate=2, complete=0),
            first,
            same_source,
            moved_source,
            incomplete,
            record(31, "gpu.gp0_word", submission_id=7,
                   word_address=0x1004, projection_id=1),
            record(32, "gpu.gp0_word", submission_id=7,
                   word_address=0x1008, projection_id=2),
            record(33, "gpu.gp0_word", submission_id=7,
                   word_address=0x100C, projection_id=3),
        ]
        expressions = {
            int(item["expression_id"]): item
            for item in records
            if item["event"] == "mips.value_expr"
        }
        for projected in (first, same_source, moved_source):
            fingerprints = runtime_model_fingerprints(projected, expressions)
            self.assertIsNotNone(fingerprints)
            projected.update(
                model_fingerprint_complete=1,
                model_fingerprint_schema=1,
                model_source_fingerprint=fingerprints[0],
                model_program_fingerprint=fingerprints[1],
            )
        projection_by_id = {
            int(projected["projection_id"]): projected
            for projected in (first, same_source, moved_source)
        }
        for item in records:
            if item["event"] != "gpu.gp0_word":
                continue
            projected = projection_by_id[int(item["projection_id"])]
            item.update(
                intrinsic_provenance=1 << 7,
                model_source_fingerprint=(
                    projected["model_source_fingerprint"]
                ),
                model_program_fingerprint=(
                    projected["model_program_fingerprint"]
                ),
            )
        incomplete.update(
            model_fingerprint_complete=0,
            model_fingerprint_schema=1,
            model_source_fingerprint=0,
            model_program_fingerprint=0,
        )

        report = analyze(records)

        self.assertEqual(
            report["counts"]["model_value_flow_rooted_projections"], 4
        )
        self.assertEqual(
            report["counts"]["complete_model_value_flow_projections"], 3
        )
        self.assertEqual(
            report["counts"]["distinct_model_value_flow_lineages"], 2
        )
        self.assertEqual(
            report["counts"]["distinct_model_value_flow_programs"], 1
        )
        self.assertEqual(
            report["counts"]["runtime_model_fingerprint_projections"], 3
        )
        self.assertEqual(
            report["counts"]["gpu_model_fingerprint_joined_words"], 3
        )
        self.assertEqual(
            report["integrity"]["model_value_flow_digest_collisions"], []
        )
        self.assertEqual(
            report["integrity"]["runtime_model_fingerprint_mismatches"], []
        )
        self.assertEqual(
            report["integrity"]["runtime_model_fingerprint_collisions"], []
        )
        self.assertEqual(
            report["integrity"]["gpu_model_fingerprint_mismatches"], []
        )
        lineages = report["model_value_flow_lineages"]
        self.assertEqual(
            sorted(item["projection_count"] for item in lineages), [1, 2]
        )
        repeated = next(
            item for item in lineages if item["projection_count"] == 2
        )
        self.assertEqual(repeated["submitted_uses"], 2)
        self.assertFalse(repeated["source_digest_is_object_identity"])
        self.assertEqual(len(repeated["source_digest"]), 64)
        program = report["model_value_flow_programs"][0]
        self.assertEqual(program["projection_count"], 3)
        self.assertEqual(len(program["source_digests"]), 2)
        self.assertFalse(program["program_digest_is_object_identity"])
        self.assertEqual(
            report["transform_lineages"][0][
                "model_value_flow_programs"
            ],
            [program["program_digest"]],
        )

        records[-3]["model_program_fingerprint"] ^= 1
        corrupted = analyze(records)
        self.assertEqual(
            corrupted["integrity"]["gpu_model_fingerprint_mismatches"],
            [{
                "seq": 31,
                "projection_id": 1,
                "reasons": ["program-fingerprint-transport-mismatch"],
            }],
        )
        records[-3]["model_program_fingerprint"] ^= 1
        records[-3].pop("model_source_fingerprint")
        records[-3].pop("model_program_fingerprint")
        missing_fields = analyze(records)
        self.assertEqual(
            missing_fields["integrity"]["gpu_model_fingerprint_mismatches"],
            [{
                "seq": 31,
                "projection_id": 1,
                "reasons": ["missing-gpu-fingerprint-fields"],
            }],
        )

    def test_runtime_model_fingerprint_mismatch_fails_closed(self):
        projected = projection(5, 1, 1, 101)
        projected.update(
            vxy_expr=2,
            vz_expr=4,
            value_flow_complete=1,
            model_fingerprint_complete=1,
            model_fingerprint_schema=1,
            model_source_fingerprint=1,
            model_program_fingerprint=2,
        )
        records = [
            expression(1, 1, 2, 0x00020001, address=0x80030000,
                       width=4, pc=0x80041000),
            expression(2, 2, 24, 0x00020001, lhs=1, immediate=0,
                       pc=0x80042000),
            expression(3, 3, 2, 3, address=0x80030004, width=4,
                       pc=0x80041004),
            expression(4, 4, 24, 3, lhs=3, immediate=1,
                       pc=0x80042004),
            projected,
        ]

        report = analyze(records)

        self.assertEqual(
            report["integrity"]["runtime_model_fingerprint_mismatches"],
            [{
                "projection_id": 1,
                "reasons": [
                    "source-fingerprint-mismatch",
                    "program-fingerprint-mismatch",
                ],
            }],
        )

    def test_partial_runtime_model_fingerprint_schema_fails_closed(self):
        projected = projection(5, 1, 1, 101)
        projected.update(
            vxy_expr=2,
            vz_expr=4,
            value_flow_complete=1,
            model_fingerprint_complete=1,
            model_fingerprint_schema=1,
            model_source_fingerprint=1,
        )
        records = [
            expression(1, 1, 2, 0x00020001, address=0x80030000,
                       width=4, pc=0x80041000),
            expression(2, 2, 24, 0x00020001, lhs=1, immediate=0,
                       pc=0x80042000),
            expression(3, 3, 2, 3, address=0x80030004,
                       width=4, pc=0x80041004),
            expression(4, 4, 24, 3, lhs=3, immediate=1,
                       pc=0x80042004),
            projected,
        ]

        report = analyze(records)

        mismatch = report["integrity"][
            "runtime_model_fingerprint_mismatches"
        ]
        self.assertEqual(mismatch[0]["projection_id"], 1)
        self.assertIn(
            "missing-runtime-fingerprint-fields",
            mismatch[0]["reasons"],
        )

    def test_missing_runtime_model_fingerprint_record_fails_new_schema(self):
        complete = projection(1, 1, 1, 101)
        complete.update(
            model_fingerprint_complete=0,
            model_fingerprint_schema=1,
            model_source_fingerprint=0,
            model_program_fingerprint=0,
        )
        missing = projection(2, 2, 1, 101)

        report = analyze([complete, missing])

        self.assertEqual(
            report["integrity"]["runtime_model_fingerprint_mismatches"],
            [{
                "projection_id": 2,
                "reasons": ["missing-runtime-fingerprint-fields"],
            }],
        )

    def test_projection_words_outside_capture_id_window_are_not_integrity_errors(self):
        records = [
            projection(1, 10, 1, 101),
            record(2, "gpu.gp0_word", submission_id=7,
                   word_address=0x1004, projection_id=9),
            record(3, "gpu.gp0_word", submission_id=7,
                   word_address=0x1008, projection_id=11),
        ]
        report = analyze(records)
        self.assertEqual(report["integrity"]["missing_projection_ids"], [])
        self.assertEqual(
            report["counts"]["out_of_window_projection_words"], 2
        )

    def test_canonical_affine_preserves_sign_and_rejects_zero(self):
        positive, scale = canonical_affine((2, 4, 0))
        negative, negative_scale = canonical_affine((-2, -4, 0))
        self.assertEqual((positive, scale), ((1, 2, 0), 2))
        self.assertEqual((negative, negative_scale), ((-1, -2, 0), 2))
        self.assertNotEqual(positive, negative)
        with self.assertRaisesRegex(ValueError, "zero affine"):
            canonical_affine((0, 0, 0))

    def test_dma_payload_join_normalizes_kseg_word_address(self):
        records = [
            projection(1, 1, 1, 101),
            record(2, "gpu.dma_node", submission_id=7,
                   node_address=0x1000, words=1, traversal_ordinal=0),
            record(3, "gpu.gp0_word", submission_id=7,
                   word_address=0x80001004, projection_id=1),
        ]
        report = analyze(records)
        self.assertEqual(report["counts"]["joined_projection_words"], 1)
        self.assertEqual(
            report["transform_lineages"][0]["dma_nodes"], [0x1000]
        )

    def test_explicit_dma_word_provenance_fails_closed_on_node_mismatch(self):
        records = [
            projection(1, 1, 1, 101),
            projection(2, 2, 1, 101),
            record(3, "gpu.dma_node", submission_id=7,
                   list_start=0x1000, node_address=0x1000, words=2,
                   traversal_ordinal=0),
            record(4, "gpu.gp0_word", submission_id=7,
                   word_address=0x1004, projection_id=1,
                   dma_provenance=1, dma_list_start=0x1000,
                   dma_node_address=0x1000, dma_traversal_ordinal=0),
            record(5, "gpu.gp0_word", submission_id=7,
                   word_address=0x1008, projection_id=2,
                   dma_provenance=1, dma_list_start=0x1000,
                   dma_node_address=0x2000, dma_traversal_ordinal=0),
        ]

        report = analyze(records)

        self.assertEqual(
            report["counts"]["explicit_dma_projection_words"], 2
        )
        self.assertEqual(report["counts"]["joined_projection_words"], 2)
        self.assertEqual(report["counts"]["dma_joined_projection_words"], 1)
        self.assertEqual(
            report["integrity"]["dma_word_provenance_mismatches"],
            [{
                "seq": 5,
                "submission_id": 7,
                "word_address": 0x1008,
                "reasons": ["node-address-mismatch"],
            }],
        )

    def test_explicit_non_dma_word_never_inherits_current_node(self):
        records = [
            projection(1, 1, 1, 101),
            record(2, "gpu.dma_node", submission_id=7,
                   list_start=0x1000, node_address=0x1000, words=1,
                   traversal_ordinal=0),
            record(3, "gpu.gp0_word", submission_id=7,
                   word_address=0x1004, projection_id=1,
                   dma_provenance=0, dma_list_start=-1,
                   dma_node_address=-1, dma_traversal_ordinal=-1),
        ]

        report = analyze(records)

        self.assertEqual(report["counts"]["joined_projection_words"], 1)
        self.assertEqual(report["counts"]["explicit_dma_projection_words"], 0)
        self.assertEqual(report["counts"]["dma_joined_projection_words"], 0)
        self.assertEqual(
            report["integrity"]["dma_word_provenance_mismatches"], []
        )

    def test_duplicate_projection_id_is_always_an_integrity_error(self):
        duplicate = projection(2, 1, 2, 101)
        report = analyze([projection(1, 1, 2, 101), duplicate])
        self.assertEqual(
            report["integrity"]["projection_id_collisions"],
            [{"projection_id": 1, "reason": "duplicate-same-affine"}],
        )

    def test_cli_writes_versioned_report(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            trace = directory / "capture.jsonl"
            output = directory / "report.json"
            trace.write_text(
                "\n".join(
                    json.dumps(item) for item in [projection(1, 1, 1, 10)]
                )
                + "\n",
                encoding="utf-8",
            )
            completed = subprocess.run(
                [sys.executable, str(TOOLS / "depth_family_trace.py"),
                 str(trace), "--output", str(output)],
                check=False,
                capture_output=True,
                text=True,
            )
            report = json.loads(output.read_text(encoding="utf-8"))

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(report["schema"], "ps1-depth-family-trace/v1")
        self.assertEqual(report["counts"]["gte_projections"], 1)

    def test_reader_streams_only_provenance_events(self):
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory) / "capture.jsonl"
            trace.write_text(
                "\n".join(
                    json.dumps(item)
                    for item in [
                        record(1, "gpu.polygon_route", route=1),
                        projection(2, 7, 1, 10),
                        record(3, "timing.vblank", index=10),
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            streamed = list(read_trace(trace))

        self.assertEqual([item["event"] for item in streamed],
                         ["gte.projection"])


if __name__ == "__main__":
    unittest.main()
