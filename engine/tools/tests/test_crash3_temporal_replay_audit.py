import unittest

from tools.crash3_temporal_replay_audit import EVENT, audit


class Crash3TemporalReplayAuditTest(unittest.TestCase):
    def quad(self):
        return [dict(event=EVENT, frame=10, packet=12, operation=3,
                     first_vertex=(i // 3) * 3, triangle=i // 3, triangles=2,
                     source_corner=corner, valid=1, pc=0x80044938,
                     native_x=100, native_y=100, x=139000,
                     old_x=corner * 1000, old_y=1000, old_w=100000)
                for i, corner in enumerate((0, 1, 2, 1, 2, 3))]

    def test_coherent_quad_and_wide_projection(self):
        result = audit(self.quad())
        self.assertTrue(result["coherence_passed"])
        self.assertEqual(result["counts"]["interpolated_coherent_quads"], 1)
        self.assertEqual(result["projection_samples_by_frame"], [{"frame": 10, "wide_075": 6}])

    def test_partial_quad_fails_but_current_endpoint_is_complete(self):
        records = self.quad()
        for record in records[:3]:
            record["valid"] = 0
        self.assertEqual(audit(records)["counts"]["mixed_phase_quads"], 1)
        for record in records:
            record["valid"] = 0
        self.assertTrue(audit(records)["coherence_passed"])

    def test_conflicting_shared_corner_is_not_hidden_by_valid_flags(self):
        records = self.quad()
        records[3]["old_x"] += 1000
        self.assertEqual(audit(records)["counts"]["incoherent_shared_corners"], 1)

    def test_empty_and_truncated_traces_fail(self):
        self.assertFalse(audit([])["coherence_passed"])
        self.assertFalse(audit(self.quad()[:3])["coherence_passed"])

    def test_native_projection_is_reported_not_mislabelled_as_visibility(self):
        records = self.quad()
        for record in records:
            record["x"] = 100000
        result = audit(records)
        self.assertTrue(result["coherence_passed"])
        self.assertEqual(result["projection_samples_by_frame"], [{"frame": 10, "native_100": 6}])


if __name__ == "__main__":
    unittest.main()
