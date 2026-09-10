#!/usr/bin/env python3
"""Game-bring-up smoke test -- boot, capture VRAM, diff against reference.

Workflow:
  1. Launch `ps1Runtime` with a config and `PS1_VRAM_DUMP_PATH=<capture>`
     in the environment; SIGTERM after `--duration` seconds.
  2. The runtime catches SIGTERM, exits its main loop, and writes the
     final 1024x512 VRAM as a P6 PPM to the env-var path.
  3. Compare the capture against a reference PPM (the "oracle" captured
     externally from PCSX-Redux into `audit_notes/<game>/title_ref.ppm`).
  4. Exit 0 if mean per-channel absolute difference is within tolerance,
     1 otherwise.

This is the regression contract for game bring-up: when the diff stays
green over a refactor, the title screen still renders. When it goes red,
something changed that affects pixels.

PPM-native parser (P6 binary). No PIL / numpy / scikit-image needed.

Usage:
    smoke_test.py --config configs/crash.toml --ref audit_notes/crash/title_ref.ppm
    smoke_test.py --config configs/rayman.toml --ref audit_notes/rayman/title_ref.ppm \\
                  --duration 30 --tolerance 24 --roi 0,0,320,240
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import time
from pathlib import Path


def parse_ppm(path: Path) -> tuple[int, int, bytes]:
    """Return (width, height, raw_rgb_bytes) for a P6 binary PPM."""
    with path.open("rb") as f:
        data = f.read()

    if not data.startswith(b"P6"):
        raise ValueError(f"{path}: not a P6 PPM (got {data[:2]!r})")

    # Header: P6\n W H\n maxval\n  (comments starting with # may appear)
    pos = 2
    fields: list[bytes] = []
    while len(fields) < 3:
        # skip whitespace
        while pos < len(data) and data[pos:pos+1] in (b" ", b"\t", b"\n", b"\r"):
            pos += 1
        # comment line
        if data[pos:pos+1] == b"#":
            while pos < len(data) and data[pos:pos+1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and data[pos:pos+1] not in (b" ", b"\t", b"\n", b"\r"):
            pos += 1
        fields.append(data[start:pos])

    w, h, maxval = (int(f) for f in fields)
    if maxval != 255:
        raise ValueError(f"{path}: only 8-bit PPM supported (maxval={maxval})")

    # one whitespace byte after header, then raw RGB
    pos += 1
    expected = w * h * 3
    body = data[pos:pos + expected]
    if len(body) != expected:
        raise ValueError(f"{path}: body size {len(body)} != expected {expected}")
    return w, h, body


def crop(rgb: bytes, w: int, h: int, roi: tuple[int, int, int, int]) -> bytes:
    """Crop ROI = (x, y, rw, rh) out of rgb (assumed w*h*3 bytes)."""
    x, y, rw, rh = roi
    if x < 0 or y < 0 or x + rw > w or y + rh > h:
        raise ValueError(f"ROI {roi} out of {w}x{h}")
    out = bytearray()
    row_bytes = rw * 3
    for row in range(y, y + rh):
        start = (row * w + x) * 3
        out += rgb[start:start + row_bytes]
    return bytes(out)


def mad(a: bytes, b: bytes) -> tuple[float, int]:
    """Mean absolute difference + max single-pixel-channel difference."""
    if len(a) != len(b):
        raise ValueError(f"size mismatch: {len(a)} vs {len(b)}")
    total = 0
    worst = 0
    for x, y in zip(a, b):
        d = abs(x - y)
        total += d
        if d > worst:
            worst = d
    return total / len(a), worst


def brightness(rgb: bytes) -> float:
    """Average luminance approximation (R+G+B)/3 mean over the buffer."""
    if not rgb:
        return 0.0
    return sum(rgb) / len(rgb)


def run_game(runtime: Path, config: Path, duration: int,
             capture_out: Path) -> int:
    """Launch ps1Runtime for `duration` seconds with PS1_VRAM_DUMP_PATH set.

    SIGTERM after `duration` so the runtime can dump VRAM in its shutdown
    path. Returns 0 on graceful termination, the runtime's exit code if it
    exits on its own.
    """
    cmd = [str(runtime), "--config", str(config)]
    capture_out.parent.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["PS1_VRAM_DUMP_PATH"] = str(capture_out)
    print(f"[smoke] launching: {' '.join(cmd)} (duration={duration}s)")
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=env)
    try:
        proc.wait(timeout=duration)
        rc = proc.returncode
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        rc = 0  # natural timeout, treat as success of the test window

    if not capture_out.exists():
        print(f"[smoke] WARN: runtime did not produce {capture_out}",
              file=sys.stderr)
    else:
        print(f"[smoke] captured VRAM -> {capture_out}")
    return rc


def parse_roi(s: str) -> tuple[int, int, int, int]:
    parts = s.split(",")
    if len(parts) != 4:
        raise argparse.ArgumentTypeError(f"--roi expects x,y,w,h (got {s!r})")
    return tuple(int(p) for p in parts)  # type: ignore[return-value]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", type=Path, required=True,
                    help="game TOML config (e.g. configs/crash.toml)")
    ap.add_argument("--ref", type=Path, required=True,
                    help="reference PPM (e.g. audit_notes/crash/title_ref.ppm)")
    ap.add_argument("--runtime", type=Path,
                    default=Path("build/ps1Runtime/ps1Runtime"),
                    help="path to ps1Runtime binary")
    ap.add_argument("--duration", type=int, default=15,
                    help="seconds to run game before capturing")
    ap.add_argument("--capture", type=Path,
                    default=Path("/tmp/smoke_capture.ppm"),
                    help="where to write the captured VRAM")
    ap.add_argument("--roi", type=parse_roi,
                    default=(0, 0, 320, 240),
                    help="region of interest x,y,w,h (default: 0,0,320,240)")
    ap.add_argument("--tolerance", type=float, default=16.0,
                    help="max mean abs diff per channel (0-255, default 16)")
    ap.add_argument("--skip-run", action="store_true",
                    help="compare existing --capture against --ref (no run)")
    args = ap.parse_args()

    if not args.ref.exists():
        print(f"[smoke] FAIL: reference {args.ref} does not exist",
              file=sys.stderr)
        print(f"        capture one externally from PCSX-Redux and save here",
              file=sys.stderr)
        return 2

    if not args.skip_run:
        if not args.runtime.exists():
            print(f"[smoke] FAIL: runtime {args.runtime} not built -- "
                  "`cmake --build build`", file=sys.stderr)
            return 2
        if not args.config.exists():
            print(f"[smoke] FAIL: config {args.config} not found",
                  file=sys.stderr)
            return 2
        rc = run_game(args.runtime, args.config, args.duration, args.capture)
        if rc != 0:
            print(f"[smoke] WARN: runtime exit code {rc}", file=sys.stderr)

    if not args.capture.exists():
        print(f"[smoke] FAIL: no capture at {args.capture}", file=sys.stderr)
        return 1

    rw, rh, ref_rgb = parse_ppm(args.ref)
    cw, ch, cap_rgb = parse_ppm(args.capture)
    print(f"[smoke] ref:     {args.ref}    {rw}x{rh}")
    print(f"[smoke] capture: {args.capture}  {cw}x{ch}")

    ref_roi = crop(ref_rgb, rw, rh, args.roi)
    cap_roi = crop(cap_rgb, cw, ch, args.roi)

    mean_diff, worst_diff = mad(ref_roi, cap_roi)
    cap_brightness = brightness(cap_roi)
    ref_brightness = brightness(ref_roi)
    print(f"[smoke] roi:           x={args.roi[0]} y={args.roi[1]} "
          f"w={args.roi[2]} h={args.roi[3]}")
    print(f"[smoke] mean diff:     {mean_diff:.2f} / 255  "
          f"(tolerance {args.tolerance})")
    print(f"[smoke] worst diff:    {worst_diff}")
    print(f"[smoke] brightness:    ref={ref_brightness:.1f} "
          f"capture={cap_brightness:.1f}")

    if mean_diff <= args.tolerance:
        print(f"[smoke] PASS")
        return 0
    print(f"[smoke] FAIL -- diff {mean_diff:.2f} > tolerance {args.tolerance}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
