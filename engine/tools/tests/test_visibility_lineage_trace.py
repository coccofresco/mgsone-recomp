import json
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))

from depth_family_trace import AFFINE_FIELDS, runtime_model_fingerprints
from visibility_lineage_trace import (
    _owner_fields,
    _runtime_dispatch_key,
    _signature_id,
    _summarize_numeric_affine_cohorts,
    _summarize_visibility_gauge_constraints,
    analyze_visibility_lineages,
)


ALL_EVIDENCE = 0x3F
TRANSPORT_EVIDENCE = 0x01 | 0x02 | 0x08 | 0x10


def signed64(value):
    value &= (1 << 64) - 1
    return value - (1 << 64) if value & (1 << 63) else value


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


def audit(frame, owners=2):
    return record(
        "replay.visibility_audit",
        frame,
        status=0,
        owners=owners,
        atlas_x=40,
        atlas_y=80,
        width=2,
        height=1,
        samples=2,
        agreement=1,
        background_agreement=0,
        owned_agreement=1,
        ot_owned=2,
        depth_owned=2,
        conflicts=1,
        coverage_divergence=0,
        indeterminate=0,
        inconsistent=0,
        conflict_pairs=1,
    )


def catalog(
    frame, owners, eligible, emitted, conflict_referenced, *, owner_schema=1
):
    return record(
        "replay.visibility_catalog",
        frame,
        catalog_schema=1,
        owner_schema=owner_schema,
        owners=owners,
        eligible_owners=eligible,
        emitted_owners=emitted,
        conflict_referenced_owners=conflict_referenced,
    )


def owner(
    frame,
    token,
    lineage,
    *,
    source_ordinal,
    packet_sequence,
    submission_id,
    triangle_index,
    evidence=ALL_EVIDENCE,
    owner_schema=1,
):
    indices = (0, 1, 2) if triangle_index == 0 else (0, 2, 3)
    fields = {
        "owner_schema": owner_schema,
        "owner": token,
        "packet_sequence": packet_sequence,
        "submission_id": submission_id,
        "source_ordinal": source_ordinal,
        "frame_index": 0,
        "operation_index": source_ordinal,
        "triangle_index": triangle_index,
        "route": 0,
        "opaque": 1,
        "fully_precise": 1,
        "authentic_depth": 1,
        "comparable": 1,
        "valid_topology": 1,
        "complete_evidence": evidence,
        "coherent_projection_context": int(bool(evidence & 0x01)),
        "coherent_transform_lineage": int(bool(evidence & 0x02)),
        "coherent_dma_traversal": int(bool(evidence & 0x10)),
    }
    if owner_schema >= 2:
        fields["coherent_transform_value"] = int(bool(evidence & 0x01))
    for vertex, index in enumerate(indices):
        prefix = f"v{vertex}_"
        fields.update(
            {
                prefix + "index": index,
                prefix + "evidence": evidence,
                prefix + "projection_id": frame * 100 + token * 10 + vertex + 1,
                prefix + "projection_context": 77,
                prefix + "transform_lineage": lineage * 1000 + 1,
                prefix + "model_source": lineage * 1000 + vertex + 2,
                prefix + "model_flow_source": lineage * 1000 + vertex + 20,
                prefix + "model_flow_program": lineage * 1000 + vertex + 30,
                prefix + "gp0_source": lineage * 1000 + vertex + 40,
                prefix + "dma_list": lineage * 1000 + 50,
                prefix + "dma_node": lineage * 1000 + 60,
                prefix + "dma_ordinal": lineage + 70,
            }
        )
        if owner_schema >= 2:
            fields[prefix + "transform_value"] = lineage * 1000 + 99
        if not evidence & 0x01:
            fields[prefix + "projection_id"] = 0
            fields[prefix + "projection_context"] = 0
            if owner_schema >= 2:
                fields[prefix + "transform_value"] = 0
        if not evidence & 0x02:
            fields[prefix + "transform_lineage"] = 0
        if not evidence & 0x04:
            fields[prefix + "model_source"] = 0
        if not evidence & 0x20:
            fields[prefix + "model_flow_source"] = 0
            fields[prefix + "model_flow_program"] = 0
        if not evidence & 0x08:
            fields[prefix + "gp0_source"] = 0
        if not evidence & 0x10:
            fields[prefix + "dma_list"] = 0
            fields[prefix + "dma_node"] = 0
            fields[prefix + "dma_ordinal"] = 0
    return record("replay.visibility_owner", frame, **fields)


