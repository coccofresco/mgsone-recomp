import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from tools.crash3_temporal_phase_validate import midpoint_discontinuity_probe, validate, validate_page_causality


def write_ppm(path: Path, value: bytes) -> None:
    path.write_bytes(b"P6\n2 1\n255\n" + value)


class Crash3TemporalPhaseValidateTest(unittest.TestCase):
    def test_verified_scene_cut_resets_phase_but_still_requires_exact_control(self):
        with TemporaryDirectory() as temporary:
            root = Path(temporary)
            capture, control = root / "capture", root / "control"
            capture.mkdir()
            control.mkdir()
            for serial, (frame, phase) in enumerate((
                (10, "ordinary"), (12, "midpoint"), (13, "endpoint"),
                (14, "ordinary"), (16, "midpoint"), (17, "endpoint"),
            ), 1):
                write_ppm(capture / f"frame-{frame}-present-{serial}-{phase}-scanout.ppm",
                          bytes([(serial - 1) * 32] * 6))
            for serial, (frame, value) in enumerate(((12, 64), (14, 96), (16, 160)), 1):
                write_ppm(control / f"frame-{frame}-present-{serial}-ordinary-scanout.ppm",
                          bytes([value] * 6))
            events = []
            for frame, page in ((12, 512), (16, 0)):
                events.extend((
                    dict(event="renderer.crash3.temporal_schedule", frame=frame,
                         seq=frame, midpoint_selected=1, midpoint_source_x=page,
                         midpoint_source_y=0, previous_source_x=512-page, previous_source_y=0),
                    dict(event="renderer.crash3.temporal_schedule", frame=frame+1,
                         seq=frame+1, endpoint_resolve=1, pending_source_x=page, pending_source_y=0),
                ))
            barrier = dict(event="renderer.crash3.temporal_underlay_history_barrier",
                           frame=14, previous_frame=12, history_discontinuity_mask=6)
            events.append(dict(event="renderer.crash3.temporal_schedule", frame=14,
                               seq=14, current_endpoint_direct_resolve=1,
                               midpoint_source_x=0, midpoint_source_y=0))
            trace = root / "trace.jsonl"
            trace.write_text("\n".join(json.dumps(e) for e in [*events, barrier]))
            report = validate(capture, control, trace_path=trace)
            self.assertTrue(report["passed"], report["failures"])
            self.assertEqual(report["accepted_scene_barrier_serials"], [4])
            self.assertEqual(len(report["control"]["endpoint_matches"]), 3)
            events[-1]["current_endpoint_direct_resolve"] = 0
            trace.write_text("\n".join(json.dumps(e) for e in [*events, barrier]))
            self.assertTrue(validate(capture, trace_path=trace)["temporal_phase_errors"])
            events[-1]["current_endpoint_direct_resolve"] = 1
            trace.write_text("\n".join(json.dumps(e) for e in [*events, barrier]))
            write_ppm(control / "frame-14-present-2-ordinary-scanout.ppm", bytes([97] * 6))
            self.assertFalse(validate(capture, control, trace_path=trace)["passed"])
            barrier["history_discontinuity_mask"] = 16  # unrelated object arrival
            trace.write_text("\n".join(json.dumps(e) for e in [*events, barrier]))
            self.assertTrue(validate(capture, trace_path=trace)["temporal_phase_errors"])

    def test_left_edge_hole_cannot_hide_inside_the_global_average(self):
        a = bytes([100, 100, 100] * 6)
        b = bytes([101, 101, 101] * 6)
        midpoint = bytes([0, 0, 0] + [100, 100, 100] * 5)
        with TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name, pixels in (
                ("frame-10-present-1-ordinary-scanout.ppm", a),
                ("frame-12-present-2-midpoint-scanout.ppm", midpoint),
                ("frame-13-present-3-endpoint-scanout.ppm", b),
            ):
                (root / name).write_bytes(b"P6\n6 1\n255\n" + pixels)
            thresholds = dict(maximum_midpoint_only_dark_fraction=0.25,
                              maximum_stable_endpoint_outlier_fraction=0.25)
            self.assertTrue(validate(root, **thresholds)["passed"])
            report = validate(root, **thresholds, maximum_regional_discontinuity_fraction=0.25)
            self.assertFalse(report["passed"])
            self.assertEqual({f["region"] for f in report["regional_discontinuity_failures"]}, {"left"})
            regions = report["triplets"][0]["discontinuity_probe"]["vertical_thirds"]
            self.assertEqual(regions["left"]["midpoint_only_dark_fraction"], 0.5)
            self.assertEqual(regions["right"]["midpoint_only_dark_fraction"], 0.0)

    def test_regional_probe_rejects_inconsistent_scanout_width(self):
        for width in (0, -1, 4):
            with self.subTest(width=width), self.assertRaisesRegex(ValueError, "width"):
                midpoint_discontinuity_probe(bytes(18), bytes(18), bytes(18), scanout_width=width)

    def test_accepts_midpoint_then_endpoint_from_same_render_page(self):
        with TemporaryDirectory() as temporary:
            trace = Path(temporary) / "trace.jsonl"
            events = [
                {
                    "seq": 10,
                    "frame": 100,
                    "event": "renderer.crash3.temporal_schedule",
                    "midpoint_selected": 1,
                    "endpoint_resolve": 0,
                    "midpoint_source_x": 0,
                    "midpoint_source_y": 0,
                    "previous_source_x": 512,
                    "previous_source_y": 0,
                },
                {
                    "seq": 11,
                    "frame": 101,
                    "event": "renderer.crash3.temporal_schedule",
                    "midpoint_selected": 0,
                    "endpoint_resolve": 1,
                    "pending_source_x": 0,
                    "pending_source_y": 0,
                },
            ]
            trace.write_text(
                "".join(json.dumps(event) + "\n" for event in events),
                encoding="utf-8",
            )

            report = validate_page_causality(trace)

            self.assertTrue(report["passed"])
            self.assertEqual(report["resolved_endpoint_pairs"], 1)
            self.assertEqual(report["distinct_endpoint_page_pairs"], 1)

    def test_rejects_endpoint_from_previous_double_buffer_page(self):
        with TemporaryDirectory() as temporary:
            trace = Path(temporary) / "trace.jsonl"
            events = [
                {
                    "seq": 20,
                    "frame": 200,
                    "event": "renderer.crash3.temporal_schedule",
                    "midpoint_selected": 1,
                    "endpoint_resolve": 0,
                    "midpoint_source_x": 0,
                    "midpoint_source_y": 0,
                    "previous_source_x": 512,
                    "previous_source_y": 0,
                },
                {
                    "seq": 21,
                    "frame": 201,
                    "event": "renderer.crash3.temporal_schedule",
                    "midpoint_selected": 0,
                    "endpoint_resolve": 1,
                    "pending_source_x": 512,
                    "pending_source_y": 0,
                },
            ]
            trace.write_text(
                "".join(json.dumps(event) + "\n" for event in events),
                encoding="utf-8",
            )

            report = validate_page_causality(trace)

            self.assertFalse(report["passed"])
            self.assertEqual(len(report["page_route_errors"]), 1)

    def test_rejects_endpoint_not_presented_on_next_vblank(self):
        with TemporaryDirectory() as temporary:
            trace = Path(temporary) / "trace.jsonl"
            events = [
                {
                    "seq": 30,
                    "frame": 300,
                    "event": "renderer.crash3.temporal_schedule",
                    "midpoint_selected": 1,
                    "endpoint_resolve": 0,
                    "midpoint_source_x": 0,
                    "midpoint_source_y": 0,
                    "previous_source_x": 512,
                    "previous_source_y": 0,
                },
                {
                    "seq": 34,
                    "frame": 302,
                    "event": "renderer.crash3.temporal_schedule",
                    "midpoint_selected": 0,
                    "endpoint_resolve": 1,
                    "pending_source_x": 0,
                    "pending_source_y": 0,
                },
            ]
            trace.write_text(
                "".join(json.dumps(event) + "\n" for event in events),
                encoding="utf-8",
            )

            report = validate_page_causality(trace)

            self.assertFalse(report["passed"])
            self.assertEqual(
                report["page_route_errors"][0]["error"],
                "deferred endpoint was not presented on the next VBlank",
            )

    def test_accepts_distinct_midpoint_and_exact_control_endpoint(self):
        with TemporaryDirectory() as temporary:
            root = Path(temporary)
            capture = root / "capture"
            control = root / "control"
            capture.mkdir()
            control.mkdir()
            a = bytes([0, 0, 0, 0, 0, 0])
            m = bytes([32, 0, 0, 0, 0, 0])
            b = bytes([64, 0, 0, 0, 0, 0])
            write_ppm(capture / "frame-10-present-1-ordinary-scanout.ppm", a)
            write_ppm(capture / "frame-12-present-2-midpoint-scanout.ppm", m)
            write_ppm(capture / "frame-13-present-3-endpoint-scanout.ppm", b)
            write_ppm(control / "frame-12-present-1-ordinary-scanout.ppm", b)

            report = validate(capture, control)

            self.assertTrue(report["passed"])
            self.assertEqual(report["moving_triplets"], 1)
            self.assertEqual(report["distinct_moving_triplets"], 1)
            self.assertEqual(len(report["control"]["endpoint_matches"]), 1)

    def test_rejects_endpoint_copy_as_midpoint(self):
        with TemporaryDirectory() as temporary:
            capture = Path(temporary)
            a = bytes([0, 0, 0, 0, 0, 0])
            b = bytes([64, 0, 0, 0, 0, 0])
            write_ppm(capture / "frame-10-present-1-ordinary-scanout.ppm", a)
            write_ppm(capture / "frame-12-present-2-midpoint-scanout.ppm", b)
            write_ppm(capture / "frame-13-present-3-endpoint-scanout.ppm", b)

            report = validate(capture, minimum_distinct_fraction=1.0)

            self.assertFalse(report["passed"])
            self.assertEqual(report["midpoint_matches_b_serials"], [2])

    def test_rejects_phase_or_serial_discontinuity(self):
        with TemporaryDirectory() as temporary:
            capture = Path(temporary)
            a = bytes([0, 0, 0, 0, 0, 0])
            m = bytes([32, 0, 0, 0, 0, 0])
            b = bytes([64, 0, 0, 0, 0, 0])
            write_ppm(capture / "frame-10-present-1-ordinary-scanout.ppm", a)
            write_ppm(capture / "frame-12-present-3-midpoint-scanout.ppm", m)
            write_ppm(capture / "frame-13-present-4-midpoint-scanout.ppm", b)

            report = validate(capture)

            self.assertFalse(report["passed"])
            self.assertTrue(report["serial_gaps"])
            self.assertTrue(report["temporal_phase_errors"])

    def test_reports_and_can_gate_midpoint_only_holes(self):
        with TemporaryDirectory() as temporary:
            capture = Path(temporary)
            a = bytes([96, 80, 64, 20, 30, 40])
            m = bytes([0, 0, 0, 20, 30, 40])
            b = bytes([96, 80, 64, 20, 30, 40])
            write_ppm(capture / "frame-10-present-1-ordinary-scanout.ppm", a)
            write_ppm(capture / "frame-12-present-2-midpoint-scanout.ppm", m)
            write_ppm(capture / "frame-13-present-3-endpoint-scanout.ppm", b)

            report = validate(
                capture,
                maximum_midpoint_only_dark_fraction=0.0,
                maximum_stable_endpoint_outlier_fraction=0.0,
            )

            self.assertFalse(report["passed"])
            self.assertEqual(report["midpoint_dark_failure_serials"], [2])
            self.assertEqual(report["midpoint_outlier_failure_serials"], [2])
            probe = report["triplets"][0]["discontinuity_probe"]
            self.assertEqual(probe["midpoint_only_dark_pixels"], 1)
            self.assertEqual(probe["stable_endpoint_outlier_pixels"], 1)


if __name__ == "__main__":
    unittest.main()
