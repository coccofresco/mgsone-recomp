#!/usr/bin/env python3
"""Extract donor-confirmed computed-transfer profiles for recompilation."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def number(value: str) -> int:
    return int(value, 0)


def region(value: str) -> tuple[int, int]:
    start, separator, end = value.replace("-", ":", 1).partition(":")
    if not separator:
        raise argparse.ArgumentTypeError("region must be START:END")
    result = (number(start), number(end))
    if result[1] <= result[0]:
        raise argparse.ArgumentTypeError("region end must be greater than start")
    return result


def read_events(path: Path) -> tuple[str, set[tuple[int, int]], set[int]]:
    source = path.stem
    edges: set[tuple[int, int]] = set()
    coverage: set[int] = set()
    with path.open(encoding="utf-8") as stream:
        for lineno, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                record: dict[str, Any] = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{lineno}: invalid JSON: {exc}") from exc
            source = str(record.get("source", source))
            if record.get("event") == "mips.edge":
                edges.add((int(record["source_pc"]), int(record["target_pc"])))
            elif record.get("event") == "mips.pc_first":
                coverage.add(int(record["pc"]))
    return source, edges, coverage


def instruction(executable: bytes, pc: int, load_address: int, file_offset: int) -> int | None:
    offset = file_offset + pc - load_address
    if offset < 0 or offset + 4 > len(executable):
        return None
    return int.from_bytes(executable[offset : offset + 4], "little")


def computed_transfer_kind(raw: int | None) -> str | None:
    """Classify a computed JR/JALR, excluding the ordinary ``jr $ra``."""
    if raw is None or (raw >> 26) != 0:
        return None
    function = raw & 0x3F
    source_register = (raw >> 21) & 0x1F
    destination_register = (raw >> 11) & 0x1F
    if function == 0x08:  # JR
        return "jump" if source_register != 31 else None
    if function == 0x09:  # JALR (rd=$zero is a tail jump)
        return "call" if destination_register != 0 else "jump"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("traces", nargs="+", type=Path)
    parser.add_argument("--exe", required=True, type=Path, help="PS-X EXE containing the profiled code")
    parser.add_argument("--load-address", type=number, default=0x80010000)
    parser.add_argument("--file-offset", type=number, default=0x800)
    parser.add_argument("--region", required=True, type=region, metavar="START:END")
    parser.add_argument("--toml", action="store_true", help="print a TOML indirect_targets array")
    args = parser.parse_args()

    executable = args.exe.read_bytes()
    start, end = args.region
    donors = []
    for path in args.traces:
        source, edges, coverage = read_events(path)
        transfers = {
            (source_pc, target, kind)
            for source_pc, target in edges
            if start <= source_pc < end
            and start <= target < end
            and (kind := computed_transfer_kind(
                instruction(executable, source_pc, args.load_address, args.file_offset)
            )) is not None
        }
        donors.append((source, transfers, {pc for pc in coverage if start <= pc < end}))

    confirmed_transfers = (
        set.intersection(*(transfers for _, transfers, _ in donors)) if donors else set()
    )
    confirmed = {target for _, target, _ in confirmed_transfers}
    confirmed_sources = {source_pc for source_pc, _, _ in confirmed_transfers}
    coverage_common = set.intersection(*(coverage for _, _, coverage in donors)) if donors else set()
    coverage_union = set.union(*(coverage for _, _, coverage in donors)) if donors else set()

    for source, transfers, coverage in donors:
        targets = {target for _, target, _ in transfers}
        confirmed_targets = {
            target for _, target, _ in confirmed_transfers
        }
        print(
            f"{source}: computed_targets={len(targets)} coverage={len(coverage)} "
            f"coverage_only_here={len(coverage - coverage_common)}"
        )
        if targets - confirmed_targets:
            values = ", ".join(
                f"0x{target:08X}" for target in sorted(targets - confirmed_targets)
            )
            print(f"  unconfirmed: {values}")
    print(
        f"confirmed={len(confirmed)} coverage_common={len(coverage_common)} "
        f"coverage_union={len(coverage_union)}"
    )

    if args.toml:
        print("indirect_targets = [")
        for target in sorted(confirmed):
            print(f'  "0x{target:08X}",')
        print("]")
        print("indirect_source_pcs = [")
        for source_pc in sorted(confirmed_sources):
            print(f'  "0x{source_pc:08X}",')
        print("]")
    else:
        for target in sorted(confirmed):
            print(f"0x{target:08X}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
