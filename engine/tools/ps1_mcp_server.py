#!/usr/bin/env python3
"""
PS1-Recomp MCP Server

Exposes tools for autonomous debugging of the ps1-recomp emulator.
Claude can run the game, inspect VRAM, read RAM, and iterate without human
involvement.

Registered in .mcp.json at the project root.
"""

import base64
import json
import os
import re
import signal
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path
from typing import Optional

from mcp.server.fastmcp import FastMCP

PROJECT_ROOT   = Path(__file__).parent.parent
RUNTIME_BIN    = PROJECT_ROOT / "build" / "ps1Runtime" / "ps1Runtime"
CONFIG_DIR     = PROJECT_ROOT / "configs"
TOOLS_DIR      = PROJECT_ROOT / "tools"
RECOMPILED_SRC = PROJECT_ROOT / "ps1Runtime" / "src" / "recompiled_out.cpp"

mcp = FastMCP("ps1-recomp")


# Code index (lazy, cached)

_code_index: Optional[dict] = None   # addr_hex -> line_number (1-based)
_code_lines: Optional[list] = None   # all lines of recompiled_out.cpp


def _ensure_index():
    global _code_index, _code_lines
    if _code_index is not None:
        return
    if not RECOMPILED_SRC.exists():
        _code_index = {}
        _code_lines = []
        return
    _code_lines = RECOMPILED_SRC.read_text(errors="replace").splitlines()
    _code_index = {}
    # Match: "void func_801XXXXX(uint8_t* rdram, recomp_context* ctx)"
    pat = re.compile(r'^void (func_[0-9A-Fa-f]{8})\s*\(')
    for i, line in enumerate(_code_lines):
        m = pat.match(line)
        if m:
            name = m.group(1)
            # Extract hex address from name (func_801B954C -> 801B954C)
            addr = name[5:].upper()
            _code_index[addr] = i  # 0-based line index


def _get_function_body(start_line: int, max_lines: int = 200) -> list[str]:
    """Return lines of the function starting at start_line (0-based)."""
    lines = _code_lines
    result = [lines[start_line]]
    depth = lines[start_line].count("{") - lines[start_line].count("}")
    i = start_line + 1
    while i < len(lines) and (depth > 0 or i == start_line + 1):
        result.append(lines[i])
        depth += lines[i].count("{") - lines[i].count("}")
        i += 1
        if len(result) >= max_lines:
            result.append(f"    // ... [{len(lines) - i} more lines truncated]")
            break
    return result


# helpers

def _analyze_vram(ppm_path: str) -> dict:
    """Read a VRAM PPM dump and return region pixel counts + color histogram."""
    try:
        with open(ppm_path, "rb") as f:
            f.readline()        # P6
            f.readline()        # width height
            f.readline()        # maxval
            data = bytearray(f.read())
        W = 1024

        def count_nonzero(ox, oy, w, h):
            n = 0
            for row in range(0, h, 2):
                for col in range(0, w, 2):
                    i = ((oy + row) * W + (ox + col)) * 3
                    if data[i] or data[i + 1] or data[i + 2]:
                        n += 1
            return n

        # Color histogram for the display framebuffer (fb0 at x=0,y=0)
        color_counts = Counter()
        for row in range(0, 240, 4):
            for col in range(0, 320, 4):
                i = (row * W + col) * 3
                rgb = (data[i], data[i+1], data[i+2])
                if any(rgb):
                    color_counts[rgb] += 1
        top_colors = [{"rgb": list(c), "count": n}
                      for c, n in color_counts.most_common(10)]

        max_brightness = max(
            (max(data[i], data[i+1], data[i+2])
             for i in range(0, min(len(data), 320 * 240 * 3), 3)),
            default=0,
        )

        return {
            "path": ppm_path,
            "regions": {
                "fb0_x0_y0":   count_nonzero(0,   0,   320, 240),
                "fb1_x0_y256": count_nonzero(0,   256, 320, 240),
                "fb2_x320_y0": count_nonzero(320, 0,   320, 240),
                "tex_x640_y0": count_nonzero(640, 0,   384, 256),
            },
            "top_colors_fb0": top_colors,
            "max_brightness_fb0": max_brightness,
        }
    except Exception as e:
        return {"error": str(e)}


