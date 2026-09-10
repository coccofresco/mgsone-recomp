import pathlib
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from visibility_lineage_trace import _owner_fields, _runtime_dispatch_key
from visibility_stack_trace import analyze_visibility_stack_records


COMPONENTS = ("1" * 64, "2" * 64, "3" * 64)
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


def topology_lineage(owner_records):
    entries = []
    for component, owner_record in zip(COMPONENTS, owner_records):
        key = _runtime_dispatch_key(_owner_fields(owner_record, "test owner"))
        _, transform, triangle, vertices = key
        entries.append(
            {
                "component_sha256": component,
                "transform_lineage_hex": f"{transform:016x}",
                "triangle_index": triangle,
                "vertices": [
                    {
                        "index": index,
                        "model_source_present": present,
                        "model_source_hex": f"{source:016x}",
                        "model_flow_source_hex": f"{flow_source:016x}",
                        "model_flow_program_hex": f"{flow_program:016x}",
                        "gp0_node_offset": offset,
                    }
                    for (
                        index,
                        present,
                        source,
                        flow_source,
                        flow_program,
                        offset,
                    ) in vertices
                ],
            }
        )
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
                "components": len(entries),
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


def depth_bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def audit(frame, *, status=0, complete=1, attempted=10, capacity=16, overflow=0):
    return record(
        "replay.visibility_stack_audit",
        frame,
        stack_schema=1,
        status=status,
        complete=complete,
        owners=3,
        atlas_x=40,
        atlas_y=80,
        width=2,
        height=1,
        attempted_records=attempted,
        record_capacity=capacity,
        overflow=overflow,
        retained_records=min(attempted, capacity),
        stale_records=0,
        boundary_records=0,
        duplicate_owner_records=0,
        inconsistent_duplicate_depth_records=0,
        ot_evidence_samples=2 if complete else 0,
        complete_owned_samples=2 if complete else 0,
        missing_ot_fragment_samples=0,
        ot_depth_mismatch_samples=0,
        invalid_owner_samples=0,
        invalid_depth_samples=0,
        occluded_ot_samples=2 if complete else 0,
        maximum_distinct_owners_per_sample=3 if complete else 0,
        occluder_pairs=2 if complete else 0,
    )


def occluder(frame, physical, samples):
    return record(
        "replay.visibility_stack_occluder",
        frame,
        stack_schema=1,
        ot_owner=3,
        physical_owner=physical,
        samples=samples,
        first_atlas_x=40,
        first_atlas_y=80,
        minimum_ot_depth_bits=depth_bits(0.8),
        maximum_ot_depth_bits=depth_bits(0.9),
        minimum_physical_depth_bits=depth_bits(0.1 * physical),
        maximum_physical_depth_bits=depth_bits(0.2 * physical),
    )


def catalog(frame, emitted=3):
    return record(
        "replay.visibility_catalog",
        frame,
        catalog_schema=1,
        owner_schema=2,
        owners=3,
        eligible_owners=3,
        emitted_owners=emitted,
        conflict_referenced_owners=0,
    )


def complete_fixture(frame=7):
    owner_records = [owner(frame, token, token) for token in range(1, 4)]
    records = [
        audit(frame),
        occluder(frame, 1, 2),
        occluder(frame, 2, 1),
        catalog(frame),
        *owner_records,
    ]
    return records, topology_lineage(owner_records)


class VisibilityStackTraceTests(unittest.TestCase):
    def test_maps_every_latent_occluder_without_a_layer_or_family_limit(self):
        records, topology = complete_fixture()
        report = analyze_visibility_stack_records(records, topology)

        self.assertTrue(report["valid"], report["errors"])
        self.assertEqual(report["complete_frames"], 1)
        self.assertEqual(len(report["occluder_dispatch_pairs"]), 2)
        self.assertEqual(len(report["occluder_topology_pairs"]), 2)
        self.assertTrue(
            report["constraint_coverage"]["ready_for_constraint_refinement"]
        )
        self.assertFalse(report["proof_scope"]["fixed_layer_limit_used"])
        self.assertFalse(report["proof_scope"]["family_count_inferred"])
        self.assertFalse(report["proof_scope"]["integer_or_tenth_prior_used"])

    def test_overflow_is_validated_but_rejected_as_incomplete_evidence(self):
        frame = 8
        owner_records = [owner(frame, token, token) for token in range(1, 4)]
        records = [
            audit(
                frame,
                status=4,
                complete=0,
                attempted=20,
                capacity=10,
                overflow=1,
            ),
            catalog(frame),
            *owner_records,
        ]
        report = analyze_visibility_stack_records(records)

        self.assertFalse(report["valid"])
        self.assertEqual(report["overflow_frames"], 1)
        self.assertTrue(any("at least 20 records" in e for e in report["errors"]))

    def test_missing_pair_endpoint_fails_closed(self):
        records, topology = complete_fixture()
        records = [
            item
            for item in records
            if not (
                item["event"] == "replay.visibility_owner"
                and item["owner"] == 1
            )
        ]
        for item in records:
            if item["event"] == "replay.visibility_catalog":
                item["emitted_owners"] = 2
        report = analyze_visibility_stack_records(records, topology)

        self.assertFalse(report["valid"])
        self.assertTrue(any("endpoint lacks owner record" in e for e in report["errors"]))


if __name__ == "__main__":
    unittest.main()
