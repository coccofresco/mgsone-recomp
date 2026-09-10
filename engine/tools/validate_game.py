#!/usr/bin/env python3
"""
validate_game.py -- Automated validation pipeline for ps1-recomp.

Runs the game, captures VRAM snapshots at multiple time points, analyzes
pixel content, and reports whether display is correct.

Usage:
    python3 tools/validate_game.py [--config configs/rayman.toml] [--duration 20]

Exit codes:
    0 = all checks passed
    1 = one or more checks failed
    2 = build or runtime error
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

# Configuration

PROJECT_ROOT = Path(__file__).parent.parent
BUILD_DIR    = PROJECT_ROOT / "build"
RUNTIME_BIN  = BUILD_DIR / "ps1Runtime" / "ps1Runtime"
TESTS_BIN    = BUILD_DIR / "ps1Test" / "ps1Runtime_tests"

# VRAM dimensions (PS1 always 1024x512, ABGR1555 = 2 bytes/pixel)
VRAM_W, VRAM_H = 1024, 512
VRAM_BYTES = VRAM_W * VRAM_H * 2

# Data classes

@dataclass
class CheckResult:
    name: str
    passed: bool
    detail: str = ""

@dataclass
class Report:
    checks: list = field(default_factory=list)

    def add(self, name: str, passed: bool, detail: str = ""):
        self.checks.append(CheckResult(name, passed, detail))

    def passed(self) -> bool:
        return all(c.passed for c in self.checks)

    def print(self):
        print("\n" + "="*60)
        print("  VALIDATION REPORT")
        print("="*60)
        for c in self.checks:
            icon = "[ok]" if c.passed else "[fail]"
            status = "PASS" if c.passed else "FAIL"
            print(f"  [{icon}] {status:4}  {c.name}")
            if c.detail:
                for line in c.detail.strip().splitlines():
                    print(f"             {line}")
        print("="*60)
        total = len(self.checks)
        passed_n = sum(1 for c in self.checks if c.passed)
        print(f"  {passed_n}/{total} checks passed")
        print("="*60 + "\n")

# Step 1: Build

def step_build(report: Report, target: str = "") -> bool:
    print("[1/5] Building project...")
    cmd = ["cmake", "--build", str(BUILD_DIR), f"-j{os.cpu_count() or 4}"]
    if target:
        cmd += ["--target", target]
    result = subprocess.run(cmd, capture_output=True, text=True,
                            cwd=str(PROJECT_ROOT))
    ok = result.returncode == 0
    detail = ""
    if not ok:
        # Show last 20 lines of errors
        lines = (result.stdout + result.stderr).splitlines()
        detail = "\n".join(lines[-20:])
    report.add("Build succeeds", ok, detail)
    return ok

# Step 2: Unit Tests

def step_unit_tests(report: Report) -> bool:
    print("[2/5] Running unit tests...")
    result = subprocess.run(
        ["ctest", "--test-dir", str(BUILD_DIR), "--output-on-failure", "-j4"],
        capture_output=True, text=True, cwd=str(PROJECT_ROOT)
    )
    ok = result.returncode == 0
    # Extract pass/fail counts from ctest output
    m = re.search(r"(\d+) tests? passed", result.stdout + result.stderr)
    total_m = re.search(r"(\d+) tests?", result.stdout + result.stderr)
    count = m.group(1) if m else "?"
    detail = f"{count} tests passed"
    if not ok:
        # Show failed test names
        lines = result.stdout.splitlines()
        failed = [l for l in lines if "FAILED" in l]
        detail = "\n".join(failed[:10]) or (result.stdout + result.stderr)[-500:]
    report.add("All unit tests pass", ok, detail)
    return ok

# Step 3: Game Runtime

def step_runtime(report: Report, config: Path, duration: int,
                 vram_dir: Path) -> tuple[bool, str, list[Path]]:
    print(f"[3/5] Running game for {duration}s (config={config.name})...")

    if not RUNTIME_BIN.exists():
        report.add("Runtime binary exists", False, str(RUNTIME_BIN))
        return False, "", []

    report.add("Runtime binary exists", True)

    # Environment: ask game to dump VRAM periodically
    env = os.environ.copy()
    env["PS1_VRAM_DUMP"] = str(vram_dir / "vram_%05d.ppm")

    proc = subprocess.Popen(
        [str(RUNTIME_BIN), "--config", str(config)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, env=env, cwd=str(PROJECT_ROOT)
    )

    time.sleep(duration)
    proc.terminate()
    try:
        stdout, stderr = proc.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, stderr = proc.communicate()

    log = stdout + stderr
    vram_files = sorted(vram_dir.glob("vram_*.ppm"))

    # Check: process didn't crash
    crashed = proc.returncode not in (0, -15, -2)  # 0=ok, -15=SIGTERM, -2=SIGINT
    report.add("Runtime no crash (no SIGSEGV)", not crashed,
               f"exit code={proc.returncode}" if crashed else "")

    # Check: no VSync timeouts
    vsync_timeouts = log.count("VSync timeout")
    report.add("No VSync timeouts", vsync_timeouts == 0,
               f"{vsync_timeouts} timeout(s) found" if vsync_timeouts else "")

    # Check: NULL dispatch count (should be low, a few at startup is ok)
    null_dispatch = log.count("NULL dispatch")
    report.add("NULL dispatch < 10 (only startup)", null_dispatch < 10,
               f"{null_dispatch} NULL dispatch(es)")

    # Check: GPU display area was set
    display_set = "[GPU] Display area set:" in log or "GP1(0x05)" in log
    report.add("GP1(0x05) display area sent", display_set,
               "PutDispEnv never sent a display area command" if not display_set else "")

    # Extract frame count
    m = re.search(r"(\d+) frames?", log)
    fps_line = next((l for l in log.splitlines() if "fps" in l.lower()), "")
    report.add("Frames rendered", bool(m),
               fps_line or (f"{m.group(1)} frames" if m else "no frame count found"))

    return True, log, vram_files

# Step 4: VRAM Analysis

def load_ppm_as_bytes(path: Path) -> Optional[bytes]:
    """Load a binary PPM (P6) and return raw RGB pixel bytes."""
    try:
        data = path.read_bytes()
        # Parse header: "P6\n<W> <H>\n<MAXVAL>\n"
        lines = []
        i = 0
        while len(lines) < 3:
            end = data.index(b'\n', i)
            lines.append(data[i:end].decode())
            i = end + 1
        if lines[0] != "P6":
            return None
        w, h = map(int, lines[1].split())
        return data[i:], w, h
    except Exception:
        return None

def count_nonzero_region(rgb_data: bytes, w: int, h: int,
                          rx: int, ry: int, rw: int, rh: int) -> int:
    """Count non-black pixels in a rectangular region of an RGB image."""
    count = 0
    for y in range(ry, min(ry + rh, h)):
        for x in range(rx, min(rx + rw, w)):
            idx = (y * w + x) * 3
            r, g, b = rgb_data[idx], rgb_data[idx+1], rgb_data[idx+2]
            if r > 8 or g > 8 or b > 8:  # threshold for "non-black"
                count += 1
    return count

def step_vram_analysis(report: Report, vram_files: list[Path], log: str) -> bool:
    print(f"[4/5] Analyzing {len(vram_files)} VRAM snapshot(s)...")

    if not vram_files:
        # Try to check log-based display info
        display_y = None
        m = re.search(r"displayVRAMYStart_?\s*[=:]\s*(\d+)", log)
        if m:
            display_y = int(m.group(1))
        if display_y is not None:
            # Rayman title screen uses Y=109
            report.add("VRAM: display Y in valid range",
                       display_y < 256,
                       f"displayVRAMY={display_y}")
        else:
            report.add("VRAM: snapshots available", False,
                       "No VRAM snapshots and no display Y in log. "
                       "Set PS1_VRAM_DUMP env var or add VRAM dump support.")
        return display_y is not None and display_y < 256

    ok = True

    # Analyze the last snapshot (most representative)
    last = vram_files[-1]
    result = load_ppm_as_bytes(last)
    if result is None:
        report.add("VRAM: snapshot readable", False, f"Failed to parse {last.name}")
        return False

    rgb, w, h = result

    # Expected regions for Rayman:
    # - Title screen: single 640x262 buffer at (0,109) -- large pixels in mid-VRAM
    # - Gameplay: double buffer at (0,0) and (320,0) -- pixels in top area
    # - Texture area: (0,256) to (1023,511) -- should NOT be displayed

    # Check 1: Is there content in the framebuffer area (Y=0..256)?
    fb_pixels = count_nonzero_region(rgb, w, h, 0, 0, 640, 256)
    total_possible = 640 * 256
    fb_pct = 100 * fb_pixels / total_possible
    report.add("VRAM: content in framebuffer area (Y<256)",
               fb_pixels > 1000,
               f"{fb_pixels:,} non-black pixels ({fb_pct:.1f}% of 640x256 area)")

    # Check 2: Title screen at Y=109 region (should have pixels)
    title_pixels = count_nonzero_region(rgb, w, h, 0, 109, 640, 147)
    report.add("VRAM: content in title-screen region (Y=109..256)",
               title_pixels > 500,
               f"{title_pixels:,} non-black pixels in Y=109..256")

    # Check 3: Display area sanity -- check log for what display Y was set to
    for line in log.splitlines():
        if "displayVRAMYStart" in line or "display Y" in line.lower():
            print(f"         Log: {line.strip()}")

    # PutDispEnv log analysis
    put_lines = [l for l in log.splitlines() if "PutDispEnv" in l or "GP1.*05" in l]
    if put_lines:
        detail = "\n".join(put_lines[:5])
        report.add("VRAM: PutDispEnv log entries found", True, detail)
    else:
        report.add("VRAM: PutDispEnv log entries found", False,
                   "Add fmt::print to hle_PutDispEnv to see vx/vy values")

    ok = fb_pixels > 1000
    return ok

# Step 5: Summary Patch Status

def step_patch_status(report: Report, log: str):
    print("[5/5] Checking patch/HLE status from log...")

    checks = {
        "Patch #12 VSync HLE active":
            lambda l: "hle_VSync" in l or "VSync" in l,
        "Patch #16 PutDispEnv HLE active":
            lambda l: "PutDispEnv" in l or "hle_PutDispEnv" in l,
        "GPU initialized":
            lambda l: "[GPU]" in l or "GPU" in l,
        "BIOS A-table active":
            lambda l: "BIOS" in l or "bios" in l,
    }
    for name, check in checks.items():
        passed = check(log)
        report.add(name, passed)

# Main

def main():
    parser = argparse.ArgumentParser(description="ps1-recomp game validation pipeline")
    parser.add_argument("--config",   default="configs/rayman.toml",
                        help="Game config TOML (default: configs/rayman.toml)")
    parser.add_argument("--duration", type=int, default=15,
                        help="Seconds to run the game (default: 15)")
    parser.add_argument("--skip-build", action="store_true",
                        help="Skip build step (assume already built)")
    parser.add_argument("--skip-tests", action="store_true",
                        help="Skip unit test step")
    parser.add_argument("--no-game",  action="store_true",
                        help="Only build and test, skip game launch")
    args = parser.parse_args()

    config = PROJECT_ROOT / args.config
    if not config.exists():
        print(f"ERROR: Config not found: {config}")
        print("  (ROM-based config required to run the game)")
        sys.exit(2)

    report = Report()

    # Step 1: Build
    if not args.skip_build:
        ok = step_build(report)
        if not ok:
            report.print()
            sys.exit(2)

    # Step 2: Unit tests
    if not args.skip_tests:
        step_unit_tests(report)

    if args.no_game:
        report.print()
        sys.exit(0 if report.passed() else 1)

    # Step 3: Game runtime
    with tempfile.TemporaryDirectory() as tmpdir:
        vram_dir = Path(tmpdir)
        ok, log, vram_files = step_runtime(report, config, args.duration, vram_dir)

        if ok:
            # Step 4: VRAM analysis
            step_vram_analysis(report, vram_files, log)

            # Save log for inspection
            log_path = PROJECT_ROOT / "build" / "validate_last_run.log"
            log_path.write_text(log)
            print(f"         Full log saved: {log_path}")

    # Step 5: Patch status
    step_patch_status(report, log if ok else "")

    # Final report
    report.print()
    sys.exit(0 if report.passed() else 1)


if __name__ == "__main__":
    main()
