#!/usr/bin/env python3
"""Run a game and print the structured metric report.

Replaces log parsing as the source of truth.  Every capped log in this project
under-reports; the JSON written by ps1::metrics does not.

Needs a real display: see pick_video_driver below.

Usage:
    tools/run_report.py configs/crash.toml [seconds]
"""
import json
import os
import subprocess
import sys
import tempfile

INTERESTING_PREFIXES = ("gp0.op.", "draw_otag.", "gool.", "hle.libgpu",
                        "vram_dump.", "fill_rect", "cpu_to_vram")

# Video drivers under which a run's numbers cannot be trusted.  `dummy` has no
# Vulkan/WSI at all, so the runtime exits before the first frame. `offscreen`
# has no representative present pacing, so the main loop can free-run: six
# measured runs gave cpu_to_vram of ?, ?, 2, 2, 1, 973 and gool.interpret 0..26,
# against 1458 exactly and 298..300 on a vsynced display.  This tool exists
# because every capped log in this project under-reports -- defaulting it to a
# driver that silently reports a broken run would reintroduce that same failure.
DEGRADED_DRIVERS = ("offscreen", "dummy")


def pick_video_driver():
    """Return the SDL_VIDEODRIVER to run under, or False when there is no
    display.

    Derived from the session rather than hardcoded, so an X11-only box gets
    x11 and a Wayland one gets wayland; an explicit SDL_VIDEODRIVER always
    wins, with a warning when it names a driver known to degrade the run.

    Deriving beats leaving the variable unset: with no override SDL picks x11
    even in a Wayland session whenever DISPLAY points at XWayland (verified
    here -- the runtime connects to @/tmp/.X11-unix/X1), so "let SDL choose"
    silently measures a different backend than the one the audit numbers were
    taken on.  Refuse outright when neither variable is set instead of
    printing numbers from a boot that never rendered; same signal as
    ps1Test/runtime/test_golden_frame.cpp, which skips on it.
    """
    explicit = os.environ.get("SDL_VIDEODRIVER")
    if explicit:
        if explicit in DEGRADED_DRIVERS:
            print(f"warning: SDL_VIDEODRIVER={explicit} degrades the boot; "
                  "counters below are not comparable to a real run.",
                  file=sys.stderr)
        return explicit
    if os.environ.get("WAYLAND_DISPLAY"):
        return "wayland"
    if os.environ.get("DISPLAY"):
        return "x11"
    print("no display: set WAYLAND_DISPLAY or DISPLAY and run again. A "
          "headless run boots degraded and reports numbers that look real "
          "but are not (see DEGRADED_DRIVERS in this file). To measure one "
          "anyway, set SDL_VIDEODRIVER explicitly.", file=sys.stderr)
    return False


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    config = sys.argv[1]
    seconds = sys.argv[2] if len(sys.argv) > 2 else "15"

    metrics_path = os.path.join(tempfile.gettempdir(), "ps1_run_report.json")
    if os.path.exists(metrics_path):
        os.remove(metrics_path)

    driver = pick_video_driver()
    if driver is False:
        return 1

    env = dict(os.environ)
    env.update(PS1_METRICS=metrics_path, SDL_AUDIODRIVER="dummy",
               SDL_VIDEODRIVER=driver)
    cmd = [
        "timeout", "--kill-after=5", seconds,
        "./build/ps1Runtime/ps1Runtime", "--config", config,
    ]
    subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, check=False)

    if not os.path.exists(metrics_path):
        print(f"no metric report written under SDL_VIDEODRIVER={driver} -- "
              "did the runtime reach a shutdown path? If this SDL build has "
              "no such driver, set SDL_VIDEODRIVER to one it does have.",
              file=sys.stderr)
        return 1

    with open(metrics_path) as fh:
        report = json.load(fh)

    counters = report.get("counters", {})
    state = report.get("state", {})

    print(f"frames: {counters.get('frames', 0)}")
    print("\ncounters:")
    for key in sorted(counters):
        if key == "frames":
            continue
        if key.startswith(INTERESTING_PREFIXES):
            print(f"  {key:<32} {counters[key]}")
    print("\nstate:")
    for key in sorted(state):
        print(f"  {key:<32} {state[key]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
