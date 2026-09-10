#!/usr/bin/env python3
"""Cross-reference psyz PSY-Q symbol map against our psyq_signatures.toml.

psyz (https://github.com/Xeeynamo/psyz) ships symbol maps captured from
matching decompilation of PSY-Q 4.0 and 4.7:

    decomp/symbols.400.txt        # text-section symbols (functions + consts)
    decomp/symbols.400.bss.txt    # bss-section symbols (state addresses)
    decomp/symbols.470.txt
    decomp/symbols.470.bss.txt

Each line follows the pattern `name = 0xADDR;` (optionally with a `// ...`
comment).

Our DB (ps1Analyzer/data/psyq_signatures.toml) holds hashes for ~3463
PSY-Q functions sourced from 14 SDK versions. We deduplicate by hash but
do not currently carry a "canonical address in PSY-Q 4.0" -- that is what
psyz can give us.

This script reports:

  1. Coverage  - how many psyz names also appear as a [[signature]] name
                 in our DB, broken down by library.
  2. Gaps      - psyz names that are NOT in our DB (candidates we may have
                 missed when hashing the .OBJ files).
  3. Over      - signature names in our DB that are NOT in psyz (functions
                 unique to other PSY-Q versions, or local helpers psyz
                 didn't surface). This is mostly a sanity check -- proves
                 the multi-version DB carries real width.

Usage:
    cross_reference_psyz_symbols.py \\
        --psyz /path/to/PS1Recomp-workspace/psyz \\
        [--db ps1Analyzer/data/psyq_signatures.toml] \\
        [--version 400|470|both]
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path

try:
    import tomllib  # py311+
except ModuleNotFoundError:
    import tomli as tomllib  # type: ignore[no-redef]


SYMBOL_RE = re.compile(r"^\s*([A-Za-z_][\w]*)\s*=\s*(0x[0-9a-fA-F]+)\s*;")


def parse_symbol_file(path: Path) -> dict[str, int]:
    out: dict[str, int] = {}
    with path.open() as f:
        for line in f:
            m = SYMBOL_RE.match(line)
            if not m:
                continue
            name, addr = m.group(1), int(m.group(2), 16)
            # later definitions win -- symbol files rarely repeat, but be safe
            out[name] = addr
    return out


# psyz symbols.{ver}.txt mixes function and data symbols. From inspection of
# the linker script (decomp/psyq.ld) and the symbol distribution, the text
# segment ends well before the data/bss segment. The cleanest boundary that
# works for both 4.0 and 4.7 is 0x800B0000 -- below it is .text/.rodata,
# at or above it is .data/.sbss/.bss. Filtering by this lets us compare
# apples-to-apples against our DB (which hashes STT_FUNC only).
TEXT_DATA_BOUNDARY = 0x800B0000


def split_text_data(syms: dict[str, int]) -> tuple[dict[str, int], dict[str, int]]:
    text = {n: a for n, a in syms.items() if a < TEXT_DATA_BOUNDARY}
    data = {n: a for n, a in syms.items() if a >= TEXT_DATA_BOUNDARY}
    return text, data


def load_db(path: Path) -> dict[str, dict]:
    with path.open("rb") as f:
        data = tomllib.load(f)
    sigs = data.get("signature", [])
    return {s["name"]: s for s in sigs if "name" in s}


def report_section(title: str, items: list[str]) -> None:
    print(f"\n## {title} ({len(items)})")
    if not items:
        print("  (none)")
        return
    for s in items[:30]:
        print(f"  {s}")
    if len(items) > 30:
        print(f"  ... +{len(items) - 30} more")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--psyz", type=Path, required=True,
                    help="path to the psyz repo root (contains decomp/)")
    ap.add_argument("--db", type=Path,
                    default=Path("ps1Analyzer/data/psyq_signatures.toml"),
                    help="path to our psyq_signatures.toml")
    ap.add_argument("--version", choices=["400", "470", "both"], default="both",
                    help="which PSY-Q symbol set to compare (default: both)")
    args = ap.parse_args()

    decomp = args.psyz / "decomp"
    if not decomp.is_dir():
        print(f"error: {decomp} not found", file=sys.stderr)
        return 2

    versions = ["400", "470"] if args.version == "both" else [args.version]
    text_syms: dict[str, dict[str, int]] = {}
    data_syms: dict[str, dict[str, int]] = {}
    bss_syms: dict[str, dict[str, int]] = {}
    for v in versions:
        raw = parse_symbol_file(decomp / f"symbols.{v}.txt")
        text_syms[v], data_syms[v] = split_text_data(raw)
        bss_syms[v] = parse_symbol_file(decomp / f"symbols.{v}.bss.txt")

    db = load_db(args.db)

    print(f"# psyz x ps1-recomp cross-reference")
    print()
    print(f"DB: {args.db} ({len(db)} signature names)")
    for v in versions:
        print(f"psyz PSY-Q {v[:1]}.{v[1:]}: "
              f"{len(text_syms[v])} functions (<0x{TEXT_DATA_BOUNDARY:08x}), "
              f"{len(data_syms[v])} data/rodata symbols, "
              f"{len(bss_syms[v])} explicit bss symbols")

    # Coverage: a psyz function symbol name that matches one of our signatures.
    for v in versions:
        names_psyz = set(text_syms[v].keys())
        names_db = set(db.keys())
        matched = names_psyz & names_db
        only_psyz = sorted(names_psyz - names_db)
        only_db = sorted(names_db - names_psyz)

        print()
        print(f"================ PSY-Q {v[:1]}.{v[1:]} ================")
        print(f"matched: {len(matched)} / {len(names_psyz)} psyz functions "
              f"({100*len(matched)/max(1,len(names_psyz)):.1f}%)")
        print(f"matched: {len(matched)} / {len(names_db)} db "
              f"({100*len(matched)/max(1,len(names_db)):.1f}%)")

        # Breakdown by library (from our DB), counting only matched names.
        by_lib_matched: dict[str, int] = defaultdict(int)
        by_lib_db: dict[str, int] = defaultdict(int)
        for n, sig in db.items():
            by_lib_db[sig.get("library", "?")] += 1
            if n in matched:
                by_lib_matched[sig.get("library", "?")] += 1
        print()
        print("matched by library:")
        for lib in sorted(by_lib_db):
            m, t = by_lib_matched[lib], by_lib_db[lib]
            pct = 100 * m / max(1, t)
            print(f"  {lib:12s} {m:4d} / {t:4d}  ({pct:5.1f}%)")

        report_section(f"PSY-Q {v} functions MISSING from our DB", only_psyz)
        report_section(f"DB names NOT in PSY-Q {v} symbol map (other versions)",
                       only_db)

        # Library-bucketing the gap: psyz doesn't tell us which library each
        # symbol comes from, but the standard PSY-Q name prefixes are stable.
        prefix_to_lib = [
            ("CD_", "libcd"), ("Cd", "libcd"), ("ds_", "libds"), ("DS_", "libds"),
            ("DA_", "libcd"), ("ST", "libcd"),
            ("GPU_", "libgpu"), ("Draw", "libgpu"), ("Put", "libgpu"),
            ("Get", "libgpu"), ("Tim", "libgpu"), ("PSD", "libgpu"),
            ("set", "libgte"), ("apply", "libgte"), ("rot", "libgte"),
            ("PAD", "libapi"), ("pad", "libapi"),
            ("SIO", "libsio"), ("sio", "libsio"),
            ("COMB", "libcomb"), ("comb", "libcomb"),
            ("CQ_", "libapi"),  # command queue helpers -- used by libapi/libcd
            ("HMD", "libhmd"), ("PMD", "libgpu"),
        ]

        def guess_lib(name: str) -> str:
            for pref, lib in prefix_to_lib:
                if name.startswith(pref):
                    return lib
            return "?"

        gap_by_lib: dict[str, list[str]] = defaultdict(list)
        for n in only_psyz:
            gap_by_lib[guess_lib(n)].append(n)
        print(f"\n## Gap functions bucketed by inferred library")
        for lib in sorted(gap_by_lib, key=lambda k: -len(gap_by_lib[k])):
            print(f"  {lib:10s} {len(gap_by_lib[lib]):4d}")
        # Print the libcd gap in full -- it is the Crash-critical bucket.
        cd_gap = sorted(gap_by_lib["libcd"])
        if cd_gap:
            print(f"\n## Full libcd gap (Crash-critical)")
            for n in cd_gap:
                print(f"  {n:30s} 0x{text_syms[v][n]:08x}")

        # Quick win: for matched names, dump (name, psyz addr, our hash_full)
        # so callers can spot inconsistencies. Limited to first 20.
        print(f"\n## Sample matched bindings (name -> psyz addr, db hash_full)")
        for n in sorted(matched)[:20]:
            addr = text_syms[v][n]
            sig = db[n]
            print(f"  {n:30s} 0x{addr:08x}  {sig.get('hash_full', '?')[:16]}"
                  f"  ({sig.get('library', '?')})")

    return 0


if __name__ == "__main__":
    sys.exit(main())
