#!/usr/bin/env python3
"""Compare Crash 3's demo transition at semantic CPU checkpoints.

Unlike a raw event diff, this aligns emulator/runtime traces by guest PC. Frame
numbers are deliberately not used for alignment: BIOS and frontend scheduling
can move the same transition to a different VBlank without changing CPU state.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


SCHEMA = "ps1trace/v1"
STATE_FIELDS = (
    "ra", "sp", "a0", "a1", "a2", "a3", "s0", "s1", "s2", "s3",
    "v0", "v1", "hi", "lo",
)
DEFAULT_CHECKPOINTS = (
    ("model_decode_entry", 0x80039FA0),
    ("shared_decode_helper", 0x8003A7C4),
    ("decode_resume", 0x8003A240),
    ("decode_restore", 0x8003A348),
    ("renderer_entry", 0x80041A30),
    ("renderer_dispatch", 0x80041B98),
    ("renderer_pc_plus_8", 0x80041BA0),
    ("first_demo_rtpt", 0x800417AC),
)
CD_FIELDS = {
    "cdrom.command": ("command", "parameters"),
    "cdrom.interrupt": ("type",),
}


@dataclass
class Trace:
    path: Path
    source: str = "unknown"
    checkpoints: dict[int, list[dict[str, Any]]] = field(default_factory=dict)
    cd: list[dict[str, Any]] = field(default_factory=list)
    gte: list[dict[str, Any]] = field(default_factory=list)
    gpu: list[dict[str, Any]] = field(default_factory=list)


def parse_checkpoint(spec: str) -> tuple[str, int]:
    name, separator, raw_address = spec.partition("=")
    if not separator or not name or not raw_address:
        raise ValueError(f"invalid checkpoint {spec!r}; expected name=0xADDRESS")
    return name, int(raw_address, 0)


def load_trace(path: Path, addresses: set[int]) -> Trace:
    result = Trace(path=path, checkpoints={address: [] for address in addresses})
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
                    f"{path}:{lineno}: expected schema {SCHEMA!r}, "
                    f"got {record.get('schema')!r}"
                )
            result.source = str(record.get("source", result.source))
            event = record.get("event")
            if event == "mips.pc":
                pc = int(record.get("pc", -1))
                if pc in result.checkpoints:
                    result.checkpoints[pc].append(record)
            elif event in CD_FIELDS:
                result.cd.append(record)
            elif event == "gte.command":
                result.gte.append(record)
            elif isinstance(event, str) and event.startswith("gpu."):
                result.gpu.append(record)
    return result


def state(record: dict[str, Any]) -> dict[str, int | None]:
    return {field: record.get(field) for field in STATE_FIELDS}


def state_differences(
    baseline: dict[str, Any], candidate: dict[str, Any]
) -> dict[str, list[int | None]]:
    return {
        field: [baseline.get(field), candidate.get(field)]
        for field in STATE_FIELDS
        if baseline.get(field) != candidate.get(field)
    }


def first_pc_order(trace: Trace) -> list[int]:
    first = []
    for pc, records in trace.checkpoints.items():
        if records:
            first.append((int(records[0].get("seq", 0)), pc))
    return [pc for _, pc in sorted(first)]


def cd_signature(trace: Trace, renderer_frame: int | None, radius: int = 8) -> list[list[Any]]:
    if renderer_frame is None:
        selected = trace.cd[-radius * 2 :]
    else:
        before = [r for r in trace.cd if int(r.get("frame", 0)) <= renderer_frame]
        after = [r for r in trace.cd if int(r.get("frame", 0)) > renderer_frame]
        selected = before[-radius:] + after[:radius]
    return [
        [
            record.get("frame"),
            record.get("event"),
            *[record.get(name) for name in CD_FIELDS[record["event"]]],
            record.get("lba"),
        ]
        for record in selected
    ]


def build_report(
    traces: list[Trace], checkpoints: list[tuple[str, int]]
) -> dict[str, Any]:
    baseline = traces[0]
    rows = []
    first_divergence: dict[str, Any] = {}
    for name, address in checkpoints:
        base_records = baseline.checkpoints[address]
        row: dict[str, Any] = {
            "name": name,
            "pc": address,
            "traces": {},
        }
        for trace in traces:
            records = trace.checkpoints[address]
            entry: dict[str, Any] = {
                "hits": len(records),
                "first_frame": records[0].get("frame") if records else None,
                "first_state": state(records[0]) if records else None,
            }
            if trace is baseline:
                entry["vs_baseline"] = "baseline" if records else "missing"
            elif not base_records or not records:
                entry["vs_baseline"] = "missing"
            else:
                differences = state_differences(base_records[0], records[0])
                entry["vs_baseline"] = "match" if not differences else "different"
                entry["differences"] = differences
                if differences and trace.source not in first_divergence:
                    first_divergence[trace.source] = {
                        "name": name,
                        "pc": address,
                        "reason": "register_state",
                        "differences": differences,
                    }
            if trace is not baseline and entry["vs_baseline"] == "missing" and trace.source not in first_divergence:
                first_divergence[trace.source] = {
                    "name": name,
                    "pc": address,
                    "reason": "missing_checkpoint",
                }
            row["traces"][trace.source] = entry
        rows.append(row)

    per_trace = {}
    renderer_pc = dict(checkpoints).get("renderer_entry")
    for trace in traces:
        renderer_records = trace.checkpoints.get(renderer_pc, []) if renderer_pc else []
        renderer_frame = int(renderer_records[0]["frame"]) if renderer_records else None
        per_trace[trace.source] = {
            "path": str(trace.path),
            "first_checkpoint_order": first_pc_order(trace),
            "renderer_frame": renderer_frame,
            "cd_near_renderer": cd_signature(trace, renderer_frame),
            "gte_records": len(trace.gte),
            "gpu_records": len(trace.gpu),
        }
    return {
        "schema": "ps1-demo-transition-report/v1",
        "baseline": baseline.source,
        "checkpoints": rows,
        "first_divergence": first_divergence,
        "traces": per_trace,
    }


def hex_value(value: int | None) -> str:
    return "-" if value is None else f"0x{value:08X}"


def print_text(report: dict[str, Any]) -> None:
    sources = list(report["traces"])
    print(f"baseline: {report['baseline']}")
    print("checkpoint                 pc          " + "  ".join(f"{s:>18}" for s in sources))
    for row in report["checkpoints"]:
        cells = []
        for source in sources:
            entry = row["traces"][source]
            verdict = entry["vs_baseline"]
            cells.append(f"{verdict}:{entry['hits']}@{entry['first_frame']}")
        print(f"{row['name']:<26} {hex_value(row['pc'])}  " + "  ".join(f"{c:>18}" for c in cells))

    print("\nfirst divergence from baseline:")
    for source in sources[1:]:
        divergence = report["first_divergence"].get(source)
        if divergence is None:
            print(f"  {source}: none in captured checkpoints")
            continue
        details = ""
        if divergence.get("differences"):
            details = " (" + ", ".join(divergence["differences"]) + ")"
        print(
            f"  {source}: {divergence['name']} {hex_value(divergence['pc'])}, "
            f"{divergence['reason']}{details}"
        )

    print("\nobserved checkpoint order:")
    for source in sources:
        order = " -> ".join(hex_value(pc) for pc in report["traces"][source]["first_checkpoint_order"])
        print(f"  {source}: {order or '-'}")

    print("\nCD commands/interrupts around first renderer entry:")
    for source in sources:
        signature = report["traces"][source]["cd_near_renderer"]
        compact = " ".join(
            f"f{item[0]}:{item[1].removeprefix('cdrom.')}({','.join(str(v) for v in item[2:] if v is not None)})"
            for item in signature
        )
        print(f"  {source}: {compact or '-'}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("traces", nargs="+", type=Path, help="JSONL traces; first is the baseline")
    parser.add_argument(
        "--checkpoint", action="append", default=[], metavar="NAME=ADDRESS",
        help="replace the built-in Crash 3 checkpoint list",
    )
    parser.add_argument("--json", action="store_true", help="emit the machine-readable report")
    args = parser.parse_args()

    try:
        checkpoints = (
            [parse_checkpoint(spec) for spec in args.checkpoint]
            if args.checkpoint else list(DEFAULT_CHECKPOINTS)
        )
        addresses = {address for _, address in checkpoints}
        traces = [load_trace(path, addresses) for path in args.traces]
        report = build_report(traces, checkpoints)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))

    if args.json:
        json.dump(report, sys.stdout, indent=2)
        print()
    else:
        print_text(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
