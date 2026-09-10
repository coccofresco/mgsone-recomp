#!/usr/bin/env python3
"""
Automated game feedback tool for ps1-recomp.

Runs the game for a fixed duration, captures VRAM dumps at multiple points,
parses logs, and produces a structured JSON report that Claude can read to
understand the game's current state without any human involvement.

Usage:
    python3 tools/run_and_report.py [--duration SECONDS] [--output FILE]

Outputs a JSON report to stdout (and optionally a file).
"""

import argparse
import json
import os
import subprocess
import sys
import time
import threading
import tempfile
import signal
from pathlib import Path

PROJECT_ROOT = Path(__file__).parent.parent
RUNTIME_BIN  = PROJECT_ROOT / "build" / "runtime" / "ps1Runtime"
CONFIG       = PROJECT_ROOT / "configs" / "rayman.toml"

# VRAM analysis

def analyze_vram(ppm_path: str) -> dict:
    """Read a VRAM PPM dump and return region pixel counts + sample colors."""
    try:
        with open(ppm_path, "rb") as f:
            f.readline()          # P6
            dims = f.readline()   # width height
            f.readline()          # maxval
            data = bytearray(f.read())
        W = 1024
        def count_nonzero(ox, oy, w, h):
            n = 0
            for row in range(0, h, 2):
                for col in range(0, w, 2):
                    i = ((oy + row) * W + (ox + col)) * 3
                    if data[i] or data[i+1] or data[i+2]:
                        n += 1
            return n
        regions = {
            "fb0_x0_y0":   count_nonzero(0,   0,   320, 240),
            "fb1_x0_y256": count_nonzero(0,   256, 320, 240),
            "fb2_x320_y0": count_nonzero(320, 0,   320, 240),
            "tex_x640_y0": count_nonzero(640, 0,   384, 256),
        }
        # Sample a few pixels from the likely front buffer to judge content
        sample = []
        fb_x, fb_y = (0, 0)
        for (rx, ry) in [(fb_x+80, fb_y+60), (fb_x+160, fb_y+120), (fb_x+240, fb_y+180)]:
            i = (ry * W + rx) * 3
            sample.append((data[i], data[i+1], data[i+2]))
        return {"regions": regions, "sample_pixels": sample, "path": ppm_path}
    except Exception as e:
        return {"error": str(e)}

# Log parsing

def parse_log(stdout: str, stderr: str) -> dict:
    lines_out = stdout.splitlines()
    lines_err = stderr.splitlines()

    result = {
        "frame_status": [],
        "gpu_commands": [],
        "dispatch_nulls": 0,
        "cd_cb_count": 0,
        "dma3_count": 0,
        "dma2_count": 0,
        "vsync_calls": 0,
        "errors": [],
        "warnings": [],
        "vram_dump_path": None,    # early frame200 dump (may be absent)
        "vram_final_path": None,   # end-of-run dump (most useful)
    }

    for line in lines_out:
        if "I_STAT=" in line and "I_MASK=" in line:
            result["frame_status"].append(line.strip())
        if "Final VRAM dumped to" in line:
            # "[DEBUG] Final VRAM dumped to /tmp/vram_final.ppm (frame 1207)"
            parts = line.split("Final VRAM dumped to ")
            if len(parts) > 1:
                result["vram_final_path"] = parts[1].split(" (")[0].strip()
        elif "VRAM dumped to" in line:
            parts = line.split("to ")
            if len(parts) > 1:
                result["vram_dump_path"] = parts[1].strip()
        # GPU DMA Ch2 transfers are logged to stdout
        if "DMA] Ch2" in line or "DMA] LL" in line:
            result["dma2_count"] += 1
        if "DMA] Ch3" in line:
            result["dma3_count"] += 1

    for line in lines_err:
        if "LinkedList #" in line or "FillRect #" in line or "CPU->VRAM #" in line:
            result["gpu_commands"].append(line.strip())
        if "NULL function pointer" in line:
            result["dispatch_nulls"] += 1
        if "CD-CB" in line:
            result["cd_cb_count"] += 1
        if "VSYNC-DBG" in line:
            # "[VSYNC-DBG] VSync call #1200 (game loop active)"
            try:
                result["vsync_calls"] = int(line.split("#")[1].split()[0])
            except Exception:
                pass
        if "error" in line.lower() or "SEGFAULT" in line or "Segmentation" in line:
            result["errors"].append(line.strip())
        if "WARN" in line.upper():
            result["warnings"].append(line.strip())

    # Keep only last 20 GPU commands (most recent activity)
    result["gpu_commands"] = result["gpu_commands"][-20:]
    # Keep only last 5 frame status lines
    result["frame_status"] = result["frame_status"][-5:]

    return result

# Main

