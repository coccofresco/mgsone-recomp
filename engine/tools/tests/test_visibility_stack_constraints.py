import copy
import pathlib
import struct
import sys
import unittest
from fractions import Fraction

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from visibility_candidate_trace import _parse_topology_dispatch
from visibility_lineage_trace import (
    _owner_fields,
    _runtime_dispatch_key,
    _runtime_dispatch_key_report_entry,
    _signature_id,
)
from visibility_stack_constraints import (
    _canonical_sha256,
    _cyclic_strong_components,
    analyze_visibility_stack_constraints,
)


COMPONENT_A = "1" * 64
COMPONENT_B = "2" * 64
COMPONENT_C = "3" * 64
ALL_EVIDENCE = 0x3F


def record(event, frame, **fields):
    return {
        "schema": "ps1trace/v1",
        "source": "test",
        "seq": 1,
        "frame": frame,
        "host_us": 0,
        "event": event,
        **fields,
    }


def owner(frame, token, lineage):
    fields = {
        "owner_schema": 2,
        "owner": token,
        "packet_sequence": token,
        "submission_id": 1,
        "source_ordinal": token,
        "frame_index": 0,
        "operation_index": token,
        "triangle_index": 0,
        "route": 0,
        "opaque": 1,
        "fully_precise": 1,
        "authentic_depth": 1,
        "comparable": 1,
        "valid_topology": 1,
        "complete_evidence": ALL_EVIDENCE,
        "coherent_projection_context": 1,
        "coherent_transform_lineage": 1,
        "coherent_transform_value": 1,
        "coherent_dma_traversal": 1,
    }
    for vertex in range(3):
        prefix = f"v{vertex}_"
        fields.update(
            {
                prefix + "index": vertex,
                prefix + "evidence": ALL_EVIDENCE,
                prefix + "projection_id": lineage * 100 + vertex + 1,
                prefix + "projection_context": 77,
                prefix + "transform_value": 99,
                prefix + "transform_lineage": lineage,
                prefix + "model_source": lineage * 100 + vertex + 2,
                prefix + "model_flow_source": lineage * 100 + vertex + 20,
                prefix + "model_flow_program": lineage * 100 + vertex + 30,
                prefix + "gp0_source": 0x1001 + vertex * 8,
                prefix + "dma_list": 0x801,
                prefix + "dma_node": 0x1001,
                prefix + "dma_ordinal": 7,
            }
        )
    return record("replay.visibility_owner", frame, **fields)


def runtime_key(owner_record):
    return _runtime_dispatch_key(_owner_fields(owner_record, "test owner"))


def runtime_entry(component, owner_record):
    return {
        "component_sha256": component,
        **_runtime_dispatch_key_report_entry(runtime_key(owner_record)),
    }


