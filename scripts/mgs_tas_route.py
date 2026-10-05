#!/usr/bin/env python3
"""Run state-gated, input-only actions adapted from a PJM movie.

The default runner keeps one native process alive and checks read-only game
state at each TAS VBlank. It records the effective inputs and saves a portable
checkpoint at the milestone. --verify replays that whole segment once.
"""
import argparse
import itertools
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

from mgs_campaign import terminal_checkpoint
from mgs_tas import ROOT, game_state, identity, sha256, write_json
from mgs_tas_import import BUTTONS, parse_pjm


def gcl_variable(field):
    """Validate a raw retail GCL operand and resolve its read-only USA view."""
    if not re.fullmatch(r'gcl_[0-9a-f]{8}', field):
        return None
    raw = int(field[4:], 16)
    kind, scope, flag, offset = (raw >> 24) & 15, (raw >> 20) & 15, (raw >> 16) & 15, raw & 65535
    if (raw >> 28 != 1 or scope not in (0, 8) or kind not in (1, 2, 3, 4, 6, 8)
            or (flag > 7 if kind == 4 else flag != 0)):
        return None
    width = 2 if kind in (1, 6, 8) else 1
    if offset + width > (192 if scope == 8 else 2048):
        return None
    return (0xB7510 if scope == 8 else 0xB6440) + offset, kind, flag


WATCHER_OFFSETS = {'x': 0x20, 'y': 0x22, 'z': 0x24, 'facing': 0x2a, 'pitch': 0x28,
                   'route': 0xb7d, 'target_x': 0xbe0, 'target_z': 0xbe4,
                   'vision_facing': 0xb8c, 'vision_angle': 0xb8e,
                   'vision_range': 0xb90, 'visible': 0xb92}
CAMERA_OFFSETS = {k: v for k, v in WATCHER_OFFSETS.items() if v <= 0x2a}
CAMERA_OFFSETS.update(phase=0x1e8, timer=0x1ec)
EVPANEL_OFFSETS = dict(phase=0x2e, selection=0x32, timer=0x36, door_phase=0x42, occupants=0x44)
ZAKO_OFFSETS = dict(x=0x20, y=0x22, z=0x24, facing=0x2a, life=0x8fc)
REVOLVER_OFFSETS = dict(x=0x24, y=0x26, z=0x28, facing=0x2e, life=0x854, phase=0x8fc, timer=0x900)


def watcher_variable(field):
    match = re.fullmatch(r'(watcher|camera|evpanel|zako|revolver)_([0-9a-f]{4})_([a-z_]+)', field)
    if not match:
        return None
    camera = match[1] == 'camera'
    elevator = match[1] == 'evpanel'
    zako = match[1] == 'zako'
    revolver = match[1] == 'revolver'
    if match[3] not in (REVOLVER_OFFSETS if revolver else ZAKO_OFFSETS if zako else EVPANEL_OFFSETS if elevator else CAMERA_OFFSETS if camera else WATCHER_OFFSETS):
        return None
    return int(match[2], 16) | (0x80000 if revolver else 0x40000 if zako else 0x20000 if elevator else 0x10000 if camera else 0), match[3]


def actor_variable(field):
    match = re.fullmatch(r'actor_([a-z0-9_]{1,48})_count', field)
    return match[1] + '.c' if match else None


def actor_counts(ram):
    """Read complete USA GV actor lists; malformed lists mean unknown, not zero."""
    if len(ram) != 2097152:
        return None
    word = lambda p: struct.unpack_from('<I', ram, p & 0x1fffffff)[0]
    counts = {}
    visited = set()
    for level in range(9):
        head = 0x800af390 + level * 68
        tail, previous, actor = head + 32, head, word(head + 4)
        while actor != tail:
            if (not 0x80010000 <= actor <= 0x801fffe0 or actor in visited or
                    len(visited) >= 1024 or word(actor) != previous):
                return None
            visited.add(actor)
            name = word(actor + 20) & 0x1fffffff
            if name and (word(actor + 8) or word(actor + 12)):
                if not 0x10000 <= name < len(ram):
                    return None
                end = ram.find(b'\0', name, min(name + 64, len(ram)))
                if end < 0:
                    return None
                label = ram[name:end].decode('ascii', errors='replace')
                counts[label] = counts.get(label, 0) + 1
            previous, actor = actor, word(actor + 4)
        if word(tail) != previous:
            return None
    return counts


