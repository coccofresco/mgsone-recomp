#!/usr/bin/env python3
"""Audit paired native loading runs without confusing scripted waits with I/O.

Use mgs_scenarios.py run --trace-loading and identical configurations except
game.fast_loading. Disc LBAs and stage names are resolved from the retail ISO.
The report measures original loader actor lifetimes at VBlank resolution,
not physical monitor latency or the duration of cutscenes/fades.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import tomllib

from mgs_assets import Disc
from mgs_tas import sha256, write_json

ASSETS = {'/MGS/STAGE.DIR', '/MGS/RADIO.DAT', '/MGS/FACE.DAT'}


def texture_inventory(ram):
    """USA DG_TEXTURE inventory; excludes only the live/resident usage byte.

    Equal sector counts do not prove PCX init succeeded: a depleted transient
    heap silently skips uploads. Compare registered IDs and their full layout.
    Empty slots and hash-table slot order are not material properties.
    """
    if len(ram) != 2 * 1024 * 1024:
        raise ValueError('expected complete USA RAM witness')
    rows = []
    for slot in range(512):
        record = ram[0xb46c8 + slot*12:0xb46c8 + (slot+1)*12]
        texture, = struct.unpack_from('<H', record)
        if texture:
            rows.append((texture, (record[:2] + record[3:]).hex()))
    rows.sort()
    if len({row[0] for row in rows}) != len(rows):
        raise ValueError('duplicate registered texture ID')
    return dict(count=len(rows), records=rows,
                sha256=hashlib.sha256(b''.join(bytes.fromhex(r[1]) for r in rows)).hexdigest())


def compare_textures(slow_ram, fast_ram):
    before, after = texture_inventory(slow_ram), texture_inventory(fast_ram)
    if before['records'] != after['records']:
        raise ValueError(f"registered texture content differs: {before['count']} paced, {after['count']} fast")
    return {k:before[k] for k in ('count', 'sha256')}


def texture_witness(ram):
    """Compact material-content witness for all scene/transition reports."""
    inventory = texture_inventory(ram)
    return {k: inventory[k] for k in ('count', 'sha256')}


def catalog(disc):
    files = disc.files()
    entry = files['/MGS/STAGE.DIR']
    table = disc.read(entry['lba'], 2048)
    size, = struct.unpack_from('<I', table)
    if not 0 < size <= 2044 or size % 12:
        raise ValueError('invalid retail stage table')
    stages = {}
    for offset in range(4, 4 + size, 12):
        name, sector = struct.unpack_from('<8sI', table, offset)
        stages[entry['lba'] + sector] = name.split(b'\0')[0].decode('ascii')
    return files, stages


def analyze(rows, files, stages):
    loads, requests = [], []
    active = request = None
    unpaced = 0
    for row in rows:
        event, frame = row['event'], row['frame']
        if event == 'cdrom.read_begin':
            request = dict(lba=row['target_lba'], begin=frame, sectors=0)
            requests.append(request)
        elif event == 'cdrom.sector_read':
            names = [name for name, entry in files.items()
                     if entry['lba'] <= row['lba'] < entry['lba'] + (entry['size'] + 2047) // 2048]
            if row['unpaced']:
                if len(names) != 1 or names[0] not in ASSETS:
                    raise ValueError('accelerated sector outside explicit data archives')
                unpaced += 1
            if request is not None:
                request['sectors'] += 1
                request['last'] = frame
        elif event == 'mgs.loading_sample':
            if row['loader'] < 0 and active:
                raise ValueError('invalid actor lists during loading')
            if row['loader'] > 0 and active is None:
                active = dict(begin=frame)
            elif row['loader'] == 0 and active is not None:
                matching = [r for r in requests if active['begin'] <= r['begin'] < frame
                            and r['lba'] in stages]
                if not matching:
                    raise ValueError('loader interval has no identified stage read')
                active.update(end=frame, vblanks=frame-active['begin'],
                              seconds_at_60hz=(frame-active['begin']) / 60,
                              stages=[stages[r['lba']] for r in matching],
                              sectors=[r['sectors'] for r in matching])
                loads.append(active)
                active = None
    if active or not loads:
        raise ValueError('missing or incomplete loader interval')
    return dict(loads=loads, unpaced_sectors=unpaced)


def session(path, files, stages):
    result = json.loads((path / 'result.json').read_text())
    if result['status'] != 'complete' or result.get('game_errors') or result.get('interpreter_counters'):
        raise ValueError('session did not complete cleanly')
    config = result['identity']['config']
    if sha256(Path(config['path'])) != config['sha256']:
        raise ValueError('recorded configuration changed after the run')
    trace = path / 'loading.jsonl'
    evidence = analyze((json.loads(s) for s in trace.open()), files, stages)
    evidence.update(session=str(path.resolve()), result_sha256=sha256(path / 'result.json'),
                    trace_sha256=sha256(trace), runtime_sha256=result['runtime_sha256'])
    return evidence, result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('slow', type=Path)
    parser.add_argument('fast', type=Path)
    parser.add_argument('--disc', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('refusing to overwrite evidence')
    files, stages = catalog(Disc(args.disc))
    slow, before = session(args.slow, files, stages)
    fast, after = session(args.fast, files, stages)
    if slow['runtime_sha256'] != fast['runtime_sha256']:
        raise ValueError('A/B runs must use the same executable')
    configs = [tomllib.loads(Path(r['identity']['config']['path']).read_text()) for r in (before, after)]
    if configs[0]['game'].pop('fast_loading') or not configs[1]['game'].pop('fast_loading'):
        raise ValueError('expected fast_loading=false then true')
    if configs[0] != configs[1]:
        raise ValueError('A/B configurations differ beyond fast_loading')
    parents = [json.loads((p.parent / 'result.json').read_text()) for p in (args.slow, args.fast)]
    if any(p['status'] != 'complete' or p.get('failures') for p in parents):
        raise ValueError('scenario contract did not complete cleanly')
    for key in ('route_sha256', 'scenario'):
        if parents[0][key] != parents[1][key]:
            raise ValueError('A/B routes or source cards differ')
    keys = ('stage', 'current_stage', 'difficulty', 'disc', 'life', 'max_life', 'weapons', 'items')
    for key in keys:
        if before['game_state'][key] != after['game_state'][key]:
            raise ValueError(f'gameplay endpoint differs: {key}')
    textures = compare_textures((args.slow/'final-ram.bin').read_bytes(),
                                (args.fast/'final-ram.bin').read_bytes())
    if slow['unpaced_sectors'] or not fast['unpaced_sectors']:
        raise ValueError('expected distinct paced and accelerated paths')
    if len(slow['loads']) != len(fast['loads']):
        raise ValueError('A/B loader sequences differ')
    comparisons = []
    for a, b in zip(slow['loads'], fast['loads']):
        if (a['stages'], a['sectors']) != (b['stages'], b['sectors']):
            raise ValueError('stage read sequence/payload sector count changed')
        if b['vblanks'] > a['vblanks']:
            raise ValueError('accelerated loader regressed')
        comparisons.append(dict(stages=a['stages'], sectors=a['sectors'],
                                slow_vblanks=a['vblanks'], fast_vblanks=b['vblanks'],
                                reduction_percent=100 * (1 - b['vblanks']/a['vblanks'])))
    report = dict(format='mgs-loading-comparison-1', status='complete', scope=__doc__,
                  disc_sha256=sha256(args.disc), slow=slow, fast=fast,
                  comparisons=comparisons, matching_endpoint_fields=list(keys),
                  matching_textures=textures,
                  scenario_results=[dict(path=str(p.parent / 'result.json'),
                      sha256=sha256(p.parent / 'result.json')) for p in (args.slow, args.fast)])
    write_json(args.output, report)
    print(json.dumps(dict(status='complete', comparisons=comparisons)))


if __name__ == '__main__':
    main()
