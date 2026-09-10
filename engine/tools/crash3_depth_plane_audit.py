#!/usr/bin/env python3
"""Measure raw Crash 3 Vulkan float-depth PFM captures.

Scene values store GTE/presentation W divided by 2^32. The explicit sky class
remains below 1 in presented-surface R32, but operational D32 puts it exactly
at 1 to avoid self-occlusion. Pair those captures to distinguish sky from clear;
an unpaired operational D32 cannot prove that distinction. The host-scene D32
is the modern scene attachment; the legacy operational-occlusion D32 belongs
to replay and cannot certify modern scene occlusion. This audit keeps
IEEE-754 bit identity and measures the useful log2(W) range to catch a
genuinely flat/binary plane independently from the F8 colour mapping.
"""

from __future__ import annotations

import argparse
import array
import json
import math
import re
import struct
import sys
from pathlib import Path
from typing import Any


REPORT_SCHEMA = "ps1-crash3-depth-plane-audit/v2"
FLOAT_ONE_BITS = 0x3F800000
BACKGROUND_MIN_BITS = 0x3F000000
EXPONENT_MASK = 0x7F800000
SIGN_MASK = 0x80000000
MANTISSA_MASK = 0x007FFFFF
LOG_BUCKETS = 4096
FRAME_NAME = re.compile(r"frame-(\d+)(?:-present-(\d+))?")


def _read_header_line(stream, path: Path) -> bytes:
    while True:
        line = stream.readline()
        if not line:
            raise ValueError(f"{path}: truncated PFM header")
        line = line.strip()
        if line and not line.startswith(b"#"):
            return line


def read_pfm_bits(path: Path) -> tuple[int, int, array.array[int]]:
    with path.open("rb") as stream:
        if _read_header_line(stream, path) != b"Pf":
            raise ValueError(f"{path}: expected single-channel Pf payload")
        dimensions = _read_header_line(stream, path).split()
        if len(dimensions) != 2:
            raise ValueError(f"{path}: malformed PFM dimensions")
        width, height = (int(value) for value in dimensions)
        if width <= 0 or height <= 0:
            raise ValueError(f"{path}: invalid PFM dimensions {width}x{height}")
        scale = float(_read_header_line(stream, path))
        if not math.isfinite(scale) or scale == 0.0 or abs(scale) != 1.0:
            raise ValueError(f"{path}: unsupported PFM scale {scale!r}")
        payload = stream.read()
    expected_bytes = width * height * 4
    if len(payload) != expected_bytes:
        raise ValueError(
            f"{path}: PFM payload has {len(payload)} bytes, expected {expected_bytes}"
        )
    words = array.array("I")
    if words.itemsize != 4:
        raise RuntimeError("depth audit requires a 32-bit unsigned array type")
    words.frombytes(payload)
    file_little_endian = scale < 0.0
    if file_little_endian != (sys.byteorder == "little"):
        words.byteswap()
    return width, height, words


def _float_from_bits(bits: int) -> float:
    return struct.unpack("=f", struct.pack("=I", bits))[0]


def _log2_raw_w(bits: int) -> float:
    exponent = ((bits & EXPONENT_MASK) >> 23) - 127
    significand = 1.0 + (bits & MANTISSA_MASK) / float(1 << 23)
    return exponent + math.log2(significand) + 32.0


def _histogram_quantile(histogram: list[int], total: int, quantile: float) -> float:
    if total <= 0:
        return 0.0
    target = max(1, math.ceil(total * quantile))
    cumulative = 0
    for bucket, count in enumerate(histogram):
        cumulative += count
        if cumulative >= target:
            return (bucket + 0.5) * 32.0 / len(histogram)
    return 32.0


def _raw_w_float_ulp(log2_w: float) -> float:
    if log2_w <= -95.0:
        return 0.0
    # R32 stores W / 2^32. Normalized binary32 precision therefore advances
    # W by 2^(floor(log2(W))-23), independent of the normalization itself.
    return math.ldexp(1.0, math.floor(log2_w) - 23)