def watcher_actors(ram):
    """Locate linked, named patrol/camera/elevator actors; reject ambiguous names."""
    found = {}
    for base in range(0, len(ram) - 0xc50, 4):
        prev, nxt, act = struct.unpack_from('<3I', ram, base)
        if not 0x80010000 <= act < 0x80200000:
            continue
        name = struct.unpack_from('<I', ram, base + 20)[0] & 0x1fffffff
        prev, nxt = prev & 0x1fffffff, nxt & 0x1fffffff
        if not (0 < prev < len(ram) - 8 and 0 < nxt < len(ram) - 8):
            continue
        camera = ram[name:name + 9] == b'camera.c\0'
        elevator = ram[name:name + 10] == b'evpanel.c\0'
        zako = ram[0xb7500:0xb7505] == b's03d\0' and ram[name:name + 7] == b'zako.c\0'
        revolver = ram[0xb7500:0xb7505] == b's04b\0' and ram[name:name + 11] == b'revolver.c\0'
        if ((not camera and not elevator and not zako and not revolver and ram[name:name + 10] != b'watcher.c\0') or
            struct.unpack_from('<I', ram, prev + 4)[0] != base | 0x80000000 or
            struct.unpack_from('<I', ram, nxt)[0] != base | 0x80000000):
            continue
        actor_id = struct.unpack_from('<H', ram, base + (0x54 if revolver else 0x2c if elevator else 0x50))[0] | (
            0x80000 if revolver else 0x40000 if zako else 0x20000 if elevator else 0x10000 if camera else 0)
        found[actor_id] = None if actor_id in found else base
    return found


