#!/usr/bin/env python3
"""Audit bounded temporal replay traces, not whole-image correctness.

Use PS1_TRACE_CRASH3_TEMPORAL_REPLAY=1 and the event
renderer.crash3.temporal_replay_vertex with the Vulkan readback frame bounds.
Quad checks inspect the actual previous-vertex flags sent to Vulkan. The X
comparison is a diagnostic of non-clamped, on-screen packet samples only; it
does not certify sidecar geometry, visibility, display aspect or the camera.
Trace positions are rounded to millipixels; the runtime coherence helper
compares full float records, not these rounded diagnostic coordinates.
"""

import argparse
from collections import defaultdict
import json
from pathlib import Path


EVENT = "renderer.crash3.temporal_replay_vertex"


def audit(records):
    counts = defaultdict(int)
    projection = defaultdict(lambda: defaultdict(int))
    failures = []
    pending = []
    previous_key = None

    def flush():
        if not pending or pending[0]["triangles"] != 2:
            return
        counts["quads"] += 1
        if len(pending) != 6:
            counts["incomplete_quad_traces"] += 1
            failures.append({"kind": "incomplete_quad_trace", "draw": previous_key})
            return
        valid = [r for r in pending if r["valid"]]
        if not valid:
            counts["current_endpoint_quads"] += 1
        elif len(valid) != 6:
            counts["mixed_phase_quads"] += 1
            failures.append({"kind": "mixed_phase_quad", "draw": previous_key})
        else:
            shared = defaultdict(set)
            for r in valid:
                shared[r["source_corner"]].add((r["old_x"], r["old_y"], r["old_w"]))
            if any(len(values) != 1 for values in shared.values()):
                counts["incoherent_shared_corners"] += 1
                failures.append({"kind": "incoherent_shared_corner", "draw": previous_key})
            else:
                counts["interpolated_coherent_quads"] += 1

    for record in records:
        if record.get("event") != EVENT:
            continue
        key = (record["frame"], record["packet"], record["operation"],
               record["first_vertex"] - record["triangle"] * 3)
        if key != previous_key:
            flush()
            pending = []
            previous_key = key
        pending.append(record)
        counts["vertex_records"] += 1
        native_x = record.get("native_x", -1024)
        native_y = record.get("native_y", -1024)
        # Exclude the centre (where both projections coincide), clamped and
        # off-screen source coordinates. Quantized PS1 X allows a 2-pixel
        # diagnostic tolerance; no renderer decision depends on this test.
        if (record["pc"] == 0 or not 0 < native_x < 512
                or not 0 < native_y < 256 or abs(native_x - 256) <= 16):
            continue
        group = projection[record["frame"]]
        x = record["x"] / 1000
        if abs(x - (native_x * 0.75 + 64)) < 2:
            group["wide_075"] += 1
        elif abs(x - native_x) < 2:
            group["native_100"] += 1
        else:
            group["other"] += 1
    flush()
    if not counts["vertex_records"]:
        failures.append({"kind": "empty_trace"})
    return {
        "schema": "crash3-temporal-replay-audit/v1",
        "scope": "Replay previous-vertex coherence; X classes are diagnostics, not a whole-scene aspect gate.",
        "coherence_passed": not failures,
        "counts": dict(counts),
        "failures": failures,
        "projection_samples_by_frame": [
            {"frame": frame, **dict(values)} for frame, values in sorted(projection.items())
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    args = parser.parse_args()
    with args.trace.open(encoding="utf-8") as stream:
        result = audit(json.loads(line) for line in stream if line.strip())
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["coherence_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
