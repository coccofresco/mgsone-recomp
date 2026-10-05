#!/usr/bin/env python3
"""Chain input-only native TAS segments, with exact checkpoint/card ancestry."""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

from mgs_tas import ROOT, BINARY, CONFIG, game_state, sha256, write_json

BUTTONS = frozenset('select l3 r3 start up right down left l2 r2 l1 r1 triangle circle cross square'.split())


def intervals(sequence, first_vblank):
    cursor, lines = first_vblank, []
    for token in sequence.split(','):
        buttons, duration = token.strip().split(':')
        duration = int(duration)
        if duration <= 0:
            raise ValueError('each action must last at least one VBlank')
        names = [] if buttons == 'wait' else buttons.split('+')
        if set(names) - BUTTONS or len(set(names)) != len(names):
            raise ValueError(f'unknown or repeated buttons: {buttons}')
        for name in names:
            lines.append(f'{cursor} {cursor + duration - 1} {name}')
        cursor += duration
    return cursor - first_vblank, '\n'.join(lines) + '\n'


def terminal_checkpoint(parent):
    result = json.loads((parent / 'result.json').read_text())
    manifest = json.loads((parent / 'manifest.json').read_text())
    if result['status'] != 'complete' or manifest['status'] != 'complete':
        raise ValueError('parent recording is incomplete')
    movie = parent / 'movie.mgst'
    if sha256(movie) != manifest['artifacts']['movie.mgst']:
        raise ValueError('parent movie changed')
    terminal = movie.read_text().splitlines()[-1].split()
    if len(terminal) != 6 or terminal[0] != 'E':
        raise ValueError('parent has no terminal witness')
    frames, vblank = int(terminal[1]), int(terminal[3])
    suffix = f'checkpoint_{frames:09d}_vblank_{vblank}.ps1s'
    for item in result['checkpoints']:
        state = parent / item['file']
        if state.name == suffix:
            if sha256(state) != item['sha256']:
                raise ValueError('parent checkpoint changed')
            return state, vblank
    raise ValueError('parent has no checkpoint at its exact terminal witness')