def snapshot(ram, observations=()):
    result = game_state(ram)
    result['story'] = struct.unpack_from('<h', ram, 0xB644A)[0]
    result['game_over_timer'] = struct.unpack_from('<i', ram, 0xADB44)[0]
    result['pad_origin'] = struct.unpack_from('<i', ram, 0xADAA8)[0] & 4095
    control = result['control'] & 0x1fffffff
    result['yaw'] = (struct.unpack_from('<H', ram, control + 10)[0] & 4095
                     if 0 < control < len(ram) - 80 else None)
    result['turn_yaw'] = (struct.unpack_from('<H', ram, control + 0x4e)[0] & 4095
                         if 0 < control < len(ram) - 80 else None)
    result['socom_ammo'] = result['weapons'][0]
    result['guards_alive'] = result['guards_in_room'] = None
    result['ambush_phase'] = result['ambush_kills'] = None
    if result['stage'] == 's03d':
        result['ambush_phase'] = struct.unpack_from('<i', ram, 0xdfb70)[0]
        result['ambush_kills'] = struct.unpack_from('<i', ram, 0xdfbe8)[0]
        for name, maximum in (('ambush_phase', 15), ('ambush_kills', 18)):
            if not 0 <= result[name] <= maximum:
                result[name] = None
        guards = {}
        name = 0
        while (name := ram.find(b'zako.c\0', name)) >= 0:
            pointer = struct.pack('<I', name | 0x80000000)
            name += 1
            pos = 0
            while (pos := ram.find(pointer, pos)) >= 0:
                base = pos - 20
                pos += 1
                if base < 0 or base % 4 or base + 0x900 > len(ram):
                    continue
                act, = struct.unpack_from('<I', ram, base + 8)
                target, = struct.unpack_from('<I', ram, base + 0x8FC)
                target &= 0x1FFFFFFF
                if not 0x80010000 <= act < 0x80200000 or not 0 < target < len(ram) - 40:
                    continue
                life, = struct.unpack_from('<h', ram, target + 38)
                x, _, z = struct.unpack_from('<3h', ram, base + 32)
                if life > 0:
                    guards[base] = 4500 <= x <= 11000 and -2000 <= z <= 3000
        result['guards_alive'] = len(guards)
        result['guards_in_room'] = sum(guards.values())
    for index, axis in enumerate(('x', 'y', 'z')):
        result[axis] = result['player_position'][index]
    # Identify the live USA title actor by callback + filename, not an assumed
    # heap address or Integral Work layout. USA OpenAct 800D272C reads +0xA48;
    # its case-7 switch reads +0xA4C. Integral uses +0xA74/+0xA78 instead.
    result['title_state'] = result['title_selection'] = result['title_level'] = None
    if result['stage'] == 'title':
        marker = struct.pack('<I', 0x800D26E0)
        start = 0
        matches = []
        while (p := ram.find(marker, start)) >= 0:
            start = p + 1
            base = p - 8
            if base < 0 or base % 4 or base + 0x24BC > len(ram):
                continue
            filename = struct.unpack_from('<I', ram, base + 20)[0] & 0x1FFFFFFF
            if ram[filename:filename + 7] == b'open.c\0':
                matches.append(base)
        if len(matches) == 1:
            for name, offset in [('title_state', 0xA48), ('title_selection', 0xA4C),
                                 ('title_level', 0x24B8)]:
                result[name] = struct.unpack_from('<i', ram, matches[0] + offset)[0]
    watchers = watcher_actors(ram) if any(watcher_variable(f) for f in observations) else {}
    actors = actor_counts(ram) if any(actor_variable(f) for f in observations) else None
    for field in observations:
        actor = actor_variable(field)
        if actor:
            result[field] = actors.get(actor, 0) if actors is not None else None
        watcher = watcher_variable(field)
        if watcher:
            actor_id, member = watcher
            base = watchers.get(actor_id)
            offsets = (REVOLVER_OFFSETS if actor_id & 0x80000 else ZAKO_OFFSETS if actor_id & 0x40000 else EVPANEL_OFFSETS if actor_id & 0x20000 else
                       CAMERA_OFFSETS if actor_id & 0x10000 else WATCHER_OFFSETS)
            if member == 'life' and actor_id & 0x40000:
                target = (struct.unpack_from('<I', ram, base + 0x8fc)[0] & 0x1fffffff
                          if base is not None else 0)
                result[field] = (struct.unpack_from('<h', ram, target + 38)[0]
                                 if 0 < target <= len(ram) - 40 else None)
            else:
                result[field] = (None if base is None else struct.unpack_from(
                    '<i' if ((actor_id & 0x10000 and member in ('phase', 'timer')) or
                             (actor_id & 0x80000 and member in ('life', 'phase', 'timer'))) else
                    '<b' if member == 'route' else '<h',
                    ram, base + offsets[member])[0])
        variable = gcl_variable(field)
        if variable:
            offset, kind, flag = variable
            result[field] = (struct.unpack_from('<h' if kind == 1 else '<H', ram, offset)[0]
                             if kind in (1, 6, 8) else
                             (ram[offset] >> flag) & 1 if kind == 4 else ram[offset])
    return result


FIELDS = frozenset(snapshot(bytes(2097152)))
OPERATORS = frozenset(('eq', 'ne', 'min', 'max', 'mask'))


def check(conditions, state):
    """All named predicates must match. A missing actor never satisfies ne."""
    for field, expected in conditions.items():
        if field not in FIELDS and not gcl_variable(field) and not watcher_variable(field) and not actor_variable(field):
            raise ValueError(f'unknown observation: {field}')
        value = state.get(field)
        if value is None:
            return False
        if not isinstance(expected, dict):
            if value != expected:
                return False
            continue
        if set(expected) - OPERATORS or not set(expected) & (OPERATORS - {'mask'}):
            raise ValueError(f'invalid predicate for {field}: {expected}')
        if 'mask' in expected:
            value &= expected['mask']
        if ('eq' in expected and value != expected['eq'] or
                'ne' in expected and value == expected['ne'] or
                'min' in expected and value < expected['min'] or
                'max' in expected and value > expected['max']):
            return False
    return True


