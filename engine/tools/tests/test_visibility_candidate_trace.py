import pathlib
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from visibility_candidate_trace import analyze_candidate_records
from visibility_lineage_trace import _owner_fields, _runtime_dispatch_key


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


def owner(frame, token, lineage, evidence=ALL_EVIDENCE):
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
        "complete_evidence": evidence,
        "coherent_projection_context": int(bool(evidence & 1)),
        "coherent_transform_lineage": int(bool(evidence & 2)),
        "coherent_transform_value": int(bool(evidence & 1)),
        "coherent_dma_traversal": int(bool(evidence & 16)),
    }
    for vertex in range(3):
        prefix = f"v{vertex}_"
        fields.update(
            {
                prefix + "index": vertex,
                prefix + "evidence": evidence,
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
        if not evidence & 32:
            fields[prefix + "model_flow_source"] = 0
            fields[prefix + "model_flow_program"] = 0
    return record("replay.visibility_owner", frame, **fields)


def manifest(owner_records):
    lines = [
        "schema ps1-visibility-runtime-manifest/v1",
        "fingerprint_schema 1",
        "scope hypothesis",
        "family_count_prior_used 0",
        "integer_or_tenth_prior_used 0",
    ]
    for component in COMPONENTS[: len(owner_records)]:
        lines.append(f"component {component} 1 1")
    for component, owner_record in zip(COMPONENTS, owner_records):
        key = _runtime_dispatch_key(_owner_fields(owner_record, "test owner"))
        _, transform, triangle, vertices = key
        fields = ["entry", component, f"{transform:016x}", str(triangle)]
        for index, present, source, flow_source, flow_program, offset in vertices:
            fields += [
                str(index),
                "1" if present else "0",
                f"{source:016x}",
                f"{flow_source:016x}",
                f"{flow_program:016x}",
                str(offset),
            ]
        lines.append(" ".join(fields))
    lines.append(f"end {len(owner_records)} {len(owner_records)}")
    return "\n".join(lines) + "\n"


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


def outcome(frame, classification, ot, strict, lifted, samples=1):
    return record(
        "replay.visibility_candidate_outcome",
        frame,
        outcome_schema=1,
        candidate_mode=2,
        classification=classification,
        ot_owner=ot,
        strict_owner=strict,
        lifted_owner=lifted,
        samples=samples,
        first_atlas_x=40,
        first_atlas_y=80,
        minimum_ot_depth_bits=depth_bits(0.8),
        maximum_ot_depth_bits=depth_bits(0.8),
        minimum_strict_depth_bits=depth_bits(0.2),
        maximum_strict_depth_bits=depth_bits(0.2),
        minimum_lifted_depth_bits=depth_bits(0.3),
        maximum_lifted_depth_bits=depth_bits(0.3),
    )


def fixture():
    frame = 10
    owner_records = [owner(frame, index + 1, index + 1) for index in range(3)]
    records = [
        record(
            "replay.visibility_candidate_audit",
            frame,
            candidate_schema=2,
            candidate_mode=2,
            identity_control=0,
            manifest_generation=1,
            status=0,
            raster_gate_pass=0,
            samples=6,
            baseline_agreement=2,
            preserved_agreement=1,
            baseline_conflicts=4,
            resolved_to_ot=2,
            retained_by_strict=1,
            changed_winner=1,
            baseline_coverage_divergence=0,
            candidate_coverage_divergence=0,
            regressions=1,
            indeterminate=0,
            inconsistent=0,
            conflict_pairs=2,
        ),
        record(
            "replay.visibility_catalog",
            frame,
            catalog_schema=1,
            owner_schema=2,
            owners=3,
            eligible_owners=3,
            emitted_owners=3,
            conflict_referenced_owners=3,
        ),
        *owner_records,
        outcome(frame, 2, 2, 1, 1),
        outcome(frame, 3, 2, 1, 3),
        outcome(frame, 6, 1, 1, 3),
    ]
    return records, owner_records


class VisibilityCandidateTraceTest(unittest.TestCase):
    def test_joins_every_failure_triplet_to_manifest_components(self):
        records, owner_records = fixture()
        report = analyze_candidate_records(records, manifest(owner_records))

        self.assertTrue(report["valid"], report["errors"])
        self.assertFalse(report["raster_gate_pass_all_frames"])
        self.assertEqual(report["owner_dispatch"]["mapped"], 3)
        self.assertEqual(report["owner_dispatch"]["unmapped_keys"], 0)
        self.assertEqual(len(report["failure_component_triplets"]), 3)
        changed = next(
            value
            for value in report["failure_component_triplets"]
            if value["classification"] == "conflict-changed-winner"
        )
        self.assertEqual(changed["ot_component"], COMPONENTS[1])
        self.assertEqual(changed["strict_component"], COMPONENTS[0])
        self.assertEqual(changed["lifted_component"], COMPONENTS[2])

    def test_labels_failures_with_independent_partial_topology(self):
        records, owner_records = fixture()
        report = analyze_candidate_records(
            records,
            manifest(owner_records),
            topology_lineage(owner_records),
        )

        self.assertTrue(report["valid"], report["errors"])
        self.assertEqual(report["topology_partition"]["components"], 3)
        self.assertEqual(report["owner_topology_dispatch"]["mapped"], 3)
        changed = next(
            value
            for value in report["failure_topology_triplets"]
            if value["classification"] == "conflict-changed-winner"
        )
        self.assertEqual(changed["ot_topology_component"], COMPONENTS[1])
        self.assertEqual(changed["strict_topology_component"], COMPONENTS[0])
        self.assertEqual(changed["lifted_topology_component"], COMPONENTS[2])
        self.assertFalse(
            report["proof_scope"][
                "topology_component_count_inferred_as_family_count"
            ]
        )

    def test_distinguishes_complete_unknown_and_incomplete_keys(self):
        records, owner_records = fixture()
        report = analyze_candidate_records(records, manifest(owner_records[:2]))

        self.assertTrue(report["valid"], report["errors"])
        self.assertEqual(report["owner_dispatch"]["mapped"], 2)
        self.assertEqual(report["owner_dispatch"]["unmapped_keys"], 1)
        self.assertEqual(report["owner_dispatch"]["unknown_distinct_dispatch_keys"], 1)
        self.assertTrue(
            any(
                value["lifted_component"] == "unmapped-key"
                for value in report["failure_component_triplets"]
            )
        )

        incomplete_records, _ = fixture()
        incomplete = owner(10, 3, 3, evidence=ALL_EVIDENCE & ~32)
        incomplete_records[4] = incomplete
        report = analyze_candidate_records(
            incomplete_records, manifest(owner_records[:2])
        )
        self.assertTrue(report["valid"], report["errors"])
        self.assertEqual(report["owner_dispatch"]["incomplete_keys"], 1)

    def test_rejects_outcome_totals_or_prior_contamination(self):
        records, owner_records = fixture()
        records[-1]["samples"] = 2
        report = analyze_candidate_records(records, manifest(owner_records))
        self.assertFalse(report["valid"])
        self.assertIn("outcome aggregates", " ".join(report["errors"]))

        contaminated = manifest(owner_records).replace(
            "family_count_prior_used 0", "family_count_prior_used 1"
        )
        with self.assertRaisesRegex(ValueError, "contaminated"):
            analyze_candidate_records(fixture()[0], contaminated)

        dirty_topology = topology_lineage(owner_records)
        dirty_topology["advisory_priors"][
            "family_count_is_correctness_limit"
        ] = True
        with self.assertRaisesRegex(ValueError, "contaminated"):
            analyze_candidate_records(
                fixture()[0], manifest(owner_records), dirty_topology
            )


if __name__ == "__main__":
    unittest.main()
