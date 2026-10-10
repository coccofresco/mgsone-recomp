#!/usr/bin/env python3
"""Validate the real USA Disc 1 -> 2 transition using isolated, input-only TAS.

Start from the s12c warehouse checkpoint documented in mgs-disc-change.md.
The recipe reaches the exit, waits for original disc verification, then moves
Snake in s13a. No story, inventory, position or filesystem RAM is injected.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys

from mgs_tas import ROOT, game_state, sha256, write_json
from mgs_tas_route import actor_counts, compile_route


def require_gameplay(state, actors):
    """A stage name alone also matches loading and the fade back to Disc 1."""
    assert (state['stage'], state['disc'], state['pause_level']) == ('s13a', 2, 0), state
    assert state['control'] and state['load_request'] == 0, state
    assert not state['game_status'] & 3691511808, state
    assert not state['player_status'] & 536870912, state
    x, _, z = state['player_position']
    assert 7000 < x < 8500 and 8000 < z < 9800, state
    assert actors is not None and actors.get('snake.c') == 1, actors
    assert all(actors.get(name, 0) == 0 for name in ('change.c', 'loader.c', 'fadeio.c')), actors


def audit(session):
    result = json.loads((session / 'result.json').read_text())
    metrics = json.loads((session / 'metrics.json').read_text())['counters']
    log = (session / 'runtime.log').read_text(errors='replace')
    ram = (session / 'final-ram.bin').read_bytes()
    state = game_state(ram)
    initial = (session / 'initial.ps1s').read_bytes()[476:476 + 2097152]
    before = game_state(initial)
    actors = actor_counts(ram)
    assert result['status'] == 'complete' and not result['game_errors'], result
    assert not result.get('interpreter_counters'), result.get('interpreter_counters')
    assert 'CD timeout' not in log, 'libcd timed out during the transition'
    assert metrics.get('mgs.disc.automatic_switch') == 1, metrics
    assert metrics.get('mgs.disc.original_validation_complete') == 1, metrics
    assert (before['stage'], before['disc']) == ('s12c', 1), before
    require_gameplay(state, actors)
    assert struct.unpack_from('<I', ram, 0xAF368)[0] == 1, 'original FS_DiskNum'
    for name in ('life', 'max_life', 'difficulty', 'weapons', 'items'):
        assert before[name] == state[name], (name, before[name], state[name])
    frames = 0
    for line in (session / 'movie.mgst').read_text().splitlines():
        if line.startswith('F '):
            fields = list(map(int, line.split()[1:]))
            assert fields[4] & 8, 'the automated transition must not inject Start'
            frames += 1
    return {'scope': 's12c exit -> automatic disc change -> controlled movement in s13a',
            'frames': frames, 'initial': before, 'final': state,
            'final_actors': actors, 'stable_gameplay_vblanks': 60,
            'switches': 1, 'original_validations': 1, 'cd_timeouts': 0,
            'start_presses': 0, 'interpreter_calls': 0,
            'seed_sha256': sha256(session / 'initial.ps1s'),
            'movie_sha256': sha256(session / 'movie.mgst'),
            'runtime_sha256': result['runtime_sha256']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--state', type=Path, required=True)
    parser.add_argument('--vulkan', action='store_true', help='hidden 4K Vulkan run with bounded captures')
    parser.add_argument('--replay', action='store_true', help='also verify the recorded movie once')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    recipe = json.loads((ROOT / 'docs/mgs-disc-change.route.json').read_text())
    route = output / 'route.mgsr'
    route.write_text(compile_route(recipe, [], {}))
    session = output / 'session'
    rendering = ['--hidden', '--capture-every', '60'] if args.vulkan else ['--headless']
    command = [sys.executable, str(ROOT / 'scripts/mgs_tas.py'), 'record', str(session),
               '--state', str(args.state.resolve()), '--native-route', str(route),
               '--frames', '4500', '--timeout', '180', '--end-at-checkpoint',
               '--checkpoint-interval', '60', '--native-every', '60', *rendering]
    subprocess.run(command, cwd=ROOT, check=True)
    evidence = audit(session)
    if args.replay:
        replay = output / 'replay'
        subprocess.run([sys.executable, str(ROOT / 'scripts/mgs_tas.py'), 'replay', str(session),
                        '--output', str(replay), '--headless', '--timeout', '180'], cwd=ROOT, check=True)
        result = json.loads((replay / 'result.json').read_text())
        assert result['status'] == 'complete' and not result['game_errors'], result
        assert not result.get('interpreter_counters'), result
        assert 'CD timeout' not in (replay / 'runtime.log').read_text(errors='replace')
        recorded = json.loads((session / 'result.json').read_text())
        for key in ('completed_frames', 'terminal_boundary', 'game_state', 'game_ticks'):
            assert result[key] == recorded[key], (key, result[key], recorded[key])
        require_gameplay(result['game_state'], actor_counts((replay / 'final-ram.bin').read_bytes()))
        evidence['replay_result'] = str(replay / 'result.json')
    write_json(output / 'evidence.json', evidence)
    print(json.dumps(evidence, indent=2))


if __name__ == '__main__':
    main()