def action_runs(action, movie, metadata):
    """Return (duration, pressed-high pad0, pad1), preserving source order."""
    runs = []
    if 'sampled_game_runs' in action:
        if set(action) != {'sampled_game_runs'}:
            raise ValueError('sampled game inputs cannot mix clock domains')
        runs = action['sampled_game_runs']
        if (not isinstance(runs, list) or not runs or any(
                not isinstance(r, list) or len(r) != 3 or any(type(v) is not int for v in r) or
                not 1 <= r[0] <= 10000000 or any(not 0 <= p <= 65535 for p in r[1:]) for r in runs)):
            raise ValueError('sampled_game_runs require bounded duration/pad0/pad1 triples')
        return [tuple(r) for r in runs]
    if 'source_frames' in action:
        first, last = action['source_frames']
        if not 1 <= first <= last <= len(movie):
            raise ValueError('source frame range outside PJM')
        if any(first <= e['frame'] <= last for e in metadata['events']):
            raise ValueError('source range includes an unsupported system event')
        minimum_hold = action.get('minimum_hold', 1)
        minimum_release = action.get('minimum_release', 1)
        if minimum_hold < 1 or minimum_release < 1:
            raise ValueError('minimum hold/release must be positive')
        adjustments = {}
        for edit in action.get('run_adjustments', []):
            if (not isinstance(edit, dict) or set(edit) != {'first_frame', 'extra_frames'} or
                    any(type(v) is not int for v in edit.values()) or
                    edit['first_frame'] in adjustments):
                raise ValueError('run_adjustments require unique integer first_frame/extra_frames')
            adjustments[edit['first_frame']] = edit['extra_frames']
        cursor = first
        for pads, group in itertools.groupby(movie[first - 1:last]):
            original_duration = sum(1 for _ in group)
            duration = original_duration + adjustments.pop(cursor, 0)
            if duration < 1:
                raise ValueError('run adjustment cannot remove an original input edge')
            duration = max(duration, minimum_hold if any(pads) else minimum_release)
            runs.append((duration, *pads))
            cursor += original_duration
        if adjustments:
            raise ValueError('run adjustment does not identify the first frame of a selected input run')
    elif 'sequence' in action:
        for token in action['sequence'].split(','):
            chord, duration = token.split(':')
            names = [] if chord == 'wait' else chord.split('+')
            if len(names) != len(set(names)) or set(names) - set(BUTTONS):
                raise ValueError(f'invalid button chord: {chord}')
            runs.append((int(duration), sum(1 << BUTTONS.index(name) for name in names), 0))
    else:
        raise ValueError('action requires source_frames or sequence')
    settle = action.get('settle_frames', 0)
    if settle < 0:
        raise ValueError('negative settle_frames')
    if settle:
        runs.append((settle, 0, 0))
    if not runs or any(duration < 1 for duration, _, _ in runs):
        raise ValueError('empty action or nonpositive duration')
    return runs


def render_input(runs, first_vblank):
    lines = []
    cursor = first_vblank
    for duration, first, second in runs:
        for port, mask in enumerate((first, second)):
            for bit, name in enumerate(BUTTONS):
                if mask & (1 << bit):
                    lines.append(f'{cursor} {cursor + duration - 1} {name} {port}')
        cursor += duration
    return cursor - first_vblank, '\n'.join(lines) + '\n'


