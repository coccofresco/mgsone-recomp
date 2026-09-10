#!/usr/bin/env python3
"""Production-line tracker for ps1-recomp.

This CLI keeps an honest inventory of the workflow capabilities that move
the project from "boots Rayman" to "works on Crash + every PsyQ game with
reproducible per-game regression coverage". Inspired by TombaRecomp's
explicit ISSUES tracking and the 1379.tech article's tooling write-up.

Each item has:
  - a slug id
  - a level (1 = foundation, 2 = instrumentation, 3 = scale)
  - a shell check (exit 0 = item delivered)
  - a one-line description

Run `production_line.py status` to see the table.
Run `production_line.py next` to see the next pending item with detail.
Run `production_line.py check <id>` to re-check a single item.

The manifest is the source of truth -- edit ITEMS below to add/remove
capabilities. Implementation files referenced here should land in tools/
or the repo root (audit_notes/, ISSUES.md, progress/).
"""

from __future__ import annotations

import argparse
import dataclasses
import shutil
import subprocess
import sys
from typing import Callable


@dataclasses.dataclass
class Item:
    id: str
    level: int
    title: str
    desc: str
    check_cmd: str        # shell command -- exit 0 if delivered
    how_to: str = ""      # one-line hint for implementing

    def is_done(self) -> bool:
        return subprocess.run(self.check_cmd, shell=True,
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0


# Repository production line.  Order within a level is suggested execution
# order -- earlier items unblock later ones.
ITEMS: list[Item] = [
    # Level 1 -- Foundation: visual regression + per-game issue tracking
    Item("audit-notes-dir", 1,
         "audit_notes/ directory",
         "Per-game directory for reference screenshots and reverse-eng notes",
         "test -d audit_notes && test -f audit_notes/README.md",
         "mkdir audit_notes && add README listing layout (audit_notes/<game>/)"),

    Item("issues-md", 1,
         "ISSUES.md at repo root",
         "Game-specific blockers with Status + Root Cause + Fix + Metrics format (TombaRecomp style)",
         "test -f ISSUES.md && grep -q 'Root Cause' ISSUES.md",
         "Create ISSUES.md with template; first entry = current Crash hang"),

    Item("progress-dir", 1,
         "progress/ visual tracker",
         "Dated VRAM/screenshot snapshots over time (e.g. progress/2026-05-15_crash_silent.ppm)",
         "test -d progress && test -f progress/README.md",
         "mkdir progress && README explaining naming + retention"),

    Item("smoke-test-tool", 1,
         "tools/smoke_test.py -- VRAM diff regression",
         "Run game N sec, snapshot VRAM, compare against reference PPM, exit 0/1",
         "test -x tools/smoke_test.py && tools/smoke_test.py --help >/dev/null 2>&1",
         "PPM-native diff (no PIL); --config --duration --ref --tolerance args"),

    Item("crash-ref-screenshot", 1,
         "audit_notes/crash/title_ref.ppm",
         "Reference VRAM of Crash title screen captured from PCSX-Redux (oracle)",
         "test -f audit_notes/crash/title_ref.ppm",
         "Boot Crash in PCSX-Redux, save VRAM dump, convert to PPM 1024x512"),

    # Level 2 -- Instrumentation: trace + measure
    Item("hle-trace-doc", 2,
         "PS1_HLE_TRACE documented",
         "Existing PS1_HLE_TRACE=1 env var documented in CONTRIBUTING.md (recipe + sample output)",
         "grep -q 'PS1_HLE_TRACE' CONTRIBUTING.md",
         "Add 'Debugging HLE dispatch' section to CONTRIBUTING.md"),

    Item("trace-replay-tool", 2,
         "tools/trace_replay.py -- trace consumer",
         "Parse PS1_HLE_TRACE output, summarise: call counts, last N calls, hot loops",
         "test -x tools/trace_replay.py && tools/trace_replay.py --help >/dev/null 2>&1",
         "Read stderr trace; bucket by function name; flag tight loops (>N consecutive identical)"),

    Item("pace-measure-tool", 2,
         "tools/pace_measure.py -- fps/dma/vsync rate",
         "Measure VSync rate, DMA throughput, GPU command rate over a window (TombaRecomp's _mdec_pace.py analog)",
         "test -x tools/pace_measure.py",
         "Wrap run_game with --duration, compute rates, emit JSON"),

    Item("pcsx-redux-mcp-polish", 2,
         "tools/pcsx_redux_mcp/ usable",
         "Existing MCP server polished + documented: setup, allowed tools, sample queries",
         "test -f tools/pcsx_redux_mcp/README.md && grep -q 'allowed_tools' tools/pcsx_redux_mcp/README.md",
         "Audit existing server.py, document MCP wiring, list capabilities"),

    Item("ghidra-mcp-integration", 2,
         "tools/ghidra/ MCP server",
         "Ghidra MCP plugin wired so Claude can query disasm authoritatively (1379.tech model)",
         "test -f tools/ghidra/mcp_server.py || test -f tools/ghidra/MCP_SETUP.md",
         "Install GhidraMCP plugin in tools/ghidra/, document setup, add to .mcp.json"),

    # Level 3 -- Scale: framework / game split
    Item("framework-pin-file", 3,
         "framework.pin protocol",
         "Format file (similar to TombaRecomp's psxrecomp-v4.pin) so satellite repos can pin our SHA",
         "test -f docs/FRAMEWORK_PIN_SCHEMA.md",
         "Document: filename, fields (branch, sha), how satellite consumes it"),

    Item("crash-satellite-repo", 3,
         "CrashRecomp/ satellite repo skeleton",
         "Extract configs/crash.toml + seeds + crash-specific patches to a sibling repo that pins our SHA",
         "test -d ../CrashRecomp",
         "git init ../CrashRecomp, move crash assets, add framework.pin"),
]


# Output helpers


def status_glyph(done: bool) -> str:
    return "[done]   " if done else "[pending]"


def render_table(items: list[Item]) -> None:
    print(f"{'STATUS':<10}  {'LVL':<3}  {'ID':<28}  TITLE")
    print("-" * 90)
    for it in items:
        print(f"{status_glyph(it.is_done()):<10}  L{it.level}   {it.id:<28}  {it.title}")


def cmd_status(args: argparse.Namespace) -> int:
    items = [i for i in ITEMS if args.level is None or i.level == args.level]
    render_table(items)
    done_count = sum(1 for i in items if i.is_done())
    print()
    print(f"{done_count} / {len(items)} delivered "
          f"({100 * done_count / len(items):.0f}%)")
    return 0


def cmd_next(args: argparse.Namespace) -> int:
    for it in ITEMS:
        if it.is_done():
            continue
        print(f"== Next pending item ==")
        print(f"  id:          {it.id}")
        print(f"  level:       {it.level}")
        print(f"  title:       {it.title}")
        print(f"  description: {it.desc}")
        print(f"  how to:      {it.how_to or '(see description)'}")
        print(f"  check cmd:   {it.check_cmd}")
        return 0
    print("All items delivered -- extend ITEMS in tools/production_line.py")
    return 0


def cmd_check(args: argparse.Namespace) -> int:
    for it in ITEMS:
        if it.id != args.id:
            continue
        done = it.is_done()
        print(f"{status_glyph(done)} {it.id}: {it.title}")
        if not done:
            print(f"  check cmd: {it.check_cmd}")
            print(f"  how to:    {it.how_to or '(see description)'}")
        return 0 if done else 1
    print(f"error: unknown id '{args.id}'", file=sys.stderr)
    print(f"known ids: {', '.join(i.id for i in ITEMS)}", file=sys.stderr)
    return 2


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    ap_status = sub.add_parser("status", help="show production line table")
    ap_status.add_argument("--level", type=int, choices=[1, 2, 3],
                           help="filter by level")
    ap_status.set_defaults(func=cmd_status)

    ap_next = sub.add_parser("next", help="show next pending item")
    ap_next.set_defaults(func=cmd_next)

    ap_check = sub.add_parser("check", help="check a single item by id")
    ap_check.add_argument("id")
    ap_check.set_defaults(func=cmd_check)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
