#!/usr/bin/env python3
"""Run isolated differential transitions from scenarios or a verified parent pair."""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

from mgs_campaign import terminal_checkpoint
from mgs_scenarios import card_entries, compare
from mgs_tas import ROOT, sha256, write_json
from mgs_tas_route import check, compile_route, snapshot
from mgs_fixture import checkpoint_bytes, patch_ram
from mgs_loading_check import texture_witness

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


def timeout_boundary(row, engine):
    """A route deadline is resumable for diagnosis; a crash/divergence is not."""
    if (row.get('status') != 'failed' or row.get('invariant_changes') or
            row.get('final_goal_satisfied') or row.get('game_errors') or
            row.get('interpreter_counters') or
            row.get('returncode') != (1 if engine == 'native' else 43)):
        return False
    execution = read(Path(row['execution_result']['path']))
    if execution.get('status') != 'failed':
        return False
    if engine == 'native':
        logs = execution.get('route_log', [])
        return bool(logs and logs[-1].startswith('[Route] status: failed condition timeout: '))
    log = Path(row['event_log']['path']).read_text()
    return bool(re.search(r'\[donor-route\] failed after \d+ frames: condition timeout: [^\n]+\n?$', log))


def continued_pair(path, resume_timeout=False):
    """Inherit exact witnesses; explicit clean timeout resumes remain diagnostic."""
    report = read(path)
    diagnostic = resume_timeout and report.get('status') == 'needs-review'
    if (report.get('format') != 'mgs-situation-result-1' or
            (report.get('status') != 'complete' and not diagnostic)
            or not report.get('seed_states_and_cards_unchanged') or report.get('differences')):
        raise ValueError('continuation requires a completed, matching situation pair')
    parents, textures = {}, {}
    for engine in ('native', 'donor'):
        row = report['engines'][engine]
        if row['status'] != 'complete' and not diagnostic:
            raise ValueError('parent engine did not complete')
        for key in ('execution_result', 'event_log'):
            record = row[key]
            if sha256(Path(record['path'])) != record['sha256']:
                raise ValueError('parent execution evidence changed')
        if diagnostic and not timeout_boundary(row, engine):
            raise ValueError('diagnostic continuation requires two clean route timeouts')
        session = path.parent / engine / 'session'
        ram = (session / 'final-ram.bin').read_bytes()
        textures[engine] = texture_witness(ram)
        observed = snapshot(ram, row['final_state'])
        # New read-only telemetry may add fields. Every recorded field must
        # still match, alongside the complete RAM/checkpoint/hash witnesses.
        if any(k not in observed or observed[k] != v for k, v in row['final_state'].items()):
            raise ValueError('parent terminal observations changed')
        state = terminal_checkpoint(session, allow_route_timeout=diagnostic)[0] if engine == 'native' else session / 'final.state'
        # Also verify the RAM/state pairing and native payload checksum.
        checkpoint_bytes(state.read_bytes(), ram, ram, engine)
        for record in row.get('terminal_artifacts', {}).values():
            if sha256(Path(record['path'])) != record['sha256']:
                raise ValueError('parent terminal artifact changed')
        parents[engine] = dict(session=str(session), terminal_state=str(state), final_ram_sha256=sha256(session / 'final-ram.bin'),
                               binaries=row['identity'])
    if textures['native'] != textures['donor']:
        raise ValueError('parent texture inventories differ; reload the scene through the original loader')
    return parents, report.get('synthetic_ancestry', False) or not report.get('inputs_only', True)



def assess_metrics(minimums, metrics):
    """Require actual device work as well as successful story transitions."""
    if not isinstance(minimums, dict) or any(
        not isinstance(k, str) or not k or type(v) is not int or v < 1
        for k, v in minimums.items()):
        raise ValueError('native counter requirements must be named positive integers')
    counters = metrics.get('counters', {})
    rows = {key: dict(minimum=value, observed=counters.get(key)) for key, value in minimums.items()}
    return dict(passed=all(type(v['observed']) is int and v['observed'] >= v['minimum']
                           for v in rows.values()), counters=rows)

def assess(recipe, initial, final, execution_ok):
    differences = {k: dict(before=initial.get(k), after=final.get(k))
                   for k in recipe['preserve'] if initial.get(k) != final.get(k)}
    terminal = recipe['route']['steps'][-1]['until']
    goal = check(terminal, final)
    return dict(status='complete' if execution_ok and goal and not differences else 'failed',
                execution_ok=execution_ok, final_goal_satisfied=goal,
                invariant_changes=differences, final_state=final)


