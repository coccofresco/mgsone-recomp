"""Synthetic relocated source programs; no game/emulator files needed."""
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from crash3_source_oracle_compare import RAM_SIZE, ordered_programs, compare_program


def ram_fixture(base=0x100, data=0x200):
    ram = bytearray(RAM_SIZE)
    struct.pack_into("<4I", ram, base, 0x0100ffff, 42, 4, 3)
    for index, payload in enumerate(((1,0,7), (2,1,2,2), (1,0,9))):
        pointer = data+index*16
        struct.pack_into("<I", ram, base+16+4*index, 0x80000000+pointer)
        struct.pack_into("<"+"H"*len(payload), ram, pointer, *payload)
    return ram


def test_complete_ordered_program_ignores_only_pointer_relocation():
    left, right = ram_fixture(), ram_fixture(0x1000,0x2000)
    result = compare_program(left, right, 42)
    assert result["matches"]
    assert result["checkpoint"][0]["state_count"] == 2
    assert len(result["checkpoint"][0]["ordered_item_sha256"]) == 3
    struct.pack_into("<H", right, 0x2024, 8)  # terminal full list matters
    assert not compare_program(left, right, 42)["matches"]


def test_malformed_or_absent_program_is_not_a_matching_empty_result():
    left, right = ram_fixture(), ram_fixture()
    assert not compare_program(left,right,43)["matches"]
    struct.pack_into("<I", right, 0x110, 0x1f800000)  # outside RAM
    assert not ordered_programs(right,42)
    struct.pack_into("<I", left, 0x10c, 4098)
    assert not ordered_programs(left,42)
    with pytest.raises(ValueError, match="2 MiB"):
        ordered_programs(b"PS1S",42)


def test_conflicting_duplicate_entries_fail_even_when_one_copy_matches():
    left, right = ram_fixture(), ram_fixture()
    duplicate = ram_fixture(0x1000,0x2000)
    right[0x1000:0x1030] = duplicate[0x1000:0x1030]
    right[0x2000:0x2030] = duplicate[0x2000:0x2030]
    assert compare_program(left,right,42)["matches"]
    struct.pack_into("<H", right, 0x2024, 8)
    assert not compare_program(left,right,42)["matches"]
