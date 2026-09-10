"""Unit tests for tools/psyq_lib_extract.py.

Builds a synthetic PsyQ .LIB in memory, runs the extractor, and verifies:
  - all members are recovered
  - per-member symbol lists (multi-symbol case) are preserved
  - obj_bytes always start with the LNK\\x02 magic
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from psyq_lib_extract import extract  # noqa: E402


def _build_member(tag: bytes, ts: int, mid: int, names: list[bytes], obj: bytes) -> bytes:
    """Build a single LIB member matching the real PsyQ format."""
    sym_table = b"".join(bytes([len(n)]) + n for n in names) + b"\x00"
    body = sym_table + obj
    # member size = 8 (tag) + 4 (ts) + 4 (id) + 4 (size itself) + body
    size = 20 + len(body)
    header = tag.ljust(8, b" ")[:8] + struct.pack("<III", ts, mid, size)
    return header + body


def _build_lib(members: list[bytes]) -> bytes:
    return b"LIB\x01" + b"".join(members)


def test_single_symbol_member(tmp_path: Path):
    obj = b"LNK\x02" + b"\xAB" * 32
    lib = _build_lib([_build_member(b"SYS", 1, 1, [b"VSync"], obj)])
    libfile = tmp_path / "fake.lib"
    libfile.write_bytes(lib)

    out = tmp_path / "out"
    members = extract(libfile, out)
    assert len(members) == 1
    primary, size, tag, names = members[0]
    assert primary == "VSync"
    assert tag == "SYS"
    assert names == ["VSync"]
    assert (out / "VSync.OBJ").read_bytes() == obj
    assert size == len(obj)


def test_multi_symbol_member(tmp_path: Path):
    """LIBGPU EXT member-style: many public symbols, one OBJ blob."""
    obj = b"LNK\x02" + b"\xCD" * 100
    sym_names = [b"LoadClut", b"LoadClut2", b"LoadTPage",
                 b"SetDefDrawEnv", b"SetDefDispEnv"]
    lib = _build_lib([_build_member(b"EXT", 1, 1, sym_names, obj)])
    libfile = tmp_path / "fake.lib"
    libfile.write_bytes(lib)

    out = tmp_path / "out"
    members = extract(libfile, out)
    assert len(members) == 1
    primary, _size, tag, names = members[0]
    assert primary == "LoadClut"
    assert tag == "EXT"
    assert names == ["LoadClut", "LoadClut2", "LoadTPage",
                     "SetDefDrawEnv", "SetDefDispEnv"]
    # Filename is the FIRST public symbol; the others are sibling functions
    # that the parser will resolve once the .OBJ is fed to it.
    assert (out / "LoadClut.OBJ").exists()
    assert (out / "LoadClut.OBJ").read_bytes().startswith(b"LNK\x02")


def test_multiple_members_preserves_order(tmp_path: Path):
    objA = b"LNK\x02" + b"\x01" * 16
    objB = b"LNK\x02" + b"\x02" * 24
    objC = b"LNK\x02" + b"\x03" * 32
    lib = _build_lib([
        _build_member(b"SYS",  1, 1, [b"AddPrim"], objA),
        _build_member(b"PRIM", 2, 2, [b"CatPrim", b"BreakDraw"], objB),
        _build_member(b"BREAK", 3, 3, [b"ResetGraph"], objC),
    ])
    libfile = tmp_path / "fake.lib"
    libfile.write_bytes(lib)

    members = extract(libfile, tmp_path / "out")
    assert [m[0] for m in members] == ["AddPrim", "CatPrim", "ResetGraph"]
    assert members[1][3] == ["CatPrim", "BreakDraw"]


def test_rejects_non_lib(tmp_path: Path):
    bad = tmp_path / "bad.lib"
    bad.write_bytes(b"NOTALIB\x00")
    with pytest.raises(SystemExit):
        extract(bad, tmp_path / "out")


def test_real_libgpu_extracts_without_warnings(tmp_path: Path, capsys):
    """Regression test: the v3.5 LIBGPU.LIB must extract without LNK\\x02
    warnings (the bug we fixed in Session 0.2). Skipped unless PSYQ_SDK_ROOT
    points at a local CONSOLIDATED SDK tree."""
    import os
    sdk = os.environ.get("PSYQ_SDK_ROOT")
    if not sdk:
        pytest.skip("PSYQ_SDK_ROOT not set -- skipping local SDK regression")
    libfile = Path(sdk) / "v3.5" / "lib" / "LIBGPU.LIB"
    if not libfile.exists():
        pytest.skip(f"PsyQ SDK not present at {sdk}")

    members = extract(libfile, tmp_path / "out")
    assert len(members) >= 10, f"expected >=10 LIBGPU members, got {len(members)}"
    captured = capsys.readouterr()
    assert "LNK" not in captured.err, f"unexpected warnings:\n{captured.err}"
    # Every emitted .OBJ must begin with LNK\x02.
    for primary, _sz, _tag, _names in members:
        data = (tmp_path / "out" / f"{primary}.OBJ").read_bytes()
        assert data[:4] == b"LNK\x02"


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
