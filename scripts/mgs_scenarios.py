#!/usr/bin/env python3
"""Independent USA scene checks through retail memory-card loading.

Save payloads are never edited. Each case uses isolated cards and reports only
load/control coverage, not a completed fight or a continuous campaign.
"""
import argparse
import functools
import json
import operator
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import zlib

from mgs_tas import CONFIG, ROOT, identity, sha256, write_json
from mgs_tas_route import compile_route, snapshot


def card_entries(card):
    if len(card) != 131072 or card[:2] != b'MC':
        raise ValueError('expected a raw 128 KiB PS1 memory card')
    entries = []
    for block in range(1, 16):
        directory = card[block * 128:(block + 1) * 128]
        if directory[0] != 0x51:
            continue
        if functools.reduce(operator.xor, directory) != 0:
            raise ValueError(f'bad directory checksum in block {block}')
        name = directory[10:30].split(b'\0')[0].decode('ascii')
        if not name.startswith('BASLUS-00594G'):
            raise ValueError(f'not a USA MGS save: {name}')
        if struct.unpack_from('<I', directory, 4)[0] != 8192 or directory[8:10] != b'\xff\xff':
            raise ValueError('only complete single-block MGS saves are supported')
        payload = card[block * 8192:(block + 1) * 8192]
        size, crc = struct.unpack_from('<II', payload, 0x100)
        if size != 0xa40:
            raise ValueError('unsupported MGS save record size')
        data = payload[0x108:0x100 + size]
        if zlib.crc32(data) != crc:
            raise ValueError(f'bad save CRC in block {block}')
        version, variable_bytes = struct.unpack_from('<II', data)
        if (version, variable_bytes) != (0x5f, 0x800):
            raise ValueError('save layout is not USA; Integral is not interchangeable')
        stage = data[24:40].split(b'\0')[0].decode('ascii')
        if not stage or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789_' for c in stage):
            raise ValueError('invalid saved stage')
        linked = struct.unpack_from('<95h', data, 0x38)
        story = struct.unpack_from('<h', data, 0x38 + 190 + 10)[0]
        entries.append(dict(block=block, name=name, stage=stage, story=story,
                            difficulty=linked[1], disc=linked[4], life=linked[11],
                            max_life=linked[12], stance=linked[16]))
    return entries


def isolated_card(card, block=None):
    entries = card_entries(card)
    if block is not None and block not in [e['block'] for e in entries]:
        raise ValueError('selected block has no supported save')
    output = bytearray(card)
    for index in range(1, 16):
        if index == block:
            continue
        directory = bytearray(128)
        directory[0] = 0xa0
        directory[8:10] = b'\xff\xff'
        directory[-1] = functools.reduce(operator.xor, directory[:-1])
        output[index * 128:(index + 1) * 128] = directory
        output[index * 8192:(index + 1) * 8192] = bytes(8192)
    return bytes(output)


def control_expected(entry):
    expected = {k: entry[k] for k in ('stage', 'story', 'difficulty', 'disc')}
    if (entry['stage'], entry['story']) == ('s09a', 137):
        # USA ea54.gcx SHA256 342760667d67d9a88f7b15a836ab4b607dfd2f5c42b45eb9c4e7948d0ce6b3a9:
        # main @8019 assigns 138, then demo @8057 calls fafa (63994),
        # whose eval @6238 assigns 141. Reloading this save plays that scene.
        expected['story'] = 141
    elif (entry['stage'], entry['story']) == ('s11b', 173):
        # USA ea54.gcx SHA256 a8709ddead8f93f5ab855e592bb113caff18d3c852674340b36dd641e9a588cd:
        # main @10872 assigns 174 when story <174, immediately on entry.
        expected['story'] = 174
    elif (entry['stage'], entry['story']) == ('s17a', 253):
        # USA ea54.gcx e7e31410d01191af79ec8a7a87f7d98fe20a5d8cbb56f4f3a9e63e6f9700772f:
        # main @8583 sets 255 before demo s2401a0 (@8621).
        expected['story'] = 255
    return expected