def analyze_depth_file(path: Path) -> dict[str, Any]:
    width, height, words = read_pfm_bits(path)
    depth_plane = next((name for name in (
        "host-scene-occlusion-d32", "operational-occlusion-d32",
        "presented-surface-r32", "pre-raw-w-d32",
    ) if name in path.name), "unknown")
    operational = depth_plane in ("host-scene-occlusion-d32", "operational-occlusion-d32")
    surface_path = path.with_name(path.name.replace(depth_plane, "presented-surface-r32"))
    surface_words = None
    if operational and surface_path.is_file():
        surface_width, surface_height, surface_words = read_pfm_bits(surface_path)
        if (surface_width, surface_height) != (width, height):
            raise ValueError(f"{path}: paired presented-surface dimensions disagree")
    scene_values: set[int] = set()
    background_values: set[int] = set()
    histogram = [0] * LOG_BUCKETS
    scene_pixels = 0
    background_pixels = 0
    clear_pixels = 0
    invalid_pixels = 0
    minimum_scene_bits = FLOAT_ONE_BITS
    maximum_scene_bits = 0

    for index, bits in enumerate(words):
        exponent = bits & EXPONENT_MASK
        positive_finite = not (bits & SIGN_MASK) and exponent != EXPONENT_MASK
        if positive_finite and 0 < bits < BACKGROUND_MIN_BITS:
            scene_pixels += 1
            scene_values.add(bits)
            minimum_scene_bits = min(minimum_scene_bits, bits)
            maximum_scene_bits = max(maximum_scene_bits, bits)
            log2_w = min(max(_log2_raw_w(bits), 0.0), 32.0)
            bucket = min(int(log2_w / 32.0 * LOG_BUCKETS), LOG_BUCKETS - 1)
            histogram[bucket] += 1
        elif positive_finite and BACKGROUND_MIN_BITS <= bits < FLOAT_ONE_BITS:
            background_pixels += 1
            background_values.add(bits)
        elif bits == FLOAT_ONE_BITS:
            if surface_words is not None and BACKGROUND_MIN_BITS <= surface_words[index] < FLOAT_ONE_BITS:
                background_pixels += 1
                background_values.add(bits)
            else:
                clear_pixels += 1
        else:
            invalid_pixels += 1

    total = len(words)
    quantiles = {
        name: _histogram_quantile(histogram, scene_pixels, quantile)
        for name, quantile in (
            ("p01", 0.01),
            ("p10", 0.10),
            ("p50", 0.50),
            ("p90", 0.90),
            ("p99", 0.99),
        )
    }
    median_log_w = quantiles["p50"]
    return {
        "source": str(path),
        "depth_plane": depth_plane,
        "legacy_replay_depth_not_host_scene": depth_plane == "operational-occlusion-d32",
        "paired_presented_surface": str(surface_path) if surface_words is not None else None,
        "clear_may_include_background": operational and surface_words is None,
        "width": width,
        "height": height,
        "pixels": total,
        "scene_pixels": scene_pixels,
        "scene_fraction": scene_pixels / total,
        "scene_distinct_depth_values": len(scene_values),
        "scene_distinct_value_fraction": (
            len(scene_values) / scene_pixels if scene_pixels else 0.0
        ),
        "background_pixels": background_pixels,
        "background_fraction": background_pixels / total,
        "background_distinct_depth_values": len(background_values),
        "clear_pixels": clear_pixels,
        "clear_fraction": clear_pixels / total,
        "invalid_pixels": invalid_pixels,
        "invalid_fraction": invalid_pixels / total,
        "minimum_scene_encoded": (
            _float_from_bits(minimum_scene_bits) if scene_pixels else None
        ),
        "maximum_scene_encoded": (
            _float_from_bits(maximum_scene_bits) if scene_pixels else None
        ),
        "scene_log2_raw_w": quantiles,
        "median_raw_w_float_ulp": (
            _raw_w_float_ulp(median_log_w) if scene_pixels else None
        ),
        "binary_like_scene": len(scene_values) <= 2,
    }


def _capture_sort_key(path: Path) -> tuple[int, int, str]:
    match = FRAME_NAME.search(path.name)
    if match is None:
        return (sys.maxsize, sys.maxsize, str(path))
    return (
        int(match.group(1)),
        int(match.group(2)) if match.group(2) is not None else 0,
        str(path),
    )


def _is_all_clear(capture: dict[str, Any]) -> bool:
    return (
        capture["clear_pixels"] == capture["pixels"]
        and capture["scene_pixels"] == 0
        and capture["background_pixels"] == 0
        and capture["invalid_pixels"] == 0
    )


