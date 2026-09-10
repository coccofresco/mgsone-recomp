import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))

from crash3_resident_coverage import analyze_catalog


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


def face(ordinal, points):
    item = trace(
        "game.crash3.resident_projection_face",
        face_schema=1,
        capture_id=9,
        output_ordinal=ordinal,
        topology_ordinal=ordinal,
        packed=ordinal,
        world=0,
        half=0,
        source_selected=1,
        near_clipped=0,
        material_decoded=1,
        semi_transparent=0,
    )
    for index, (x, y, w) in enumerate(points):
        item[f"v{index}_x_bits"] = bits(x)
        item[f"v{index}_y_bits"] = bits(y)
        item[f"v{index}_w_bits"] = bits(w)
    return item


class Crash3ResidentCoverageTest(unittest.TestCase):
    def write(self, faces, expected=None, *, catalog_schema=1, face_schema=1):
        directory = tempfile.TemporaryDirectory()
        path = Path(directory.name) / "catalog.jsonl"
        catalog = trace(
            "game.crash3.resident_projection_catalog",
            catalog_schema=catalog_schema,
            face_schema=face_schema,
            capture_id=9,
            topology_fingerprint=-1,
            source_faces=2,
            resident_faces=2,
            projected_faces=len(faces) if expected is None else expected,
            near_generated_faces=0,
            near_rejected_faces=0,
        )
        path.write_text(
            "".join(json.dumps(item) + "\n" for item in [catalog, *faces]),
            encoding="utf-8",
        )
        self.addCleanup(directory.cleanup)
        return path

    def test_rasterizes_complete_quad_and_welds_internal_edge(self):
        faces = [
            face(0, [(0, 0, 1), (8, 0, 1), (0, 8, 1)]),
            face(1, [(8, 0, 1), (8, 8, 1), (0, 8, 1)]),
        ]
        report = analyze_catalog(
            self.write(faces), width=8, height=8, native_margin=1
        )
        self.assertEqual(
            report["coverage"]["complete"]["projected_covered_pixels"], 64
        )
        self.assertEqual(
            report["projected_topology"]["exact_boundary_edges"], 4
        )
        self.assertEqual(
            report["projected_topology"]["nonmanifold_exact_edges"], 0
        )
        self.assertEqual(report["topology_fingerprint"], (1 << 64) - 1)

    def test_rejects_incomplete_catalog(self):
        with self.assertRaisesRegex(ValueError, "incomplete catalog"):
            analyze_catalog(
                self.write([face(0, [(0, 0, 1), (1, 0, 1), (0, 1, 1)])], 2),
                width=8,
                height=8,
                native_margin=1,
            )

    def test_accepts_current_catalog_and_face_schemas(self):
        faces = [
            {
                **face(0, [(0, 0, 1), (8, 0, 1), (0, 8, 1)]),
                "face_schema": 3,
            },
            {
                **face(1, [(8, 0, 1), (8, 8, 1), (0, 8, 1)]),
                "face_schema": 3,
            },
        ]
        report = analyze_catalog(
            self.write(faces, catalog_schema=4, face_schema=3),
            width=8,
            height=8,
            native_margin=1,
        )
        self.assertEqual(
            report["coverage"]["complete"]["projected_covered_pixels"], 64
        )

    def test_reports_intrinsic_background_underlay_union(self):
        background = {
            **face(0, [(2, 0, 1), (6, 0, 1), (2, 8, 1)]),
            "face_schema": 3,
            "resource_flags": 1,
        }
        report = analyze_catalog(
            self.write([background], catalog_schema=4, face_schema=3),
            width=8,
            height=8,
            native_margin=1,
            background_underlay_scale=2.0,
        )
        self.assertEqual(
            report["catalog"]["opaque_background_underlay_faces"], 1
        )
        self.assertGreater(
            report["presented_union_coverage"]["complete"][
                "projected_covered_pixels"
            ],
            report["coverage"]["complete"]["projected_covered_pixels"],
        )


if __name__ == "__main__":
    unittest.main()