def inspect_ram(ram):
    result = game_state(ram)
    # USA GCL_GetVar at 800227B8 selects 800B6440 (ordinary script variables)
    # and 800B7510 (linked game state). These are read-only diagnostic views.
    result['dock_script'] = {'lift_flags': ram[0xB6440 + 0x2d],
                             'guard_flags': ram[0xB6440 + 0x67],
                             'lift_state_hash': struct.unpack_from('<H', ram, 0xB6440 + 0x40)[0]}
    result['watchers'] = []
    for offset in range(0, len(ram) - 0xC50, 4):
        act, = struct.unpack_from('<I', ram, offset + 8)
        if not 0x80010000 <= act < 0x80200000:
            continue
        filename, = struct.unpack_from('<I', ram, offset + 20)
        p = filename & 0x1fffffff
        if p >= len(ram) - 10 or ram[p:p + 10] != b'watcher.c\0':
            continue
        result['watchers'].append({
            'address': hex(offset | 0x80000000),
            'name_hash': struct.unpack_from('<H', ram, offset + 0x50)[0],
            'position': list(struct.unpack_from('<3h', ram, offset + 0x20)),
            'patrol_count': struct.unpack_from('<i', ram, offset + 0x9e8)[0],
            'patrol_root': ram[offset + 0xb7d],
            'target': list(struct.unpack_from('<3h', ram, offset + 0xbe0))})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    inspect = commands.add_parser('inspect')
    inspect.add_argument('ram', type=Path)
    step = commands.add_parser('step')
    step.add_argument('session', type=Path)
    seed = step.add_mutually_exclusive_group(required=True)
    seed.add_argument('--parent', type=Path)
    seed.add_argument('--state', type=Path)
    step.add_argument('--cards', type=Path)
    step.add_argument('--sequence', required=True, help='e.g. right:20,wait:10,up:90,wait:600')
    step.add_argument('--expect-stage')
    step.add_argument('--allow-death', action='store_true', help='record a deliberate death/continue regression')
    step.add_argument('--native-every', type=int, default=300)
    step.add_argument('--checkpoint-interval', type=int, default=600)
    step.add_argument('--timeout', type=int, default=300)
    step.add_argument('--period-us', type=int, default=1000)
    step.add_argument('--runtime', type=Path, default=BINARY)
    step.add_argument('--config', type=Path, default=CONFIG)
    args = parser.parse_args()
    if args.command == 'inspect':
        print(json.dumps(inspect_ram(args.ram.read_bytes()), indent=2))
        return 0
    output = args.session.resolve()
    if output.exists():
        raise ValueError('destination already exists; recordings are immutable')
    if args.parent:
        parent = args.parent.resolve()
        state, vblank = terminal_checkpoint(parent)
        cards = args.cards or parent / 'cards'
    else:
        state = args.state.resolve()
        match = re.fullmatch(r'checkpoint_\d+_vblank_(\d+)\.ps1s', state.name)
        if not match:
            raise ValueError('explicit seed must be a numbered VBlank checkpoint')
        vblank = int(match[1])
        cards = args.cards or ROOT / 'data/memory_cards'
    for name in ('card1.mcd', 'card2.mcd'):
        if not (cards / name).is_file():
            raise ValueError(f'missing inherited card: {cards / name}')
    frames, script = intervals(args.sequence, vblank + 1)
    inputs = output.parent / '.inputs'
    inputs.mkdir(parents=True, exist_ok=True)
    input_file = inputs / (output.name + '.input')
    with input_file.open('x') as f:
        f.write(script)
    ancestry = {'format': 'mgs-campaign-step-1', 'parent': str(args.parent) if args.parent else None,
                'initial': str(state), 'initial_sha256': sha256(state),
                'initial_vblank': vblank, 'sequence': args.sequence,
                'expected_stage': args.expect_stage,
                'inherited_cards': {name: sha256(cards / name) for name in ('card1.mcd', 'card2.mcd')}}
    command = [sys.executable, str(ROOT / 'scripts/mgs_tas.py'), 'record', str(output),
               '--state', str(state), '--cards', str(cards), '--input-script', str(input_file),
               '--frames', str(frames), '--headless', '--runtime', str(args.runtime),
               '--config', str(args.config), '--end-at-checkpoint']
    for key in ('native_every', 'checkpoint_interval', 'timeout', 'period_us'):
        command += ['--' + key.replace('_', '-'), str(getattr(args, key))]
    run = subprocess.run(command, cwd=ROOT)
    ancestry['recording_returncode'] = run.returncode
    result_path = output / 'result.json'
    result = json.loads(result_path.read_text()) if result_path.exists() else {}
    ancestry['requested_frames'] = frames
    ancestry['recorded_tail_frames'] = result.get('movie_frames', 0) - frames
    native = bool(result) and not result.get('interpreter_counters') and not result.get('game_errors')
    ancestry['native_execution_without_game_errors'] = native
    ram = output / 'final-ram.bin'
    if ram.exists():
        ancestry['final'] = inspect_ram(ram.read_bytes())
    reached = not args.expect_stage or ancestry.get('final', {}).get('stage') == args.expect_stage
    ancestry['expected_stage_reached'] = reached
    alive = args.allow_death or ancestry.get('final', {}).get('life', 0) > 0
    ancestry['alive_requirement_satisfied'] = alive
    try:
        terminal, _ = terminal_checkpoint(output)
        ancestry['terminal_checkpoint'] = str(terminal.relative_to(output))
        chainable = True
    except (OSError, ValueError, KeyError) as error:
        ancestry['terminal_checkpoint_error'] = str(error)
        chainable = False
    if output.is_dir():
        write_json(output / 'route-step.json', ancestry)
    return run.returncode or (0 if reached and alive and native and chainable else 2)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError) as error:
        print(f'Campaign: {error}', file=sys.stderr)
        sys.exit(1)
