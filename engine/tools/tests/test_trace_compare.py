import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from trace_compare import anchor_trace, analyze, compare, compare_native_latches, parse_anchor, read_trace  # noqa: E402


def record(seq, frame, event, **fields):
    return {
        "schema": "ps1trace/v1",
        "source": "test",
        "seq": seq,
        "frame": frame,
        "host_us": frame * 20_000,
        "event": event,
        **fields,
    }


class TraceCompareTest(unittest.TestCase):
    @staticmethod
    def latch(frame, **fields):
        return record(frame, frame - 1, "gpu.vblank_latch", latch_frame=frame,
                      hash_valid=1, native_hash=frame + (1 << 60),
                      aperture_w=512, aperture_h=256, **fields)

    def test_native_latches_ignore_host_phase_and_modern_images(self):
        left = [self.latch(f, modern_hash=1) for f in range(10, 13)]
        right = [dict(self.latch(f, modern_hash=2), frame=f, host_us=0)
                 for f in range(10, 13)]
        result = compare_native_latches(left, right)
        self.assertTrue(result["passed"])
        self.assertEqual(result["matching_latches"], 3)
        self.assertEqual(result["latch_range"], [10, 12])

    def test_native_latches_require_entire_range_not_intersection(self):
        left = [self.latch(10), self.latch(11)]
        right = [self.latch(11), self.latch(12)]
        result = compare_native_latches(left, right)
        self.assertFalse(result["passed"])
        self.assertEqual(result["expected_latches"], 3)
        self.assertEqual(result["failure_counts"], {"right_missing": 1, "left_missing": 1})
        self.assertTrue(compare_native_latches(left, right, 11, 11)["passed"])

    def test_native_latches_reject_duplicates_missing_hash_and_changed_aperture(self):
        left = [self.latch(f) for f in range(10, 14)]
        right = [self.latch(10), self.latch(10),
                 dict(self.latch(11), hash_valid=0),
                 dict(self.latch(12), aperture_w=320),
                 dict(self.latch(13), native_hash=8)]
        result = compare_native_latches(left, right, max_diffs=2)
        self.assertFalse(result["passed"])
        self.assertEqual(result["failure_counts"], {
            "right_duplicate": 1, "right_invalid_hash": 1, "native_image_mismatch": 2})
        self.assertEqual(len(result["first_failures"]), 2)

    def test_native_latches_do_not_accept_outer_frame_as_identity(self):
        bad = dict(self.latch(10))
        del bad["latch_frame"]
        with self.assertRaisesRegex(ValueError, "latch_frame"):
            compare_native_latches([bad], [self.latch(10)])
        with self.assertRaisesRegex(ValueError, "no native latches"):
            compare_native_latches([self.latch(10)], [self.latch(10)], 20, 30)
        with self.assertRaisesRegex(ValueError, "start exceeds end"):
            compare_native_latches([self.latch(10)], [self.latch(10)], 30, 20)

    def test_native_latches_reject_non_integral_or_missing_hashes(self):
        for fields in ({"native_hash": float(1 << 60)}, {"native_hash": None},
                       {"native_hash": -(1 << 63) - 1}, {"native_hash": 1 << 64},
                       {"aperture_w": 0}, {"aperture_h": True}):
            with self.subTest(fields=fields):
                result = compare_native_latches([self.latch(10)], [dict(self.latch(10), **fields)])
                self.assertEqual(result["failure_counts"], {"right_invalid_hash": 1})

    def test_native_latches_preserve_signed_and_unsigned_hash_bits(self):
        left = [dict(self.latch(10), native_hash=(1 << 64) - 1)]
        right = [dict(self.latch(10), native_hash=-1)]
        self.assertTrue(compare_native_latches(left, right)["passed"])

    def test_analyze_detects_double_buffer_pairing_error(self):
        records = [
            record(1, 1, "timing.vblank", index=1),
            record(2, 1, "gpu.vblank_latch", display_x=0, display_y=0,
                   display_draw_overlap=0),
            record(3, 1, "host.present", display_x=512, display_y=0),
        ]
        report = analyze(records)
        self.assertEqual(report["display"]["latch_present_mismatches"], [{
            "frame": 1, "latched": [0, 0], "presented": [512, 0]
        }])
        self.assertEqual(report["display"]["latch_to_present_us_median"], 0)

    def test_compare_uses_stable_fields_and_finds_first_divergence(self):
        left = [
            record(1, 1, "gpu.display_start", x=0, y=0, word=0x05000000),
            record(2, 2, "gpu.display_start", x=512, y=0, word=0x05000200),
        ]
        right = [
            record(1, 20, "gpu.display_start", x=0, y=0, donor_extra=9),
            record(2, 21, "gpu.display_start", x=0, y=256),
        ]
        result = compare(left, right, 5)["gpu.display_start"]
        self.assertEqual(result["matching_prefix"], 1)
        self.assertEqual(result["mismatches"][0]["left"], [512, 0])
        self.assertEqual(result["mismatches"][0]["right"], [0, 256])

    def test_compare_treats_raw_gp0_words_as_stable(self):
        left = [record(1, 1, "gpu.gp0_word", word=0x2C808080)]
        right = [record(1, 9, "gpu.gp0_word", word=0x24808080)]
        result = compare(left, right, 5)["gpu.gp0_word"]
        self.assertEqual(result["matching_prefix"], 0)
        self.assertEqual(result["mismatches"][0]["left"], [0x2C808080])
        self.assertEqual(result["mismatches"][0]["right"], [0x24808080])

    def test_compare_reports_an_entirely_missing_event_stream(self):
        left = [record(1, 1, "spu.key_on", voices=3)]
        right = [record(1, 1, "timing.vblank", index=1)]
        result = compare(left, right, 5)
        self.assertEqual(result["spu.key_on"]["left_count"], 1)
        self.assertEqual(result["spu.key_on"]["right_count"], 0)
        self.assertEqual(result["spu.key_on"]["matching_prefix"], 0)

    def test_compare_realigns_after_inserted_event(self):
        left = [
            record(1, 1, "spu.key_on", voices=1),
            record(2, 2, "spu.key_on", voices=99),
            record(3, 3, "spu.key_on", voices=2),
            record(4, 4, "spu.key_on", voices=3),
        ]
        right = [
            record(1, 1, "spu.key_on", voices=1),
            record(2, 2, "spu.key_on", voices=2),
            record(3, 3, "spu.key_on", voices=3),
        ]

        result = compare(left, right, 5)["spu.key_on"]
        self.assertEqual(result["matching_prefix"], 1)
        self.assertEqual(result["aligned_matches"], 3)
        self.assertEqual(result["aligned_left_only"], 1)
        self.assertEqual(result["aligned_right_only"], 0)
        self.assertAlmostEqual(result["aligned_similarity"], 6 / 7)

    def test_analyze_reports_missing_and_duplicate_present_pairs(self):
        records = [
            record(1, 1, "gpu.vblank_latch", display_x=0, display_y=0),
            record(2, 1, "host.present", display_x=0, display_y=0),
            record(3, 1, "host.present", display_x=0, display_y=0),
            record(4, 2, "gpu.vblank_latch", display_x=512, display_y=0),
            record(5, 3, "host.present", display_x=512, display_y=0),
        ]
        display = analyze(records)["display"]
        self.assertEqual(display["duplicate_present_frames"], [1])
        self.assertEqual(display["latch_without_present_frames"], [2])
        self.assertEqual(display["present_without_latch_frames"], [3])

    def test_analyze_reports_flip_and_audio_cadence(self):
        records = [
            record(1, 1, "gpu.display_start", x=0, y=0),
            record(2, 3, "gpu.display_start", x=512, y=0),
            record(3, 5, "gpu.display_start", x=0, y=0),
            record(4, 5, "spu.key_on", voices=1),
            record(5, 5, "spu.key_on", voices=2),
            record(6, 6, "spu.key_off", voices=3),
            record(7, 6, "cdrom.command", command=6),
            record(8, 7, "cdrom.command", command=6),
        ]
        report = analyze(records)
        self.assertEqual(report["display"]["write_interval_frames_histogram"], {"2": 2})
        self.assertEqual(report["audio"]["frames_with_multiple_key_ons"], [5])
        self.assertEqual(report["cdrom"]["command_histogram"], {6: 2})

    def test_read_trace_rejects_wrong_schema(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.jsonl"
            path.write_text(json.dumps({"schema": "old", "event": "x"}) + "\n")
            with self.assertRaisesRegex(ValueError, "expected 'ps1trace/v1'"):
                read_trace(path)

    def test_anchor_trims_and_normalizes_boot_offset(self):
        records = [
            record(1, 20, "spu.key_on", voices=1),
            record(2, 30, "gpu.display_start", x=512, y=0),
            record(3, 31, "timing.vblank", index=31),
            record(4, 32, "spu.key_on", voices=3),
        ]
        event, fields = parse_anchor("gpu.display_start:x=512,y=0")
        anchored, metadata = anchor_trace(records, event, fields)
        self.assertEqual(metadata["source_frame"], 30)
        self.assertEqual(anchored[0]["frame"], 0)
        self.assertEqual(anchored[1]["frame"], 1)
        self.assertEqual(anchored[1]["index"], 1)
        self.assertEqual(anchored[2]["host_us"], 40_000)


if __name__ == "__main__":
    unittest.main()
