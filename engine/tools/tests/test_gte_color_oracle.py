import itertools
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "donor_trace"))
import gte_color_oracle as oracle


def test_synthetic_corpus_is_deterministic_and_covers_each_mode():
    samples = list(oracle.generated_samples(36))
    assert samples == list(oracle.generated_samples(36))
    assert len(samples) == 12 * 36
    for command in oracle.COMMANDS:
        modes = {before[1] & ((1 << 19) | (1 << 10))
                 for before, _ in samples if before[1] & 63 == command}
        assert modes == {0, 1 << 19, 1 << 10, (1 << 19) | (1 << 10)}
    assert all(len(before) == 66 and after is None for before, after in samples)


def test_capture_preserves_pc_and_all_register_words(tmp_path):
    before, _ = next(oracle.generated_samples(36))
    before = (0x80042F18, *before[1:])
    after = tuple(range(64))
    path = tmp_path / "sample.bin"
    path.write_bytes(oracle.MAGIC + oracle.RECORD.pack(*before, *after))
    assert list(oracle.captured_samples(path)) == [(before, after)]


@pytest.mark.parametrize("payload", [b"", b"GTECOL1", b"GTECOL2\0"])
def test_capture_rejects_invalid_header(tmp_path, payload):
    path = tmp_path / "invalid.bin"
    path.write_bytes(payload)
    with pytest.raises(ValueError, match="header"):
        list(oracle.captured_samples(path))


def test_capture_rejects_truncated_tail_unless_prefix_is_explicit(tmp_path):
    before, _ = next(oracle.generated_samples(36))
    path = tmp_path / "truncated.bin"
    path.write_bytes(oracle.MAGIC + oracle.RECORD.pack(*before, *range(64)) + b"x")
    with pytest.raises(ValueError, match="truncated"):
        list(oracle.captured_samples(path))
    assert len(list(itertools.islice(oracle.captured_samples(path), 1))) == 1


def test_capture_rejects_commands_outside_declared_coverage(tmp_path):
    path = tmp_path / "unsupported.bin"
    path.write_bytes(oracle.MAGIC + struct.pack("<130I", 0, 0x30, *([0] * 128)))
    with pytest.raises(ValueError, match="unsupported"):
        list(oracle.captured_samples(path))
