#!/usr/bin/env python3
"""Compare one guest-PC state sequence after a semantic anchor PC."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


FIELDS = (
    "ra", "sp", "a0", "a1", "a2", "a3", "s0", "s1", "s2", "s3",
    "v0", "v1", "hi", "lo",
)


def address(value: str) -> int:
    return int(value, 0)


def sequence(path: Path, anchor_pc: int, target_pc: int) -> tuple[str, list[dict]]:
    source = path.stem
    anchored = False
    records = []
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if not line.strip():
                continue
            record = json.loads(line)
            source = str(record.get("source", source))
            if record.get("event") != "mips.pc":
                continue
            pc = int(record.get("pc", -1))
            if not anchored and pc == anchor_pc:
                anchored = True
            if anchored and pc == target_pc:
                records.append(record)
    if not anchored:
        raise ValueError(f"{path}: anchor PC 0x{anchor_pc:08X} not found")
    return source, records


def values(record: dict) -> tuple:
    return tuple(record.get(field) for field in FIELDS)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("traces", nargs="+", type=Path)
    parser.add_argument("--anchor-pc", required=True, type=address)
    parser.add_argument("--target-pc", required=True, type=address)
    args = parser.parse_args()

    traces = [sequence(path, args.anchor_pc, args.target_pc) for path in args.traces]
    baseline_source, baseline = traces[0]
    print(f"baseline={baseline_source} records={len(baseline)}")
    for source, candidate in traces[1:]:
        prefix = 0
        for left, right in zip(baseline, candidate):
            if values(left) != values(right):
                break
            prefix += 1
        print(f"{source}: records={len(candidate)} matching_prefix={prefix}")
        if prefix < min(len(baseline), len(candidate)):
            left = baseline[prefix]
            right = candidate[prefix]
            print(
                f"  first_difference occurrence={prefix} "
                f"frames={left.get('frame')}/{right.get('frame')}"
            )
            for field in FIELDS:
                if left.get(field) != right.get(field):
                    print(
                        f"    {field}: 0x{int(left.get(field, 0)):08X} / "
                        f"0x{int(right.get(field, 0)):08X}"
                    )
        elif len(baseline) != len(candidate):
            print("  captured sequences have a common prefix but different lengths")
        else:
            print("  exact match")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