def load_recipe(entry, control_check=True):
    saved = {k: entry[k] for k in ('stage', 'story', 'difficulty', 'disc')}
    expected = control_expected(entry)
    ready = dict(expected, control={'ne': 0}, pause_level=0,
                 game_status={'mask': 0xdc080000, 'eq': 0},
                 player_status={'mask': 0x20000000, 'eq': 0})
    safe = dict(expected, life={'min': 1}, game_over_timer=0)
    steps = [
        dict(name='startup', until=dict(stage='title', title_state=6),
             action=dict(sequence='start:6,wait:120'), attempts=30, max_frames=4000),
        dict(name='main-menu', until=dict(stage='title', title_state=7),
             action=dict(sequence='start:6,wait:120'), attempts=20, max_frames=2600),
        dict(name='select-load', guard=dict(stage='title', title_state=7),
             until=dict(title_selection=1), action=dict(sequence='wait:8,down:6,wait:20'),
             attempts=7, max_frames=300),
        dict(name='load-only-save', until=saved,
             action=dict(sequence='wait:30,circle:6,wait:90'), attempts=55, max_frames=7000),
        dict(name='wait-control', until=ready, stable_frames=30,
             guard=dict(life={'min': 1}, game_over_timer=0),
             action=dict(sequence='wait:30,circle:6,wait:90'), attempts=30, max_frames=4000),
    ]
    if expected != saved or entry['stage'] in ('s18a', 's19a'):
        # The mission-log UI (abst.c) is unpaused but still needs Circle.
        # Stop confirming as soon as Snake is allocated; keep neutral input
        # through the following cutscene so Circle cannot become a punch.
        confirmation = dict(steps[-1], name='confirm-mission-log',
                            until=dict(control={'ne': 0}), stable_frames=1)
        steps.insert(-1, confirmation)
        del steps[-1]['action']
    if control_check:
        steps += [
            dict(name='crouch', until=dict(ready, stance=1), stable_frames=3,
                 guard=safe, action=dict(sequence='wait:18,cross:6,wait:30'),
                 attempts=8, max_frames=500),
            dict(name='stand', until=dict(ready, stance=0), stable_frames=30,
                 guard=safe, action=dict(sequence='wait:18,cross:6,wait:30'),
                 attempts=8, max_frames=500),
        ]
        if entry['stage'] == 's18a':
            # Snake18 (Liquid fight) keeps posture in f8FE. Its CD768/CDE8C
            # routines set/clear PLAYER_SQUAT, without updating linked stance.
            steps[-2].update(name='duck', until=dict(ready, player_status={'mask': 0x20000020, 'eq': 0x20}))
            steps[-1].update(name='recover', until=dict(ready, player_status={'mask': 0x20000020, 'eq': 0}))
    return {'format': 'mgs-tas-route-1', 'steps': steps}


def prepare(source, block, destination):
    raw = source.read_bytes()
    entries = card_entries(raw)
    entry = next((e for e in entries if e['block'] == block), None)
    if entry is None or entry['stage'] == 'rank':
        raise ValueError('select a gameplay save block')
    first, second = isolated_card(raw, block), isolated_card(raw)
    destination.mkdir(parents=True, exist_ok=False)
    (destination / 'cards').mkdir()
    (destination / 'cards/card1.mcd').write_bytes(first)
    (destination / 'cards/card2.mcd').write_bytes(second)
    entry.update(format='mgs-scene-case-1', source_card=str(source.resolve()),
                 source_sha256=sha256(source),
                 card_sha256={n: sha256(destination / 'cards' / n)
                              for n in ('card1.mcd', 'card2.mcd')})
    write_json(destination / 'scenario.json', entry)
    return entry


