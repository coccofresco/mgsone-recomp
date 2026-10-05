#!/usr/bin/env python3
"""Index an observed original-emulator TAS trace and locate native checkpoints.

Frame labels are PSXjin movie.framecount(), not CSV row numbers. Matching poses
are alignment candidates, never proof of native gameplay or equal execution.
"""
import argparse
import csv
import itertools
import json
from pathlib import Path

from mgs_tas import sha256, write_json
from mgs_tas_import import parse_pjm
from mgs_tas_route import snapshot

FIELDS = ('frame stage story difficulty x y z yaw pad_origin life stance pause '
          'game_status player_status alert weapon item').split()
TICK_FIELDS = ['game_tick', 'pad0', 'pad1']


def read_trace(path):
    frames = {}
    duplicates = 0
    previous = -1
    with Path(path).open(newline='') as stream:
        source = csv.DictReader(stream, delimiter='\t')
        if source.fieldnames not in (FIELDS, FIELDS + TICK_FIELDS):
            raise ValueError('unsupported reference trace columns')
        for number, row in enumerate(source, 2):
            if None in row or any(v is None for v in row.values()):
                raise ValueError(f'incomplete reference row {number}; wait for trace flush/exit')
            row = {key: value if key == 'stage' else int(value) for key, value in row.items()}
            frame = row['frame']
            if frame < 0 or (frame < previous and any(r['stage'] for r in frames.values())):
                raise ValueError(f'non-monotone gameplay frame at row {number}')
            duplicates += frame in frames
            frames[frame] = row
            previous = frame
    if not frames:
        raise ValueError('empty reference trace')
    return list(frames.values()), duplicates


def ready(row):
    return (row['yaw'] >= 0 and row['pause'] == 0 and
            not row['game_status'] & 0xdc080000 and not row['player_status'] & 0x20000000)


def index_trace(rows):
    intervals = []
    for _, group in itertools.groupby(rows, lambda r: (r['stage'], r['story'], r['difficulty'])):
        part = list(group)
        intervals.append({'stage': part[0]['stage'], 'story': part[0]['story'],
                          'difficulty': part[0]['difficulty'],
                          'first_frame': part[0]['frame'], 'last_frame': part[-1]['frame'],
                          'first_ready_frame': next((r['frame'] for r in part if ready(r)), None),
                          'first': part[0], 'last': part[-1]})
    return intervals


def matches(rows, state, first, last, tolerance):
    """Preserve stationary/recurring ambiguity as separate candidate intervals."""
    candidates = []
    for row in rows:
        if not first <= row['frame'] <= last:
            continue
        if any(row[k] != state[k] for k in ('stage', 'story', 'difficulty', 'stance')):
            continue
        if row['pause'] != state['pause_level'] or (row['yaw'] >= 0) != bool(state['control']):
            continue
        distance = sum((row[k] - state[k]) ** 2 for k in ('x', 'y', 'z'))
        if distance <= tolerance ** 2:
            candidates.append((row, distance))
    groups = []
    for _, group in itertools.groupby(enumerate(candidates), lambda item: item[1][0]['frame'] - item[0]):
        part = [item[1] for item in group]
        best, distance = min(part, key=lambda item: item[1])
        yaw_error = (abs((best['yaw'] - state['yaw'] + 2048) % 4096 - 2048)
                     if best['yaw'] >= 0 and state.get('yaw') is not None else None)
        groups.append({'first_frame': part[0][0]['frame'], 'last_frame': part[-1][0]['frame'],
                       'nearest_frame': best['frame'], 'position_error': round(distance ** .5, 3),
                       'yaw_error': yaw_error,
                       'reference': best})
    return sorted(groups, key=lambda group: group['position_error'])


