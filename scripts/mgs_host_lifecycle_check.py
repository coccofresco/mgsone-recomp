#!/usr/bin/env python3
"""Isolated native shutdown/save-update checks; requires privately imported media.

No player profile is modified. The base config supplies media/render settings;
each run gets new card/state directories under a previously absent output path.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time
import tomllib

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def check(runtime, base_config, output, previous_runtime=None):
    output.mkdir(parents=True, exist_ok=False)
    template = base_config.read_text()
    settings = tomllib.loads(template)
    if not all(key in settings.get('paths', {}) for key in ('memcard_dir', 'savestate_dir')):
        raise ValueError('Base config must declare both save directories.')
    report = dict(schema=1, runtime_sha256=digest(runtime), cases=[],
                  scope='Native headless logic and SDL events; no Vulkan or physical Steam test.')

    def profile(name):
        directory = output / name
        for kind in ('memory_cards', 'savestates'):
            (directory / kind).mkdir(parents=True, exist_ok=True)
        config = template
        for key, folder in (('memcard_dir', 'memory_cards'), ('savestate_dir', 'savestates')):
            config = re.sub(r'(?m)^' + key + r'\s*=.*$', lambda _: key + ' = ' + json.dumps(str(directory / folder)), config)
        path = directory / 'runtime.toml'; path.write_text(config)
        return path

    def run(name, variables, config=None, binary=runtime, send_signal=None, require_clean=True, markers=()):
        config = config or profile(name)
        env = {k:v for k,v in os.environ.items() if not k.startswith(('PS1_', 'PS1RECOMP_'))}
        env.update(SDL_AUDIODRIVER='dummy', SDL_VIDEODRIVER='dummy',
                   PS1_DIAGNOSTIC_HEADLESS='1', PS1_VBLANK_PERIOD_US='1000',
                   MGS_CONTROLLER_CONFIG=str(config.parent / 'controllers.toml'), **variables)
        log = output / (name + '.log')
        started = time.monotonic(); sent = False
        with log.open('wb') as stream:
            process = subprocess.Popen([str(binary), '--config', str(config)], cwd=ROOT, env=env,
                                       stdout=stream, stderr=subprocess.STDOUT)
            try:
                deadline = started + 35
                while process.poll() is None:
                    if send_signal and not sent and '[Shutdown probe] READY' in log.read_text(errors='replace'):
                        process.send_signal(send_signal); sent = True
                    if time.monotonic() > deadline:
                        process.kill(); process.wait(); raise TimeoutError(name)
                    time.sleep(.02)
            finally:
                if process.poll() is None: process.kill(); process.wait()
        text = log.read_text(errors='replace')
        clean = re.search(r'\[Shutdown\] guest and ticker joined cleanly in (\d+) ms', text)
        passed = (process.returncode == 0 and (bool(clean) or not require_clean)
                  and (not send_signal or sent) and all(marker in text for marker in markers))
        result = dict(name=name, passed=passed, returncode=process.returncode,
                      elapsed_seconds=round(time.monotonic()-started, 3),
                      shutdown_ms=int(clean[1]) if clean else None, signal_sent=sent,
                      runtime_sha256=digest(binary), log=log.name)
        report['cases'].append(result)
        (output / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
        print(f'{name}: {"PASS" if passed else "FAIL"}, shutdown {result["shutdown_ms"]} ms', flush=True)
        if not passed: raise RuntimeError(f'{name} failed; see {log}')

    for frame in (15, 180, 1800):
        run(f'boot-{frame}', dict(PS1_EXIT_VBLANK=str(frame)))
    run('pause-save-load-resume', dict(PS1_HOST_UI_PROBE='1'), markers=('[Host probe] PASS',))
    for mode in ('window', 'menu'):
        run('quit-'+mode, dict(PS1_HOST_EXIT_PROBE=mode), markers=(f'[Shutdown probe] {mode} confirmation while parked',))
    for mode in ('signal-running', 'signal-paused'):
        for sig in (signal.SIGTERM, signal.SIGINT):
            run(mode+'-'+sig.name, dict(PS1_HOST_EXIT_PROBE=mode), send_signal=sig)

    if previous_runtime:
        report['previous_runtime_sha256'] = digest(previous_runtime)
        config = profile('update-profile')
        slot = config.parent / 'savestates/quick_slot_0.ps1s'
        run('previous-build-save', dict(PS1_AUTOSAVE_VBLANK='600', PS1_EXIT_AFTER_AUTOSAVE='1'),
            config, previous_runtime, require_clean=False, markers=('[SaveState] Saved to',))
        before = digest(slot)
        cards = {p.name: digest(p) for p in (config.parent / 'memory_cards').glob('*.mcd')}
        run('updated-build-load', dict(PS1_AUTOLOAD_VBLANK='180', PS1_EXIT_AFTER_LOAD_VBLANKS='60'),
            config, markers=('[SaveState] Loaded from', '[SaveState] guest dispatcher resumed'))
        if digest(slot) != before: raise RuntimeError('Loading changed the original state file')
        run('updated-build-save', dict(PS1_AUTOLOAD_VBLANK='180', PS1_AUTOSAVE_VBLANK='630', PS1_EXIT_AFTER_AUTOSAVE='1'),
            config, markers=('[SaveState] Loaded from', '[SaveState] Saved to'))
        backups = sorted((slot.parent / (slot.name+'.backups')).glob('*.bak'))
        if not any(digest(path) == before for path in backups): raise RuntimeError('Pre-update state not preserved')
        after = digest(slot)
        run('previous-build-load-new-state', dict(PS1_AUTOLOAD_VBLANK='180', PS1_EXIT_AFTER_LOAD_VBLANKS='60'),
            config, previous_runtime, require_clean=False, markers=('[SaveState] Loaded from',))
        if digest(slot) != after: raise RuntimeError('Old reader changed the new state')
        if cards != {p.name: digest(p) for p in (config.parent / 'memory_cards').glob('*.mcd')}:
            raise RuntimeError('Save-state update test rewrote memory cards')
        report['update'] = dict(format=12, previous_state_sha256=before, updated_state_sha256=after,
                                pre_update_backup_preserved=True, cards_unchanged=True,
                                previous_build_can_read_new_state=True)
    report['passed'] = True
    (output / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, default=ROOT/'build/ps1Runtime/ps1Runtime')
    parser.add_argument('--base-config', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--previous-runtime', type=Path)
    args = parser.parse_args()
    check(args.runtime.resolve(), args.base_config.resolve(), args.output.resolve(),
          args.previous_runtime.resolve() if args.previous_runtime else None)


if __name__ == '__main__':
    main()
