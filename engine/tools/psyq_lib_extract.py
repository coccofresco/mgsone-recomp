#!/usr/bin/env python3
"""Extract members from a Sony PsyQ LIB archive into individual .OBJ files.

LIB format (reverse-engineered):
  4 bytes  "LIB\\x01" magic
  For each member:
    8 bytes  compiler/section tag, e.g. "EXT     " (space-padded)
    4 bytes  timestamp (LE)
    4 bytes  member ID (LE)
    4 bytes  member size in bytes from start of this member's tag to
             start of next member's tag (LE)
    public symbol table:
      repeated:
        1 byte  N = name length
        if N == 0: end of table (the byte itself is consumed as terminator)
        N bytes ASCII symbol name (no null after)
    rest     LNK\\x02 object content, padded to fit the member size
"""
import re
import struct
import sys
from pathlib import Path


_FILENAME_SAFE = re.compile(r"[^A-Za-z0-9._-]")


def _sanitize(name: str) -> str:
    """Make `name` safe for use as a filename component.

    Some PsyQ libs (e.g. v4.0 LIBETC) ship symbol names containing
    non-printable bytes (NUL included). Treat them as opaque IDs.
    """
    cleaned = _FILENAME_SAFE.sub("_", name)
    return cleaned or "_unnamed_"


def extract(lib_path: Path, out_dir: Path) -> list[tuple[str, int, str, list[str]]]:
    data = lib_path.read_bytes()
    if data[:4] != b"LIB\x01":
        raise SystemExit(f"not a PsyQ LIB: {lib_path}")
    out_dir.mkdir(parents=True, exist_ok=True)
    members: list[tuple[str, int, str, list[str]]] = []
    pos = 4
    while pos < len(data):
        if pos + 20 > len(data):
            break
        tag = data[pos : pos + 8].decode("ascii", errors="replace").rstrip()
        (_ts, _idx, size) = struct.unpack_from("<III", data, pos + 8)
        if size == 0:
            break
        member_end = pos + size
        # Read symbol table: length-prefixed names, terminator = 0-length name.
        ptr = pos + 20
        names: list[str] = []
        while ptr < member_end:
            n = data[ptr]
            ptr += 1
            if n == 0:
                break
            names.append(data[ptr : ptr + n].decode("ascii", errors="replace"))
            ptr += n
        obj_bytes = data[ptr:member_end]
        if obj_bytes[:4] != b"LNK\x02":
            print(
                f"warn: member tag={tag!r} names={names!r} has no LNK\\x02 magic",
                file=sys.stderr,
            )
        # Use first symbol name as filename; fall back to tag if empty.
        primary = names[0] if names else tag.replace(" ", "_") or f"obj_{pos:08x}"
        safe = _sanitize(primary)
        # Disambiguate if sanitization causes a collision with an earlier member.
        candidate = safe
        suffix = 2
        while (out_dir / f"{candidate}.OBJ").exists():
            candidate = f"{safe}_{suffix}"
            suffix += 1
        out_path = out_dir / f"{candidate}.OBJ"
        out_path.write_bytes(obj_bytes)
        # Preserve the original (possibly junky) name in the returned tuple
        # so callers see the actual symbol; only the on-disk file is renamed.
        members.append((candidate, len(obj_bytes), tag, names))
        pos = member_end
    return members


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: psyq_lib_extract.py <input.LIB> <output_dir>", file=sys.stderr)
        return 2
    members = extract(Path(sys.argv[1]), Path(sys.argv[2]))
    print(f"extracted {len(members)} members:")
    for primary, size, tag, names in members:
        extras = f"  +{len(names)-1} more" if len(names) > 1 else ""
        print(f"  {primary:<24} {size:6} bytes  ({tag}){extras}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