def conflict(frame, ot, depth):
    return record(
        "replay.visibility_conflict",
        frame,
        ot_owner=ot["owner"],
        depth_owner=depth["owner"],
        samples=1,
        ot_source_ordinal=ot["source_ordinal"],
        depth_source_ordinal=depth["source_ordinal"],
        ot_submission_id=ot["submission_id"],
        depth_submission_id=depth["submission_id"],
        ot_frame_index=ot["frame_index"],
        depth_frame_index=depth["frame_index"],
        ot_operation_index=ot["operation_index"],
        depth_operation_index=depth["operation_index"],
        ot_triangle_index=ot["triangle_index"],
        depth_triangle_index=depth["triangle_index"],
        ot_packet_sequence=ot["packet_sequence"],
        depth_packet_sequence=depth["packet_sequence"],
        min_x=40,
        min_y=80,
        max_x=40,
        max_y=80,
        min_ot_depth_bits=0x3F400000,
        max_ot_depth_bits=0x3F400000,
        min_strict_depth_bits=0x3E800000,
        max_strict_depth_bits=0x3E800000,
    )


def frame_records(
    frame,
    first_lineage=1,
    second_lineage=2,
    submission_id=1,
    owner_schema=1,
):
    depth = owner(
        frame,
        1,
        first_lineage,
        source_ordinal=10,
        packet_sequence=frame * 10 + 1,
        submission_id=submission_id,
        triangle_index=0,
        owner_schema=owner_schema,
    )
    ot = owner(
        frame,
        2,
        second_lineage,
        source_ordinal=20,
        packet_sequence=frame * 10 + 2,
        submission_id=submission_id,
        triangle_index=1,
        owner_schema=owner_schema,
    )
    return [audit(frame), depth, ot, conflict(frame, ot, depth)]


def catalog_frame_records(frame, submission_id):
    records = frame_records(frame, submission_id=submission_id)
    third = owner(
        frame,
        3,
        3,
        source_ordinal=30,
        packet_sequence=frame * 10 + 3,
        submission_id=submission_id,
        triangle_index=0,
    )
    return [
        audit(frame, owners=3),
        records[1],
        records[2],
        third,
        records[3],
        catalog(frame, owners=3, eligible=3, emitted=3, conflict_referenced=2),
    ]


def share_first_projection(records):
    records[2]["v0_projection_id"] = records[1]["v0_projection_id"]
    return records