def drive_step(step, observe, execute, movie, metadata, emit):
    """Closed-loop step. Budget includes waits, failed attempts and save tails."""
    budget = step.get('max_frames', 3600)
    poll = step.get('poll_frames', 60)
    attempts = step.get('attempts', 1)
    if min(budget, poll, attempts) < 1 or not step.get('until'):
        raise ValueError('positive limits and an explicit until condition are required')
    when = step.get('when', {})
    guard = step.get('guard', {})
    abort = step.get('abort_when', {})
    # Validate every predicate before running even if a previous one is false.
    for conditions in (when, guard, abort, step['until']):
        for field, expected in conditions.items():
            check({field: expected}, {field: 0})
    runs = action_runs(step['action'], movie, metadata) if 'action' in step else None
    state = observe()
    spent = used = 0
    while True:
        if abort and check(abort, state):
            raise ValueError(f"{step['name']}: abort condition matched: {state}")
        if not check(guard, state):
            raise ValueError(f"{step['name']}: guard failed: {state}")
        if check(step['until'], state):
            emit('satisfied', {'frames': spent, 'attempts': used, 'state': state})
            return
        if spent >= budget:
            raise ValueError(f"{step['name']}: condition timeout after {spent} VBlanks: {state}")
        if runs and used < attempts and check(when, state):
            selected = runs
            kind = 'action'
            used += 1
        else:
            selected = [(min(poll, budget - spent), 0, 0)]
            kind = 'wait'
        if sum(run[0] for run in selected) > budget - spent:
            raise ValueError(f"{step['name']}: action exceeds remaining budget")
        emit(kind, {'attempt': used, 'before': state, 'action': step.get('action') if kind == 'action' else None})
        advanced = execute(selected, kind)
        if advanced < sum(run[0] for run in selected):
            raise ValueError('native execution did not consume the requested input')
        spent += advanced
        state = observe()


