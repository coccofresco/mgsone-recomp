#!/usr/bin/env python3
"""Extract per-function MIPS opcode signatures from a PsyQ .LIB.

Pipeline (per .LIB):
    1. psyq_lib_extract.py  -> split into per-member .OBJ in a temp dir
    2. psyq-obj-parser      -> convert each .OBJ to a relocatable MIPS ELF
    3. read .symtab + .text -> for each STT_FUNC symbol, hash the function bytes

For each function we emit two hashes:
    hash_masked  - opcodes with immediate fields zeroed (relocation-tolerant):
                     * I-type / loads / stores / ALU-imm / LUI -> bits  0..15
                     * J / JAL                                 -> bits  0..25
                     * branches (BEQ/BNE/REGIMM/etc.)          -> bits  0..15
    hash_full    - hash of the raw bytes (sanity check / exact-version key)

Output is a TOML document compatible with the future Analyzer DB:

    [[signature]]
    name        = "ResetGraph"
    library     = "libgpu"
    size        = 64
    hash_masked = "0123456789abcdef"
    hash_full   = "fedcba9876543210"
    match_mode  = "masked"          # "full" if size <= 24 (BIOS wrappers)
    subsystem   = "graphics"        # joined from psyq_metadata.toml; "" if unknown
    stub_type   = "stub"            # joined from psyq_metadata.toml; "" if unknown
    sources     = ["v3.5/LIBGPU/SYS"]

Usage:
    extract_psyq_signatures.py LIB [LIB ...] -o sigs.toml \
        [--label LABEL] [--metadata ps1Analyzer/data/psyq_metadata.toml]

The optional --label tags every signature emitted from a given run (e.g.
"v3.5/LIBGPU"). Multiple invocations can be merged later by the analyzer.

`library` is derived per-.LIB from the stem (LIBGPU.LIB -> "libgpu"); it is
NOT version-prefixed since the same function from libgpu across versions is
the same library. `subsystem`/`stub_type` are joined from the metadata file;
unknown names get empty strings (the analyzer falls back to prefix-based
classification).
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import subprocess
import sys
import tempfile
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

from elftools.elf.elffile import ELFFile

try:
    from psyq_lib_extract import extract as lib_extract
except ImportError:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from psyq_lib_extract import extract as lib_extract


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_PARSER = SCRIPT_DIR / "psyq-obj-parser-bin" / "psyq-obj-parser"
DEFAULT_METADATA = SCRIPT_DIR.parent / "ps1Analyzer" / "data" / "psyq_metadata.toml"

# Threshold below which masked hashes are too lossy (BIOS A0/B0/C0 wrappers
# differ only in their function index and collide after immediate-masking).
# See docs/sessao-0.2 -- discovered while validating LIBAPI.
FULL_MATCH_SIZE_LIMIT = 24


# ---------------------------------------------------------------------------
# MIPS opcode masking
# ---------------------------------------------------------------------------

_BRANCH_OPS = {0x01, 0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17}
_IMM_ALU_OPS = {0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F}


def mask_immediates(word: int) -> int:
    """Zero immediate fields in a 32-bit MIPS instruction.

    Conservative: we keep COP0/1/2/3 instructions exact since GTE encoding
    uses funct/rs/rt/rd which are critical and have no relocatable imm.
    """
    op = (word >> 26) & 0x3F
    if op == 0x00:               # SPECIAL (R-type): no imm
        return word
    if op == 0x02 or op == 0x03: # J, JAL: 26-bit target
        return word & 0xFC000000
    if op in _BRANCH_OPS:        # branch offsets in low 16 bits
        return word & 0xFFFF0000
    if op in _IMM_ALU_OPS:       # ADDI/ADDIU/SLTI/SLTIU/ANDI/ORI/XORI/LUI
        return word & 0xFFFF0000
    if op >= 0x20:               # loads / stores (LB, LW, SW, LWC2, SWC2, ...)
        return word & 0xFFFF0000
    # COP0/1/2/3 (op 0x10..0x13) and anything unrecognised: keep exact.
    return word


# ---------------------------------------------------------------------------
# ELF inspection
# ---------------------------------------------------------------------------

@dataclass
class FuncSig:
    name: str
    size: int
    hash_masked: str
    hash_full: str
    library: str = ""
    match_mode: str = ""
    subsystem: str = ""
    stub_type: str = ""
    sources: list[str] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Schema helpers
# ---------------------------------------------------------------------------

def derive_library(lib_path: Path) -> str:
    """`LIBGPU.LIB` / `libgpu.lib` / `LIBGPU.lib` -> `libgpu`."""
    return lib_path.stem.lower()


def derive_default_label(lib_path: Path) -> str:
    """Auto-label: `<version>/<LIBSTEM>` based on path layout.

    Expects `.../psyq_sdks/CONSOLIDATED/<version>/lib/LIBxxx.LIB`. Falls
    back to just the stem if the layout is non-standard. The version is
    important so the merged sources field shows which SDK release each
    masked-hash came from (otherwise N versions of LIBGPU all collapse
    to a single "LIBGPU/SYS" source).
    """
    stem_upper = lib_path.stem.upper()
    parent = lib_path.parent
    if parent.name.lower() == "lib" and parent.parent.name:
        return f"{parent.parent.name}/{stem_upper}"
    return stem_upper


def classify_match_mode(size: int) -> str:
    """`full` for tiny (<=24B) BIOS wrappers, otherwise `masked`.

    Wrappers like `addiu t1,zero,IDX; lui at,0; addiu at,at,B0; jr at` differ
    only in IDX, so masked hashes collide. `hash_full` keeps them distinct.
    """
    return "full" if size <= FULL_MATCH_SIZE_LIMIT else "masked"


def load_metadata(path: Path) -> dict[str, tuple[str, str]]:
    """Read psyq_metadata.toml -> {name: (subsystem, stub_type)}.

    Missing file is non-fatal (returns empty dict): the generator still works,
    every signature just gets blank subsystem/stub_type.
    """
    if not path.exists():
        return {}
    with open(path, "rb") as f:
        doc = tomllib.load(f)
    out: dict[str, tuple[str, str]] = {}
    for entry in doc.get("function", []):
        name = entry.get("name")
        if not name:
            continue
        out[name] = (entry.get("subsystem", ""), entry.get("stub_type", ""))
    return out


def metadata_for(name: str, metadata: dict[str, tuple[str, str]]) -> tuple[str, str]:
    """Lookup honoring `Name@v2` variant suffixes (variants share the base entry)."""
    if name in metadata:
        return metadata[name]
    base = name.split("@", 1)[0] if "@" in name else name
    return metadata.get(base, ("", ""))


def _section_funcs(elf: ELFFile) -> list[FuncSig]:
    """Walk every text-bearing section, hash each function within it."""
    symtab = elf.get_section_by_name(".symtab")
    if symtab is None:
        return []

    # Group all STT_FUNC symbols by their section index so we can compute
    # implicit sizes from the gap to the next symbol in the same section.
    by_section: dict[int, list] = {}
    for sym in symtab.iter_symbols():
        info = sym["st_info"]
        if info["type"] != "STT_FUNC":
            continue
        if not sym.name:
            continue
        shndx = sym["st_shndx"]
        if isinstance(shndx, str):  # SHN_UNDEF / SHN_ABS / etc.
            continue
        by_section.setdefault(shndx, []).append(sym)

    out: list[FuncSig] = []
    for shndx, syms in by_section.items():
        section = elf.get_section(shndx)
        if section is None:
            continue
        # Only hash code sections (PROGBITS + executable).
        sh_flags = section["sh_flags"]
        if section["sh_type"] != "SHT_PROGBITS" or not (sh_flags & 0x4):
            continue
        data = section.data()
        if not data:
            continue
        sec_size = section["sh_size"]

        syms.sort(key=lambda s: (s["st_value"], s.name))
        for i, s in enumerate(syms):
            start = s["st_value"]
            declared = s["st_size"]
            if declared > 0:
                end = start + declared
            elif i + 1 < len(syms):
                # Next symbol with strictly greater address.
                nxt = next((t for t in syms[i + 1 :] if t["st_value"] > start), None)
                end = nxt["st_value"] if nxt else sec_size
            else:
                end = sec_size

            if end <= start or start >= sec_size:
                continue
            end = min(end, sec_size)
            body = data[start:end]
            if len(body) < 4 or len(body) % 4 != 0:
                # PsyQ functions are word-aligned; ignore stray padding.
                continue

            masked_words = bytearray()
            for off in range(0, len(body), 4):
                word = struct.unpack_from("<I", body, off)[0]
                masked_words += struct.pack("<I", mask_immediates(word))

            out.append(
                FuncSig(
                    name=s.name,
                    size=len(body),
                    hash_masked=hashlib.sha256(masked_words).hexdigest()[:16],
                    hash_full=hashlib.sha256(body).hexdigest()[:16],
                )
            )
    return out


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def _run_parser(parser: Path, obj: Path, elf_out: Path) -> bool:
    proc = subprocess.run(
        [str(parser), str(obj), "-o", str(elf_out), "-n"],
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0 or not elf_out.exists():
        sys.stderr.write(
            f"warn: psyq-obj-parser failed for {obj.name}: rc={proc.returncode}\n"
        )
        if proc.stderr:
            sys.stderr.write(proc.stderr[:400])
        return False
    return True


def process_lib(
    lib_path: Path,
    parser: Path,
    label: str,
) -> dict[str, FuncSig]:
    """Returns a name->FuncSig map for every function found in the .LIB.

    Sets `library` and `match_mode` on each signature; subsystem/stub_type
    are filled in later by `apply_metadata` (after merge across LIBs, since
    variant suffixes are only assigned during merge).
    """
    library = derive_library(lib_path)
    sigs: dict[str, FuncSig] = {}
    with tempfile.TemporaryDirectory(prefix="psyq_sig_") as td:
        tmp = Path(td)
        objs_dir = tmp / "objs"
        members = lib_extract(lib_path, objs_dir)
        for primary, _size, tag, _names in members:
            obj_path = objs_dir / f"{primary}.OBJ"
            if not obj_path.exists():
                continue
            elf_path = tmp / f"{primary}.elf"
            if not _run_parser(parser, obj_path, elf_path):
                continue
            with open(elf_path, "rb") as f:
                elf = ELFFile(f)
                for sig in _section_funcs(elf):
                    sig.library = library
                    sig.match_mode = classify_match_mode(sig.size)
                    src = f"{label}/{tag}" if label else tag
                    if sig.name in sigs:
                        # Same name within one .LIB: skip duplicates silently
                        # (the lib resolves to one definition at link time).
                        if src not in sigs[sig.name].sources:
                            sigs[sig.name].sources.append(src)
                        continue
                    sig.sources.append(src)
                    sigs[sig.name] = sig
    return sigs


def apply_metadata(
    sigs: dict[str, FuncSig],
    metadata: dict[str, tuple[str, str]],
) -> None:
    """Fill in subsystem/stub_type on every sig (in place). Variant-aware."""
    for sig in sigs.values():
        sub, stub = metadata_for(sig.name, metadata)
        sig.subsystem = sub
        sig.stub_type = stub


def merge(into: dict[str, FuncSig], extra: dict[str, FuncSig]) -> None:
    """Merge `extra` into `into`. Distinct hash variants are renamed.

    When the incoming sig's hash matches the BASE name OR any existing
    `@vN` variant of that name, sources are coalesced. Only a genuinely
    new hash creates a new variant slot. Without the variant scan, three
    libs producing hashes A,B,A would get base=A + @v2=B + @v3=A (a
    bogus duplicate of base).
    """
    for name, sig in extra.items():
        if name not in into:
            into[name] = sig
            continue
        # Collect every existing entry for this base name (base + @vN).
        candidates = [name]
        idx = 2
        while f"{name}@v{idx}" in into:
            candidates.append(f"{name}@v{idx}")
            idx += 1
        # Coalesce if any candidate already has this masked hash.
        coalesced = False
        for cand in candidates:
            if into[cand].hash_masked == sig.hash_masked:
                cur = into[cand]
                for src in sig.sources:
                    if src not in cur.sources:
                        cur.sources.append(src)
                coalesced = True
                break
        if coalesced:
            continue
        # Genuinely new variant -> next free @vN slot.
        sig.name = f"{name}@v{idx}"
        into[sig.name] = sig


def write_toml(path: Path, sigs: dict[str, FuncSig]) -> None:
    lines: list[str] = []
    lines.append("# generated by tools/extract_psyq_signatures.py")
    lines.append(f"# {len(sigs)} functions\n")
    for name in sorted(sigs):
        s = sigs[name]
        lines.append("[[signature]]")
        lines.append(f'name        = "{name}"')
        lines.append(f'library     = "{s.library}"')
        lines.append(f"size        = {s.size}")
        lines.append(f'hash_masked = "{s.hash_masked}"')
        lines.append(f'hash_full   = "{s.hash_full}"')
        lines.append(f'match_mode  = "{s.match_mode}"')
        lines.append(f'subsystem   = "{s.subsystem}"')
        lines.append(f'stub_type   = "{s.stub_type}"')
        srcs = ", ".join(f'"{src}"' for src in s.sources)
        lines.append(f"sources     = [{srcs}]")
        lines.append("")
    path.write_text("\n".join(lines))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("libs", nargs="+", type=Path, help=".LIB archive(s)")
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument(
        "--label",
        default="",
        help="prefix for the 'sources' field (e.g. v3.5/LIBGPU); applied to every LIB",
    )
    ap.add_argument(
        "--parser",
        type=Path,
        default=DEFAULT_PARSER,
        help=f"path to psyq-obj-parser (default: {DEFAULT_PARSER})",
    )
    ap.add_argument(
        "--metadata",
        type=Path,
        default=DEFAULT_METADATA,
        help=f"path to psyq_metadata.toml (default: {DEFAULT_METADATA}); "
             "missing -> empty subsystem/stub_type fields",
    )
    args = ap.parse_args()

    if not args.parser.exists():
        ap.error(f"parser not found: {args.parser}")

    metadata = load_metadata(args.metadata)
    if not metadata:
        sys.stderr.write(
            f"note: no metadata loaded from {args.metadata} "
            "-- subsystem/stub_type will be blank\n"
        )

    merged: dict[str, FuncSig] = {}
    for lib in args.libs:
        if not lib.exists():
            sys.stderr.write(f"warn: skipping missing {lib}\n")
            continue
        label = args.label or derive_default_label(lib)
        sys.stderr.write(f"::  {lib}  (label={label})\n")
        sigs = process_lib(lib, args.parser, label)
        sys.stderr.write(f"    {len(sigs)} functions\n")
        merge(merged, sigs)

    apply_metadata(merged, metadata)

    write_toml(args.output, merged)
    sys.stderr.write(f"wrote {args.output}  ({len(merged)} unique signatures)\n")

    # Echo the first 5 alphabetically -- useful as a quick smoke check.
    print("first 5 signatures (alphabetical):")
    for name in sorted(merged)[:5]:
        s = merged[name]
        print(
            f"  {name:<22} lib={s.library:<7} size={s.size:4}  mode={s.match_mode:<6}"
            f"  sub={s.subsystem or '-':<10} stub={s.stub_type or '-'}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