def sampled_runs(rows, first, last):
    """Use pads observed after game updates, excluding the initial anchor tick.

    GV_Time advances before dgd.c waits for video and reads the next pad. Use
    the last observation of each tick: its first observation can still contain
    the preceding tick's pad. A script can also mask an interaction *within*
    the same tick; reject that ambiguous transition instead of erasing it.
    Repeated VBlanks add no input ticks. These are observed game pad states,
    which may include scripted demo inputs, not a CPU input-read trace.
    """
    by_frame = {r['frame']: r for r in rows}
    if first not in by_frame or last not in by_frame or 'game_tick' not in by_frame[first]:
        raise ValueError('retiming requires a complete sampled-pad trace over both anchors')
    previous = by_frame[first]['game_tick']
    pads = []
    for _, group in itertools.groupby(rows, lambda r: r.get('game_tick')):
        group = list(group)
        row = group[-1]
        if not first < row['frame'] <= last:
            continue
        tick = row['game_tick']
        if tick == previous:
            continue
        if (tick - previous) & 0xffffffff != 1:
            raise ValueError('missing/reset source game tick; split at this transition')
        for earlier in group[:-1]:
            if (earlier.get('game_status', 0) != row.get('game_status', 0) and
                    any(earlier[f'pad{p}'] & ~row[f'pad{p}'] & 0xf00f for p in (0, 1))):
                raise ValueError(f'filtered interaction at source frame {earlier["frame"]}; '
                                 'split here and gate the original PJM action on game state')
        sample = row['pad0'], row['pad1']
        if any(not 0 <= p <= 65535 for p in sample):
            raise ValueError('invalid sampled controller mask')
        pads.append(sample)
        previous = tick
    if not pads:
        raise ValueError('no observed game updates between anchors')
    return [[sum(1 for _ in group), *pad] for pad, group in itertools.groupby(pads)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    index = sub.add_parser('index')
    index.add_argument('trace', type=Path)
    index.add_argument('destination', type=Path)
    index.add_argument('--movie', type=Path, required=True)
    index.add_argument('--catalog', type=Path)
    match = sub.add_parser('match')
    match.add_argument('trace', type=Path)
    match.add_argument('ram', type=Path, help='2 MiB RAM dump or native PS1S checkpoint')
    match.add_argument('--first', type=int, required=True)
    match.add_argument('--last', type=int, required=True)
    match.add_argument('--tolerance', type=int, default=250)
    retime = sub.add_parser('retime')
    retime.add_argument('trace', type=Path)
    retime.add_argument('destination', type=Path)
    retime.add_argument('--movie', type=Path, required=True)
    retime.add_argument('--first', type=int, required=True)
    retime.add_argument('--last', type=int, required=True)
    retime.add_argument('--until-stage', required=True)
    args = parser.parse_args()
    rows, duplicates = read_trace(args.trace)
    if args.command == 'retime':
        metadata, _, _ = parse_pjm(args.movie.read_bytes())
        runs = sampled_runs(rows, args.first, args.last)
        recipe = {'format': 'mgs-tas-route-1', 'source_sha256': metadata['source_sha256'],
                  'source_url': 'https://tasvideos.org/3462S',
                  'description': 'Original TAS pads sampled by the game, replayed on native GV_Time updates.',
                  'reference_evidence': {'path': str(args.trace), 'sha256': sha256(args.trace),
                                         'frames': [args.first, args.last], 'game_ticks': sum(r[0] for r in runs)},
                  'steps': [{'name': 'original-sampled-inputs',
                             'guard': {'difficulty': 3, 'life': {'min': 1}},
                             'until': {'stage': args.until_stage, 'control': {'ne': 0}},
                             'action': {'sampled_game_runs': runs},
                             'max_frames': sum(r[0] for r in runs) * 4 + 1200}]}
        if args.destination.exists():
            raise ValueError('refusing to overwrite a retimed recipe')
        write_json(args.destination, recipe)
        print(f'{sum(r[0] for r in runs)} sampled game ticks, {len(runs)} pad runs')
        return
    if args.command == 'match':
        ram = args.ram.read_bytes()
        if len(ram) != 2097152:
            # Native SaveState header: magic/version/context, then complete RAM.
            if (ram[:4] != b'PS1S' or int.from_bytes(ram[4:8], 'little') != 7 or
                    len(ram) < 476 + 2097152):
                raise ValueError('not complete RAM or a native checkpoint')
            ram = ram[476:476 + 2097152]
        state = snapshot(ram)
        print(json.dumps({'native': state, 'candidates': matches(
            rows, state, args.first, args.last, args.tolerance),
            'scope': 'pose/state candidates; verify nearby inputs and actor phases'}, indent=2))
        return
    metadata, _, _ = parse_pjm(args.movie.read_bytes())
    if rows[-1]['frame'] > metadata['frames']:
        raise ValueError('reference trace extends beyond source movie')
    intervals = index_trace(rows)
    if args.catalog:
        catalog = json.loads(args.catalog.read_text())
        stages = {s['stage']: s for s in catalog['stages']}
        for interval in intervals:
            entry = stages.get(interval['stage'], {})
            interval['script_resources'] = [{k: s[k] for k in ('resource', 'sha256')}
                                            for s in entry.get('scripts', [])]
    result = {'format': 'mgs-tas-reference-index-1', 'trace_sha256': sha256(args.trace),
              'source': metadata, 'first_frame': rows[0]['frame'], 'last_frame': rows[-1]['frame'],
              'observed_frames': len(rows), 'duplicate_frame_rows': duplicates,
              'missing_frames': rows[-1]['frame'] - rows[0]['frame'] + 1 - len(rows),
              'intervals': intervals,
              'scope': 'PSXjin reference execution only; not native validation'}
    if args.catalog:
        result['catalog_sha256'] = sha256(args.catalog)
    if args.destination.exists():
        raise ValueError('refusing to overwrite a reference index')
    write_json(args.destination, result)
    print(f"{len(rows)} observed frames, {len(intervals)} stage/story intervals; {args.destination}")


if __name__ == '__main__':
    main()
