import json
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))

from visibility_audit_trace import analyze_visibility_trace


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


def audit(frame=7, **overrides):
    fields = {
        "status": 0,
        "owners": 2,
        "atlas_x": 40,
        "atlas_y": 80,
        "width": 4,
        "height": 2,
        "samples": 8,
        "agreement": 5,
        "background_agreement": 2,
        "owned_agreement": 3,
        "ot_owned": 5,
        "depth_owned": 5,
        "conflicts": 2,
        "coverage_divergence": 0,
        "indeterminate": 1,
        "inconsistent": 0,
        "conflict_pairs": 1,
    }
    fields.update(overrides)
    return record("replay.visibility_audit", frame, **fields)


def conflict(frame=7, **overrides):
    fields = {
        "ot_owner": 2,
        "depth_owner": 1,
        "samples": 2,
        "ot_source_ordinal": 20,
        "depth_source_ordinal": 10,
        "min_x": 41,
        "min_y": 80,
        "max_x": 42,
        "max_y": 81,
    }
    fields.update(overrides)
    return record("replay.visibility_conflict", frame, **fields)


class VisibilityAuditTraceTest(unittest.TestCase):
    def analyze(self, records, **options):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            path.write_text("".join(json.dumps(item) + "\n" for item in records))
            return analyze_visibility_trace(path, **options)

    def test_accepts_complete_owned_conflict_frame(self):
        report = self.analyze([audit(), conflict()])
        self.assertTrue(report["valid"])
        self.assertEqual(report["classifiable_frames"], 1)
        self.assertEqual(report["totals"]["conflicts"], 2)
        self.assertFalse(report["inference_scope"]["family_count_inferred"])

    def test_rejects_empty_frame_when_owned_coverage_is_required(self):
        empty = audit(
            agreement=8,
            background_agreement=8,
            owned_agreement=0,
            ot_owned=0,
            depth_owned=0,
            conflicts=0,
            indeterminate=0,
            conflict_pairs=0,
        )
        report = self.analyze([empty], require_owned_every_frame=True)
        self.assertFalse(report["valid"])
        self.assertEqual(report["inconclusive_frames"], 1)

    def test_rejects_missing_pair_record_and_partition_mismatch(self):
        broken = audit(agreement=6)
        report = self.analyze([broken])
        self.assertFalse(report["valid"])
        self.assertTrue(any("classifications sum" in e for e in report["errors"]))
        self.assertTrue(any("conflict records" in e for e in report["errors"]))

    def test_rejects_owner_order_and_out_of_region_bounds(self):
        bad = conflict(ot_source_ordinal=9, max_x=44)
        report = self.analyze([audit(), bad])
        self.assertFalse(report["valid"])
        self.assertTrue(any("not later" in e for e in report["errors"]))
        self.assertTrue(any("outside audit region" in e for e in report["errors"]))


if __name__ == "__main__":
    unittest.main()
