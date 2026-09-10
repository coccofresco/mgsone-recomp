import math
import struct
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))

from crash3_depth_plane_audit import analyze_depth_file, audit_depth_files


class Crash3DepthPlaneAuditTest(unittest.TestCase):
    def write(self, values, *, scale=-1.0):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "depth.pfm"
        endian = "<" if scale < 0 else ">"
        path.write_bytes(
            f"Pf\n{len(values)} 1\n{scale}\n".encode("ascii")
            + b"".join(struct.pack(endian + "f", value) for value in values)
        )
        return path

    def test_separates_continuous_scene_background_clear_and_invalid(self):
        path = self.write([2**-20, 2**-19, 2**-20, 0.99999994, 1.0, 0.0, math.nan])
        report = analyze_depth_file(path)
        self.assertEqual(report["scene_pixels"], 3)
        self.assertEqual(report["scene_distinct_depth_values"], 2)
        self.assertEqual(report["background_pixels"], 1)
        self.assertEqual(report["clear_pixels"], 1)
        self.assertEqual(report["invalid_pixels"], 2)
        self.assertTrue(report["binary_like_scene"])

    def test_gate_rejects_a_flat_scene(self):
        report = audit_depth_files(
            [self.write([2**-20, 2**-20, 2**-20])],
            minimum_scene_distinct_values=3,
        )
        self.assertEqual(len(report["failures"]), 1)

    def test_operational_far_plane_uses_paired_surface_to_identify_sky(self):
        depth = self.write([1.0, 1.0, 2**-20])
        operational = depth.with_name("frame-2.operational-occlusion-d32.pfm")
        depth.rename(operational)
        unpaired = analyze_depth_file(operational)
        self.assertTrue(unpaired["clear_may_include_background"])
        surface = self.write([0.99999994, 1.0, 2**-20])
        operational.with_name("frame-2.presented-surface-r32.pfm").write_bytes(surface.read_bytes())
        paired = analyze_depth_file(operational)
        self.assertEqual(paired["depth_plane"], "operational-occlusion-d32")
        self.assertTrue(paired["legacy_replay_depth_not_host_scene"])
        self.assertFalse(paired["clear_may_include_background"])
        self.assertEqual(paired["background_pixels"], 1)
        self.assertEqual(paired["clear_pixels"], 1)
        self.assertEqual(paired["scene_pixels"], 1)

    def test_modern_host_scene_is_distinct_from_legacy_replay_depth(self):
        depth = self.write([1.0, 1.0, 2**-20, 2**-19])
        host = depth.with_name("frame-2-present-3.host-scene-occlusion-d32.pfm")
        depth.rename(host)
        self.assertTrue(analyze_depth_file(host)["clear_may_include_background"])
        surface = self.write([0.99999994, 1.0, 2**-20, 2**-19])
        host.with_name("frame-2-present-3.presented-surface-r32.pfm").write_bytes(surface.read_bytes())
        report = analyze_depth_file(host)
        self.assertEqual(report["depth_plane"], "host-scene-occlusion-d32")
        self.assertFalse(report["legacy_replay_depth_not_host_scene"])
        self.assertFalse(report["clear_may_include_background"])
        self.assertEqual(report["background_pixels"], 1)
        self.assertEqual(report["clear_pixels"], 1)
        self.assertEqual(report["scene_distinct_depth_values"], 2)

    def test_rejects_mismatched_paired_surface_dimensions(self):
        depth = self.write([1.0, 1.0])
        operational = depth.with_name("frame-2.operational-occlusion-d32.pfm")
        depth.rename(operational)
        surface = self.write([0.99999994])
        operational.with_name("frame-2.presented-surface-r32.pfm").write_bytes(surface.read_bytes())
        with self.assertRaisesRegex(ValueError, "dimensions disagree"):
            analyze_depth_file(operational)

    def test_gate_skips_double_buffer_warmup_then_requires_dense_depth(self):
        warmup = self.write([1.0, 1.0, 1.0])
        populated = self.write([2**-20, 2**-19, 2**-18, 0.99999994])
        report = audit_depth_files(
            [warmup, populated],
            minimum_scene_distinct_values=3,
            maximum_leading_clear_captures=1,
            maximum_clear_fraction=0.1,
            maximum_invalid_fraction=0.0,
        )
        self.assertEqual(report["qualified_captures"], 1)
        self.assertEqual(report["failures"], [])

    def test_gate_rejects_clear_or_invalid_bands_after_warmup(self):
        report = audit_depth_files(
            [self.write([2**-20, 1.0, 0.0, 2**-19])],
            minimum_scene_distinct_values=2,
            maximum_clear_fraction=0.2,
            maximum_invalid_fraction=0.2,
        )
        self.assertEqual(len(report["failures"]), 2)

    def test_gate_requires_at_least_one_capture_after_warmup(self):
        report = audit_depth_files(
            [self.write([1.0])],
            maximum_leading_clear_captures=1,
        )
        self.assertRegex(report["failures"][0], "qualified captures")

    def test_gate_rejects_excessive_leading_clear_warmup(self):
        report = audit_depth_files(
            [self.write([1.0]), self.write([1.0]), self.write([2**-20])],
            maximum_leading_clear_captures=1,
        )
        self.assertRegex(report["failures"][0], "leading all-clear")

    def test_all_clear_relapse_after_qualification_is_not_skipped(self):
        report = audit_depth_files(
            [self.write([2**-20, 2**-19]), self.write([1.0, 1.0])],
            minimum_scene_distinct_values=2,
            maximum_leading_clear_captures=1,
            maximum_clear_fraction=0.1,
        )
        self.assertEqual(report["leading_clear_captures"], 0)
        self.assertEqual(len(report["failures"]), 2)

    def test_reads_big_endian_pfm(self):
        report = analyze_depth_file(self.write([2**-18, 1.0], scale=1.0))
        self.assertEqual(report["scene_pixels"], 1)
        self.assertEqual(report["clear_pixels"], 1)


if __name__ == "__main__":
    unittest.main()
