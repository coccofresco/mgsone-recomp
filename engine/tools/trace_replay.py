#!/usr/bin/env python3
"""Consumer for PS1_HLE_TRACE output.

The runtime emits one line per psyq_dispatch call when run with
`PS1_HLE_TRACE=1`:

    [PSYQ] <name> (a0=<HEX> a1=<HEX> a2=<HEX> RA=<HEX>)

This script reads that log and answers the questions you actually have:
  - Which function was the *last* one called (hang diagnosis)?
  - What was the top-N call sequence (boot order)?
  - Are there tight loops (same function N+ times consecutively)?
  - Call counts grouped by library?
  - Are there any [PSYQ] WARN/FATAL lines mixed in (missing HLEs)?

PPM/numpy/external deps avoided -- stdlib only.

Usage:
    trace_replay.py /tmp/crash_trace.log
    trace_replay.py /tmp/crash_trace.log --tail 30
    trace_replay.py /tmp/crash_trace.log --top 20
    trace_replay.py /tmp/crash_trace.log --loop-threshold 5
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path


CALL_RE = re.compile(
    r"^\[PSYQ\]\s+(?P<name>\S+)\s+\(a0=(?P<a0>[0-9A-Fa-f]+)\s+"
    r"a1=(?P<a1>[0-9A-Fa-f]+)\s+a2=(?P<a2>[0-9A-Fa-f]+)\s+"
    r"RA=(?P<ra>[0-9A-Fa-f]+)\)\s*$"
)

WARN_RE = re.compile(r"^\[PSYQ\]\s+(WARN|FATAL):\s*(?P<msg>.+)$")


class Call:
    __slots__ = ("name", "a0", "a1", "a2", "ra", "lineno")

    def __init__(self, name: str, a0: int, a1: int, a2: int, ra: int,
                 lineno: int) -> None:
        self.name = name
        self.a0 = a0
        self.a1 = a1
        self.a2 = a2
        self.ra = ra
        self.lineno = lineno

    def fmt(self) -> str:
        return (f"L{self.lineno:>6}  {self.name:<32}  "
                f"a0={self.a0:08X} a1={self.a1:08X} a2={self.a2:08X}  "
                f"RA={self.ra:08X}")


def parse(path: Path) -> tuple[list[Call], list[tuple[int, str]]]:
    calls: list[Call] = []
    warnings: list[tuple[int, str]] = []
    with path.open() as f:
        for lineno, line in enumerate(f, start=1):
            m = CALL_RE.match(line)
            if m:
                calls.append(Call(
                    name=m["name"],
                    a0=int(m["a0"], 16),
                    a1=int(m["a1"], 16),
                    a2=int(m["a2"], 16),
                    ra=int(m["ra"], 16),
                    lineno=lineno,
                ))
                continue
            mw = WARN_RE.match(line)
            if mw:
                warnings.append((lineno, mw["msg"].strip()))
    return calls, warnings


def library_of(name: str) -> str:
    """Heuristic: 'libcd_CdInit' -> 'libcd', else 'other'."""
    if "_" in name:
        return name.split("_", 1)[0]
    return "other"


def detect_loops(calls: list[Call], threshold: int) -> list[tuple[str, int, int]]:
    """Find runs of consecutive identical-name calls of length >= threshold.

    Returns list of (name, start_lineno, run_length).
    """
    runs: list[tuple[str, int, int]] = []
    if not calls:
        return runs
    cur_name = calls[0].name
    cur_start = calls[0].lineno
    cur_len = 1
    for c in calls[1:]:
        if c.name == cur_name:
            cur_len += 1
            continue
        if cur_len >= threshold:
            runs.append((cur_name, cur_start, cur_len))
        cur_name = c.name
        cur_start = c.lineno
        cur_len = 1
    if cur_len >= threshold:
        runs.append((cur_name, cur_start, cur_len))
    return runs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", type=Path,
                    help="trace log written by PS1_HLE_TRACE=1 (stderr)")
    ap.add_argument("--tail", type=int, default=10,
                    help="how many trailing calls to print (default 10)")
    ap.add_argument("--top", type=int, default=15,
                    help="how many top-by-count functions to print (default 15)")
    ap.add_argument("--loop-threshold", type=int, default=10,
                    help="flag runs of identical calls >= this length (default 10)")
    ap.add_argument("--filter", type=str, default=None,
                    help="only consider calls whose name contains this substring")
    args = ap.parse_args()

    if not args.log.exists():
        print(f"error: {args.log} does not exist", file=sys.stderr)
        return 2

    calls, warnings = parse(args.log)
    if args.filter:
        calls = [c for c in calls if args.filter in c.name]

    print(f"# trace_replay  {args.log}")
    print(f"  total dispatch calls : {len(calls)}")
    print(f"  warnings / fatals    : {len(warnings)}")
    if args.filter:
        print(f"  filter active        : '{args.filter}'")

    if not calls:
        print("  (no dispatch calls parsed -- wrong format or empty log?)")
        return 1

    # Top callers by count
    counter = Counter(c.name for c in calls)
    print()
    print(f"## Top {args.top} by call count")
    for name, n in counter.most_common(args.top):
        pct = 100 * n / len(calls)
        print(f"  {n:>7}  {pct:5.1f}%  {name}")

    # By library
    lib_counter: Counter[str] = Counter()
    for c in calls:
        lib_counter[library_of(c.name)] += 1
    print()
    print(f"## By library")
    for lib, n in lib_counter.most_common():
        pct = 100 * n / len(calls)
        print(f"  {n:>7}  {pct:5.1f}%  {lib}")

    # Tail
    print()
    print(f"## Last {args.tail} calls (where the hang likely is)")
    for c in calls[-args.tail:]:
        print(f"  {c.fmt()}")

    # Tight loops
    loops = detect_loops(calls, args.loop_threshold)
    if loops:
        print()
        print(f"## Tight loops (>= {args.loop_threshold} identical consecutive)")
        for name, start, length in loops:
            print(f"  {length:>6}x at line {start}  {name}")

    # Warnings
    if warnings:
        print()
        print(f"## Runtime warnings / fatals (top 10)")
        seen: dict[str, int] = defaultdict(int)
        for _, msg in warnings:
            seen[msg] += 1
        for msg, n in sorted(seen.items(), key=lambda kv: -kv[1])[:10]:
            print(f"  {n:>6}x  {msg}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