def run_checkpoint_driver(args):
    if args.state:
        raise ValueError('--state requires the persistent runner; omit --checkpoint-driver')
    recipe = json.loads(args.recipe.read_text())
    if recipe.get('format') != 'mgs-tas-route-1' or not recipe.get('steps'):
        raise ValueError('unsupported or empty route recipe')
    metadata, movie, _ = parse_pjm(args.movie.read_bytes())
    if metadata['source_sha256'] != recipe['source_sha256']:
        raise ValueError('PJM fingerprint differs from route recipe')
    native_identity = identity()
    if native_identity['exe']['sha256'] != '3342f75717c5134aec832547691c2354e67a253abfcfa9e0e823e2133bb1fc32':
        raise ValueError('route observations require the verified USA v1.0 executable')
    for step in recipe['steps']:
        if step.get('stable_frames', 1) != 1:
            raise ValueError('stable frame observations require the persistent runner')
        if not re.fullmatch(r'[a-z0-9_-]+', step['name']):
            raise ValueError('step names must use lowercase letters, digits, hyphens or underscores')
        if 'action' in step:
            if set(step['action']) & {'move_to', 'sampled_game_runs'}:
                raise ValueError('movement/game-clock actions require the persistent runner')
            action_runs(step['action'], movie, metadata)
    if args.parent:
        state, vblank = terminal_checkpoint(args.parent.resolve())
        cards = args.parent.resolve() / 'cards'
    else:
        boot = args.boot.resolve()
        manifest = json.loads((boot / 'manifest.json').read_text())
        state, cards = boot / 'initial.ps1s', boot / 'cards'
        if sha256(state) != manifest['initial_sha256']:
            raise ValueError('bootstrap state fingerprint changed')
        vblank = manifest.get('initial_vblank')
        if vblank is None:
            vblank = json.loads((boot / 'metrics.json').read_text())['counters']['timing.vblank']
    output = args.destination.resolve()
    output.mkdir(parents=True, exist_ok=False)
    shutil.copy2(args.recipe, output / 'recipe.json')
    report = {'format': 'mgs-tas-route-result-1', 'status': 'running',
              'identity': native_identity,
              'source': metadata, 'recipe_sha256': sha256(output / 'recipe.json'),
              'initial_state': str(state), 'initial_sha256': sha256(state),
              'segments': [], 'completed_steps': [], 'verification_requested': args.verify}
    ram = state.read_bytes()[476:476 + 2097152]
    current_step = ''
    observations = {field for step in recipe['steps']
                    for kind in ('when', 'until', 'guard', 'abort_when')
                    for field in step.get(kind, {})}
    def observe():
        return snapshot(ram, observations)
    def emit(event, detail):
        with (output / 'events.jsonl').open('a') as stream:
            stream.write(json.dumps({'event': event, 'step': current_step, 'vblank': vblank, **detail}) + '\n')
        print(f"[Route] {current_step}: {event} at VBlank {vblank}", flush=True)
    def execute(runs, kind):
        nonlocal state, cards, ram, vblank
        folder = output / f'{len(report["segments"]):04d}-{current_step}-{kind}'
        frames, script = render_input(runs, vblank + 1)
        inputs = output / 'inputs'
        inputs.mkdir(exist_ok=True)
        input_file = inputs / (folder.name + '.input')
        input_file.write_text(script)
        command = [sys.executable, str(ROOT / 'scripts/mgs_tas.py'), 'record', str(folder),
                   '--state', str(state), '--cards', str(cards), '--input-script', str(input_file),
                   '--frames', str(frames), '--end-at-checkpoint', '--headless',
                   '--period-us', str(args.period_us), '--timeout', str(args.timeout),
                   '--checkpoint-interval', str(args.checkpoint_interval),
                   '--native-every', str(args.native_every)]
        with (output / 'driver.log').open('a') as log:
            process = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        result = json.loads((folder / 'result.json').read_text()) if (folder / 'result.json').exists() else {}
        report['segments'].append({'path': str(folder), 'step': current_step, 'kind': kind,
                                   'result_sha256': sha256(folder / 'result.json') if result else None})
        if process.returncode or result.get('status') != 'complete' or result.get('interpreter_counters') or result.get('game_errors'):
            raise ValueError(f'native segment failed: {folder}; see driver.log')
        state, vblank = terminal_checkpoint(folder)
        cards, ram = folder / 'cards', (folder / 'final-ram.bin').read_bytes()
        if args.verify:
            with (output / 'driver.log').open('a') as log:
                replay = subprocess.run([sys.executable, str(ROOT / 'scripts/mgs_tas.py'), 'replay', str(folder),
                                         '--output', str(folder / 'verified-replay'), '--headless',
                                         '--period-us', str(args.period_us + 500), '--timeout', str(args.timeout)],
                                        cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
            if replay.returncode:
                raise ValueError(f'native replay failed: {folder}')
            report['segments'][-1]['verified_replay_sha256'] = sha256(folder / 'verified-replay/result.json')
        write_json(output / 'result.json', report)
        return result['completed_frames']
    try:
        for step in recipe['steps']:
            current_step = step['name']
            drive_step(step, observe, execute, movie, metadata, emit)
            report['completed_steps'].append(current_step)
        report['status'] = 'complete'
    except (OSError, ValueError, KeyError, KeyboardInterrupt) as error:
        report.update(status='failed', error=str(error), failed_step=current_step)
        print(f'[Route] {error}', file=sys.stderr)
    report.update(final_state=observe(), terminal_state=str(state), terminal_vblank=vblank,
                  terminal_cards=str(cards), terminal_sha256=sha256(state))
    write_json(output / 'result.json', report)
    print(f"[Route] {report['status']}: {output}")
    return 0 if report['status'] == 'complete' else 1


def compile_route(recipe, movie, metadata):
    lines = ['MGSROUTE 1']
    supported = {'stage', 'game_status', 'load_request', 'game_over_timer', 'player_status', 'pause_level', 'alert_mode', 'alert_level',
                 'control', 'difficulty', 'disc', 'life', 'max_life', 'weapon', 'item',
                 'stance', 'story', 'x', 'y', 'z', 'yaw', 'turn_yaw', 'pad_origin', 'socom_ammo',
                 'guards_alive', 'guards_in_room', 'ambush_phase', 'ambush_kills',
                 'title_state', 'title_selection', 'title_level'}
    if recipe.get('format') != 'mgs-tas-route-1' or not recipe.get('steps'):
        raise ValueError('unsupported or empty route')
    for step in recipe['steps']:
        if not re.fullmatch(r'[a-z0-9_-]+', step['name']) or not step.get('until'):
            raise ValueError('every step requires a valid name and explicit goal')
        budget, attempts = step.get('max_frames', 3600), step.get('attempts', 1)
        if not 1 <= budget <= 10000000 or not 1 <= attempts <= 1000000:
            raise ValueError('invalid step budget/attempts')
        lines.append(f"STEP {step['name']} {budget} {attempts}")
        stable = step.get('stable_frames', 1)
        if not isinstance(stable, int) or not 1 <= stable <= 3600:
            raise ValueError('invalid stable goal duration')
        if stable != 1:
            lines.append(f'STABLE {stable}')
        for key, command in [('when', 'WHEN'), ('until', 'UNTIL'), ('guard', 'GUARD'), ('abort_when', 'ABORT')]:
            for field, expected in step.get(key, {}).items():
                if field not in supported and not gcl_variable(field) and not watcher_variable(field) and not actor_variable(field):
                    raise ValueError(f'observation not supported by persistent runner: {field}')
                predicates = expected if isinstance(expected, dict) else {'eq': expected}
                if set(predicates) - OPERATORS or not set(predicates) & (OPERATORS - {'mask'}):
                    raise ValueError('invalid predicate operator')
                mask = predicates.get('mask', 0xffffffff)
                if not isinstance(mask, int) or not 0 <= mask <= 0xffffffff:
                    raise ValueError('invalid predicate mask')
                for op, value in predicates.items():
                    if op == 'mask':
                        continue
                    if field == 'stage':
                        if op not in ('eq', 'ne') or not isinstance(value, str) or not re.fullmatch('[a-z0-9_]{1,8}', value):
                            raise ValueError('invalid stage predicate')
                    elif not isinstance(value, int):
                        raise ValueError('numeric observation requires an integer')
                    lines.append(f'{command} {field} {op} {value} {mask}')
        if 'action' in step:
            action = step['action']
            if 'move_to' in action:
                target = action['move_to']
                if set(action) != {'move_to'} or not isinstance(target, dict) or set(target) != {'x','z'} or any(
                        not isinstance(v, int) or not -32768 <= v <= 32767 for v in target.values()):
                    raise ValueError('move_to requires signed 16-bit x,z and cannot mix actions')
                lines.append(f'MOVE {target["x"]} {target["z"]}')
            else:
                if 'sampled_game_runs' in action:
                    lines.append('CLOCK GAME')
                for duration, pad0, pad1 in action_runs(action, movie, metadata):
                    lines.append(f'INPUT {duration} {pad0} {pad1}')
        lines.append('END')
    return '\n'.join(lines) + '\n'


def run(args):
    recipe = json.loads(args.recipe.read_text())
    metadata, movie, _ = parse_pjm(args.movie.read_bytes())
    if recipe.get('source_sha256') != metadata['source_sha256']:
        raise ValueError('PJM fingerprint differs from route recipe')
    program = compile_route(recipe, movie, metadata)
    native_identity = identity()
    if native_identity['exe']['sha256'] != '3342f75717c5134aec832547691c2354e67a253abfcfa9e0e823e2133bb1fc32':
        raise ValueError('route observations require the verified USA v1.0 executable')
    if args.parent:
        state, _ = terminal_checkpoint(args.parent.resolve())
        cards = args.parent.resolve() / 'cards'
    elif args.state:
        if not args.cards:
            raise ValueError('--state requires explicit --cards from the same checkpoint session')
        state, cards = args.state.resolve(), args.cards.resolve()
    else:
        boot = args.boot.resolve()
        state, cards = boot / 'initial.ps1s', boot / 'cards'
        if sha256(state) != json.loads((boot / 'manifest.json').read_text())['initial_sha256']:
            raise ValueError('bootstrap checkpoint changed')
    output = args.destination.resolve()
    output.mkdir(parents=True, exist_ok=False)
    shutil.copy2(args.recipe, output / 'recipe.json')
    (output / 'route.mgsr').write_text(program)
    session = output / 'session'
    command = [sys.executable, str(ROOT / 'scripts/mgs_tas.py'), 'record', str(session),
               '--state', str(state), '--cards', str(cards), '--native-route', str(output / 'route.mgsr'),
               '--headless', '--end-at-checkpoint', '--period-us', str(args.period_us),
               '--checkpoint-interval', str(args.checkpoint_interval),
               '--timeout', str(args.timeout), '--native-every', str(args.native_every)]
    print(f'[Route] persistent execution: {output}', flush=True)
    with (output / 'driver.log').open('w') as log:
        code = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT).returncode
    result_path = session / 'result.json'
    result = json.loads(result_path.read_text()) if result_path.exists() else {}
    complete = code == 0 and result.get('status') == 'complete' and not result.get('interpreter_counters') and not result.get('game_errors')
    complete = complete and any(line == '[Route] status: complete ' for line in result.get('route_log', []))
    report = {'format': 'mgs-tas-route-result-2', 'mode': 'persistent',
              'status': 'complete' if complete else 'failed', 'source': metadata,
              'recipe_sha256': sha256(output / 'recipe.json'), 'program_sha256': sha256(output / 'route.mgsr'),
              'identity': native_identity, 'session': str(session),
              'initial_state': str(state), 'initial_sha256': sha256(state),
              'result_sha256': sha256(result_path) if result else None,
              'route_log': result.get('route_log', []), 'completed_frames': result.get('completed_frames'),
              'elapsed_seconds': result.get('elapsed_seconds'),
              'verification_requested': args.verify, 'verified_replay': False}
    if (session / 'final-ram.bin').exists():
        observations = {field for step in recipe['steps']
                        for kind in ('when', 'until', 'guard', 'abort_when')
                        for field in step.get(kind, {})}
        report['final_state'] = snapshot((session / 'final-ram.bin').read_bytes(), observations)
    if complete:
        terminal, vblank = terminal_checkpoint(session)
        report.update(terminal_state=str(terminal), terminal_vblank=vblank, terminal_sha256=sha256(terminal))
        if args.verify:
            print('[Route] milestone reached; verifying the full recorded route once', flush=True)
            with (output / 'replay-driver.log').open('w') as log:
                replay_code = subprocess.run([sys.executable, str(ROOT / 'scripts/mgs_tas.py'), 'replay', str(session),
                                              '--output', str(output / 'verified-replay'), '--headless',
                                              '--period-us', str(args.period_us + 500), '--timeout', str(args.timeout)],
                                             cwd=ROOT, stdout=log, stderr=subprocess.STDOUT).returncode
            report['verified_replay'] = replay_code == 0
            report['status'] = 'complete' if replay_code == 0 else 'failed'
    write_json(output / 'result.json', report)
    for line in report['route_log']:
        print(line)
    print(f"[Route] {report['status']}: {report['completed_frames']} frames, {report['elapsed_seconds']} seconds")
    return 0 if report['status'] == 'complete' else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('recipe', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--movie', type=Path, required=True)
    seed = parser.add_mutually_exclusive_group(required=True)
    seed.add_argument('--parent', type=Path)
    seed.add_argument('--boot', type=Path)
    seed.add_argument('--state', type=Path, help='explicit checkpoint, including one retained after an adaptive abort')
    parser.add_argument('--cards', type=Path, help='required with --state; inherited checkpoint backing cards')
    parser.add_argument('--verify', action='store_true', help='replay the complete recording once after reaching the milestone')
    parser.add_argument('--checkpoint-driver', action='store_true', help='slow diagnostic mode: a separate native session per action')
    parser.add_argument('--period-us', type=int, default=1000)
    parser.add_argument('--timeout', type=int, default=120)
    parser.add_argument('--native-every', type=int, default=120)
    parser.add_argument('--checkpoint-interval', type=int, default=3600,
                        help='periodic checkpoints for branching near an unsuccessful macro')
    args = parser.parse_args()
    try:
        return run_checkpoint_driver(args) if args.checkpoint_driver else run(args)
    except (OSError, ValueError, KeyError) as error:
        print(f'[Route] {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