def audit_depth_files(
    paths: list[Path],
    minimum_scene_distinct_values: int = 0,
    maximum_leading_clear_captures: int = 0,
    minimum_qualified_captures: int = 1,
    maximum_clear_fraction: float = 1.0,
    maximum_invalid_fraction: float = 1.0,
) -> dict[str, Any]:
    if maximum_leading_clear_captures < 0:
        raise ValueError("maximum_leading_clear_captures must not be negative")
    if minimum_qualified_captures < 0:
        raise ValueError("minimum_qualified_captures must not be negative")
    if not 0.0 <= maximum_clear_fraction <= 1.0:
        raise ValueError("maximum_clear_fraction must be between zero and one")
    if not 0.0 <= maximum_invalid_fraction <= 1.0:
        raise ValueError("maximum_invalid_fraction must be between zero and one")
    ordered_paths = (
        sorted(paths, key=_capture_sort_key)
        if paths and all(FRAME_NAME.search(path.name) for path in paths)
        else list(paths)
    )
    captures = [analyze_depth_file(path) for path in ordered_paths]
    leading_clear = 0
    for capture in captures:
        if not _is_all_clear(capture):
            break
        capture["sequence_classification"] = "leading_clear_warmup"
        leading_clear += 1
    for capture in captures[leading_clear:]:
        capture["sequence_classification"] = "qualified"
    evaluated = captures[leading_clear:]
    failures = []
    if leading_clear > maximum_leading_clear_captures:
        failures.append(
            f"{leading_clear} leading all-clear captures exceed "
            f"{maximum_leading_clear_captures}"
        )
    if len(evaluated) < minimum_qualified_captures:
        failures.append(
            f"only {len(evaluated)} qualified captures remain, expected at least "
            f"{minimum_qualified_captures}"
        )
    if minimum_scene_distinct_values > 0:
        failures.extend(
            f"{capture['source']}: only "
            f"{capture['scene_distinct_depth_values']} distinct scene depths"
            for capture in evaluated
            if capture["scene_distinct_depth_values"] < minimum_scene_distinct_values
        )
    failures.extend(
        f"{capture['source']}: clear fraction "
        f"{capture['clear_fraction']:.6f} exceeds {maximum_clear_fraction:.6f}"
        for capture in evaluated
        if capture["clear_fraction"] > maximum_clear_fraction
    )
    failures.extend(
        f"{capture['source']}: invalid fraction "
        f"{capture['invalid_fraction']:.6f} exceeds {maximum_invalid_fraction:.6f}"
        for capture in evaluated
        if capture["invalid_fraction"] > maximum_invalid_fraction
    )
    return {
        "schema": REPORT_SCHEMA,
        "captures": captures,
        "leading_clear_captures": leading_clear,
        "qualified_captures": len(evaluated),
        "thresholds": {
            "minimum_scene_distinct_values": minimum_scene_distinct_values,
            "maximum_leading_clear_captures": maximum_leading_clear_captures,
            "minimum_qualified_captures": minimum_qualified_captures,
            "maximum_clear_fraction": maximum_clear_fraction,
            "maximum_invalid_fraction": maximum_invalid_fraction,
        },
        "failures": failures,
        "qualification": (
            "Classification uses raw IEEE-754 R32 values, not the F8 palette. "
            "Scene is (0, 0.5), explicit far background is [0.5, 1), and "
            "1.0 in operational D32 requires paired presented-surface R32 to "
            "distinguish background from clear; unpaired reports flag that ambiguity."
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("depth", type=Path, nargs="+")
    parser.add_argument("--minimum-scene-distinct-values", type=int, default=0)
    parser.add_argument("--maximum-leading-clear-captures", type=int, default=0)
    parser.add_argument("--minimum-qualified-captures", type=int, default=1)
    parser.add_argument("--maximum-clear-fraction", type=float, default=1.0)
    parser.add_argument("--maximum-invalid-fraction", type=float, default=1.0)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--compact", action="store_true")
    args = parser.parse_args()
    try:
        report = audit_depth_files(
            args.depth,
            minimum_scene_distinct_values=args.minimum_scene_distinct_values,
            maximum_leading_clear_captures=args.maximum_leading_clear_captures,
            minimum_qualified_captures=args.minimum_qualified_captures,
            maximum_clear_fraction=args.maximum_clear_fraction,
            maximum_invalid_fraction=args.maximum_invalid_fraction,
        )
    except (OSError, ValueError) as exc:
        print(f"crash3_depth_plane_audit: {exc}", file=sys.stderr)
        return 2
    payload = json.dumps(report, indent=None if args.compact else 2, sort_keys=True)
    if args.output:
        args.output.write_text(payload + "\n", encoding="utf-8")
    else:
        print(payload)
    return 1 if report["failures"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
