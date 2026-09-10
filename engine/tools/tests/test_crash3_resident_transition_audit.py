import json
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))

from crash3_resident_transition_audit import (
    analyze_transition_trace,
    resource_set_fingerprint,
)


RESOURCES = {
    "a": (0x101, 0xA001),
    "b": (0x103, 0xB001),
    "c": (0x105, 0xC001),
    "d": (0x107, 0xD001),
    # Same EID as B with different immutable geometry: never a second valid
    # resource identity in one resident construction.
    "x": (0x103, 0xB999),
}


def record(event, frame, **fields):
    return {
        "schema": "ps1trace/v1",
        "source": "test",
        "seq": frame,
        "frame": frame,
        "host_us": 0,
        "event": event,
        **fields,
    }


def capture(
    frame,
    names,
    *,
    active,
    generation,
    observations=0,
    validated=True,
    candidate=False,
    committed=False,
    shared=0,
    disjoint=False,
    candidate_presented=False,
    reprojected=0,
    retained=0,
):
    keys = tuple(sorted(RESOURCES[name] for name in names))
    active_keys = tuple(sorted(RESOURCES[name] for name in active))
    fingerprint = resource_set_fingerprint(keys)
    active_fingerprint = resource_set_fingerprint(active_keys) if active_keys else 0
    rows = [
        record(
            "game.crash3.resident_projection_catalog",
            frame,
            catalog_schema=3,
            face_schema=2,
            capture_id=frame,
            topology_fingerprint=frame,
            resource_set_fingerprint=fingerprint,
            active_cohort_fingerprint=active_fingerprint,
            active_cohort_generation=generation,
            candidate_observations=observations,
            shared_active_resources=shared,
            resource_set_validated=int(validated),
            placement_validated=int(validated),
            resource_identity_conflict=0,
            cohort_candidate=int(candidate),
            cohort_disjoint=int(disjoint),
            cohort_committed=int(committed),
            active_cohort_reprojected=int(reprojected > 0),
            candidate_cohort_presented=int(candidate_presented),
            cohort_presentation_valid=1,
            cohort_presentation_applied=int(reprojected > 0 or retained > 0),
            cohort_presentation_enabled=1,
            retained_resources=retained,
            retained_resource_faces=retained * 2,
            reprojected_active_resources=reprojected,
            reprojected_active_faces=reprojected * 2,
            world_count=len(keys),
            source_faces=0,
            resident_faces=len(keys) * 2,
            projected_faces=len(keys) * 2,
            near_generated_faces=0,
            near_rejected_faces=0,
        )
    ]
    for world, (resource_id, geometry) in enumerate(keys):
        offset = (10, 20, 30)
        placement = validated or world == 0
        if not validated and world == 1:
            offset = (11, 20, 30)
            placement = False
        origin = (world * 400, world * 800, world * 1200)
        translation = tuple(origin[index] // 4 + offset[index] for index in range(3))
        rows.append(
            record(
                "game.crash3.resident_projection_world",
                frame,
                world_schema=2,
                capture_id=frame,
                world=world,
                resource_pair_valid=1,
                resource_entry=0x80010000 + world * 0x100,
                resource_id=resource_id,
                info_origin_x=origin[0],
                info_origin_y=origin[1],
                info_origin_z=origin[2],
                info_flags=0,
                translation_x=translation[0],
                translation_y=translation[1],
                translation_z=translation[2],
                geometry_fingerprint=geometry,
                identity_valid=1,
                placement_validated=int(placement),
            )
        )
    return rows


def capture_v4(
    frame,
    names,
    *,
    valid_names,
    active,
    generation,
    resource_observations,
    scene_members,
    scene_count,
    admissions=0,
    revocations=0,
    candidate_observations=0,
    candidate=False,
    committed=False,
    reprojected=0,
):
    keys = tuple(sorted(RESOURCES[name] for name in names))
    active_keys = tuple(sorted(RESOURCES[name] for name in active))
    fingerprint = resource_set_fingerprint(keys)
    active_fingerprint = resource_set_fingerprint(active_keys) if active_keys else 0
    globally_valid = set(names) == set(valid_names)
    rows = [
        record(
            "game.crash3.resident_projection_catalog",
            frame,
            catalog_schema=4,
            face_schema=3,
            face_trace_enabled=0,
            capture_id=frame,
            topology_fingerprint=frame,
            resource_set_fingerprint=fingerprint,
            active_cohort_fingerprint=active_fingerprint,
            active_cohort_generation=generation,
            candidate_observations=candidate_observations,
            shared_active_resources=0,
            resource_set_validated=int(globally_valid),
            placement_validated=int(globally_valid),
            resource_identity_conflict=0,
            cohort_candidate=int(candidate),
            cohort_disjoint=0,
            cohort_committed=int(committed),
            active_cohort_reprojected=int(reprojected > 0),
            candidate_cohort_presented=0,
            cohort_presentation_valid=1,
            cohort_presentation_applied=int(reprojected > 0 or scene_count > len(active)),
            cohort_presentation_enabled=1,
            retained_resources=max(0, scene_count - len(active)),
            retained_resource_faces=max(0, scene_count - len(active)) * 2,
            scene_cache_resources=scene_count,
            scene_cache_faces=scene_count * 2,
            individually_validated_resources=len(valid_names),
            individually_admitted_resources=admissions,
            individually_revoked_resources=revocations,
            reprojected_active_resources=reprojected,
            reprojected_active_faces=reprojected * 2,
            world_count=len(keys),
            source_faces=1,
            resident_faces=len(valid_names) * 2,
            projected_faces=scene_count * 2,
            near_generated_faces=0,
            near_rejected_faces=0,
        )
    ]
    name_by_key = {RESOURCES[name]: name for name in names}
    for world, (resource_id, geometry) in enumerate(keys):
        name = name_by_key[(resource_id, geometry)]
        valid = name in valid_names
        origin = (world * 400, world * 800, world * 1200)
        offset = (10, 20, 30)
        translation = tuple(origin[index] // 4 + offset[index] for index in range(3))
        rows.append(
            record(
                "game.crash3.resident_projection_world",
                frame,
                world_schema=2,
                capture_id=frame,
                world=world,
                resource_pair_valid=1,
                resource_entry=0x80010000 + world * 0x100,
                resource_id=resource_id,
                info_valid=int(valid),
                info_origin_x=origin[0],
                info_origin_y=origin[1],
                info_origin_z=origin[2],
                info_flags=0,
                translation_x=translation[0],
                translation_y=translation[1],
                translation_z=translation[2],
                geometry_fingerprint=geometry,
                identity_valid=int(valid),
                placement_validated=int(valid),
                individual_payload_complete=int(valid),
                individual_source_witness=int(valid and name == "a"),
                # Placement is an independent structural witness; an invalid
                # identity/payload can still happen to share the source offset.
                individual_placement_witness=1,
                individually_validated=int(valid),
                individual_observations=resource_observations.get(name, 0),
                scene_cache_member=int(name in scene_members),
            )
        )
    return rows


class Crash3ResidentTransitionAuditTest(unittest.TestCase):
    def write(self, rows):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "transition.jsonl"
        path.write_text(
            "".join(json.dumps(item) + "\n" for item in rows), encoding="utf-8"
        )
        return path

    def sequence(self):
        return [
            *capture(1, "a", active="", generation=0, observations=1, candidate=True),
            *capture(
                2,
                "a",
                active="a",
                generation=1,
                observations=2,
                candidate=True,
                committed=True,
            ),
            *capture(
                3,
                "ab",
                active="a",
                generation=1,
                validated=False,
                reprojected=1,
            ),
            *capture(
                4,
                "ab",
                active="a",
                generation=1,
                observations=1,
                candidate=True,
                shared=1,
                candidate_presented=True,
            ),
            *capture(
                5,
                "ab",
                active="ab",
                generation=2,
                observations=2,
                candidate=True,
                committed=True,
                shared=1,
            ),
            *capture(
                6,
                "bc",
                active="ab",
                generation=2,
                observations=1,
                candidate=True,
                shared=1,
                candidate_presented=True,
                retained=1,
            ),
            *capture(
                7,
                "bc",
                active="bc",
                generation=3,
                observations=2,
                candidate=True,
                committed=True,
                shared=1,
                retained=1,
            ),
            *capture(
                8,
                "d",
                active="bc",
                generation=3,
                observations=1,
                candidate=True,
                disjoint=True,
                candidate_presented=True,
            ),
            *capture(
                9,
                "d",
                active="d",
                generation=4,
                observations=2,
                candidate=True,
                committed=True,
                disjoint=True,
            ),
        ]

    def test_audits_repeated_overlap_and_disjoint_handoffs(self):
        report = analyze_transition_trace(self.write(self.sequence()))
        self.assertEqual(report["captures"], 9)
        self.assertEqual(report["invalid_partial_table_holds"], 1)
        self.assertEqual(report["committed_transitions"], 3)
        self.assertEqual(report["overlapping_transitions"], 2)
        self.assertEqual(report["disjoint_transitions"], 1)
        self.assertEqual(report["final_active_generation"], 4)
        self.assertEqual(report["resource_geometry_variant_ids"], 0)
        # The synthetic fixture assigns ordinal placements afresh in each
        # capture, so cross-time reuse is expected and remains diagnostic.
        self.assertEqual(report["authored_placement_resource_variant_count"], 2)
        self.assertEqual(
            report["simultaneous_authored_placement_conflict_count"], 0
        )

    def test_distinguishes_multiple_captures_in_one_trace_frame(self):
        rows = self.sequence()
        for item in rows:
            if item["frame"] == 2:
                # Keep capture_id=2: only the presentation timestamp collides.
                item["frame"] = 1
        report = analyze_transition_trace(self.write(rows))
        self.assertEqual(report["captures"], 9)
        self.assertEqual(report["committed_transitions"], 3)
        self.assertEqual(report["final_active_generation"], 4)

    def test_rejects_unpresented_valid_incoming_candidate(self):
        rows = self.sequence()
        for item in rows:
            if item["event"].endswith("projection_catalog") and item["frame"] == 4:
                item["candidate_cohort_presented"] = 0
        with self.assertRaisesRegex(ValueError, "incoming candidate was not presented"):
            analyze_transition_trace(self.write(rows))

    def individual_sequence(self):
        return [
            *capture_v4(
                1,
                "a",
                valid_names="a",
                active="",
                generation=0,
                resource_observations={"a": 1},
                scene_members="",
                scene_count=0,
                candidate_observations=1,
                candidate=True,
            ),
            *capture_v4(
                2,
                "a",
                valid_names="a",
                active="a",
                generation=1,
                resource_observations={"a": 2},
                scene_members="a",
                scene_count=1,
                admissions=1,
                candidate_observations=2,
                candidate=True,
                committed=True,
            ),
            *capture_v4(
                3,
                "abd",
                valid_names="ab",
                active="a",
                generation=1,
                resource_observations={"a": 2, "b": 1},
                scene_members="a",
                scene_count=1,
                reprojected=1,
            ),
            *capture_v4(
                4,
                "abd",
                valid_names="ab",
                active="a",
                generation=1,
                resource_observations={"a": 2, "b": 2},
                scene_members="ab",
                scene_count=2,
                admissions=1,
                reprojected=1,
            ),
            *capture_v4(
                5,
                "acd",
                valid_names="ac",
                active="a",
                generation=1,
                resource_observations={"a": 2, "c": 1},
                scene_members="a",
                scene_count=2,
                reprojected=1,
            ),
            *capture_v4(
                6,
                "acd",
                valid_names="ac",
                active="a",
                generation=1,
                resource_observations={"a": 2, "c": 2},
                scene_members="ac",
                scene_count=2,
                admissions=1,
                revocations=1,
                reprojected=1,
            ),
            *capture_v4(
                7,
                "a",
                valid_names="a",
                active="a",
                generation=1,
                resource_observations={"a": 2},
                scene_members="a",
                scene_count=2,
            ),
            *capture_v4(
                8,
                "a",
                valid_names="a",
                active="a",
                generation=1,
                resource_observations={"a": 2},
                scene_members="a",
                scene_count=1,
                revocations=1,
            ),
        ]

    def test_audits_independent_row_admission_and_symmetric_revocation(self):
        report = analyze_transition_trace(self.write(self.individual_sequence()))
        self.assertEqual(report["captures"], 8)
        self.assertEqual(report["invalid_partial_table_holds"], 4)
        self.assertEqual(report["individually_admitted_resources"], 3)
        self.assertEqual(report["individually_revoked_resources"], 2)
        self.assertEqual(report["maximum_scene_resources"], 2)
        self.assertEqual(report["final_active_generation"], 1)

    def test_rejects_route_trace_that_claims_early_individual_admission(self):
        rows = self.individual_sequence()
        for item in rows:
            if item["event"].endswith("projection_catalog") and item["frame"] == 3:
                item["individually_admitted_resources"] = 1
        with self.assertRaisesRegex(ValueError, "admitted-resource aggregate differs"):
            analyze_transition_trace(self.write(rows))

    def test_audits_duplicate_eid_rows_as_individually_invalid(self):
        rows = capture_v4(
            1,
            "abx",
            valid_names="abx",
            active="",
            generation=0,
            resource_observations={"a": 1, "b": 0, "x": 0},
            scene_members="",
            scene_count=0,
        )
        catalog = rows[0]
        catalog["resource_set_validated"] = 0
        catalog["placement_validated"] = 0
        catalog["individually_validated_resources"] = 1
        for world in rows[1:]:
            if world["resource_id"] == RESOURCES["b"][0]:
                world["individually_validated"] = 0
                world["individual_observations"] = 0

        report = analyze_transition_trace(self.write(rows))
        self.assertEqual(report["captures"], 1)
        self.assertEqual(report["invalid_partial_table_holds"], 1)
        self.assertEqual(report["individually_validated_captures"], 1)
        self.assertEqual(report["individually_admitted_resources"], 0)
        self.assertEqual(report["resource_geometry_variant_ids"], 1)

    def test_reports_simultaneous_placement_conflict_as_diagnostic(self):
        rows = self.sequence()
        frame_five_worlds = [
            row
            for row in rows
            if row["event"].endswith("projection_world") and row["frame"] == 5
        ]
        first_a, second_b = frame_five_worlds
        for axis in "xyz":
            second_b[f"info_origin_{axis}"] = first_a[f"info_origin_{axis}"]
            second_b[f"translation_{axis}"] = first_a[f"translation_{axis}"]
        report = analyze_transition_trace(self.write(rows))
        self.assertGreater(report["authored_placement_resource_variant_count"], 0)
        self.assertEqual(report["simultaneous_authored_placement_conflict_count"], 1)


if __name__ == "__main__":
    unittest.main()
