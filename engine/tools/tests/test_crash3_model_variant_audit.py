import json
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))

from crash3_model_variant_audit import analyze_model_variants


def variant(frame, capture, object_address=0x80010000, **overrides):
    fields = {
        "signature": 100,
        "program_signature": 200,
        "logical_model_resource": 0x80020000,
        "renderer_model_resource": 0x80020000,
        "animation_sequence": 0x80030000,
        "animation_frame": frame * 256,
        "render_selector": 0,
        "current_frame_resource": 0x80040000 + frame * 4,
        "next_frame_resource": 0x80050000 + frame * 4,
        "command_table_resource": 0x80060000,
        "faces": 100,
        "parts": 1,
    }
    fields.update(overrides)
    return {
        "schema": "ps1trace/v1",
        "source": "test",
        "seq": capture,
        "frame": frame,
        "host_us": 0,
        "event": "game.crash3.object_model_variant",
        "capture_id": capture,
        "object": object_address,
        **fields,
    }


def part(frame, capture, part_ordinal=0, object_address=0x80010000, **overrides):
    fields = {
        "renderer_node": 0x80070000 + part_ordinal * 0x70,
        "logical_model_resource": 0x80020000,
        "renderer_model_resource": 0x80020000,
        "renderer_program_fingerprint": 300,
        "renderer_geometry_fingerprint": 400,
        "projection_transform_fingerprint": 500,
        "animation_sequence": 0x80030000,
        "animation_frame": frame * 256,
        "render_selector": 0,
        "current_frame_resource": 0x80040000 + frame * 4,
        "next_frame_resource": 0x80050000 + frame * 4,
        "command_table_resource": 0x80060000,
        "material_table_resource": 0x80061000,
        "vertex_renderer": 0x8003F460,
        "scale_shift": 3,
        "faces": 100,
    }
    fields.update(overrides)
    return {
        "schema": "ps1trace/v1",
        "source": "test",
        "seq": capture,
        "frame": frame,
        "host_us": 0,
        "event": "game.crash3.object_model_part",
        "capture_id": capture,
        "object": object_address,
        "part": part_ordinal,
        **fields,
    }


class Crash3ModelVariantAuditTest(unittest.TestCase):
    def write(self, rows):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "variants.jsonl"
        path.write_text(
            "".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8"
        )
        return path

    def test_separates_program_animation_resource_and_multipart_changes(self):
        rows = [
            variant(1, 1),
            variant(2, 2, program_signature=201),
            variant(
                3,
                3,
                signature=101,
                program_signature=202,
                animation_sequence=0x80030020,
                faces=88,
            ),
            variant(
                4,
                4,
                signature=102,
                program_signature=203,
                logical_model_resource=0x80021000,
                renderer_model_resource=0x80021000,
                faces=140,
            ),
            variant(5, 5, signature=103, program_signature=204, parts=2),
        ]
        report = analyze_model_variants(self.write(rows))
        self.assertEqual(report["program_only_transitions"], 1)
        self.assertEqual(report["animation_topology_transitions"], 1)
        self.assertEqual(report["model_resource_switches"], 1)
        self.assertEqual(report["multipart_composition_transitions"], 1)
        self.assertFalse(report["highest_detail_selection_authorized"])
        self.assertEqual(len(report["resource_switch_candidates"]), 1)

    def test_reports_entry_and_exit_without_calling_them_lod(self):
        report = analyze_model_variants(
            self.write(
                [
                    variant(1, 1),
                    variant(2, 2, object_address=0x80010004),
                ]
            )
        )
        self.assertEqual(report["entries"], 2)
        self.assertEqual(report["exits"], 1)
        self.assertEqual(report["model_resource_switches"], 0)

    def test_rejects_conflicting_duplicate_object_in_one_capture(self):
        with self.assertRaisesRegex(ValueError, "conflicting variants"):
            analyze_model_variants(
                self.write(
                    [
                        variant(1, 1),
                        variant(1, 1, signature=999),
                    ]
                )
            )

    def test_part_lineage_marks_only_intrinsically_aligned_resource_pair_as_lod_candidate(self):
        report = analyze_model_variants(
            self.write(
                [
                    variant(1, 1, faces=180, parts=2),
                    part(1, 1, faces=100),
                    part(
                        1,
                        1,
                        part_ordinal=1,
                        renderer_model_resource=0x80021000,
                        renderer_program_fingerprint=301,
                        renderer_geometry_fingerprint=401,
                        faces=80,
                    ),
                ]
            )
        )
        self.assertTrue(report["part_instrumentation_present"])
        self.assertEqual(report["part_records"], 2)
        self.assertEqual(report["simultaneous_lod_candidates"], 1)
        self.assertFalse(report["highest_detail_selection_authorized"])

    def test_different_part_transforms_are_multipart_not_lod(self):
        report = analyze_model_variants(
            self.write(
                [
                    variant(1, 1, faces=180, parts=2),
                    part(1, 1, faces=100),
                    part(
                        1,
                        1,
                        part_ordinal=1,
                        renderer_model_resource=0x80021000,
                        projection_transform_fingerprint=999,
                        faces=80,
                    ),
                ]
            )
        )
        self.assertEqual(report["simultaneous_lod_candidates"], 0)
        self.assertEqual(report["simultaneous_multipart_transform"], 1)

    def test_rejects_incomplete_part_instrumentation(self):
        with self.assertRaisesRegex(ValueError, "part records, expected 2"):
            analyze_model_variants(
                self.write(
                    [
                        variant(1, 1, faces=200, parts=2),
                        part(1, 1, faces=100),
                    ]
                )
            )


if __name__ == "__main__":
    unittest.main()
