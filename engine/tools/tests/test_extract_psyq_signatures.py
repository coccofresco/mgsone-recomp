"""Unit tests for tools/extract_psyq_signatures.py.

Run from the repo root:
    python3 -m pytest tools/tests -v
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from extract_psyq_signatures import (  # noqa: E402
    FULL_MATCH_SIZE_LIMIT,
    FuncSig,
    apply_metadata,
    classify_match_mode,
    derive_default_label,
    derive_library,
    load_metadata,
    mask_immediates,
    merge,
    metadata_for,
    write_toml,
)


# ---------------------------------------------------------------------------
# mask_immediates: every opcode class gets a dedicated check
# ---------------------------------------------------------------------------

class TestMaskImmediates:
    def test_r_type_passthrough(self):
        # add $v0, $a0, $a1 -- pure R-type, no imm to mask
        word = 0x00851020
        assert mask_immediates(word) == word

    def test_r_type_jr(self):
        # jr $ra (special, funct=0x08) -- no imm
        word = 0x03E00008
        assert mask_immediates(word) == word

    def test_addiu_imm_zeroed(self):
        # addiu $t1, $zero, 0xAD  -- only the imm should disappear
        word = 0x240900AD
        assert mask_immediates(word) == 0x24090000

    def test_lui_imm_zeroed(self):
        # lui $at, 0x1F80 -- relocations against absolute addrs hit this imm
        word = 0x3C011F80
        assert mask_immediates(word) == 0x3C010000

    def test_andi_ori_xori(self):
        for word in (0x30210FFF, 0x34210FFF, 0x38210FFF):
            assert mask_immediates(word) == (word & 0xFFFF0000)

    def test_load_store_imm_zeroed(self):
        # lw $a0, 0x1234($sp)
        assert mask_immediates(0x8FA41234) == 0x8FA40000
        # sw $zero, 0x1814($at) -- used by ResetGraph
        assert mask_immediates(0xAC201814) == 0xAC200000
        # sb / sh variants
        assert mask_immediates(0xA0201D80) == 0xA0200000
        assert mask_immediates(0xA4201D80) == 0xA4200000

    def test_branches_offset_zeroed(self):
        # beq $a0, $a1, +0x40
        assert mask_immediates(0x10850010) == 0x10850000
        # bne, blez, bgtz, beql, bnel
        for word in (0x14850010, 0x18800010, 0x1C800010, 0x50850010, 0x54850010):
            assert mask_immediates(word) == (word & 0xFFFF0000)
        # REGIMM (BLTZ, BGEZ, etc.) -- opcode 0x01
        assert mask_immediates(0x04800010) == 0x04800000
        assert mask_immediates(0x04810010) == 0x04810000  # bgez

    def test_j_jal_target_zeroed(self):
        # j 0x80100000 -> encoded target = (0x80100000 >> 2) & 0x3FFFFFF
        assert mask_immediates(0x08040000) == 0x08000000
        # jal
        assert mask_immediates(0x0C040000) == 0x0C000000

    def test_cop_kept_exact(self):
        # mfc0/mtc0/cfc2/ctc2/mtc2/mfc2 -- opcode 0x10..0x13.
        # Their funct/rs/rd encoding is critical (GTE relies on it),
        # so we must NOT mask anything in COP instructions.
        for word in (0x40046800, 0x40846800, 0x48046800, 0x4884E800):
            assert mask_immediates(word) == word

    def test_two_addiu_with_diff_imm_collide_on_mask(self):
        # The whole point: addiu t1,zero,0x12 and addiu t1,zero,0xAD
        # masked-collapse to the same value (relocation-tolerance).
        a = mask_immediates(0x24090012)
        b = mask_immediates(0x240900AD)
        assert a == b == 0x24090000


# ---------------------------------------------------------------------------
# merge(): same name + same masked -> coalesce sources;
#          same name + different masked -> @v2 suffix
# ---------------------------------------------------------------------------

def _sig(name, masked, full="ff"*8, size=64, sources=None,
         library="", match_mode="", subsystem="", stub_type=""):
    return FuncSig(
        name=name,
        size=size,
        hash_masked=masked,
        hash_full=full,
        library=library,
        match_mode=match_mode,
        subsystem=subsystem,
        stub_type=stub_type,
        sources=list(sources) if sources else [],
    )


class TestMerge:
    def test_disjoint_names_both_kept(self):
        a = {"VSync": _sig("VSync", "aaaa", sources=["v3.5"])}
        b = {"DrawSync": _sig("DrawSync", "bbbb", sources=["v3.5"])}
        merge(a, b)
        assert set(a) == {"VSync", "DrawSync"}

    def test_same_name_same_masked_coalesces_sources(self):
        a = {"VSync": _sig("VSync", "aaaa", sources=["v3.5/LIBETC"])}
        b = {"VSync": _sig("VSync", "aaaa", sources=["v4.0/LIBETC"])}
        merge(a, b)
        assert set(a) == {"VSync"}
        assert a["VSync"].sources == ["v3.5/LIBETC", "v4.0/LIBETC"]

    def test_same_name_same_masked_does_not_duplicate_sources(self):
        a = {"VSync": _sig("VSync", "aaaa", sources=["v3.5/LIBETC"])}
        b = {"VSync": _sig("VSync", "aaaa", sources=["v3.5/LIBETC"])}
        merge(a, b)
        assert a["VSync"].sources == ["v3.5/LIBETC"]

    def test_same_name_different_masked_creates_variant(self):
        a = {"ResetGraph": _sig("ResetGraph", "aaaa", sources=["v3.5"])}
        b = {"ResetGraph": _sig("ResetGraph", "bbbb", sources=["v4.0"])}
        merge(a, b)
        assert set(a) == {"ResetGraph", "ResetGraph@v2"}
        assert a["ResetGraph@v2"].hash_masked == "bbbb"
        assert a["ResetGraph@v2"].sources == ["v4.0"]

    def test_three_distinct_variants_chain(self):
        a = {"X": _sig("X", "aa", sources=["s1"])}
        merge(a, {"X": _sig("X", "bb", sources=["s2"])})
        merge(a, {"X": _sig("X", "cc", sources=["s3"])})
        assert set(a) == {"X", "X@v2", "X@v3"}

    def test_hash_matching_existing_variant_coalesces(self):
        # Without the variant scan, base=A + new=B (->@v2) + new=A would
        # spuriously add A as @v3 even though it duplicates the base.
        a = {"X": _sig("X", "aa", sources=["s1"])}
        merge(a, {"X": _sig("X", "bb", sources=["s2"])})  # creates @v2
        merge(a, {"X": _sig("X", "aa", sources=["s3"])})  # must coalesce to base
        assert set(a) == {"X", "X@v2"}
        assert a["X"].sources == ["s1", "s3"]
        assert a["X@v2"].sources == ["s2"]

    def test_hash_matching_v2_coalesces_into_v2(self):
        # Same as above but the duplicate matches @v2's hash, not base's.
        a = {"X": _sig("X", "aa", sources=["s1"])}
        merge(a, {"X": _sig("X", "bb", sources=["s2"])})  # @v2
        merge(a, {"X": _sig("X", "cc", sources=["s3"])})  # @v3
        merge(a, {"X": _sig("X", "bb", sources=["s4"])})  # must coalesce to @v2
        assert set(a) == {"X", "X@v2", "X@v3"}
        assert a["X@v2"].sources == ["s2", "s4"]
        assert a["X@v3"].sources == ["s3"]


# ---------------------------------------------------------------------------
# derive_default_label: auto-derived "<version>/<LIBSTEM>" from path layout
# ---------------------------------------------------------------------------

class TestDeriveDefaultLabel:
    def test_standard_layout_uses_grandparent_as_version(self):
        p = Path("/x/CONSOLIDATED/v3.5/lib/LIBGPU.LIB")
        assert derive_default_label(p) == "v3.5/LIBGPU"

    def test_lowercase_lib_uppercased_in_label(self):
        p = Path("/x/CONSOLIDATED/foxdie_psyq_4.5/lib/libgpu.lib")
        assert derive_default_label(p) == "foxdie_psyq_4.5/LIBGPU"

    def test_non_standard_layout_falls_back_to_stem(self):
        # No "lib" directory -> can't infer version; degrade gracefully.
        p = Path("/tmp/LIBGPU.LIB")
        assert derive_default_label(p) == "LIBGPU"


# ---------------------------------------------------------------------------
# write_toml: round-trippable, deterministic, alphabetical
# ---------------------------------------------------------------------------

def test_write_toml_is_alphabetical_and_parseable(tmp_path: Path):
    sigs = {
        "Zeta":  _sig("Zeta",  "ff", size=8,  sources=["s1"],
                      library="libgpu", match_mode="full",
                      subsystem="graphics", stub_type="stub"),
        "Alpha": _sig("Alpha", "aa", size=64, sources=["s1", "s2"],
                      library="libetc", match_mode="masked",
                      subsystem="vsync", stub_type="stub"),
        "Mu":    _sig("Mu",    "55", size=12, sources=[],
                      library="libcd", match_mode="full",
                      subsystem="cdrom", stub_type="stub"),
    }
    out = tmp_path / "out.toml"
    write_toml(out, sigs)
    text = out.read_text()
    # Names in alphabetical order
    assert text.index("Alpha") < text.index("Mu") < text.index("Zeta")
    # Empty sources list emitted correctly
    assert "sources     = []" in text
    # Multi-source list properly comma-separated
    assert 'sources     = ["s1", "s2"]' in text
    # New fields emitted
    assert 'library     = "libgpu"' in text
    assert 'library     = "libetc"' in text
    assert 'match_mode  = "full"' in text
    assert 'match_mode  = "masked"' in text
    assert 'subsystem   = "graphics"' in text
    assert 'stub_type   = "stub"' in text

    # Round-trip via tomllib: every required key present.
    import tomllib
    doc = tomllib.loads(text)
    assert {s["name"] for s in doc["signature"]} == {"Alpha", "Mu", "Zeta"}
    for entry in doc["signature"]:
        assert set(entry) >= {
            "name", "library", "size", "hash_masked", "hash_full",
            "match_mode", "subsystem", "stub_type", "sources",
        }


# ---------------------------------------------------------------------------
# derive_library: .LIB stem -> lowercase library identifier
# ---------------------------------------------------------------------------

class TestDeriveLibrary:
    def test_uppercase_lib_extension(self):
        assert derive_library(Path("/tmp/LIBGPU.LIB")) == "libgpu"

    def test_lowercase_lib_extension(self):
        assert derive_library(Path("/tmp/libgpu.lib")) == "libgpu"

    def test_mixed_case(self):
        assert derive_library(Path("/tmp/LibGpu.Lib")) == "libgpu"

    def test_each_target_lib(self):
        for stem in ("LIBGPU", "LIBETC", "LIBAPI", "LIBCD", "LIBGTE"):
            assert derive_library(Path(f"/x/{stem}.LIB")) == stem.lower()


# ---------------------------------------------------------------------------
# classify_match_mode: size threshold for full vs masked hashing
# ---------------------------------------------------------------------------

class TestClassifyMatchMode:
    def test_tiny_bios_wrapper_is_full(self):
        # 4 instructions x 4 bytes = 16, classic A0/B0/C0 wrapper.
        assert classify_match_mode(16) == "full"

    def test_at_threshold_is_full(self):
        assert classify_match_mode(FULL_MATCH_SIZE_LIMIT) == "full"

    def test_just_above_threshold_is_masked(self):
        assert classify_match_mode(FULL_MATCH_SIZE_LIMIT + 4) == "masked"

    def test_typical_sdk_function_is_masked(self):
        # ResetGraph v3.5 is 344B, well above threshold.
        assert classify_match_mode(344) == "masked"

    def test_threshold_matches_documented_value(self):
        # Sessao 0.2 documented this as 24B; if we change it, tests must too.
        assert FULL_MATCH_SIZE_LIMIT == 24


# ---------------------------------------------------------------------------
# load_metadata + metadata_for: TOML lookup with @vN variant fallback
# ---------------------------------------------------------------------------

def _write_metadata(path: Path, entries: list[dict]) -> None:
    lines = []
    for e in entries:
        lines.append("[[function]]")
        for k, v in e.items():
            lines.append(f'{k} = "{v}"')
        lines.append("")
    path.write_text("\n".join(lines))


class TestLoadMetadata:
    def test_missing_file_returns_empty(self, tmp_path: Path):
        assert load_metadata(tmp_path / "nonexistent.toml") == {}

    def test_loads_function_table(self, tmp_path: Path):
        meta = tmp_path / "meta.toml"
        _write_metadata(meta, [
            {"name": "ResetGraph",  "subsystem": "graphics", "stub_type": "stub"},
            {"name": "CdInit",      "subsystem": "cdrom",    "stub_type": "stub"},
            {"name": "memcpy",      "subsystem": "libc",     "stub_type": "passthrough"},
        ])
        loaded = load_metadata(meta)
        assert loaded["ResetGraph"] == ("graphics", "stub")
        assert loaded["CdInit"]     == ("cdrom",    "stub")
        assert loaded["memcpy"]     == ("libc",     "passthrough")

    def test_skips_entries_without_name(self, tmp_path: Path):
        meta = tmp_path / "meta.toml"
        meta.write_text(
            "[[function]]\nsubsystem = \"graphics\"\nstub_type = \"stub\"\n\n"
            "[[function]]\nname = \"VSync\"\nsubsystem = \"vsync\"\nstub_type = \"stub\"\n"
        )
        loaded = load_metadata(meta)
        assert loaded == {"VSync": ("vsync", "stub")}

    def test_real_metadata_file_loads(self):
        # Smoke test: the project's actual metadata file must parse.
        repo_root = Path(__file__).resolve().parents[2]
        meta = repo_root / "ps1Analyzer" / "data" / "psyq_metadata.toml"
        if not meta.exists():
            pytest.skip(f"{meta} not present in this checkout")
        loaded = load_metadata(meta)
        # Spot-check a few entries that must be there for the matcher to work.
        assert loaded["ResetGraph"]  == ("graphics", "stub")
        assert loaded["VSync"]       == ("vsync",    "stub")
        assert loaded["CdInit"]      == ("cdrom",    "stub")
        assert loaded["memcpy"]      == ("libc",     "passthrough")
        assert loaded["FntPrint"]    == ("graphics", "skip")
        assert loaded["printf"]      == ("libc",     "skip")


class TestMetadataFor:
    def test_exact_name_hit(self):
        meta = {"VSync": ("vsync", "stub")}
        assert metadata_for("VSync", meta) == ("vsync", "stub")

    def test_variant_falls_back_to_base(self):
        meta = {"ResetGraph": ("graphics", "stub")}
        # Variants like ResetGraph@v2 should inherit the base entry.
        assert metadata_for("ResetGraph@v2", meta) == ("graphics", "stub")
        assert metadata_for("ResetGraph@v9", meta) == ("graphics", "stub")

    def test_unknown_returns_blanks(self):
        assert metadata_for("MysteryFunc", {}) == ("", "")
        assert metadata_for("MysteryFunc@v2", {}) == ("", "")


# ---------------------------------------------------------------------------
# apply_metadata: in-place fill on merged signature dict
# ---------------------------------------------------------------------------

class TestApplyMetadata:
    def test_fills_known_names(self):
        sigs = {
            "ResetGraph":     _sig("ResetGraph", "aa", library="libgpu",
                                    match_mode="masked"),
            "ResetGraph@v2":  _sig("ResetGraph@v2", "bb", library="libgpu",
                                    match_mode="masked"),
            "Unknown":        _sig("Unknown", "cc", library="libgpu"),
        }
        meta = {"ResetGraph": ("graphics", "stub")}
        apply_metadata(sigs, meta)
        assert sigs["ResetGraph"].subsystem == "graphics"
        assert sigs["ResetGraph"].stub_type == "stub"
        # Variants inherit metadata of the base name -- important for @vN.
        assert sigs["ResetGraph@v2"].subsystem == "graphics"
        assert sigs["ResetGraph@v2"].stub_type == "stub"
        # Unknown stays blank, doesn't blow up.
        assert sigs["Unknown"].subsystem == ""
        assert sigs["Unknown"].stub_type == ""

    def test_preserves_other_fields(self):
        sigs = {"X": _sig("X", "aa", size=64, library="libgpu",
                          match_mode="masked", sources=["v3.5/LIBGPU/T"])}
        apply_metadata(sigs, {"X": ("graphics", "stub")})
        s = sigs["X"]
        assert s.library == "libgpu"
        assert s.match_mode == "masked"
        assert s.size == 64
        assert s.sources == ["v3.5/LIBGPU/T"]


# ---------------------------------------------------------------------------
# End-to-end: build a fake .text and verify hashes are stable + relocation-tolerant
# ---------------------------------------------------------------------------

def _hash_words(words: list[int]) -> tuple[str, str]:
    """Mirror what _section_funcs does on a list of MIPS words."""
    import hashlib
    raw = b"".join(struct.pack("<I", w) for w in words)
    masked = b"".join(struct.pack("<I", mask_immediates(w)) for w in words)
    return (
        hashlib.sha256(masked).hexdigest()[:16],
        hashlib.sha256(raw).hexdigest()[:16],
    )


def test_masked_hash_is_relocation_tolerant():
    # Two functions that differ ONLY in the lui/addiu addend pair (relocation
    # bytes) must collapse to the same hash_masked but distinct hash_full.
    func_a = [
        0x3C011F80,            # lui   at, 0x1F80
        0xAC201814,            # sw    zero, 0x1814(at)  <-- relocated imm
        0x03E00008, 0x00000000,
    ]
    func_b = [
        0x3C011F80,
        0xAC202000,            # sw    zero, 0x2000(at)  <-- different imm
        0x03E00008, 0x00000000,
    ]
    masked_a, full_a = _hash_words(func_a)
    masked_b, full_b = _hash_words(func_b)
    assert masked_a == masked_b
    assert full_a != full_b


def test_full_hash_distinguishes_bios_wrappers():
    # The LIBAPI degenerate case: two BIOS wrappers that differ only in the
    # function index. After masking they collide, but hash_full must keep
    # them distinct so the matcher can fall back.
    open_event = [0x24090008, 0x3C010000, 0x242100B0, 0x00200008]   # B0:0x08
    close_event = [0x24090009, 0x3C010000, 0x242100B0, 0x00200008]  # B0:0x09
    masked_a, full_a = _hash_words(open_event)
    masked_b, full_b = _hash_words(close_event)
    assert masked_a == masked_b           # collision is documented & expected
    assert full_a != full_b               # but full hash still discriminates


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
