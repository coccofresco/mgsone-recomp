import json
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS / "donor_trace"))

from semantic_segment import (  # noqa: E402
    choose_match,
    parse_step,
    scan_trace,
    write_segment,
)


def record(seq, frame, event, **fields):
    return {
        "schema": "ps1trace/v1",
        "source": "test-donor",
        "seq": seq,
        "frame": frame,
        "host_us": frame * 20_000,
        "event": event,
        **fields,
    }


def write_trace(path, records):
    path.write_text(
        "".join(json.dumps(item) + "\n" for item in records),
        encoding="utf-8",
    )


class SemanticSegmentTest(unittest.TestCase):
    def test_cd_gpu_anchors_ignore_other_channels_and_normalize(self):
        records = [
            record(1, 100, "timing.vblank", index=100),
            record(2, 101, "cdrom.command", command=2, parameters=3, lba=13983),
            record(3, 101, "cdrom.interrupt", type=3),
            record(4, 101, "cdrom.command", command=14, parameters=1),
            record(5, 101, "gpu.display_start", x=0, y=0),
            record(6, 101, "cdrom.command", command=6, parameters=0),
            record(7, 102, "callback.dispatch", callback=1),
            record(8, 102, "gpu.draw_area_start", x=512, y=0),
            record(9, 103, "timing.vblank", index=103),
            record(10, 110, "cdrom.command", command=2, parameters=3, lba=1608),
            record(11, 110, "cdrom.command", command=14, parameters=1),
            record(12, 110, "cdrom.command", command=6, parameters=0),
            record(13, 111, "gpu.display_start", x=512, y=0),
        ]
        start_steps = [
            parse_step("cdrom.command:command=2,parameters=3,lba=13983"),
            parse_step("cdrom.command:command=14,parameters=1"),
            parse_step("cdrom.command:command=6,parameters=0"),
            parse_step("gpu.draw_area_start:x=512,y=0"),
        ]
        end_steps = [
            parse_step("cdrom.command:command=2,parameters=3,lba=1608"),
            parse_step("cdrom.command:command=14,parameters=1"),
            parse_step("cdrom.command:command=6,parameters=0"),
        ]

        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "input.jsonl"
            output = Path(directory) / "segment.jsonl"
            write_trace(source, records)
            trace_source, starts, ends = scan_trace(
                source, start_steps, end_steps
            )
            start = choose_match(starts, "start", None)
            end = choose_match(ends, "end", None, start.last_index)
            emitted = write_segment(source, output, start, end)
            selected = [json.loads(line) for line in output.read_text().splitlines()]

        self.assertEqual(trace_source, "test-donor")
        self.assertEqual(emitted, 11)
        self.assertEqual(selected[0]["seq"], 2)
        self.assertEqual(selected[0]["frame"], 0)
        self.assertEqual(selected[0]["host_us"], 0)
        self.assertEqual(selected[-1]["seq"], 12)
        self.assertNotIn(13, [item["seq"] for item in selected])
        vblank = next(item for item in selected if item["event"] == "timing.vblank")
        self.assertEqual(vblank["frame"], 2)
        self.assertEqual(vblank["index"], 2)

    def test_same_channel_event_breaks_pattern_and_ambiguity_requires_choice(self):
        records = [
            record(1, 1, "cdrom.command", command=2),
            record(2, 1, "cdrom.command", command=1),
            record(3, 1, "cdrom.command", command=14),
            record(4, 1, "cdrom.command", command=6),
            record(5, 2, "cdrom.command", command=2),
            record(6, 2, "cdrom.command", command=14),
            record(7, 2, "cdrom.command", command=6),
            record(8, 3, "cdrom.command", command=2),
            record(9, 3, "cdrom.command", command=14),
            record(10, 3, "cdrom.command", command=6),
        ]
        steps = [
            parse_step("cdrom.command:command=2"),
            parse_step("cdrom.command:command=14"),
            parse_step("cdrom.command:command=6"),
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.jsonl"
            write_trace(path, records)
            _, matches, _ = scan_trace(path, steps, steps)

        self.assertEqual([match.first_record["seq"] for match in matches], [5, 8])
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            choose_match(matches, "start", None)
        self.assertEqual(choose_match(matches, "start", 2).first_record["seq"], 8)


if __name__ == "__main__":
    unittest.main()
