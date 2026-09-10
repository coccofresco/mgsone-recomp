#!/usr/bin/env python3
"""Run the USA intro and check native coverage plus captured display content."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--headless', action='store_true')
    parser.add_argument('--out', type=Path, default=ROOT / 'data/native/smoke')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    # This check must measure real NTSC pacing, not an accelerated diagnostic.
    env.pop('PS1_VBLANK_PERIOD_US', None)
    env['PS1_DIAGNOSTIC_HEADLESS'] = '1' if args.headless else '0'
    env.update(SDL_AUDIODRIVER='dummy', PS1_EXIT_VBLANK='3000',
               PS1_METRICS=str(out / 'metrics.json'),
               PS1_NATIVE_DISPLAY_DUMP_DIRECTORY=str(out / 'frames'),
               PS1_NATIVE_DISPLAY_DUMP_FROM='500',
               PS1_NATIVE_DISPLAY_DUMP_TO='3000',
               PS1_NATIVE_DISPLAY_DUMP_STEP='500')
    started_wall = time.time()
    start = time.monotonic()
    with (out / 'runtime.log').open('w') as log:
        subprocess.run([str(ROOT / 'build/ps1Runtime/ps1Runtime'), '--config',
                        str(ROOT / 'data/mgs.usa.native.toml')], cwd=ROOT, env=env,
                       stdout=log, stderr=subprocess.STDOUT, timeout=75, check=True)
    assert (out / 'metrics.json').stat().st_mtime >= started_wall, 'Stale metrics'
    metrics = json.loads((out / 'metrics.json').read_text())
    counters = metrics['counters']
    assert counters.get('timing.vblank') == 3000, 'Incomplete boot probe'
    interpreter = {k: v for k, v in counters.items() if k.startswith('mips.') and v}
    assert not interpreter, f'Interpreter fallback used: {interpreter}'
    assert counters.get('bios.thread_switch', 0) > 100, 'Guest kernel did not run'
    assert counters.get('gp0.op.3C', 0) > 100, 'Intro geometry not submitted'
    elapsed = metrics['state']['host.elapsed_us'] / 1_000_000
    assert 49 <= elapsed <= 65, f'Unexpected NTSC pacing: {elapsed}'
    visible = []
    for frame in sorted((out / 'frames').glob('frame-*.ppm')):
        if frame.stat().st_mtime < started_wall:
            continue
        with frame.open('rb') as image:
            assert image.readline().strip() == b'P6'
            width, height = map(int, image.readline().split())
            assert image.readline().strip() == b'255'
            pixels = image.read()
        assert len(pixels) == width * height * 3
        if sum(value > 16 for value in pixels) > 1000:
            visible.append(frame.name)
    assert len(visible) >= 3, f'Insufficient visible frames: {visible}'
    result = {'native_boot': True, 'vblanks': 3000, 'guest_elapsed_seconds': elapsed,
              'wall_seconds': round(time.monotonic() - start, 3),
              'interpreter_fallbacks': interpreter, 'visible_frames': visible,
              'presentation': 'headless' if args.headless else 'vulkan',
              'scope': 'Initial logos and opening overlay; not a full gameplay test'}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