def _parse_log(stdout: str, stderr: str) -> dict:
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
        "vram_dump_path": None,
        "vram_final_path": None,
    }
    for line in stdout.splitlines():
        if "I_STAT=" in line and "I_MASK=" in line:
            result["frame_status"].append(line.strip())
        if "Final VRAM dumped to" in line:
            parts = line.split("Final VRAM dumped to ")
            if len(parts) > 1:
                result["vram_final_path"] = parts[1].split(" (")[0].strip()
        elif "VRAM dumped to" in line:
            parts = line.split("to ")
            if len(parts) > 1:
                result["vram_dump_path"] = parts[1].strip()
        if "DMA] Ch2" in line or "DMA] LL" in line:
            result["dma2_count"] += 1
        if "DMA] Ch3" in line:
            result["dma3_count"] += 1

    for line in stderr.splitlines():
        if "LinkedList #" in line or "FillRect #" in line or "CPU->VRAM #" in line:
            result["gpu_commands"].append(line.strip())
        if "NULL function pointer" in line:
            result["dispatch_nulls"] += 1
        if "CD-CB" in line:
            result["cd_cb_count"] += 1
        if "VSYNC-DBG" in line:
            try:
                result["vsync_calls"] = int(line.split("#")[1].split()[0])
            except Exception:
                pass
        if "error" in line.lower() or "SEGFAULT" in line or "Segmentation" in line:
            result["errors"].append(line.strip())
        if "WARN" in line.upper():
            result["warnings"].append(line.strip())

    result["gpu_commands"] = result["gpu_commands"][-20:]
    result["frame_status"] = result["frame_status"][-5:]
    return result


