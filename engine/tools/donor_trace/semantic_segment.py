#!/usr/bin/env python3
"""Extract a donor-comparable trace segment between semantic event anchors.

Anchor matching never uses guest frame numbers.  Each anchor is an ordered
sequence of event predicates.  Events whose names do not occur in the pattern
are ignored while matching, while a non-matching event on an anchor channel
breaks adjacency.  This makes a CD command sequence optionally followed by a
GPU state change stable across frontends with different VBlank scheduling.

The selected segment retains every original event and normalizes frame,
VBlank index, and host time to the first start-anchor event.  Feed the two
resulting JSONL files to tools/trace_compare.py.
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable


TOOLS_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS_DIR))

from trace_compare import SCHEMA, parse_anchor  # noqa: E402


@dataclass(frozen=True)
class AnchorStep:
    event: str
    fields: dict[str, Any]


@dataclass(frozen=True)
class AnchorMatch:
    first_index: int
    last_index: int
    first_record: dict[str, Any]
    last_record: dict[str, Any]


@dataclass
class PatternScanner:
    steps: list[AnchorStep]
    matches: list[AnchorMatch] = field(default_factory=list)

    def __post_init__(self) -> None:
        if not self.steps:
            raise ValueError("an anchor pattern must contain at least one step")
        self._events = {step.event for step in self.steps}
        self._window: collections.deque[tuple[int, dict[str, Any]]] = (
            collections.deque(maxlen=len(self.steps))
        )

    def feed(self, index: int, record: dict[str, Any]) -> None:
        if record["event"] not in self._events:
            return
        self._window.append((index, record))
        if len(self._window) != len(self.steps):
            return
        if not all(
            record_matches_step(candidate, step)
            for (_, candidate), step in zip(self._window, self.steps)
        ):
            return
        first_index, first_record = self._window[0]
        last_index, last_record = self._window[-1]
        self.matches.append(
            AnchorMatch(
                first_index,
                last_index,
                dict(first_record),
                dict(last_record),
            )
        )


def parse_step(spec: str) -> AnchorStep:
    event, fields = parse_anchor(spec)
    return AnchorStep(event, fields)


def record_matches_step(record: dict[str, Any], step: AnchorStep) -> bool:
    return record["event"] == step.event and all(
        name in record and record[name] == expected
        for name, expected in step.fields.items()
    )


def scan_trace(
    path: Path,
    start_steps: list[AnchorStep],
    end_steps: list[AnchorStep],
) -> tuple[str, list[AnchorMatch], list[AnchorMatch]]:
    start_scanner = PatternScanner(start_steps)
    end_scanner = PatternScanner(end_steps)
    source = "unknown"
    records = 0
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
                    f"{path}:{lineno}: schema {record.get('schema')!r}, "
                    f"expected {SCHEMA!r}"
                )
            if not isinstance(record.get("event"), str):
                raise ValueError(f"{path}:{lineno}: missing string event")
            if records == 0:
                source = str(record.get("source", source))
            start_scanner.feed(records, record)
            end_scanner.feed(records, record)
            records += 1
    if records == 0:
        raise ValueError(f"{path}: empty trace")
    return source, start_scanner.matches, end_scanner.matches


def _candidate_summary(matches: Iterable[AnchorMatch]) -> str:
    values = []
    for match in list(matches)[:8]:
        values.append(
            f"seq={match.first_record.get('seq', '?')}/"
            f"frame={match.first_record.get('frame', '?')}"
        )
    return ", ".join(values)


def choose_match(
    matches: list[AnchorMatch],
    label: str,
    occurrence: int | None,
    after_index: int = -1,
) -> AnchorMatch:
    candidates = [match for match in matches if match.first_index > after_index]
    if not candidates:
        raise ValueError(f"{label} anchor not found after record {after_index}")
    if occurrence is None:
        if len(candidates) != 1:
            raise ValueError(
                f"{label} anchor is ambiguous ({len(candidates)} matches: "
                f"{_candidate_summary(candidates)}); add distinguishing fields "
                f"or --{label}-occurrence"
            )
        return candidates[0]
    if occurrence < 1:
        raise ValueError(f"{label} occurrence must be at least 1")
    if occurrence > len(candidates):
        raise ValueError(
            f"{label} occurrence {occurrence} exceeds {len(candidates)} matches"
        )
    return candidates[occurrence - 1]


def normalized_record(
    record: dict[str, Any], base_frame: int, base_host_us: int
) -> dict[str, Any]:
    result = dict(record)
    if "frame" in result:
        result["frame"] = int(result["frame"]) - base_frame
    if "host_us" in result:
        result["host_us"] = int(result["host_us"]) - base_host_us
    if result["event"] == "timing.vblank" and "index" in result:
        result["index"] = int(result["index"]) - base_frame
    return result


def write_segment(
    input_path: Path,
    output_path: Path,
    start: AnchorMatch,
    end: AnchorMatch,
) -> int:
    if input_path.resolve() == output_path.resolve():
        raise ValueError("input and output paths must differ")
    if end.last_index < start.first_index:
        raise ValueError("end anchor precedes start anchor")
    if not output_path.parent.is_dir():
        raise ValueError(f"output directory does not exist: {output_path.parent}")

    base_frame = int(start.first_record.get("frame", 0))
    base_host_us = int(start.first_record.get("host_us", 0))
    temporary_path: Path | None = None
    emitted = 0
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=output_path.parent,
            prefix=f".{output_path.name}.",
            delete=False,
        ) as output:
            temporary_path = Path(output.name)
            with input_path.open(encoding="utf-8") as stream:
                record_index = 0
                for line in stream:
                    if not line.strip():
                        continue
                    if record_index > end.last_index:
                        break
                    if record_index >= start.first_index:
                        record = json.loads(line)
                        json.dump(
                            normalized_record(record, base_frame, base_host_us),
                            output,
                            separators=(",", ":"),
                        )
                        output.write("\n")
                        emitted += 1
                    record_index += 1
        os.replace(temporary_path, output_path)
    except Exception:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)
        raise
    return emitted


def _step_json(step: AnchorStep) -> dict[str, Any]:
    return {"event": step.event, "fields": step.fields}


def build_metadata(
    input_path: Path,
    output_path: Path,
    source: str,
    start_steps: list[AnchorStep],
    end_steps: list[AnchorStep],
    start: AnchorMatch,
    end: AnchorMatch,
    records: int,
) -> dict[str, Any]:
    def match_json(match: AnchorMatch) -> dict[str, Any]:
        return {
            "first_record_index": match.first_index,
            "last_record_index": match.last_index,
            "source_seq": match.first_record.get("seq"),
            "source_frame": match.first_record.get("frame"),
            "source_host_us": match.first_record.get("host_us"),
        }

    return {
        "schema": "ps1trace-semantic-segment/v1",
        "source": source,
        "input": str(input_path),
        "output": str(output_path),
        "records": records,
        "start": {
            "pattern": [_step_json(step) for step in start_steps],
            "match": match_json(start),
        },
        "end": {
            "pattern": [_step_json(step) for step in end_steps],
            "match": match_json(end),
        },
    }


def positive_integer(value: str) -> int:
    parsed = int(value, 10)
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return parsed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="input ps1trace/v1 JSONL")
    parser.add_argument("--output", required=True, type=Path, help="output JSONL")
    parser.add_argument(
        "--metadata", type=Path,
        help="optional ps1trace-semantic-segment/v1 selection report",
    )
    parser.add_argument(
        "--start", action="append", required=True, metavar="EVENT[:FIELD=VALUE,...]",
        help="ordered start-anchor step; repeat to form a sequence",
    )
    parser.add_argument(
        "--end", action="append", required=True, metavar="EVENT[:FIELD=VALUE,...]",
        help="ordered end-anchor step; repeat to form a sequence",
    )
    parser.add_argument(
        "--start-occurrence", type=positive_integer,
        help="select this 1-based start match when fields are not unique",
    )
    parser.add_argument(
        "--end-occurrence", type=positive_integer,
        help="select this 1-based end match after the selected start",
    )
    args = parser.parse_args()

    try:
        start_steps = [parse_step(spec) for spec in args.start]
        end_steps = [parse_step(spec) for spec in args.end]
        source, start_matches, end_matches = scan_trace(
            args.trace, start_steps, end_steps
        )
        start = choose_match(
            start_matches, "start", args.start_occurrence
        )
        end = choose_match(
            end_matches, "end", args.end_occurrence, start.last_index
        )
        records = write_segment(args.trace, args.output, start, end)
        metadata = build_metadata(
            args.trace, args.output, source, start_steps, end_steps,
            start, end, records,
        )
        if args.metadata:
            args.metadata.write_text(
                json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
            )
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        parser.error(str(exc))

    print(
        f"{source}: {records} records, semantic frames "
        f"{start.first_record.get('frame', '?')}.."
        f"{end.last_record.get('frame', '?')} -> {args.output}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
