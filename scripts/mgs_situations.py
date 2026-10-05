#!/usr/bin/env python3
"""Fork matched scenario checkpoints into short, input-only differential checks."""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

from mgs_campaign import terminal_checkpoint
from mgs_scenarios import compare
from mgs_tas import ROOT, sha256, write_json
from mgs_tas_route import check, compile_route, snapshot

PROBES = ('pause', 'codec', 'items', 'weapons', 'first-person', 'quick-item', 'quick-weapon')
PRESERVE = ['stage', 'story', 'difficulty', 'disc', 'life', 'max_life',
            'weapon', 'item', 'weapons', 'items', 'x', 'y', 'z', 'game_over_timer']


def builtin(name, initial):
    """Goals follow retail libgv.h, g_define.h, gamed.c and menu/*.c."""
    ready = {k: initial[k] for k in ('stage', 'story', 'difficulty', 'disc')}
    ready.update(pause_level=0, control={'ne': 0}, life={'min': 1}, game_over_timer=0,
                 game_status={'mask': 0xdc080000, 'eq': 0},
                 player_status={'mask': 0x20000000, 'eq': 0})
    safe = {k: initial[k] for k in ('stage', 'story', 'difficulty', 'disc')}
    safe.update(life={'min': 1}, game_over_timer=0)
    requires = dict(ready)
    if name in ('pause', 'codec', 'items', 'weapons'):
        button, bit = {'pause': ('start', 2), 'codec': ('select', 1),
                       'items': ('l2', 4), 'weapons': ('r2', 4)}[name]
        opened = dict(pause_level={'mask': bit, 'eq': bit})
        closed = dict(pause_level=0)
        release = f'wait:60,{button}:6,wait:60' if name in ('pause', 'codec') else None
        if name == 'pause':
            requires['game_status'] = {'mask': 0xdc084020, 'eq': 0}
        elif name == 'codec':
            requires['game_status'] = {'mask': 0xdc082420, 'eq': 0}
            requires['player_status'] = {'mask': 0x20008000, 'eq': 0}
        else:
            requires['game_status'] = {'mask': 0xdc080400, 'eq': 0}
            requires['player_status'] = {'mask': 0x20008000 | (0x200000 if name == 'items' else 0x400000), 'eq': 0}
    elif name == 'first-person':
        requires['player_status'] = {'mask': 0x20000001, 'eq': 0}
        button = 'triangle'
        opened = dict(player_status={'mask': 1, 'eq': 1})
        closed = dict(player_status={'mask': 1, 'eq': 0}, pause_level=0)
        release = None
    elif name in ('quick-item', 'quick-weapon'):
        field, button = ('item', 'l1') if name == 'quick-item' else ('weapon', 'r1')
        requires[field] = {'min': 0}
        requires['game_status'] = {'mask': 0xdc0c0400, 'eq': 0}
        requires['player_status'] = {'mask': 0x20008000 | (0x200000 if field == 'item' else 0x400000), 'eq': 0}
        opened = {field: -1}
        closed = {field: initial[field], 'pause_level': 0}
        release = f'wait:30,{button}:6,wait:30'
    else:
        raise ValueError(f'unknown situation: {name}')
    enter = dict(name='enter-' + name, until=opened, guard=safe,
                 action=dict(sequence=f'wait:12,{button}:90,wait:30'),
                 attempts=3, max_frames=600)
    leave = dict(name='leave-' + name, until=closed, guard=safe, max_frames=900)
    if release:
        leave.update(action=dict(sequence=release), attempts=6)
    settle = dict(name='control-restored', until=ready, guard=safe,
                  stable_frames=30, max_frames=600)
    return dict(format='mgs-situation-1', name=name, requires=requires,
                preserve=PRESERVE, compare=PRESERVE + ['pause_level', 'stance'],
                route=dict(format='mgs-tas-route-1', steps=[enter, leave, settle]))


def read(path):
    return json.loads(path.read_text())


def fingerprint(path):
    return dict(path=str(path.resolve()), sha256=sha256(path))


def assess(recipe, initial, final, execution_ok):
    differences = {k: dict(before=initial.get(k), after=final.get(k))
                   for k in recipe['preserve'] if initial.get(k) != final.get(k)}
    terminal = recipe['route']['steps'][-1]['until']
    goal = check(terminal, final)
    return dict(status='complete' if execution_ok and goal and not differences else 'failed',
                execution_ok=execution_ok, final_goal_satisfied=goal,
                invariant_changes=differences, final_state=final)


