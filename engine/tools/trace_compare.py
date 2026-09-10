#!/usr/bin/env python3
"""Compare normalized PS1 event traces from the runtime and donor projects.

The input format is JSON Lines, schema ``ps1trace/v1``.  Producers may add
fields; comparison only uses the stable fields declared in EVENT_FIELDS.

Examples:
  tools/trace_compare.py /tmp/crash3-recomp.jsonl /tmp/crash3-redux.jsonl
  tools/trace_compare.py beetle.jsonl swan.jsonl --anchor gpu.display_start:x=512,y=0
  tools/trace_compare.py trace.jsonl --json
  tools/trace_compare.py before.jsonl after.jsonl --native-latch-hashes --latch-from 200
"""

from __future__ import annotations

import argparse
import collections
import difflib
import json
import statistics
import sys
from pathlib import Path
from typing import Any


SCHEMA = "ps1trace/v1"
EVENT_FIELDS: dict[str, tuple[str, ...]] = {
    "timing.vblank": ("index",),
    "gpu.vblank_latch": (
        "display_x", "display_y", "display_w", "display_h",
        "draw_x0", "draw_y0", "draw_x1", "draw_y1",
        "draw_offset_x", "draw_offset_y", "display_draw_overlap",
    ),
    "host.present": ("display_x", "display_y", "display_w", "display_h"),
    "gpu.display_start": ("x", "y"),
    "gpu.display_enable": ("enabled",),
    "gpu.display_range_h": ("x0", "x1"),
    "gpu.display_range_v": ("y0", "y1"),
    "gpu.display_mode": ("mode",),
    "gpu.draw_area_start": ("x", "y"),
    "gpu.draw_area_end": ("x", "y"),
    "gpu.draw_offset": ("x", "y"),
    "gpu.fast_fill": ("x", "y", "w", "h", "color"),
    "gpu.dma_list": ("nodes", "words"),
    "gpu.gp0_word": ("word",),
    # Physical/current-sector bookkeeping differs by core. Keep LBA in the
    # trace for inspection, but compare the hardware command and FIFO count.
    "cdrom.command": ("command", "parameters"),
    "cdrom.interrupt": ("type",),
    "spu.key_on": ("voices",),
    "spu.key_off": ("voices",),
    "gte.command": (
        "command", "sf", "lm", "flag", "ir0", "ir1", "ir2", "ir3",
        "sxy0", "sxy1", "sxy2", "sz0", "sz1", "sz2", "sz3",
        "mac0", "mac1", "mac2", "mac3", "rgb2", "otz",
    ),
}