def main():
    parser = argparse.ArgumentParser(description="Run ps1Runtime and report game state")
    parser.add_argument("--duration", type=int, default=15,
                        help="Seconds to run the game (default 15)")
    parser.add_argument("--output", type=str, default=None,
                        help="Write JSON report to this file (also prints to stdout)")
    parser.add_argument("--bios-debug", action="store_true",
                        help="Enable PS1_BIOS_DEBUG (verbose BIOS logging)")
    args = parser.parse_args()

    if not RUNTIME_BIN.exists():
        print(json.dumps({"error": f"Binary not found: {RUNTIME_BIN}"}))
        sys.exit(1)
    if not CONFIG.exists():
        print(json.dumps({"error": f"Config not found: {CONFIG}"}))
        sys.exit(1)

    env = os.environ.copy()
    env["DISPLAY"] = env.get("DISPLAY", ":1")  # need a display for SDL
    if args.bios_debug:
        env["PS1_BIOS_DEBUG"] = "1"

    print(f"[run_and_report] Starting game for {args.duration}s...", file=sys.stderr)
    proc = subprocess.Popen(
        [str(RUNTIME_BIN), "--config", str(CONFIG)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        cwd=str(PROJECT_ROOT),
    )

    try:
        stdout_bytes, stderr_bytes = proc.communicate(timeout=args.duration)
    except subprocess.TimeoutExpired:
        proc.send_signal(signal.SIGTERM)
        try:
            stdout_bytes, stderr_bytes = proc.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout_bytes, stderr_bytes = proc.communicate()

    stdout = stdout_bytes.decode("utf-8", errors="replace")
    stderr = stderr_bytes.decode("utf-8", errors="replace")

    print(f"[run_and_report] Game exited (rc={proc.returncode})", file=sys.stderr)

    log = parse_log(stdout, stderr)

    vram_analysis = {}
    # Prefer the final VRAM dump (most representative of steady-state)
    final_candidates = [
        log.get("vram_final_path"),
        "/tmp/vram_final.ppm",
        log.get("vram_dump_path"),
        "/tmp/vram_frame200.ppm",
    ]
    for candidate in final_candidates:
        if candidate and os.path.exists(candidate):
            print(f"[run_and_report] Analysing VRAM dump at {candidate}...", file=sys.stderr)
            vram_analysis = analyze_vram(candidate)
            break

    report = {
        "duration_s": args.duration,
        "exit_code": proc.returncode,
        "log": log,
        "vram": vram_analysis,
        "summary": _summarise(log, vram_analysis, proc.returncode),
    }

    output = json.dumps(report, indent=2)
    print(output)

    if args.output:
        Path(args.output).write_text(output)
        print(f"[run_and_report] Report written to {args.output}", file=sys.stderr)


def _summarise(log: dict, vram: dict, rc: int) -> str:
    lines = []
    if rc == -11 or rc == 139:
        lines.append("CRASH: Segmentation fault (exit 139).")
    elif rc != 0 and rc != -15:
        lines.append(f"ABNORMAL EXIT: rc={rc}.")
    else:
        lines.append("Game ran and was terminated cleanly.")

    if log["dispatch_nulls"] > 0:
        lines.append(f"NULL dispatches: {log['dispatch_nulls']} (first 3 are benign GPU swap).")
    if log["errors"]:
        lines.append(f"Errors in log: {log['errors'][:3]}")

    gpu = log["gpu_commands"]
    dma2 = log["dma2_count"]
    vsync = log["vsync_calls"]
    if vsync > 0:
        lines.append(f"VSync calls: {vsync} total (game loop active at ~60fps).")
    if dma2 > 0:
        lines.append(f"GPU DMA Ch2 transfers: {dma2} total (game is rendering each frame).")
    if gpu:
        lines.append(f"Init GPU commands ({len(gpu)} in init window): last={gpu[-1]}")
    elif dma2 == 0:
        lines.append("NO GPU activity detected -- game may be stuck in loading/wait loop.")

    if log["dma3_count"] > 0:
        lines.append(f"CDROM DMA transfers: {log['dma3_count']} sectors loaded from disc.")
    if log["cd_cb_count"] > 0:
        lines.append(f"CDROM callbacks fired: {log['cd_cb_count']}.")

    fs = log["frame_status"]
    if fs:
        lines.append(f"Frame status (last): {fs[-1]}")
    else:
        lines.append("No frame status logged (game may not have reached frame 300).")

    if "regions" in vram:
        r = vram["regions"]
        path_label = vram.get("path", "VRAM")
        lines.append(
            f"VRAM final non-zero pixels ({path_label}): "
            f"fb0(x=0,y=0)={r.get('fb0_x0_y0',0)}, "
            f"fb1(x=0,y=256)={r.get('fb1_x0_y256',0)}, "
            f"fb2(x=320,y=0)={r.get('fb2_x320_y0',0)}, "
            f"textures(x=640)={r.get('tex_x640_y0',0)}"
        )

    return " | ".join(lines)


if __name__ == "__main__":
    main()