def provenance_stream(frame, owners, heavy):
    expressions = {}
    projection_records = []
    gp0_by_owner = []
    next_expression = 1
    for item in owners:
        gp0_records = []
        transform = item["v0_transform_lineage"]
        affine = (
            transform,
            2,
            0,
            3,
            transform,
            0,
            0,
            0,
            5,
            6,
            7,
            8,
        )
        for vertex_index in range(3):
            prefix = f"v{vertex_index}_"
            xy_expression = next_expression
            z_expression = next_expression + 1
            next_expression += 2
            for expression_id, address in (
                (xy_expression, 0x80010000 + xy_expression * 4),
                (z_expression, 0x80010000 + z_expression * 4),
            ):
                expressions[expression_id] = record(
                    "mips.value_expr",
                    frame,
                    expression_id=expression_id,
                    op=2,
                    pc=0x80020000 + expression_id * 4,
                    raw=0x8C020000,
                    complete=1,
                    lhs=0,
                    rhs=0,
                    address=address,
                    width=4,
                    signed=0,
                    value=expression_id,
                )

            projection = record(
                "gte.projection",
                frame,
                projection_id=item[prefix + "projection_id"],
                projection_context=item[prefix + "projection_context"],
                transform_lineage=item[prefix + "transform_lineage"],
                value_flow_complete=int(heavy),
                model_fingerprint_complete=int(heavy),
                model_source_fingerprint=0,
                model_program_fingerprint=0,
                vxy_expr=xy_expression,
                vz_expr=z_expression,
            )
            projection.update(dict(zip(AFFINE_FIELDS, affine)))
            fingerprints = runtime_model_fingerprints(projection, expressions)
            assert fingerprints is not None
            if heavy:
                projection["model_source_fingerprint"] = signed64(fingerprints[0])
                projection["model_program_fingerprint"] = signed64(fingerprints[1])
            else:
                projection["vxy_expr"] = 0
                projection["vz_expr"] = 0
            projection_records.append(projection)

            gp0_records.append(
                record(
                    "gpu.gp0_word",
                    frame,
                    word=0x00100010 + vertex_index,
                    word_address=item[prefix + "gp0_source"] - 1,
                    path=1,
                    ram_lookup=1,
                    submission_id=item["submission_id"],
                    command_word_index=vertex_index,
                    projection_id=item[prefix + "projection_id"],
                    projection_context=item[prefix + "projection_context"],
                    intrinsic_provenance=(
                        TRANSPORT_EVIDENCE | (0x80 if heavy else 0)
                    ),
                    transform_lineage=item[prefix + "transform_lineage"],
                    model_source=-1,
                    model_source_fingerprint=(
                        signed64(fingerprints[0]) if heavy else 0
                    ),
                    model_program_fingerprint=(
                        signed64(fingerprints[1]) if heavy else 0
                    ),
                    dma_provenance=1,
                    dma_list_start=item[prefix + "dma_list"] - 1,
                    dma_node_address=item[prefix + "dma_node"] - 1,
                    dma_traversal_ordinal=item[prefix + "dma_ordinal"],
                    signed_projection=1,
                )
            )
        gp0_by_owner.append(gp0_records)

    result = list(expressions.values()) if heavy else []
    result.extend(projection_records)
    for item, gp0_records in zip(owners, gp0_by_owner):
        result.append(
            record(
                "gpu.dma_node",
                frame,
                submission_id=item["submission_id"],
                list_start=item["v0_dma_list"] - 1,
                node_address=item["v0_dma_node"] - 1,
                traversal_ordinal=item["v0_dma_ordinal"],
                words=8,
            )
        )
        result.extend(gp0_records)
    return result


def affine_occurrence(capture, frame, validation_set, name, affine):
    return {
        "capture": capture,
        "frame": frame,
        "validation_set": validation_set,
        "numeric_affine": tuple(affine),
        "signatures": {"complete_triangle": ("test-triangle/v1", name)},
    }


