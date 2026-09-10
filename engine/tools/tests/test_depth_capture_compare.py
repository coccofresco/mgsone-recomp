import json
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))

from depth_capture_compare import compare_captures


def record(event, frame, **fields):
    return {
        "schema": "ps1trace/v1",
        "source": "test",
        "seq": 1,
        "frame": frame,
        "host_us": frame * 100,
        "event": event,
        **fields,
    }


def projection(frame, projection_id, complete=False, **overrides):
    fields = {
        "projection_id": projection_id,
        "pc": 0x80010000,
        "projection_context": 12,
        "transform_hash": 34,
        "transform_lineage": 56,
        "value_flow_complete": int(complete),
        "model_fingerprint_complete": int(complete),
        "model_source_fingerprint": 0x1234 if complete else 0,
        "model_program_fingerprint": 0x5678 if complete else 0,
        **{name: projection_id + index for index, name in enumerate(
            ("vxy_expr", "vz_expr", "rt0_expr", "rt1_expr", "rt2_expr",
             "rt3_expr", "rt4_expr", "tr0_expr", "tr1_expr", "tr2_expr"), 1)},
    }
    fields.update(overrides)
    return record("gte.projection", frame, **fields)


class DepthCaptureCompareTest(unittest.TestCase):
    def write_trace(self, directory, name, records):
        path = Path(directory) / name
        path.write_text("".join(json.dumps(item) + "\n" for item in records))
        return path

    def equivalent_pair(self):
        oracle = [
            projection(10, 100),
            projection(10, 101),
            record("gpu.gp0_word", 12, word=0x30000000, path=2,
                   command_word_index=0, projection_id=100,
                   projection_context=12, signed_projection=1,
                   intrinsic_provenance=0,
                   model_source_fingerprint=0,
                   model_program_fingerprint=0,
                   submission_id=7, word_address=0x1000, ram_lookup=1),
            record("gpu.gp0_word", 12, word=0x00020001, path=2,
                   command_word_index=1, projection_id=300,
                   projection_context=12, signed_projection=1,
                   submission_id=7, word_address=0x1004, ram_lookup=1),
            record("gpu.dma_node", 12, submission_id=7,
                   traversal_ordinal=0, words=2, list_start=0x2000,
                   node_address=0x2000, next_address=0xFFFFFF),
        ]
        instrumented = [
            record("mips.value_expr", 20, expression_id=900, op=1),
            projection(20, 500, True),
            projection(20, 501, True),
            record("gpu.gp0_word", 22, word=0x30000000, path=2,
                   command_word_index=0, projection_id=500,
                   projection_context=12, signed_projection=1,
                   intrinsic_provenance=1 << 7,
                   model_source_fingerprint=0x1234,
                   model_program_fingerprint=0x5678,
                   submission_id=99, word_address=0x1000, ram_lookup=1),
            record("gpu.gp0_word", 22, word=0x00020001, path=2,
                   command_word_index=1, projection_id=700,
                   projection_context=12, signed_projection=1,
                   submission_id=99, word_address=0x1004, ram_lookup=1),
            record("gpu.dma_node", 22, submission_id=99,
                   traversal_ordinal=0, words=2, list_start=0x2000,
                   node_address=0x2000, next_address=0xFFFFFF),
        ]
        return oracle, instrumented

    def test_rebases_frames_and_process_local_ids(self):
        oracle, instrumented = self.equivalent_pair()
        with tempfile.TemporaryDirectory() as directory:
            report = compare_captures(
                self.write_trace(directory, "oracle.jsonl", oracle),
                self.write_trace(directory, "instrumented.jsonl", instrumented),
            )
        self.assertTrue(report["equivalent"])
        self.assertTrue(report["stream_equal"])

    def test_packet_difference_fails_closed(self):
        oracle, instrumented = self.equivalent_pair()
        instrumented[3]["word"] ^= 1
        with tempfile.TemporaryDirectory() as directory:
            report = compare_captures(
                self.write_trace(directory, "oracle.jsonl", oracle),
                self.write_trace(directory, "instrumented.jsonl", instrumented),
            )
        self.assertFalse(report["equivalent"])
        self.assertEqual(report["first_mismatch"]["index"], 2)

    def test_incomplete_value_flow_cannot_certify_equal_stream(self):
        oracle, instrumented = self.equivalent_pair()
        instrumented[1]["value_flow_complete"] = 0
        with tempfile.TemporaryDirectory() as directory:
            report = compare_captures(
                self.write_trace(directory, "oracle.jsonl", oracle),
                self.write_trace(directory, "instrumented.jsonl", instrumented),
            )
        self.assertTrue(report["stream_equal"])
        self.assertFalse(report["instrumented_value_flow_complete"])
        self.assertFalse(report["equivalent"])

    def test_duplicate_projection_id_is_a_validation_failure(self):
        oracle, instrumented = self.equivalent_pair()
        instrumented[2]["projection_id"] = 500
        with tempfile.TemporaryDirectory() as directory:
            report = compare_captures(
                self.write_trace(directory, "oracle.jsonl", oracle),
                self.write_trace(directory, "instrumented.jsonl", instrumented),
            )
        self.assertFalse(report["equivalent"])
        self.assertTrue(report["validation_errors"]["instrumented"])


if __name__ == "__main__":
    unittest.main()