def run(args):
    case, output = args.scenario.resolve(), args.output.resolve()
    entry = json.loads((case / 'scenario.json').read_text())
    if args.timeout <= 0 or args.period_us < 0:
        raise ValueError('timeout must be positive and period must be nonnegative')
    for name, digest in entry['card_sha256'].items():
        if sha256(case / 'cards' / name) != digest:
            raise ValueError('scenario card changed')
    cue = args.cue or ROOT / f'data/discs/usa/disc{entry["disc"]}/Metal Gear Solid (USA) (Disc {entry["disc"]}) (v1.0).cue'
    cue = cue.resolve(strict=True)
    output.mkdir(parents=True, exist_ok=False)
    recipe = load_recipe(entry, not args.load_only)
    write_json(output / 'recipe.json', recipe)
    route = output / 'route.mgsr'
    route.write_text(compile_route(recipe, [], {}))
    env = {k: v for k, v in os.environ.items() if not k.startswith('PS1_')}
    binaries = {}
    with (output / 'driver.log').open('w') as log:
        if args.engine == 'native':
            # Both USA 1.0 discs have byte-identical executable/overlays.
            # Mount the selected image in an isolated config; the user's
            # normal launcher configuration stays untouched.
            config = output / 'native.toml'
            config.write_text(config_with_cue(CONFIG.read_text(), cue))
            boot = output / 'boot'
            subprocess.run([sys.executable, str(ROOT / 'scripts/mgs_tas.py'),
                            'bootstrap', str(boot), '--cards', str(case / 'cards'), '--config', str(config)],
                           cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
            session = output / 'session'
            command = [sys.executable, str(ROOT / 'scripts/mgs_tas.py'), 'record', str(session),
                       '--config', str(config),
                       '--state', str(boot / 'initial.ps1s'), '--cards', str(boot / 'cards'),
                       '--native-route', str(route), '--headless', '--end-at-checkpoint',
                       '--period-us', str(args.period_us), '--native-every', '600',
                       '--checkpoint-interval', '1200', '--timeout', str(args.timeout), '--trace-private']
        else:
            session = output / 'session'
            work = output / 'work'
            (work / 'save').mkdir(parents=True)
            (work / 'system').mkdir()
            for n in (1, 2):
                shutil.copy2(case / f'cards/card{n}.mcd', work / f'save/duckstation_shared_card_{n}.mcd')
            shutil.copy2(ROOT / '.local/swanstation/dep/openbios/openbios.bin', work / 'system/openbios.bin')
            env.update(PS1_DONOR_WORK_DIRECTORY=str(work), PS1_DONOR_ROUTE=str(route),
                       PS1_DONOR_ROUTE_OUTPUT=str(session), PS1_DONOR_BENCHMARK='1')
            binaries = {name: dict(path=str(path.resolve()), sha256=sha256(path))
                        for name, path in {
                            'runner': args.donor_runner,
                            'core': ROOT / '.local/swanstation/swanstation_libretro.so',
                            'bios': work / 'system/openbios.bin', 'cue': cue}.items()}
            for index, name in enumerate(re.findall(r'^\s*FILE\s+"([^"]+)"', cue.read_text(), re.M)):
                track = cue.parent / name
                binaries[f'track_{index}'] = dict(path=str(track), sha256=sha256(track))
            command = [str(args.donor_runner.resolve()), str(ROOT / '.local/swanstation/swanstation_libretro.so'),
                       str(cue.resolve()), '19000', 'swanstation_GPU_Renderer=Software',
                       'swanstation_CPU_ExecutionMode=Interpreter', 'swanstation_BIOS_PatchFastBoot=true',
                       'swanstation_BIOS_PathNTSCU=openbios.bin', 'swanstation_CPU_Overclock=165',
                       'swanstation_MemoryCards_Card1Type=Shared', 'swanstation_MemoryCards_Card2Type=Shared']
        try:
            executed = subprocess.run(command, cwd=ROOT, env=env, stdout=log,
                                      stderr=subprocess.STDOUT, timeout=args.timeout + 10)
            code = executed.returncode
        except subprocess.TimeoutExpired:
            code = 124
    result_path = session / 'result.json'
    result = json.loads(result_path.read_text()) if result_path.exists() else {}
    complete = code == 0 and result.get('status') == 'complete'
    failures = []
    if not complete:
        failures.append(f"runner exit {code}, route status {result.get('status', 'missing')}")
    if args.engine == 'native':
        complete &= ('[Route] status: complete ' in result.get('route_log', [])
                     and not result.get('interpreter_counters') and not result.get('game_errors'))
        binaries = result.get('identity', {})
        if result.get('interpreter_counters'):
            failures.append(f"interpreted execution: {result['interpreter_counters']}")
        failures.extend(result.get('game_errors', []))
        if '[Route] status: complete ' not in result.get('route_log', []):
            failures.append('route did not complete')
    else:
        failures.extend(line for line in (output / 'driver.log').read_text(errors='replace').splitlines()
                        if line.startswith('[Route] failed '))
    report = dict(format='mgs-scene-result-1', status='complete' if complete else 'failed',
                  engine=args.engine, returncode=code, scenario=entry,
                  route_sha256=sha256(route), session=str(session), binaries=binaries,
                  failures=failures,
                  expected_control=control_expected(entry),
                  checks=['retail-save-load', 'stable-control'] + ([] if args.load_only else [s['name'] for s in recipe['steps'][-2:]]),
                  scope='Only these scene checks; no boss completion or campaign continuity claim.')
    if (session / 'final-ram.bin').exists():
        report['final_ram_sha256'] = sha256(session / 'final-ram.bin')
        state = snapshot((session / 'final-ram.bin').read_bytes())
        report['final_state'] = state
        matches = all(state[k] == v for k, v in control_expected(entry).items())
        if not matches:
            report['status'] = 'failed'
            failures.append('terminal stage/story/difficulty/disc differs from scenario contract')
    else:
        report['status'] = 'failed'
        failures.append('terminal RAM is missing')
    write_json(output / 'result.json', report)
    print(f"{entry['stage']} story {entry['story']}: {report['status']} ({args.engine}) — {output}")
    return 0 if report['status'] == 'complete' else 1


def config_with_cue(text, cue):
    sections = re.split(r'(?=^\[)', text, flags=re.M)
    replaced = 0
    for index, section in enumerate(sections):
        if section.startswith('[disc]\n'):
            sections[index], count = re.subn(r'^cue_path\s*=.*$',
                lambda _: 'cue_path = ' + json.dumps(str(cue.resolve())), section, flags=re.M)
            replaced += count
    if replaced != 1:
        raise ValueError('expected exactly one disc cue in native configuration')
    return ''.join(sections)


def compare(native, donor):
    """Compare scenario semantics, not frame counts or cross-engine RAM hashes."""
    if native['engine'] != 'native' or donor['engine'] != 'donor':
        raise ValueError('comparison requires native then donor results')
    for field in ('card_sha256',):
        if native['scenario'][field] != donor['scenario'][field]:
            raise ValueError('comparison requires the same isolated retail saves')
    if native['route_sha256'] != donor['route_sha256']:
        raise ValueError('comparison requires identical route contracts')
    fields = ('stage', 'story', 'difficulty', 'disc', 'life', 'max_life', 'stance',
              'weapon', 'item', 'weapons', 'items', 'x', 'z', 'game_over_timer')
    differences = {key: {'native': native.get('final_state', {}).get(key),
                         'donor': donor.get('final_state', {}).get(key)}
                   for key in fields
                   if native.get('final_state', {}).get(key) != donor.get('final_state', {}).get(key)}
    return dict(format='mgs-scene-comparison-1',
                status='complete' if not differences and native['status'] == donor['status'] == 'complete' else 'needs-review',
                stage=native['scenario']['stage'], story=native['scenario']['story'],
                native_status=native['status'], donor_status=donor['status'],
                differences=differences, compared_fields=fields,
                scope=native['scope'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    catalog = commands.add_parser('catalog')
    catalog.add_argument('card', type=Path)
    prep = commands.add_parser('prepare')
    prep.add_argument('card', type=Path)
    prep.add_argument('block', type=int)
    prep.add_argument('destination', type=Path)
    execute = commands.add_parser('run')
    execute.add_argument('scenario', type=Path)
    execute.add_argument('output', type=Path)
    execute.add_argument('--engine', choices=('native', 'donor'), default='native')
    execute.add_argument('--donor-runner', type=Path, default=ROOT / '.local/libretro_headless')
    execute.add_argument('--cue', type=Path, help='override selected disc image for either engine')
    execute.add_argument('--period-us', type=int, default=2000)
    execute.add_argument('--timeout', type=int, default=120)
    execute.add_argument('--load-only', action='store_true')
    comparison = commands.add_parser('compare')
    comparison.add_argument('native_result', type=Path)
    comparison.add_argument('donor_result', type=Path)
    args = parser.parse_args()
    if args.command == 'catalog':
        print(json.dumps(card_entries(args.card.read_bytes()), indent=2))
        return 0
    if args.command == 'prepare':
        print(json.dumps(prepare(args.card, args.block, args.destination), indent=2))
        return 0
    if args.command == 'compare':
        report = compare(json.loads(args.native_result.read_text()), json.loads(args.donor_result.read_text()))
        print(json.dumps(report, indent=2))
        return 0 if report['status'] == 'complete' else 1
    return run(args)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f'Scenarios: {error}', file=sys.stderr)
        sys.exit(1)