def read_trace(path: Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    with path.open(encoding="utf-8") as stream:
        for lineno, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{lineno}: invalid JSON: {exc}") from exc
            if record.get("schema") != SCHEMA:
                raise ValueError(
                    f"{path}:{lineno}: schema {record.get('schema')!r}, expected {SCHEMA!r}"
                )
            if not isinstance(record.get("event"), str):
                raise ValueError(f"{path}:{lineno}: missing string event")
            records.append(record)
    if not records:
        raise ValueError(f"{path}: empty trace")
    return records


def parse_anchor(spec: str) -> tuple[str, dict[str, Any]]:
    event, separator, assignments = spec.partition(":")
    if not event:
        raise ValueError("anchor must start with an event name")

    fields: dict[str, Any] = {}
    if separator:
        for assignment in assignments.split(","):
            key, equals, raw_value = assignment.partition("=")
            if not key or not equals:
                raise ValueError(
                    f"invalid anchor field {assignment!r}; expected field=value"
                )
            try:
                fields[key] = json.loads(raw_value)
            except json.JSONDecodeError:
                fields[key] = raw_value
    return event, fields


def anchor_trace(
    records: list[dict[str, Any]], event: str, fields: dict[str, Any]
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    anchor_index = next((
        index for index, record in enumerate(records)
        if record["event"] == event
        and all(record.get(key) == value for key, value in fields.items())
    ), None)
    if anchor_index is None:
        criteria = ",".join(f"{key}={value!r}" for key, value in fields.items())
        suffix = f":{criteria}" if criteria else ""
        raise ValueError(f"anchor {event}{suffix} not found")

    anchor = records[anchor_index]
    base_frame = int(anchor.get("frame", 0))
    base_us = int(anchor.get("host_us", 0))
    normalized: list[dict[str, Any]] = []
    for original in records[anchor_index:]:
        record = dict(original)
        if "frame" in record:
            record["frame"] = int(record["frame"]) - base_frame
        if "host_us" in record:
            record["host_us"] = int(record["host_us"]) - base_us
        if record["event"] == "timing.vblank" and "index" in record:
            record["index"] = int(record["index"]) - base_frame
        normalized.append(record)

    return normalized, {
        "event": event,
        "fields": fields,
        "source_frame": base_frame,
        "source_host_us": base_us,
    }


def _event_key(record: dict[str, Any]) -> tuple[Any, ...]:
    fields = EVENT_FIELDS.get(record["event"], ())
    return (record["event"],) + tuple(record.get(field) for field in fields)


def compare_native_latches(
    left: list[dict[str, Any]], right: list[dict[str, Any]],
    first: int | None = None, last: int | None = None, max_diffs: int = 5,
) -> dict[str, Any]:
    """Compare sealed native images, not mutable VRAM or host presentation.

    An explicit range requires every latch, not merely the intersection of
    the traces. Without a range use the union of their latch extents. Restore
    epochs with repeated latch numbers must be split before comparison.
    Presentation time, the trace's outer ``frame`` and modern hashes cannot
    substitute for the native latch identity or its valid aperture hash.
    """
    if first is not None and first < 0 or last is not None and last < 0:
        raise ValueError("native latch bounds must be nonnegative")
    if first is not None and last is not None and first > last:
        raise ValueError("native latch start exceeds end")
    streams: list[dict[int, list[dict[str, Any]]]] = []
    for name, records in (("left", left), ("right", right)):
        latches: dict[int, list[dict[str, Any]]] = collections.defaultdict(list)
        for record in records:
            if record["event"] != "gpu.vblank_latch":
                continue
            frame = record.get("latch_frame")
            if type(frame) is not int or frame < 0:
                raise ValueError(f"{name}: missing or invalid native latch_frame")
            if first is not None and frame < first or last is not None and frame > last:
                continue
            latches[frame].append(record)
        if not latches:
            raise ValueError(f"{name}: no native latches in the requested range")
        streams.append(latches)
    lo = first if first is not None else min(min(stream) for stream in streams)
    hi = last if last is not None else max(max(stream) for stream in streams)
    failures: collections.Counter[str] = collections.Counter()
    examples: list[dict[str, Any]] = []
    compared = 0
    matched = 0

    def fail(kind: str, frame: int, **details: Any) -> None:
        failures[kind] += 1
        if len(examples) < max(1, max_diffs):
            examples.append({"kind": kind, "latch_frame": frame, **details})

    for frame in range(lo, hi + 1):
        values = []
        for name, stream in zip(("left", "right"), streams):
            entries = stream.get(frame, [])
            if len(entries) != 1:
                fail(f"{name}_missing" if not entries else f"{name}_duplicate", frame)
                continue
            record = entries[0]
            value = tuple(record.get(key) for key in ("native_hash", "aperture_w", "aperture_h"))
            if (record.get("hash_valid") != 1
                    or any(type(v) is not int for v in value)
                    or not -(1 << 63) <= value[0] < (1 << 64)
                    or value[1] <= 0 or value[2] <= 0):
                fail(f"{name}_invalid_hash", frame)
                continue
            # ps1trace numeric fields use int64; donors may serialize the
            # same 64 hash bits as uint64. Preserve bits, never round to float.
            values.append((value[0] & ((1 << 64) - 1), value[1], value[2]))
        if len(values) != 2:
            continue
        compared += 1
        if values[0] == values[1]:
            matched += 1
        else:
            fail("native_image_mismatch", frame, left=values[0], right=values[1])
    return {
        "passed": not failures,
        "latch_range": [lo, hi],
        "expected_latches": hi - lo + 1,
        "compared_latches": compared,
        "matching_latches": matched,
        "failure_counts": dict(failures),
        "first_failures": examples,
    }


def _measured_hz(records: list[dict[str, Any]]) -> float | None:
    ticks = [r for r in records if r["event"] == "timing.vblank"]
    if len(ticks) < 2:
        return None
    elapsed_us = ticks[-1].get("host_us", 0) - ticks[0].get("host_us", 0)
    frame_delta = ticks[-1].get("index", ticks[-1].get("frame", 0)) - ticks[0].get(
        "index", ticks[0].get("frame", 0)
    )
    return frame_delta * 1_000_000 / elapsed_us if elapsed_us > 0 else None


def analyze(records: list[dict[str, Any]]) -> dict[str, Any]:
    counts = collections.Counter(r["event"] for r in records)
    source = records[0].get("source", "unknown")
    frames = [int(r.get("frame", 0)) for r in records]
    display = [r for r in records if r["event"] == "gpu.display_start"]
    latches = [r for r in records if r["event"] == "gpu.vblank_latch"]
    presents = [r for r in records if r["event"] == "host.present"]
    key_ons = [r for r in records if r["event"] == "spu.key_on"]
    key_offs = [r for r in records if r["event"] == "spu.key_off"]
    cd_commands = [r for r in records if r["event"] == "cdrom.command"]

    transitions: list[list[int]] = []
    previous: tuple[int, int] | None = None
    writes_per_frame: collections.Counter[int] = collections.Counter()
    for record in display:
        current = (int(record.get("x", 0)), int(record.get("y", 0)))
        writes_per_frame[int(record.get("frame", 0))] += 1
        if current != previous:
            transitions.append([*current, int(record.get("frame", 0))])
            previous = current

    flip_frames = [int(record.get("frame", 0)) for record in display]
    flip_intervals = collections.Counter(
        current - previous for previous, current in zip(flip_frames, flip_frames[1:])
    )

    latch_by_frame = {
        int(r.get("frame", 0)): (r.get("display_x"), r.get("display_y"))
        for r in latches
    }
    present_mismatches = []
    present_latencies = []
    latch_frames = collections.Counter(int(r.get("frame", 0)) for r in latches)
    present_frames = collections.Counter(int(r.get("frame", 0)) for r in presents)
    latch_time_by_frame = {
        int(r.get("frame", 0)): int(r["host_us"])
        for r in latches if "host_us" in r
    }
    for record in presents:
        frame = int(record.get("frame", 0))
        latched = latch_by_frame.get(frame)
        shown = (record.get("display_x"), record.get("display_y"))
        if latched is not None and shown != latched:
            present_mismatches.append(
                {"frame": frame, "latched": list(latched), "presented": list(shown)}
            )

        if frame in latch_time_by_frame and "host_us" in record:
            present_latencies.append(int(record["host_us"]) - latch_time_by_frame[frame])

    key_on = key_ons[0] if key_ons else None
    key_on_per_frame = collections.Counter(int(r.get("frame", 0)) for r in key_ons)
    vblank_times = [
        int(r["host_us"]) for r in records
        if r["event"] == "timing.vblank" and "host_us" in r
    ]
    intervals = [b - a for a, b in zip(vblank_times, vblank_times[1:])]
    return {
        "source": source,
        "records": len(records),
        "frame_range": [min(frames), max(frames)],
        "measured_hz": _measured_hz(records),
        "vblank_interval_us_median": statistics.median(intervals) if intervals else None,
        "event_counts": dict(sorted(counts.items())),
        "display": {
            "writes": len(display),
            "transitions": transitions[:64],
            "frames_with_multiple_writes": sorted(
                frame for frame, count in writes_per_frame.items() if count > 1
            )[:64],
            "write_interval_frames_histogram": {
                str(interval): count for interval, count in sorted(flip_intervals.items())
            },
            "latch_present_mismatches": present_mismatches[:64],
            "present_without_latch_frames": sorted(
                frame for frame in present_frames if frame not in latch_frames
            )[:64],
            "latch_without_present_frames": sorted(
                frame for frame in latch_frames if frame not in present_frames
            )[:64],
            "duplicate_latch_frames": sorted(
                frame for frame, count in latch_frames.items() if count > 1
            )[:64],
            "duplicate_present_frames": sorted(
                frame for frame, count in present_frames.items() if count > 1
            )[:64],
            "negative_latch_to_present_frames": sorted(
                int(record.get("frame", 0))
                for record in presents
                if int(record.get("frame", 0)) in latch_time_by_frame
                and "host_us" in record
                and int(record["host_us"])
                    < latch_time_by_frame[int(record.get("frame", 0))]
            )[:64],
            "latch_to_present_us_median": (
                statistics.median(present_latencies) if present_latencies else None
            ),
            "latch_to_present_us_max": max(present_latencies) if present_latencies else None,
            "draw_overlap_frames": [
                int(r.get("frame", 0))
                for r in latches if int(r.get("display_draw_overlap", 0)) != 0
            ][:64],
        },
        "first_audio_key_on": None if key_on is None else {
            "frame": key_on.get("frame"),
            "host_us": key_on.get("host_us"),
            "voices": key_on.get("voices"),
        },
        "audio": {
            "key_on_writes": len(key_ons),
            "key_off_writes": len(key_offs),
            "frames_with_multiple_key_ons": sorted(
                frame for frame, count in key_on_per_frame.items() if count > 1
            )[:64],
        },
        "cdrom": {
            "commands": len(cd_commands),
            "command_histogram": dict(sorted(collections.Counter(
                int(r.get("command", -1)) for r in cd_commands
            ).items())),
        },
    }


def compare(left: list[dict[str, Any]], right: list[dict[str, Any]], max_diffs: int) -> dict[str, Any]:
    # Use the union, not the intersection: an entirely missing event stream is
    # itself a high-value donor difference and must not disappear from output.
    event_names = sorted(
        set(EVENT_FIELDS)
        & ({r["event"] for r in left} | {r["event"] for r in right})
    )
    comparisons: dict[str, Any] = {}
    for event in event_names:
        lhs = [r for r in left if r["event"] == event]
        rhs = [r for r in right if r["event"] == event]
        lhs_keys = [_event_key(record) for record in lhs]
        rhs_keys = [_event_key(record) for record in rhs]
        matcher = difflib.SequenceMatcher(
            None, lhs_keys, rhs_keys, autojunk=False
        )
        matching_blocks = [
            {"left_index": block.a, "right_index": block.b, "count": block.size}
            for block in matcher.get_matching_blocks()
            if block.size > 0
        ]
        aligned_matches = sum(block["count"] for block in matching_blocks)
        mismatches = []
        for index, (a, b) in enumerate(zip(lhs, rhs)):
            if _event_key(a) != _event_key(b):
                mismatches.append({
                    "index": index,
                    "left_frame": a.get("frame"),
                    "right_frame": b.get("frame"),
                    "left": list(_event_key(a)[1:]),
                    "right": list(_event_key(b)[1:]),
                })
                if len(mismatches) >= max_diffs:
                    break
        comparisons[event] = {
            "left_count": len(lhs),
            "right_count": len(rhs),
            "matching_prefix": next(
                (i for i, (a, b) in enumerate(zip(lhs, rhs)) if _event_key(a) != _event_key(b)),
                min(len(lhs), len(rhs)),
            ),
            "aligned_matches": aligned_matches,
            "aligned_similarity": (
                (2.0 * aligned_matches) / (len(lhs) + len(rhs))
                if lhs or rhs else 1.0
            ),
            "aligned_left_only": len(lhs) - aligned_matches,
            "aligned_right_only": len(rhs) - aligned_matches,
            "aligned_blocks": matching_blocks[:16],
            "mismatches": mismatches,
        }
        if event == "gte.command":
            comparisons[event]["strict_hash_matching_prefix"] = next(
                (
                    i for i, (a, b) in enumerate(zip(lhs, rhs))
                    if (a.get("input_hash"), a.get("output_hash"))
                    != (b.get("input_hash"), b.get("output_hash"))
                ),
                min(len(lhs), len(rhs)),
            )
    return comparisons


def _print_summary(label: str, report: dict[str, Any]) -> None:
    hz = report["measured_hz"]
    print(f"{label}: {report['source']} — {report['records']} events, frames {report['frame_range'][0]}..{report['frame_range'][1]}")
    if hz is not None:
        print(f"  clock: {hz:.3f} Hz (median {report['vblank_interval_us_median']:.0f} us)")
    display = report["display"]
    print(
        f"  display: {display['writes']} GP1(05), "
        f"{len(display['transitions'])} transitions, "
        f"{len(display['latch_present_mismatches'])} latch/present mismatches"
    )
    if display["write_interval_frames_histogram"]:
        print(f"  display write intervals (frames): {display['write_interval_frames_histogram']}")
    if display["latch_to_present_us_median"] is not None:
        print(
            f"  latch→present: median {display['latch_to_present_us_median']:.0f} us, "
            f"max {display['latch_to_present_us_max']} us"
        )
    integrity_warnings = {
        key: display[key]
        for key in (
            "present_without_latch_frames",
            "latch_without_present_frames",
            "duplicate_latch_frames",
            "duplicate_present_frames",
            "negative_latch_to_present_frames",
        )
        if display[key]
    }
    if integrity_warnings:
        print(f"  warning: latch/present integrity {integrity_warnings}")
    if display["frames_with_multiple_writes"]:
        print(f"  warning: multiple display writes in frames {display['frames_with_multiple_writes'][:8]}")
    if display["draw_overlap_frames"]:
        print(f"  warning: display overlaps draw area in frames {display['draw_overlap_frames'][:8]}")
    if report["first_audio_key_on"]:
        audio = report["first_audio_key_on"]
        print(f"  first SPU key-on: frame {audio['frame']}, {audio['host_us']} us")
    audio = report["audio"]
    print(f"  SPU writes: {audio['key_on_writes']} key-on, {audio['key_off_writes']} key-off")
    if audio["frames_with_multiple_key_ons"]:
        print(f"  audio multi-key-on frames: {audio['frames_with_multiple_key_ons'][:8]}")
    print(f"  CD commands: {report['cdrom']['commands']} {report['cdrom']['command_histogram']}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("left", type=Path)
    parser.add_argument("right", nargs="?", type=Path)
    parser.add_argument("--max-diffs", type=int, default=5)
    parser.add_argument(
        "--anchor",
        help="trim and normalize each trace at EVENT[:field=value,...]",
    )
    parser.add_argument("--json", action="store_true", dest="as_json")
    parser.add_argument("--native-latch-hashes", action="store_true",
                        help="strict native-image comparison by sealed latch identity (nonzero on failure)")
    parser.add_argument("--latch-from", type=int)
    parser.add_argument("--latch-to", type=int)
    args = parser.parse_args()
    if args.native_latch_hashes and (args.right is None or args.anchor):
        parser.error("--native-latch-hashes requires two traces and cannot use --anchor")
    if not args.native_latch_hashes and (args.latch_from is not None or args.latch_to is not None):
        parser.error("latch bounds require --native-latch-hashes")
    try:
        left = read_trace(args.left)
        right = read_trace(args.right) if args.right else None
        if args.native_latch_hashes:
            result = compare_native_latches(left, right, args.latch_from, args.latch_to,
                                            args.max_diffs)
            if args.as_json:
                print(json.dumps(result, indent=2))
            else:
                print(f"native latches: {result['matching_latches']}/{result['expected_latches']} "
                      f"identical, range {result['latch_range']}")
                if not result["passed"]:
                    print(json.dumps(result["failure_counts"], sort_keys=True))
                    print(json.dumps(result["first_failures"], indent=2))
            return 0 if result["passed"] else 1
        anchors = None
        if args.anchor:
            anchor_event, anchor_fields = parse_anchor(args.anchor)
            left, left_anchor = anchor_trace(left, anchor_event, anchor_fields)
            right_anchor = None
            if right is not None:
                right, right_anchor = anchor_trace(right, anchor_event, anchor_fields)
            anchors = {"left": left_anchor, "right": right_anchor}
    except (OSError, ValueError) as exc:
        print(exc, file=sys.stderr)
        return 2

    output: dict[str, Any] = {"left": analyze(left)}
    if anchors is not None:
        output["anchors"] = anchors
    if right is not None:
        output["right"] = analyze(right)
        output["comparison"] = compare(left, right, max(1, args.max_diffs))

    if args.as_json:
        print(json.dumps(output, indent=2))
        return 0

    _print_summary("left", output["left"])
    if right is not None:
        _print_summary("right", output["right"])
        print("comparison:")
        for event, result in output["comparison"].items():
            status = "OK" if not result["mismatches"] and result["left_count"] == result["right_count"] else "DIFF"
            print(
                f"  {status:4} {event:<28} "
                f"count {result['left_count']}/{result['right_count']}, "
                f"matching prefix {result['matching_prefix']}, "
                f"aligned {result['aligned_matches']} "
                f"({result['aligned_similarity']:.3f})"
            )
            for mismatch in result["mismatches"][:2]:
                print(
                    f"       #{mismatch['index']} frame {mismatch['left_frame']}/{mismatch['right_frame']}: "
                    f"{mismatch['left']} != {mismatch['right']}"
                )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