class VisibilityLineageTraceTest(unittest.TestCase):
    def analyze(self, discovery, holdout):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            discovery_path = root / "discovery.jsonl"
            holdout_path = root / "holdout.jsonl"
            discovery_path.write_text(
                "".join(json.dumps(item) + "\n" for item in discovery),
                encoding="utf-8",
            )
            holdout_path.write_text(
                "".join(json.dumps(item) + "\n" for item in holdout),
                encoding="utf-8",
            )
            return analyze_visibility_lineages([discovery_path], [holdout_path])

    def test_reports_recurrent_complete_candidates_without_certifying_identity(self):
        report = self.analyze(
            frame_records(7, submission_id=11),
            frame_records(8, submission_id=12),
        )
        self.assertTrue(report["valid"])
        complete = report["levels"]["complete_triangle"]
        self.assertEqual(complete["distinct_signatures"], 2)
        self.assertEqual(complete["cross_validation_unambiguous_candidates"], 2)
        self.assertFalse(
            report["inference_scope"]["persistent_object_identity_inferred"]
        )
        self.assertFalse(report["inference_scope"]["family_count_inferred"])
        endpoints = report["conflict_endpoint_coverage"]
        self.assertEqual(endpoints["raw_directed_conflict_records"], 2)
        self.assertEqual(endpoints["joined_endpoint_records"], 2)
        self.assertTrue(endpoints["all_raw_conflicts_have_joined_endpoints"])
        self.assertEqual(endpoints["both_numeric_affine_records"], 0)
        self.assertEqual(
            endpoints["endpoint_level_coverage"]["complete_triangle"][
                "both_records"
            ],
            2,
        )
        self.assertEqual(endpoints["complete_endpoint_constraint_frames"], 0)
        self.assertFalse(endpoints["frame_local_constraint_inputs_ready"])
        self.assertFalse(endpoints["post_lift_constraints_ready"])

    def test_rejects_conflict_without_raw_owner_payload(self):
        discovery = frame_records(7)
        del discovery[2]
        report = self.analyze(discovery, frame_records(8))
        self.assertFalse(report["valid"])
        self.assertTrue(
            any("missing replay.visibility_owner" in error for error in report["errors"])
        )

    def test_rejects_owner_evidence_contract_mismatch(self):
        discovery = frame_records(7)
        discovery[1]["complete_evidence"] = 0
        report = self.analyze(discovery, frame_records(8))
        self.assertFalse(report["valid"])
        self.assertTrue(
            any("complete-evidence" in error for error in report["errors"])
        )

    def test_owner_schema_two_validates_frame_local_transform_value(self):
        discovery = frame_records(7, owner_schema=2)
        holdout = frame_records(8, owner_schema=2)
        report = self.analyze(discovery, holdout)
        self.assertTrue(report["valid"], report["errors"])

        discovery[1]["v2_transform_value"] += 1
        report = self.analyze(discovery, holdout)
        self.assertFalse(report["valid"])
        self.assertTrue(
            any("transform-value coherence" in error for error in report["errors"])
        )

    def test_runtime_dispatch_requires_compact_model_construction_lineage(self):
        first = owner(
            7,
            1,
            1,
            source_ordinal=10,
            packet_sequence=71,
            submission_id=1,
            triangle_index=0,
        )
        second = dict(first)
        second["v1_model_flow_program"] += 1
        first_key = _runtime_dispatch_key(_owner_fields(first, "first"))
        second_key = _runtime_dispatch_key(_owner_fields(second, "second"))
        self.assertIsNotNone(first_key)
        self.assertIsNotNone(second_key)
        self.assertNotEqual(first_key, second_key)

        incomplete = owner(
            7,
            1,
            1,
            source_ordinal=10,
            packet_sequence=71,
            submission_id=1,
            triangle_index=0,
            evidence=ALL_EVIDENCE & ~0x20,
        )
        self.assertIsNone(
            _runtime_dispatch_key(_owner_fields(incomplete, "incomplete"))
        )

    def test_catalog_schema_must_match_owner_records(self):
        discovery = frame_records(7, owner_schema=2)
        discovery.append(catalog(7, 2, 2, 2, 2, owner_schema=1))
        report = self.analyze(discovery, frame_records(8, owner_schema=2))
        self.assertFalse(report["valid"])
        self.assertTrue(
            any(
                "owner schema disagrees with visibility owner records" in error
                for error in report["errors"]
            )
        )

    def test_duplicate_exact_signature_is_reported_as_ambiguous(self):
        discovery = frame_records(7, first_lineage=1, second_lineage=1)
        holdout = frame_records(8, first_lineage=1, second_lineage=1)
        for records in (discovery, holdout):
            # Make both catalog entries exactly the same categorical triangle
            # while retaining distinct immutable replay ordinals.
            for field, value in list(records[1].items()):
                if field.startswith("v") or field == "triangle_index":
                    records[2][field] = value
            records[3]["ot_triangle_index"] = records[2]["triangle_index"]
        report = self.analyze(discovery, holdout)
        self.assertTrue(report["valid"])
        complete = report["levels"]["complete_triangle"]
        self.assertEqual(complete["distinct_signatures"], 1)
        self.assertEqual(complete["ambiguous_within_frame_signatures"], 1)
        self.assertEqual(complete["cross_validation_unambiguous_candidates"], 0)

    def test_missing_geometry_evidence_stays_incomplete_not_guessed(self):
        evidence = ALL_EVIDENCE & ~0x04 & ~0x20
        discovery = frame_records(7)
        holdout = frame_records(8)
        for records in (discovery, holdout):
            for index in (1, 2):
                replacement = owner(
                    records[index]["frame"],
                    records[index]["owner"],
                    records[index]["owner"],
                    source_ordinal=records[index]["source_ordinal"],
                    packet_sequence=records[index]["packet_sequence"],
                    submission_id=records[index]["submission_id"],
                    triangle_index=records[index]["triangle_index"],
                    evidence=evidence,
                )
                records[index] = replacement
            records[3] = conflict(records[0]["frame"], records[2], records[1])
        report = self.analyze(discovery, holdout)
        self.assertTrue(report["valid"])
        self.assertEqual(
            report["levels"]["complete_triangle"]["distinct_signatures"], 0
        )
        self.assertEqual(
            report["conflict_endpoint_coverage"]["endpoint_level_coverage"][
                "complete_triangle"
            ]["both_records"],
            0,
        )

    def test_triangle_candidates_are_never_compared_to_family_prior(self):
        discovery = []
        holdout = []
        for offset in range(3):
            discovery.extend(
                frame_records(
                    10 + offset,
                    first_lineage=offset * 2 + 1,
                    second_lineage=offset * 2 + 2,
                    submission_id=20 + offset,
                )
            )
            holdout.extend(
                frame_records(
                    20 + offset,
                    first_lineage=offset * 2 + 1,
                    second_lineage=offset * 2 + 2,
                    submission_id=30 + offset,
                )
            )
        report = self.analyze(discovery, holdout)
        self.assertTrue(report["valid"])
        priors = report["advisory_priors"]
        self.assertEqual(priors["complete_cross_validation_triangle_candidates"], 6)
        self.assertFalse(priors["triangle_candidates_are_family_count"])
        self.assertFalse(priors["triangle_candidate_count_compared_to_family_prior"])
        self.assertFalse(priors["family_count_is_correctness_limit"])

    def test_complete_catalog_admits_non_conflict_eligible_owners(self):
        report = self.analyze(
            catalog_frame_records(7, submission_id=11),
            catalog_frame_records(8, submission_id=12),
        )

        self.assertTrue(report["valid"], report["errors"])
        self.assertEqual(report["owner_occurrences"], 6)
        self.assertEqual(report["conflict_referenced_owner_occurrences"], 4)
        self.assertEqual(report["catalog_only_owner_occurrences"], 2)
        self.assertEqual(
            report["levels"]["complete_triangle"][
                "cross_validation_unambiguous_candidates"
            ],
            3,
        )
        for capture in report["captures"]:
            self.assertEqual(capture["referenced_owner_records"], 2)
            self.assertEqual(capture["selected_owner_records"], 3)
            self.assertEqual(capture["complete_catalog_frames"], 1)
            self.assertEqual(capture["complete_catalog_owner_records"], 3)
        self.assertTrue(
            report["inference_scope"][
                "all_audited_frames_have_complete_eligible_owner_catalog"
            ]
        )
        self.assertTrue(
            report["projection_incidence_topology"][
                "non_conflict_geometry_included"
            ]
        )
        self.assertFalse(
            report["projection_incidence_topology"][
                "components_cover_all_eligible_catalog_geometry"
            ]
        )
        self.assertFalse(
            report["projection_incidence_topology"][
                "components_cover_non_conflicting_geometry"
            ]
        )

    def test_incomplete_catalog_record_fails_closed(self):
        discovery = catalog_frame_records(7, submission_id=11)
        discovery[-1]["emitted_owners"] = 2

        report = self.analyze(
            discovery,
            catalog_frame_records(8, submission_id=12),
        )

        self.assertFalse(report["valid"])
        self.assertTrue(
            any("emitted-owner count" in error for error in report["errors"])
        )
        self.assertEqual(report["captures"][0]["complete_catalog_frames"], 0)

    def test_exact_projection_incidence_repeats_as_partial_topology(self):
        report = self.analyze(
            share_first_projection(frame_records(7, submission_id=11)),
            share_first_projection(frame_records(8, submission_id=12)),
        )
        self.assertTrue(report["valid"])
        topology = report["projection_incidence_topology"]
        self.assertEqual(topology["mapped_cross_validation_triangle_signatures"], 2)
        self.assertEqual(topology["stable_triangle_adjacencies"], 1)
        self.assertEqual(topology["stable_shared_vertex_relations"], 1)
        self.assertEqual(topology["contradictory_or_variable_relations"], 0)
        self.assertEqual(topology["stable_partial_component_sizes"], [2])
        dispatch = topology["runtime_dispatch"]
        self.assertEqual(
            dispatch["target"], "projection-incidence-component/v1"
        )
        self.assertTrue(dispatch["candidate_dispatch_ready"])
        self.assertEqual(dispatch["entry_count"], 2)
        self.assertEqual(dispatch["components"], 1)
        self.assertFalse(dispatch["uses_family_count_prior"])
        self.assertFalse(dispatch["uses_integer_or_tenth_prior"])
        topology_constraints = report[
            "projection_topology_visibility_constraints"
        ]
        self.assertEqual(topology_constraints["self_component_conflict_records"], 0)
        self.assertFalse(
            topology_constraints[
                "numeric_affine_used_only_for_frame_local_anchor_attachment"
            ]
        )
        self.assertFalse(
            topology_constraints["constraint_system_ready_for_candidate_testing"]
        )
        self.assertFalse(topology["component_count_is_object_count"])
        self.assertFalse(topology["component_count_is_family_count"])

    def test_changed_projection_incidence_is_not_certified(self):
        report = self.analyze(
            share_first_projection(frame_records(7, submission_id=11)),
            frame_records(8, submission_id=12),
        )
        self.assertTrue(report["valid"])
        topology = report["projection_incidence_topology"]
        self.assertEqual(topology["stable_triangle_adjacencies"], 0)
        self.assertEqual(topology["contradictory_or_variable_relations"], 1)
        self.assertFalse(topology["topology_consistent_across_assessed_pairs"])
        self.assertFalse(topology["persistent_object_identity_certified"])

    def test_exact_affine_partition_survives_changed_numeric_matrices(self):
        discovery_first = (1, 2, 0, 3, 4, 0, 0, 0, 5, 6, 7, 8)
        discovery_second = (9, 2, 0, 3, 4, 0, 0, 0, 5, 6, 7, 8)
        holdout_first = (11, 12, 0, 13, 14, 0, 0, 0, 15, 16, 17, 18)
        holdout_second = (19, 12, 0, 13, 14, 0, 0, 0, 15, 16, 17, 18)
        occurrences = [
            affine_occurrence(0, 7, "discovery", "a", discovery_first),
            affine_occurrence(0, 7, "discovery", "b", discovery_first),
            affine_occurrence(0, 7, "discovery", "c", discovery_second),
            affine_occurrence(1, 8, "holdout", "a", holdout_first),
            affine_occurrence(1, 8, "holdout", "b", holdout_first),
            affine_occurrence(1, 8, "holdout", "c", holdout_second),
        ]

        cohorts = _summarize_numeric_affine_cohorts(occurrences)

        self.assertEqual(
            cohorts["mapped_signatures_with_affine_in_both_validation_sets"], 3
        )
        self.assertEqual(cohorts["stable_same_affine_relations"], 1)
        self.assertEqual(cohorts["stable_different_affine_relations"], 2)
        self.assertEqual(cohorts["rejected_variable_or_contradictory_relations"], 0)
        self.assertEqual(cohorts["same_relation_component_sizes"], [2])
        self.assertEqual(
            cohorts["frame_partitions"][0]["exact_affine_partition_sizes"],
            [2, 1],
        )
        self.assertTrue(cohorts["relations_consistent_across_assessed_pairs"])
        self.assertFalse(cohorts["numeric_affine_values_used_as_persistent_identity"])
        self.assertFalse(cohorts["component_count_is_family_count"])

    def test_affine_partition_split_is_rejected(self):
        first = (1, 2, 0, 3, 4, 0, 0, 0, 5, 6, 7, 8)
        second = (9, 2, 0, 3, 4, 0, 0, 0, 5, 6, 7, 8)
        occurrences = [
            affine_occurrence(0, 7, "discovery", "a", first),
            affine_occurrence(0, 7, "discovery", "b", first),
            affine_occurrence(1, 8, "holdout", "a", first),
            affine_occurrence(1, 8, "holdout", "b", second),
        ]

        cohorts = _summarize_numeric_affine_cohorts(occurrences)

        self.assertEqual(cohorts["stable_same_affine_relations"], 0)
        self.assertEqual(cohorts["rejected_variable_or_contradictory_relations"], 1)
        self.assertEqual(cohorts["same_relation_components"], 0)
        self.assertFalse(cohorts["relations_consistent_across_assessed_pairs"])

    def test_non_tenth_proportional_affines_are_measured_not_filtered(self):
        base = (1, 2, 0, 3, 4, 0, 0, 0, 5, 6, 7, 8)
        times_three = tuple(value * 3 for value in base)
        times_five = tuple(value * 5 for value in base)
        occurrences = [
            affine_occurrence(0, 7, "discovery", "a", times_three),
            affine_occurrence(0, 7, "discovery", "b", times_five),
            affine_occurrence(1, 8, "holdout", "a", times_three),
            affine_occurrence(1, 8, "holdout", "b", times_five),
        ]

        cohorts = _summarize_numeric_affine_cohorts(occurrences)

        self.assertEqual(cohorts["exact_full_affine_proportional_groups"], 2)
        self.assertFalse(cohorts["integer_or_tenth_ratio_used_as_filter"])
        non_anchor = [
            ratio
            for group in cohorts["proportional_group_preview"]
            for ratio in group["exact_ratios_from_smallest_divisor"]
            if ratio["numerator"] != ratio["denominator"]
        ]
        self.assertTrue(non_anchor)
        self.assertTrue(
            any(not ratio["is_integer_or_tenth"] for ratio in non_anchor)
        )
        self.assertFalse(cohorts["exact_proportionality_is_scale_certification"])

    def test_equivalent_heavy_pass_enriches_only_verified_owner_vertices(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            discovery_path = root / "discovery.jsonl"
            heavy_path = root / "discovery-heavy.jsonl"
            holdout_path = root / "holdout.jsonl"

            discovery = frame_records(7)
            for index in (1, 2):
                original = discovery[index]
                discovery[index] = owner(
                    7,
                    original["owner"],
                    original["owner"],
                    source_ordinal=original["source_ordinal"],
                    packet_sequence=original["packet_sequence"],
                    submission_id=original["submission_id"],
                    triangle_index=original["triangle_index"],
                    evidence=TRANSPORT_EVIDENCE,
                )
            discovery[3] = conflict(7, discovery[2], discovery[1])
            light_stream = discovery + provenance_stream(
                7, discovery[1:3], heavy=False
            )
            heavy_stream = provenance_stream(7, discovery[1:3], heavy=True)
            holdout = frame_records(8)

            discovery_path.write_text(
                "".join(json.dumps(item) + "\n" for item in light_stream),
                encoding="utf-8",
            )
            heavy_path.write_text(
                "".join(json.dumps(item) + "\n" for item in heavy_stream),
                encoding="utf-8",
            )
            holdout_path.write_text(
                "".join(json.dumps(item) + "\n" for item in holdout),
                encoding="utf-8",
            )
            report = analyze_visibility_lineages(
                [discovery_path],
                [holdout_path],
                {discovery_path: heavy_path},
            )

        self.assertTrue(report["valid"], report["errors"])
        capture = report["captures"][0]
        self.assertTrue(capture["instrumented_stream_equal"])
        self.assertEqual(capture["projection_window_owner_vertices"], 6)
        self.assertEqual(capture["verified_model_flow_owner_vertices"], 6)
        self.assertEqual(capture["verified_model_flow_owners"], 2)
        self.assertEqual(capture["verified_affine_owner_vertices"], 6)
        self.assertEqual(capture["verified_affine_owners"], 2)
        self.assertEqual(report["levels"]["geometry"]["distinct_signatures"], 4)
        endpoints = report["conflict_endpoint_coverage"]
        self.assertEqual(endpoints["both_numeric_affine_records"], 1)
        self.assertEqual(endpoints["different_frame_local_affine_records"], 1)
        self.assertEqual(endpoints["same_frame_local_affine_records"], 0)
        self.assertEqual(endpoints["complete_endpoint_constraint_frames"], 1)
        self.assertTrue(endpoints["frame_local_constraint_inputs_ready"])
        ready = endpoints["complete_endpoint_constraint_frame_preview"]
        self.assertEqual(len(ready), 1)
        self.assertTrue(ready[0]["complete_endpoint_constraint_input_ready"])

    def test_visibility_constraints_report_exact_bound_without_fitting(self):
        ot_signature = ("complete", "ot")
        depth_signature = ("complete", "depth")
        edges = []
        for capture, validation_set, frame in (
            (0, "discovery", 7),
            (1, "holdout", 8),
        ):
            edges.append(
                {
                    "capture": capture,
                    "validation_set": validation_set,
                    "frame": frame,
                    "samples": 1,
                    "depth_bounds_bits": (
                        0x3F400000,
                        0x3F400000,
                        0x3E800000,
                        0x3E800000,
                    ),
                    "ot": {
                        "signatures": {"complete_triangle": ot_signature},
                        "numeric_affine": (1,),
                    },
                    "depth": {
                        "signatures": {"complete_triangle": depth_signature},
                        "numeric_affine": (2,),
                    },
                }
            )
        captures = [
            {"conflict_edges": [edge], "validation_set": edge["validation_set"]}
            for edge in edges
        ]
        endpoint_coverage = {
            "complete_endpoint_constraint_records": 2,
            "complete_endpoint_constraint_samples": 2,
            "frame_endpoint_coverage": [
                {
                    "capture": edge["capture"],
                    "frame": edge["frame"],
                    "complete_endpoint_constraint_input_ready": True,
                }
                for edge in edges
            ],
        }
        membership = {
            _signature_id(depth_signature): "farther",
        }
        frame_affine_membership = {
            (edge["capture"], edge["frame"]): {
                (1,): "nearer",
                (2,): "farther",
            }
            for edge in edges
        }

        constraints = _summarize_visibility_gauge_constraints(
            captures,
            endpoint_coverage,
            membership,
            frame_affine_membership,
        )

        self.assertTrue(
            constraints["constraint_system_ready_for_candidate_testing"]
        )
        self.assertEqual(constraints["directed_component_constraints"], 1)
        bound = constraints["constraint_preview"][0]["strict_lower_bound"]
        self.assertEqual((bound["numerator"], bound["denominator"]), (3, 1))
        self.assertTrue(
            constraints["constraint_preview"][0]["cross_validated"]
        )
        self.assertEqual(constraints["direct_signature_mapped_endpoints"], 2)
        self.assertEqual(constraints["frame_anchor_mapped_endpoints"], 2)
        self.assertEqual(constraints["unmapped_endpoints"], 0)
        self.assertFalse(constraints["visibility_constraints_certify_gauge"])
        self.assertFalse(constraints["gauge_solution_unique"])
        self.assertFalse(
            constraints["integer_or_tenth_prior_used_to_select_factor"]
        )


if __name__ == "__main__":
    unittest.main()