def card_transition(goal, before, after):
    """Require a newly written, CRC-valid save; an existing slot is not proof."""
    if goal.get('slot') not in (1, 2) or not goal.get('fields'):
        raise ValueError('card goal requires slot 1/2 and explicit save fields')
    def matches(raw):
        return [entry for entry in card_entries(raw)
                if all(entry.get(k) == v for k, v in goal['fields'].items())]
    prior, saved = matches(before), matches(after)
    return dict(passed=bool(saved) and not prior and before != after, records=saved)


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
    synthetic_ancestry = False
    if native.get('format') == 'mgs-situation-result-1':
        if native_path != donor_path:
            raise ValueError('continue a single verified pair, not unrelated situation reports')
        parents, synthetic_ancestry = continued_pair(native_path, getattr(args, 'resume_timeout', False))
        native, donor = parents['native'], parents['donor']
        donor_cards = donor_path.parent / 'donor/work/save'
    else:
        if compare(native, donor)['status'] != 'complete':
            raise ValueError('seed scenarios must have passed the same load/control contract')
        parents = {'native': native, 'donor': donor}
        donor_cards = donor_path.parent / 'work/save'
        synthetic_ancestry = any(p.get('scenario', {}).get('synthetic_phase') for p in parents.values())
    sessions = {engine: Path(report['session']) for engine, report in parents.items()}
    initial = {}
    for engine, session in sessions.items():
        ram = session / 'final-ram.bin'
        initial[engine] = snapshot(ram.read_bytes())
        expected_hash = parents[engine].get('final_ram_sha256')
        if expected_hash:
            if sha256(ram) != expected_hash:
                raise ValueError(f'{engine} seed RAM changed')
        else:
            # Early scene reports predate standalone RAM fingerprints. Verify
            # every recorded observation and the complete checkpoint witness.
            expected = parents[engine]['final_state']
            if any(initial[engine].get(k) != v for k, v in expected.items()):
                raise ValueError(f'{engine} legacy seed observations changed')
            state = terminal_checkpoint(session)[0] if engine == 'native' else session / 'final.state'
            checkpoint_bytes(state.read_bytes(), ram.read_bytes(), ram.read_bytes(), engine)
    recipe = read(args.recipe) if args.recipe else builtin(args.probe, initial['native'])
    if recipe.get('format') != 'mgs-situation-1' or not recipe.get('requires'):
        raise ValueError('situation requires explicit initial conditions')
    for key in ('preserve', 'compare'):
        if not isinstance(recipe.get(key), list) or not recipe[key]:
            raise ValueError(f'situation requires explicit {key} fields')
    assess_metrics(recipe.get('native_counters_min', {}), {})
    program = compile_route(recipe['route'], [], {})
    observations = set(recipe['requires']) | set(recipe['preserve']) | set(recipe['compare'])
    for step in recipe['route']['steps']:
        for key in ('until', 'when', 'guard', 'abort_when'):
            observations.update(step.get(key, {}))
    for engine, session in sessions.items():
        initial[engine] = snapshot((session / 'final-ram.bin').read_bytes(), observations)
    for key in ('preserve', 'compare'):
        if any(field not in initial['native'] for field in recipe[key]):
            raise ValueError(f'unknown {key} field')
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
                  synthetic_ancestry=synthetic_ancestry,
                  scope='Only the recorded transitions and invariants; no campaign completion claim.')
    parent_report = read(native_path)
    report['diagnostic_ancestry'] = list(parent_report.get('diagnostic_ancestry', []))
    if parent_report.get('format') == 'mgs-situation-result-1' and parent_report.get('status') != 'complete':
        report['diagnostic_ancestry'].append(dict(
            parent=fingerprint(native_path), status=parent_report['status'],
            reason='Explicit clean-timeout resume. The failed parent contract remains unverified.'))
    if not all(eligibility.values()):
        report['status'] = 'not-applicable' if not any(eligibility.values()) else 'needs-review'
        report['reason'] = 'initial conditions not satisfied; no execution counted as passed'
        report['elapsed_seconds'] = round(time.monotonic() - started, 3)
        write_json(output / 'result.json', report)
        return 2
    states = {'native': (Path(parents['native']['terminal_state']) if parents['native'].get('terminal_state')
                         else terminal_checkpoint(sessions['native'])[0]),
              'donor': sessions['donor'] / 'final.state'}
    report['initial_states'] = {engine: fingerprint(path) for engine, path in states.items()}
    original_states = dict(states)
    if 'fixture' in recipe:
        fixture = recipe['fixture']
        if not fixture.get('scope') or not fixture.get('patches'):
            raise ValueError('synthetic fixture requires scope and explicit writes')
        folder = output / 'fixtures'
        folder.mkdir()
        report['synthetic_fixture'] = dict(scope=fixture['scope'], engines={})
        for engine in states:
            ram = (sessions[engine] / 'final-ram.bin').read_bytes()
            patched, changes = patch_ram(ram, fixture['patches'])
            target = folder / ('native.ps1s' if engine == 'native' else 'donor.state')
            target.write_bytes(checkpoint_bytes(states[engine].read_bytes(), ram, patched, engine))
            states[engine] = target
            initial[engine] = snapshot(patched, observations)
            report['synthetic_fixture']['engines'][engine] = dict(state=fingerprint(target), changes=changes)
    # Even a synthetic fixture must execute the tested transition itself.
    if any(check(recipe['route']['steps'][0]['until'], state) for state in initial.values()):
        raise ValueError('first transition is already satisfied in a seed/fixture')
    report['initial_cards'] = {}
    for engine in ('native', 'donor'):
        folder = output / engine
        folder.mkdir()
        session = folder / 'session'
        if engine == 'native':
            config = args.config.resolve() if args.config else Path(native['binaries']['config']['path'])
            if not args.config and sha256(config) != native['binaries']['config']['sha256']:
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
            cards = donor_cards
            report['initial_cards'][engine] = {}
            for n in (1, 2):
                name = f'duckstation_shared_card_{n}.mcd'
                report['initial_cards'][engine][f'card{n}.mcd'] = sha256(cards / name)
                shutil.copy2(cards / name, work / 'save' / name)
            if 'binaries' in donor:
                binaries = dict(donor['binaries'])
            else:
                if not args.donor_runner:
                    raise ValueError('legacy donor seed requires an explicit current harness')
                binaries = {k: v for k, v in native['binaries'].items()
                            if k == 'cue' or k.startswith('track_')}
                binaries.update(core=fingerprint(ROOT / '.local/swanstation/swanstation_libretro.so'),
                                bios=fingerprint(donor_cards.parent / 'system/openbios.bin'))
                report['legacy_donor_identity'] = 'Not recorded in seed; current core and harness explicitly fingerprinted.'
            if args.donor_runner:
                binaries['runner'] = fingerprint(args.donor_runner)
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
        if final_ram.exists():
            report['engines'][engine]['textures'] = texture_witness(final_ram.read_bytes())
        if engine == 'native' and recipe.get('native_counters_min'):
            metrics_path = session / 'metrics.json'
            checked = assess_metrics(recipe['native_counters_min'], read(metrics_path) if metrics_path.exists() else {})
            if metrics_path.exists():
                checked['artifact'] = fingerprint(metrics_path)
            report['engines'][engine]['device_work'] = checked
            if not checked['passed']:
                report['engines'][engine]['status'] = 'failed'
        if recipe.get('card_goal'):
            goal = recipe['card_goal']
            slot = goal['slot']
            if slot not in (1, 2) or not goal.get('fields'):
                raise ValueError('card goal requires slot 1/2 and explicit save fields')
            before = (sessions['native'] / f'cards/card{slot}.mcd' if engine == 'native' else
                      donor_cards / f'duckstation_shared_card_{slot}.mcd')
            after = (session / f'cards/card{slot}.mcd' if engine == 'native' else
                     work / f'save/duckstation_shared_card_{slot}.mcd')
            saved = card_transition(goal, before.read_bytes(), after.read_bytes())
            report['engines'][engine]['card_goal'] = dict(saved, before=fingerprint(before), after=fingerprint(after))
            if not saved['passed']:
                report['engines'][engine]['status'] = 'failed'
        report['engines'][engine]['event_log'] = fingerprint(
            session / 'runtime.log' if engine == 'native' else folder / 'driver.log')
        if (session / 'result.json').exists():
            report['engines'][engine]['execution_result'] = fingerprint(session / 'result.json')
        if ok and final_ram.exists():
            terminal = terminal_checkpoint(session)[0] if engine == 'native' else session / 'final.state'
            artifacts = dict(ram=fingerprint(final_ram), state=fingerprint(terminal))
            for n in (1, 2):
                card = session / f'cards/card{n}.mcd' if engine == 'native' else work / f'save/duckstation_shared_card_{n}.mcd'
                artifacts[f'card{n}'] = fingerprint(card)
            report['engines'][engine]['terminal_artifacts'] = artifacts
        write_json(output / 'result.json', report)
    report['differences'] = {field: {engine: report['engines'][engine]['final_state'].get(field)
                                   for engine in ('native', 'donor')}
                             for field in recipe['compare']
                             if report['engines']['native']['final_state'].get(field) !=
                                report['engines']['donor']['final_state'].get(field)}
    passed = all(e['status'] == 'complete' for e in report['engines'].values())
    textures = {engine: report['engines'][engine].get('textures') for engine in ('native', 'donor')}
    if not all(textures.values()) or textures['native'] != textures['donor']:
        report['differences']['textures'] = textures
    report['status'] = 'complete' if passed and not report['differences'] else 'needs-review'
    report['inputs_only'] = 'fixture' not in recipe
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
    unchanged = all(sha256(original_states[engine]) == report['initial_states'][engine]['sha256']
                    for engine in ('native', 'donor'))
    for engine in ('native', 'donor'):
        cards = sessions['native'] / 'cards' if engine == 'native' else donor_cards
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
    parser.add_argument('--resume-timeout', action='store_true',
                        help='resume two matching clean route timeouts for diagnosis; retains failed ancestry')
    parser.add_argument('--donor-runner', type=Path, help='use an explicitly fingerprinted updated harness')
    parser.add_argument('--config', type=Path, help='explicitly fingerprint a new native configuration for legacy seeds')
    argv = sys.argv[1:]
    if argv and argv[0] == 'continue':
        if len(argv) < 3:
            parser.error('continue requires parent/result.json and output directory')
        argv = [argv[1], argv[1], *argv[2:]]
    args = parser.parse_args(argv)
    if args.timeout < 1 or args.period_us < 1:
        parser.error('timeout and VBlank period must be positive')
    return run(args)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f'Situations: {error}', file=sys.stderr)
        sys.exit(1)
