import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))

from widescreen_edge_trace import analyze_catalog


def bits(value):
    payload = struct.unpack("<Q", struct.pack("<d", float(value)))[0]
    return payload if payload < (1 << 63) else payload - (1 << 64)


def trace(event, frame=7, **fields):
    return {
        "schema": "ps1trace/v1",
        "source": "test",
        "seq": 1,
        "frame": frame,
        "host_us": 0,
        "event": event,
        **fields,
    }


def owner(token, points, pc=0x8003CFB8):
    item = trace(
        "replay.visibility_owner",
        owner_schema=3,
        owner=token,
        comparable=1,
        valid_topology=1,
    )
    for index, (x, y) in enumerate(points):
        item[f"v{index}_precise_x_bits"] = bits(x)
        item[f"v{index}_precise_y_bits"] = bits(y)
        item[f"v{index}_game_profile_gte_pc"] = pc
        item[f"v{index}_game_profile_object_source"] = 0
        item[f"v{index}_transform_lineage"] = token * 10
        item[f"v{index}_dma_node"] = token * 100
    return item


class WidescreenEdgeTraceTest(unittest.TestCase):
    def write(self, records):
        directory = tempfile.TemporaryDirectory()
        path = Path(directory.name) / "catalog.jsonl"
        path.write_text("".join(json.dumps(item) + "\n" for item in records))
        self.addCleanup(directory.cleanup)
        return path

    def records(self, owners):
        return [
            trace(
                "replay.visibility_audit",
                atlas_x=2048,
                atlas_y=0,
                width=2048,
                height=1024,
            ),
            trace(
                "replay.visibility_catalog",
                catalog_schema=1,
                owner_schema=3,
                owners=len(owners),
                eligible_owners=len(owners),
                emitted_owners=len(owners),
                conflict_referenced_owners=0,
            ),
            *owners,
        ]

    def test_reports_edge_intervals_in_page_local_coordinates(self):
        owners = [
            owner(1, [(500, 10), (520, 20), (520, 40)]),
            owner(2, [(500, 80), (520, 100), (500, 120)], 0x80041A30),
            owner(3, [(512, 30), (530, 45), (530, 60)]),
        ]
        report = analyze_catalog(
            self.write(self.records(owners)), page_x=512, width=32, height=128
        )
        frame = report["frames"][0]
        self.assertEqual(frame["precise_x_range"], [-12.0, 18.0])
        self.assertEqual(
            frame["left"]["covered_intervals"],
            [[16.0, 28.0], [30.0, 30.0], [92.0, 108.0]],
        )
        self.assertEqual(frame["right"]["covered_intervals"], [])
        self.assertEqual(frame["left"]["groups"][0]["gte_pcs"], ["0x8003CFB8"])

    def test_rejects_incomplete_catalog(self):
        records = self.records([owner(1, [(0, 0), (1, 0), (0, 1)])])
        records[1]["emitted_owners"] = 2
        with self.assertRaisesRegex(ValueError, "incomplete catalog"):
            analyze_catalog(self.write(records), page_x=0)

    def test_rejects_legacy_owner_schema(self):
        records = self.records([owner(1, [(0, 0), (1, 0), (0, 1)])])
        records[1]["owner_schema"] = 2
        with self.assertRaisesRegex(ValueError, "requires owner schema 3"):
            analyze_catalog(self.write(records), page_x=0)

    def test_accepts_complete_presented_surface_catalog(self):
        triangle = owner(1, [(-1, 4), (1, 8), (-1, 12)])
        records = self.records([triangle])
        records[1] = trace(
            "replay.presented_surface_catalog",
            catalog_schema=1,
            owner_schema=3,
            owners=1,
            eligible_surface_owners=1,
            eligible_semi_transparent_owners=1,
        )
        report = analyze_catalog(self.write(records), page_x=0)
        self.assertEqual(
            report["frames"][0]["catalog_event"],
            "replay.presented_surface_catalog",
        )
        self.assertEqual(report["frames"][0]["semi_transparent_owners"], 1)


if __name__ == "__main__":
    unittest.main()