def execute(command, log, timeout, env=None):
    with log.open('w') as stream:
        try:
            return subprocess.run(command, cwd=ROOT, env=env, stdout=stream,
                                  stderr=subprocess.STDOUT, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            return 124


def run_matrix(argv):
    started = time.monotonic()
    parser = argparse.ArgumentParser(description='Independent situations on each distinct stage in scenario evidence')
    parser.add_argument('evidence', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--probes', nargs='+', choices=PROBES,
                        default=['pause', 'codec', 'items', 'weapons', 'first-person'])
    parser.add_argument('--jobs', type=int, choices=(1, 2), default=2)
    args = parser.parse_args(argv)
    evidence = read(args.evidence)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    selected, seen = [], set()
    for case in evidence['scenarios']:
        if case['stage'] in seen:
            continue
        seen.add(case['stage'])
        for probe in dict.fromkeys(args.probes):
            selected.append((case, probe))
    if not selected:
        raise ValueError('no scenario seeds in evidence')
    report = dict(format='mgs-situation-matrix-1', status='running',
                  seed_evidence=fingerprint(args.evidence), results=[])
    def one(pair):
        case, probe = pair
        name = f'{case["stage"]}-{probe}'
        if not all(c.isalnum() or c in '-_' for c in name):
            raise ValueError('invalid stage identifier')
        paths = [case[key]['path'] if isinstance(case[key], dict) else case[key]
                 for key in ('native_result', 'donor_result')]
        command = [sys.executable, str(Path(__file__).resolve()),
                   *(str(ROOT / path) for path in paths), str(output / name), '--probe', probe]
        code = execute(command, output / (name + '.log'), 240)
        result = output / name / 'result.json'
        return dict(stage=case['stage'], probe=probe, returncode=code,
                    status=read(result).get('status', 'needs-review') if result.exists() else 'needs-review',
                    result=fingerprint(result) if result.exists() else None)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for future in concurrent.futures.as_completed([pool.submit(one, pair) for pair in selected]):
            row = future.result()
            report['results'].append(row)
            write_json(output / 'result.json', report)
            print(f"{row['stage']} {row['probe']}: {row['status']}", flush=True)
    report['counts'] = {status: sum(row['status'] == status for row in report['results'])
                        for status in ('complete', 'not-applicable', 'needs-review')}
    report['status'] = ('needs-review' if report['counts']['needs-review'] else
                        'complete' if report['counts']['complete'] else 'not-applicable')
    report['elapsed_seconds'] = round(time.monotonic() - started, 3)
    write_json(output / 'result.json', report)
    return 0 if report['status'] == 'complete' else 1


def run(args):
    started = time.monotonic()
    native_path, donor_path = args.native.resolve(), args.donor.resolve()
    native, donor = read(native_path), read(donor_path)
    if compare(native, donor)['status'] != 'complete':
        raise ValueError('seed scenarios must have passed the same load/control contract')
    parents = {'native': native, 'donor': donor}
    sessions = {engine: Path(report['session']) for engine, report in parents.items()}
    initial = {}
    for engine, session in sessions.items():
        ram = session / 'final-ram.bin'
        if sha256(ram) != parents[engine]['final_ram_sha256']:
            raise ValueError(f'{engine} seed RAM changed')
        initial[engine] = snapshot(ram.read_bytes())
    recipe = read(args.recipe) if args.recipe else builtin(args.probe, initial['native'])
    if recipe.get('format') != 'mgs-situation-1' or not recipe.get('requires'):
        raise ValueError('situation requires explicit initial conditions')
    for key in ('preserve', 'compare'):
        if not isinstance(recipe.get(key), list) or not recipe[key]:
            raise ValueError(f'situation requires explicit {key} fields')
        if any(field not in initial['native'] for field in recipe[key]):
            raise ValueError(f'unknown {key} field')
    program = compile_route(recipe['route'], [], {})
    observations = set(recipe['requires'])
    for step in recipe['route']['steps']:
        for key in ('until', 'when', 'guard', 'abort_when'):
            observations.update(step.get(key, {}))
    for engine, session in sessions.items():
        initial[engine] = snapshot((session / 'final-ram.bin').read_bytes(), observations)
    eligibility = {engine: check(recipe['requires'], state) for engine, state in initial.items()}
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    write_json(output / 'recipe.json', recipe)
    route = output / 'route.mgsr'
    route.write_text(program)
    report = dict(format='mgs-situation-result-1', name=recipe['name'],
                  recipe=fingerprint(output / 'recipe.json'), route=fingerprint(route),
                  seeds=dict(native=fingerprint(native_path), donor=fingerprint(donor_path)),
                  initial_conditions_satisfied=eligibility, engines={},
                  scope='Only the recorded transitions and invariants; no campaign completion claim.')
    if not all(eligibility.values()):
        report['status'] = 'not-applicable' if not any(eligibility.values()) else 'needs-review'
        report['reason'] = 'initial conditions not satisfied; no execution counted as passed'
        report['elapsed_seconds'] = round(time.monotonic() - started, 3)
        write_json(output / 'result.json', report)
        return 2
    # The first goal must be false, so a no-op cannot certify an interaction.
    if any(check(recipe['route']['steps'][0]['until'], state) for state in initial.values()):
        raise ValueError('first transition is already satisfied in a seed')
    states = {'native': terminal_checkpoint(sessions['native'])[0],
              'donor': sessions['donor'] / 'final.state'}
    report['initial_states'] = {engine: fingerprint(path) for engine, path in states.items()}
    report['initial_cards'] = {}
    for engine in ('native', 'donor'):
        folder = output / engine
        folder.mkdir()
        session = folder / 'session'
        if engine == 'native':
            config = Path(native['binaries']['config']['path'])
            if sha256(config) != native['binaries']['config']['sha256']:
                raise ValueError('native seed configuration changed')
            cards = sessions['native'] / 'cards'
            report['initial_cards'][engine] = {f'card{n}.mcd': sha256(cards / f'card{n}.mcd') for n in (1, 2)}
            command = [sys.executable, str(ROOT / 'scripts/mgs_tas.py'), 'record', str(session),
                       '--state', str(states[engine]), '--cards', str(cards), '--config', str(config),
                       '--native-route', str(route), '--end-at-checkpoint', '--period-us', str(args.period_us),
                       '--timeout', str(args.timeout)]
            command += ['--hidden', '--capture-every', '30'] if args.vulkan else ['--headless']
            code = execute(command, folder / 'driver.log', args.timeout + 20)
            result = read(session / 'result.json') if (session / 'result.json').exists() else {}
            ok = (code == 0 and result.get('status') == 'complete' and
                  '[Route] status: complete ' in result.get('route_log', []) and
                  not result.get('game_errors') and not result.get('interpreter_counters'))
            report['engines'][engine] = dict(returncode=code, frames=result.get('completed_frames'),
                                             identity=result.get('identity'),
                                             game_errors=result.get('game_errors'),
                                             interpreter_counters=result.get('interpreter_counters'))
        else:
            work = folder / 'work'
            (work / 'save').mkdir(parents=True)
            (work / 'system').mkdir()
            cards = donor_path.parent / 'work/save'
            report['initial_cards'][engine] = {}
            for n in (1, 2):
                name = f'duckstation_shared_card_{n}.mcd'
                report['initial_cards'][engine][f'card{n}.mcd'] = sha256(cards / name)
                shutil.copy2(cards / name, work / 'save' / name)
            binaries = donor['binaries']
            for name in binaries:
                if sha256(Path(binaries[name]['path'])) != binaries[name]['sha256']:
                    raise ValueError(f'donor {name} changed')
            shutil.copy2(binaries['bios']['path'], work / 'system/openbios.bin')
            env = {k: v for k, v in os.environ.items() if not k.startswith('PS1_')}
            env.update(PS1_DONOR_WORK_DIRECTORY=str(work), PS1_DONOR_STATE_LOAD=str(states[engine]),
                       PS1_DONOR_ROUTE=str(route), PS1_DONOR_ROUTE_OUTPUT=str(session),
                       PS1_DONOR_BENCHMARK='1')
            budget = sum(step.get('max_frames', 3600) for step in recipe['route']['steps']) + 60
            command = [binaries['runner']['path'], binaries['core']['path'], binaries['cue']['path'], str(budget),
                       'swanstation_GPU_Renderer=Software', 'swanstation_CPU_ExecutionMode=Interpreter',
                       'swanstation_BIOS_PatchFastBoot=true', 'swanstation_BIOS_PathNTSCU=openbios.bin',
                       'swanstation_CPU_Overclock=165', 'swanstation_MemoryCards_Card1Type=Shared',
                       'swanstation_MemoryCards_Card2Type=Shared']
            code = execute(command, folder / 'driver.log', args.timeout, env)
            result = read(session / 'result.json') if (session / 'result.json').exists() else {}
            ok = code == 0 and result.get('status') == 'complete'
            report['engines'][engine] = dict(returncode=code, frames=result.get('frames'), identity=binaries)
        final_ram = session / 'final-ram.bin'
        final = snapshot(final_ram.read_bytes(), observations) if final_ram.exists() else {}
        report['engines'][engine].update(assess(recipe, initial[engine], final, ok))
        report['engines'][engine]['event_log'] = fingerprint(
            session / 'runtime.log' if engine == 'native' else folder / 'driver.log')
        if (session / 'result.json').exists():
            report['engines'][engine]['execution_result'] = fingerprint(session / 'result.json')
        write_json(output / 'result.json', report)
    report['differences'] = {field: {engine: report['engines'][engine]['final_state'].get(field)
                                   for engine in ('native', 'donor')}
                             for field in recipe['compare']
                             if report['engines']['native']['final_state'].get(field) !=
                                report['engines']['donor']['final_state'].get(field)}
    passed = all(e['status'] == 'complete' for e in report['engines'].values())
    report['status'] = 'complete' if passed and not report['differences'] else 'needs-review'
    report['inputs_only'] = True
    report['native_replay'] = 'not-run'
    if args.verify and report['status'] == 'complete':
        command = [sys.executable, str(ROOT / 'scripts/mgs_tas.py'), 'replay', str(output / 'native/session'),
                   '--output', str(output / 'verified-replay'), '--config', str(config),
                   '--headless', '--period-us', '3000', '--timeout', str(args.timeout)]
        code = execute(command, output / 'replay.log', args.timeout + 20)
        replay = read(output / 'verified-replay/result.json') if (output / 'verified-replay/result.json').exists() else {}
        report['native_replay'] = 'complete' if code == 0 and replay.get('status') == 'complete' else 'failed'
        if report['native_replay'] != 'complete':
            report['status'] = 'needs-review'
    unchanged = all(sha256(states[engine]) == report['initial_states'][engine]['sha256']
                    for engine in ('native', 'donor'))
    for engine in ('native', 'donor'):
        cards = sessions['native'] / 'cards' if engine == 'native' else donor_path.parent / 'work/save'
        for n in (1, 2):
            name = f'card{n}.mcd' if engine == 'native' else f'duckstation_shared_card_{n}.mcd'
            unchanged &= sha256(cards / name) == report['initial_cards'][engine][f'card{n}.mcd']
    report['seed_states_and_cards_unchanged'] = unchanged
    if not unchanged:
        report['status'] = 'needs-review'
    report['elapsed_seconds'] = round(time.monotonic() - started, 3)
    write_json(output / 'result.json', report)
    print(f"{recipe['name']}: {report['status']} — {output}")
    return 0 if report['status'] == 'complete' else 1


def main():
    if len(sys.argv) > 1 and sys.argv[1] == 'matrix':
        return run_matrix(sys.argv[2:])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('native', type=Path, help='passed native scenario result.json')
    parser.add_argument('donor', type=Path, help='matching donor scenario result.json')
    parser.add_argument('output', type=Path)
    kind = parser.add_mutually_exclusive_group(required=True)
    kind.add_argument('--probe', choices=PROBES)
    kind.add_argument('--recipe', type=Path, help='explicit mgs-situation-1 conditions, invariants and input route')
    parser.add_argument('--timeout', type=int, default=90)
    parser.add_argument('--period-us', type=int, default=2000)
    parser.add_argument('--vulkan', action='store_true')
    parser.add_argument('--verify', action='store_true', help='also replay native RAM/continuation witnesses')
    args = parser.parse_args()
    if args.timeout < 1 or args.period_us < 1:
        parser.error('timeout and VBlank period must be positive')
    return run(args)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f'Situations: {error}', file=sys.stderr)
        sys.exit(1)