def _run_game(config: str, duration: int, bios_debug: bool) -> dict:
    config_path = CONFIG_DIR / config if not config.startswith("/") else Path(config)
    if not RUNTIME_BIN.exists():
        return {"error": f"Binary not found: {RUNTIME_BIN}"}
    if not config_path.exists():
        return {"error": f"Config not found: {config_path}"}

    env = os.environ.copy()
    # Use Wayland if the compositor socket exists, else fall back to X11 at :1.
    # SDL_AUDIODRIVER=dummy avoids blocking on audio init in headless envs.
    import os as _os
    wayland_sock = f"/run/user/{_os.getuid()}/wayland-1"
    if _os.path.exists(wayland_sock):
        env["WAYLAND_DISPLAY"] = "wayland-1"
        env["SDL_VIDEODRIVER"] = "wayland"
    else:
        env.setdefault("DISPLAY", ":1")
    env["SDL_AUDIODRIVER"] = "dummy"
    if bios_debug:
        env["PS1_BIOS_DEBUG"] = "1"

    proc = subprocess.Popen(
        [str(RUNTIME_BIN), "--config", str(config_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        cwd=str(PROJECT_ROOT),
    )
    try:
        stdout_b, stderr_b = proc.communicate(timeout=duration)
    except subprocess.TimeoutExpired:
        proc.send_signal(signal.SIGTERM)
        try:
            stdout_b, stderr_b = proc.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout_b, stderr_b = proc.communicate()

    stdout = stdout_b.decode("utf-8", errors="replace")
    stderr = stderr_b.decode("utf-8", errors="replace")
    log = _parse_log(stdout, stderr)

    # Prefer final dump, then fallback
    vram_analysis = {}
    for candidate in [log.get("vram_final_path"), "/tmp/vram_final.ppm",
                       log.get("vram_dump_path"), "/tmp/vram_frame200.ppm"]:
        if candidate and os.path.exists(candidate):
            vram_analysis = _analyze_vram(candidate)
            break

    return {
        "exit_code": proc.returncode,
        "duration_s": duration,
        "log": log,
        "vram": vram_analysis,
    }


# MCP Tools

@mcp.tool()
def run_game(duration: int = 20, config: str = "rayman.toml", bios_debug: bool = False) -> str:
    """
    Run the ps1-recomp game for `duration` seconds and return a JSON report.

    The report includes:
    - exit_code, frame_status (I_STAT/I_MASK every 300 frames)
    - vsync_calls (total VSync invocations -- confirms game loop alive at 60fps)
    - dma2_count (GPU DMA Ch2 transfers -- confirms rendering activity)
    - dma3_count (CDROM DMA sectors loaded)
    - vram.regions (non-zero pixel counts in fb0, fb1, fb2, texture area)
    - vram.top_colors_fb0 (dominant colors in display framebuffer)
    - vram.max_brightness_fb0 (0 = black screen, 248+ = full content)
    - gpu_commands (init-phase draw commands like FillRect/LinkedList)
    - errors / warnings from stderr

    Use duration=15 for quick checks, 30 for deeper observation.
    """
    result = _run_game(config, duration, bios_debug)
    return json.dumps(result, indent=2)


@mcp.tool()
def get_vram_image(as_base64: bool = False) -> str:
    """
    Return the latest VRAM PPM dump as pixel analysis.

    If as_base64=True, also returns the raw PPM bytes as base64 so you can
    inspect the image. The PPM is 1024x512 RGB, representing the full PS1 VRAM.
    Display framebuffer is at (0,0) 320x240; second framebuffer at (0,256).
    """
    candidates = ["/tmp/vram_final.ppm", "/tmp/vram_frame200.ppm"]
    for path in candidates:
        if os.path.exists(path):
            analysis = _analyze_vram(path)
            if as_base64:
                with open(path, "rb") as f:
                    analysis["ppm_base64"] = base64.b64encode(f.read()).decode()
            return json.dumps(analysis, indent=2)
    return json.dumps({"error": "No VRAM dump found. Run the game first."})


@mcp.tool()
def build(target: str = "") -> str:
    """
    Build the project with cmake.

    target: optional cmake target (e.g. "ps1Runtime_tests"). Empty = build all.
    Returns stdout+stderr of the build command.
    """
    cmd = ["cmake", "--build", "build", f"-j{os.cpu_count() or 4}"]
    if target:
        cmd += ["--target", target]
    result = subprocess.run(
        cmd, capture_output=True, text=True, cwd=str(PROJECT_ROOT)
    )
    output = result.stdout + result.stderr
    # Keep last 100 lines to avoid flooding context
    lines = output.splitlines()[-100:]
    return "\n".join(lines) + f"\n[exit {result.returncode}]"


@mcp.tool()
def run_tests(filter: str = "") -> str:
    """
    Run the test suite with ctest.

    filter: optional regex to select specific tests (e.g. "SaveState").
    Returns test results.
    """
    cmd = ["ctest", "--output-on-failure", "-j4"]
    if filter:
        cmd += ["-R", filter]
    result = subprocess.run(
        cmd, capture_output=True, text=True,
        cwd=str(PROJECT_ROOT / "build")
    )
    output = result.stdout + result.stderr
    lines = output.splitlines()[-80:]
    return "\n".join(lines) + f"\n[exit {result.returncode}]"


@mcp.tool()
def read_log(log_file: str = "stderr", tail_lines: int = 100) -> str:
    """
    Read a captured log file from /tmp.

    log_file: one of "stderr", "stdout", "freeze_diag", "gpu_diag",
              or a full /tmp/*.log path.
    tail_lines: how many lines from the end to return.
    """
    mapping = {
        "stderr":      "/tmp/stderr.log",
        "stdout":      "/tmp/vsync_stdout.log",
        "freeze_diag": "/tmp/freeze_diag.log",
        "gpu_diag":    "/tmp/gpu_diag.log",
    }
    path = mapping.get(log_file, log_file if log_file.startswith("/tmp/") else f"/tmp/{log_file}")
    try:
        lines = Path(path).read_text(errors="replace").splitlines()
        return "\n".join(lines[-tail_lines:])
    except FileNotFoundError:
        return f"Log file not found: {path}"


@mcp.tool()
def read_source(file_path: str, start_line: int = 1, end_line: int = 100) -> str:
    """
    Read lines from a source file in the project.

    file_path: relative to project root (e.g. "runtime/src/gpu/gpu.cpp")
    start_line / end_line: 1-based line range to return.
    """
    full = PROJECT_ROOT / file_path
    try:
        lines = full.read_text(errors="replace").splitlines()
        chunk = lines[start_line - 1 : end_line]
        return "\n".join(f"{start_line + i:5}: {l}" for i, l in enumerate(chunk))
    except FileNotFoundError:
        return f"File not found: {full}"


# Ghidra-equivalent: code analysis tools

@mcp.tool()
def decompile_function(address: str, max_lines: int = 150) -> str:
    """
    Show the decompiled C body of a PS1 function by address.

    Equivalent to Ghidra's "Decompile" view. Uses the already-recompiled
    rayman_recompiled.cpp as the source of truth.

    address: PS1 address in hex, with or without 0x prefix
              (e.g. "0x801B954C" or "801B954C")
    max_lines: cap result at this many lines to avoid flooding context.

    Returns the function signature + body. If not found, lists nearby addresses.
    """
    _ensure_index()
    if not _code_lines:
        return "recompiled_out.cpp not found. Run patch_and_build first."

    addr = address.upper().replace("0X", "").lstrip("0") or "0"
    # Pad to 8 chars
    addr8 = addr.zfill(8)

    if addr8 not in _code_index:
        # Suggest nearby
        close = sorted(
            k for k in _code_index if abs(int(k, 16) - int(addr8, 16)) < 0x10000
        )[:10]
        return (
            f"Function 0x{addr8} not found in recompiled_out.cpp.\n"
            f"Nearby functions: {['0x' + a for a in close]}"
        )

    start = _code_index[addr8]
    body = _get_function_body(start, max_lines)
    header = f"// recompiled_out.cpp line {start + 1}  (PS1 addr 0x{addr8})\n"
    return header + "\n".join(body)


@mcp.tool()
def find_callers(address: str, max_results: int = 30) -> str:
    """
    Find all functions that call the given PS1 address (cross-references TO).

    Equivalent to Ghidra's "References To" panel.
    Searches for recomp_dispatch / CALL_INDIRECT / direct function calls.

    address: PS1 address (e.g. "0x801B954C")
    """
    _ensure_index()
    if not _code_lines:
        return "recompiled_out.cpp not found."

    addr = address.upper().replace("0X", "")
    # Patterns that reference this address in the recompiled C
    patterns = [
        re.compile(rf'\b{addr}\b', re.IGNORECASE),
        re.compile(rf'0x{addr}\b', re.IGNORECASE),
    ]

    results = []
    current_func = None
    func_pat = re.compile(r'^void (func_[0-9A-Fa-f]{8})\s*\(')

    for i, line in enumerate(_code_lines):
        m = func_pat.match(line)
        if m:
            current_func = (m.group(1), i + 1)
        if any(p.search(line) for p in patterns):
            if current_func:
                results.append({
                    "caller": current_func[0],
                    "caller_line": current_func[1],
                    "ref_line": i + 1,
                    "code": line.strip(),
                })
            if len(results) >= max_results:
                break

    if not results:
        return f"No callers found for 0x{addr} in recompiled_out.cpp."
    out = [f"Callers of 0x{addr} ({len(results)} found):"]
    for r in results:
        out.append(
            f"  {r['caller']} (line {r['caller_line']}) -> line {r['ref_line']}: {r['code']}"
        )
    return "\n".join(out)


@mcp.tool()
def find_callees(address: str, max_results: int = 50) -> str:
    """
    Find all functions/addresses called FROM the given PS1 function.

    Equivalent to Ghidra's "References From" panel.

    address: PS1 address of the function to inspect.
    """
    _ensure_index()
    if not _code_lines:
        return "recompiled_out.cpp not found."

    addr = address.upper().replace("0X", "").zfill(8)
    if addr not in _code_index:
        return f"Function 0x{addr} not found."

    start = _code_index[addr]
    body = _get_function_body(start, max_lines=500)

    # Find all 0x8........ references inside the body
    addr_pat = re.compile(r'0x(8[0-9A-Fa-f]{7})\b')
    func_pat  = re.compile(r'\b(func_[0-9A-Fa-f]{8})\b')

    refs = set()
    for line in body:
        for m in addr_pat.finditer(line):
            refs.add("0x" + m.group(1).upper())
        for m in func_pat.finditer(line):
            refs.add(m.group(1))

    if not refs:
        return f"No outgoing references found in func_80{addr[2:]}."

    # Annotate which ones are known functions
    known = {("func_" + k) for k in _code_index}
    out = [f"References from 0x{addr} ({len(refs)} unique):"]
    for r in sorted(refs):
        tag = " [known function]" if r in known or r.replace("0x", "func_").upper() in known else ""
        out.append(f"  {r}{tag}")
    return "\n".join(out[:max_results])


@mcp.tool()
def search_functions(pattern: str, max_results: int = 30) -> str:
    """
    Search for functions in the recompiled code by name pattern or hex address range.

    Examples:
      search_functions("801B9")          -> all funcs starting with 801B9...
      search_functions("DrawSync")       -> matches if any line in func contains that
      search_functions("801A9000-801AAFF") -> funcs in address range

    Returns list of matching function names + line numbers.
    """
    _ensure_index()
    if not _code_index:
        return "recompiled_out.cpp not found or index empty."

    # Range search: "0x801A9000-0x801AAFF"
    range_m = re.match(r'(?:0x)?([0-9A-Fa-f]+)\s*[--]\s*(?:0x)?([0-9A-Fa-f]+)', pattern)
    if range_m:
        lo = int(range_m.group(1), 16)
        hi = int(range_m.group(2), 16)
        results = [
            f"  func_{addr} (line {ln + 1})"
            for addr, ln in sorted(_code_index.items())
            if lo <= int(addr, 16) <= hi
        ]
        if not results:
            return f"No functions in range 0x{lo:08X}-0x{hi:08X}."
        return f"Functions in range ({len(results)}):\n" + "\n".join(results[:max_results])

    # Prefix / substring search on address
    pat_upper = pattern.upper().replace("0X", "")
    addr_matches = [
        f"  func_{addr} (line {ln + 1})"
        for addr, ln in sorted(_code_index.items())
        if pat_upper in addr
    ]

    # Content search inside function bodies
    content_matches = []
    if len(pat_upper) > 3:
        current_func = None
        matched_funcs = set()
        for i, line in enumerate(_code_lines):
            m = re.match(r'^void (func_([0-9A-Fa-f]{8}))\s*\(', line)
            if m:
                current_func = (m.group(1), m.group(2), i + 1)
            if current_func and current_func[1] not in matched_funcs:
                if pattern.lower() in line.lower():
                    content_matches.append(
                        f"  {current_func[0]} (line {current_func[2]}): {line.strip()[:80]}"
                    )
                    matched_funcs.add(current_func[1])

    out = []
    if addr_matches:
        out.append(f"Address matches ({len(addr_matches)}):")
        out += addr_matches[:max_results]
    if content_matches:
        out.append(f"\nContent matches ({len(content_matches)}):")
        out += content_matches[:max_results]
    if not out:
        return f"No functions found matching '{pattern}'."
    return "\n".join(out)


@mcp.tool()
def search_code(pattern: str, context_lines: int = 2, max_matches: int = 20) -> str:
    """
    Search the entire recompiled code for a regex pattern (like grep).

    Useful for finding all places that access a specific memory address,
    call a specific HLE function, or use a specific constant.

    Examples:
      search_code("0x801CF2CC")        -> find all refs to vblankCounter
      search_code("drainPendingCallbacks")
      search_code("JUMP_INDIRECT")

    context_lines: lines of context around each match (0-5).
    """
    _ensure_index()
    if not _code_lines:
        return "recompiled_out.cpp not found."

    try:
        pat = re.compile(pattern, re.IGNORECASE)
    except re.error as e:
        return f"Invalid regex: {e}"

    matches = []
    for i, line in enumerate(_code_lines):
        if pat.search(line):
            start = max(0, i - context_lines)
            end   = min(len(_code_lines), i + context_lines + 1)
            block = [f"  {j+1:6}: {_code_lines[j]}" for j in range(start, end)]
            matches.append(f"--- line {i+1} ---\n" + "\n".join(block))
            if len(matches) >= max_matches:
                matches.append(f"[truncated -- {max_matches} matches shown]")
                break

    if not matches:
        return f"No matches for '{pattern}' in recompiled_out.cpp."
    return f"{len(matches)} match(es) for '{pattern}':\n\n" + "\n\n".join(matches)


@mcp.tool()
def list_functions(start_addr: str = "80000000", count: int = 50) -> str:
    """
    List functions in address order starting from start_addr.

    Useful for exploring what functions exist in a given area of PS1 memory.

    start_addr: PS1 hex address to start from (e.g. "801A9000")
    count: how many functions to list
    """
    _ensure_index()
    if not _code_index:
        return "recompiled_out.cpp not found or index empty."

    lo = int(start_addr.upper().replace("0X", ""), 16)
    funcs = sorted(
        ((int(addr, 16), addr, ln) for addr, ln in _code_index.items()),
        key=lambda x: x[0],
    )
    results = [(addr, ln) for v, addr, ln in funcs if v >= lo][:count]
    if not results:
        return f"No functions found at or after 0x{lo:08X}."
    return "\n".join(f"  func_{addr}  (recompiled_out.cpp line {ln+1})" for addr, ln in results)


if __name__ == "__main__":
    mcp.run(transport="stdio")