def base_lineage(frame=7):
    base_owners = (owner(frame, 1, 11), owner(frame, 3, 33), owner(frame, 4, 44))
    entries = [
        runtime_entry(component, owner_record)
        for component, owner_record in zip(
            (COMPONENT_A, COMPONENT_B, COMPONENT_C), base_owners
        )
    ]
    return {
        "schema": "ps1-visibility-lineage-audit/v1",
        "valid": True,
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
                "entries": entries,
                "entry_count": len(entries),
                "components": 3,
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


def refinement(base, attachment_owner):
    base_dispatch, base_summary = _parse_topology_dispatch(base)
    attachment_key = runtime_key(attachment_owner)
    attachment_entry = _runtime_dispatch_key_report_entry(attachment_key)
    entries = list(base["projection_incidence_topology"]["runtime_dispatch"]["entries"])
    entries.append({"component_sha256": COMPONENT_C, **attachment_entry})
    return {
        "schema": "ps1-visibility-runtime-dispatch-refinement/v1",
        "valid": True,
        "base_topology_source_sha256": _canonical_sha256(base),
        "base_runtime_dispatch": base_summary,
        "fingerprint_evidence": {
            "capture_count": 2,
            "required_compact_fingerprints": 3,
            "cross_capture_dag_mismatches": 0,
            "all_required_fingerprints_cross_validated": True,
        },
        "attachments": [
            {
                "endpoint_dispatch_sha256": _signature_id(attachment_key),
                "complete_triangle_sha256": "4" * 64,
                "anchor_component_sha256": COMPONENT_C,
                "attachment_proof_sha256": "5" * 64,
                "exact_projection_relations": 1,
                "heavy_captures": 2,
                "runtime_dispatch_entry": attachment_entry,
                "base_component_hash_includes_attachment": False,
            }
        ],
        "attachment_count": 1,
        "runtime_dispatch": {
            "schema": "ps1-visibility-runtime-dispatch-map/v3",
            "target": "projection-incidence-component/v1",
            "basis": "test",
            "entries": entries,
            "entry_count": len(base_dispatch) + 1,
            "components": 3,
            "base_entries": len(base_dispatch),
            "cross_validated_attachment_entries": 1,
            "compact_model_fingerprint_schema": 1,
            "compact_model_fingerprints_cross_audited_against_full_dag": True,
            "ready_for_runtime_lookup": True,
            "anchor_component_hashes_recomputed": False,
            "attachment_membership_is_part_of_anchor_component_hash": False,
            "uses_screen_position": False,
            "uses_colour": False,
            "uses_observed_depth": False,
            "uses_numeric_transform_values": False,
            "uses_family_count_prior": False,
            "uses_integer_or_tenth_prior": False,
            "persistent_object_identity_certified": False,
        },
        "proof_scope": {
            "base_component_hashes_are_immutable_anchor_labels": True,
            "attachments_recompute_base_component_hashes": False,
            "exact_projection_relation_cross_validated": True,
            "compact_fingerprint_full_dag_cross_validated": True,
            "family_count_inferred": False,
            "integer_or_tenth_prior_used": False,
            "multiplier_fitted_or_recommended": False,
            "attachments_are_persistent_object_identity": False,
            "independent_post_dispatch_raster_validation_required": True,
        },
        "errors": [],
    }


def depth_bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def occluder(frame, physical, samples, ot_depth, physical_depth):
    return record(
        "replay.visibility_stack_occluder",
        frame,
        stack_schema=1,
        ot_owner=3,
        physical_owner=physical,
        samples=samples,
        first_atlas_x=40,
        first_atlas_y=80,
        minimum_ot_depth_bits=depth_bits(ot_depth - 0.05),
        maximum_ot_depth_bits=depth_bits(ot_depth),
        minimum_physical_depth_bits=depth_bits(physical_depth),
        maximum_physical_depth_bits=depth_bits(physical_depth + 0.05),
    )


def stack_records(frame, *, attachment_lineage=22, include_attachment_pair=True):
    pairs = [occluder(frame, 1, 2, 0.9, 0.2)]
    if include_attachment_pair:
        pairs.append(occluder(frame, 2, 1, 0.8, 0.1))
    audit = record(
        "replay.visibility_stack_audit",
        frame,
        stack_schema=1,
        status=0,
        complete=1,
        owners=3,
        atlas_x=40,
        atlas_y=80,
        width=2,
        height=1,
        attempted_records=10,
        record_capacity=16,
        overflow=0,
        retained_records=10,
        stale_records=0,
        boundary_records=0,
        duplicate_owner_records=0,
        inconsistent_duplicate_depth_records=0,
        ot_evidence_samples=2,
        complete_owned_samples=2,
        missing_ot_fragment_samples=0,
        ot_depth_mismatch_samples=0,
        invalid_owner_samples=0,
        invalid_depth_samples=0,
        occluded_ot_samples=2,
        maximum_distinct_owners_per_sample=3,
        occluder_pairs=len(pairs),
    )
    catalog = record(
        "replay.visibility_catalog",
        frame,
        catalog_schema=1,
        owner_schema=2,
        owners=3,
        eligible_owners=3,
        emitted_owners=3,
        conflict_referenced_owners=0,
    )
    return [
        audit,
        *pairs,
        catalog,
        owner(frame, 1, 11),
        owner(frame, 2, attachment_lineage),
        owner(frame, 3, 33),
    ]


class VisibilityStackConstraintTests(unittest.TestCase):
    def fixture(self):
        base = base_lineage()
        discovery = stack_records(7)
        holdout = stack_records(8)
        refined = refinement(base, discovery[-2])
        return base, refined, discovery, holdout

    def test_cross_validates_exact_bounds_and_counts_attachment_gain(self):
        base, refined, discovery, holdout = self.fixture()

        report = analyze_visibility_stack_constraints(
            base, refined, discovery, holdout
        )

        self.assertTrue(report["valid"])
        self.assertTrue(
            report["constraint_graph"][
                "constraint_system_ready_for_candidate_testing"
            ]
        )
        self.assertEqual(report["dispatch"]["refined_entries"], 4)
        self.assertEqual(
            report["coverage"][
                "attachment_mapping_gain_endpoint_sample_memberships"
            ],
            2,
        )
        self.assertEqual(
            report["constraint_graph"]["directed_component_constraints"], 2
        )
        attached = next(
            value
            for value in report["constraint_graph"]["constraints"]
            if value["farther_depth_component_sha256"] == COMPONENT_C
        )
        expected = Fraction(
            struct.unpack("<f", struct.pack("<f", 0.8))[0]
        ) / Fraction(struct.unpack("<f", struct.pack("<f", 0.1))[0])
        self.assertEqual(
            attached["conservative_lower_bound"]["numerator"],
            expected.numerator,
        )
        self.assertEqual(
            attached["conservative_lower_bound"]["denominator"],
            expected.denominator,
        )
        self.assertFalse(
            attached["maximum_bound_witness"][
                "extrema_are_same_sample_witness"
            ]
        )
        self.assertFalse(report["proof_scope"]["family_count_inferred"])
        self.assertFalse(report["proof_scope"]["integer_or_tenth_prior_used"])

    def test_one_set_component_edge_remains_unassessed(self):
        base, refined, discovery, _ = self.fixture()
        holdout = stack_records(8, include_attachment_pair=False)

        report = analyze_visibility_stack_constraints(
            base, refined, discovery, holdout
        )

        self.assertEqual(
            report["constraint_graph"]["single_validation_set_constraints"], 1
        )
        self.assertFalse(
            report["constraint_graph"][
                "constraint_system_ready_for_candidate_testing"
            ]
        )

    def test_unknown_holdout_endpoint_fails_closed_without_heuristic_attachment(self):
        base, refined, discovery, _ = self.fixture()
        holdout = stack_records(8, attachment_lineage=222)

        report = analyze_visibility_stack_constraints(
            base, refined, discovery, holdout
        )

        self.assertEqual(
            report["holdout"]["refined_unmapped_endpoint_sample_memberships"],
            1,
        )
        self.assertEqual(report["holdout"]["unresolved_distinct_runtime_keys"], 1)
        self.assertFalse(
            report["constraint_graph"][
                "constraint_system_ready_for_candidate_testing"
            ]
        )

    def test_rejects_prior_contamination_in_refined_dispatch(self):
        base, refined, discovery, holdout = self.fixture()
        contaminated = copy.deepcopy(refined)
        contaminated["runtime_dispatch"]["uses_integer_or_tenth_prior"] = True

        with self.assertRaisesRegex(ValueError, "contaminated"):
            analyze_visibility_stack_constraints(
                base, contaminated, discovery, holdout
            )

    def test_cycle_detection_reports_strong_components_deterministically(self):
        cycles = _cyclic_strong_components(
            {
                COMPONENT_A: {COMPONENT_B},
                COMPONENT_B: {COMPONENT_A},
            },
            {COMPONENT_C},
        )

        self.assertEqual(cycles, [[COMPONENT_A, COMPONENT_B], [COMPONENT_C]])


if __name__ == "__main__":
    unittest.main()
