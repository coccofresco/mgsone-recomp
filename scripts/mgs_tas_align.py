#!/usr/bin/env python3
"""Index original stage-script synchronization candidates and reference inputs.

This is a structural inventory, not a guessed frame-to-stage alignment. Source
conditions, byte offsets and HZD trigger volumes remain attached to candidates.
Only a native run reaching a route goal can validate a chosen correspondence.
"""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import re

from mgs_assets import Disc, stages, stage_members
from mgs_gcl_values import decode_gcl_value
from mgs_stage_probe import hazard
from mgs_tas_import import parse_pjm, BUTTONS


def hash16(name):
    result = 0
    for byte in name.encode('ascii'):
        result = (((result << 5) | (result >> 11)) + byte) & 65535
    return result


def script_tree(data, names):
    def need(pos, count, end):
        if pos < 0 or count < 0 or pos + count > min(end, len(data)):
            raise ValueError(f'truncated GCL at {pos:#x}')

    def word(pos, end):
        need(pos, 2, end)
        return int.from_bytes(data[pos:pos + 2], 'big')

    def values(pos, end, depth):
        if depth > 64:
            raise ValueError('GCL nesting exceeds 64')
        result = []
        while pos < end and data[pos]:
            start = pos
            tag = data[pos]
            if tag == 0x40:
                stop = pos + 1 + word(pos + 1, end)
                need(pos, stop - pos, end)
                result.append({'kind': 'block', 'offset': pos,
                               'commands': block(pos + 3, stop, depth + 1)})
                pos = stop
            elif tag == 0x50:
                need(pos, 3, end)
                # Subsequent options retain their own nodes; the command's
                # declared length bounds their payload and nested blocks.
                result.append({'kind': 'option', 'offset': pos, 'name': chr(data[pos + 1]),
                               'encoded_length': data[pos + 2]})
                pos += 3
            else:
                node, pos, errors = decode_gcl_value(data, pos, end)
                result.append({'offset': start, **node})
                if errors:
                    raise ValueError(f'GCL value at {start:#x}: {errors}')
            if pos <= start:
                raise ValueError('GCL parser made no progress')
        return result

    def block(pos, end, depth):
        result = []
        while pos < end and data[pos]:
            start, tag = pos, data[pos]
            if tag == 0x60:
                stop = pos + 1 + word(pos + 1, end)
                need(pos, stop - pos, end)
                need(pos, 6, stop)
                code = word(pos + 3, stop)
                name = names.get(code, f'command_{code:04x}')
                args = values(pos + 6, stop, depth)
            elif tag == 0x70:
                need(pos, 4, end)
                stop = pos + 1 + data[pos + 1]
                need(pos, stop - pos, end)
                code, name = word(pos + 2, stop), 'call'
                args = values(pos + 4, stop, depth)
            else:
                raise ValueError(f'unsupported GCL command {tag:#x} at {pos:#x}')
            need(pos, stop - pos, end)
            if stop <= pos:
                raise ValueError('empty GCL command')
            result.append({'offset': start, 'end': stop, 'name': name,
                           'code': code, 'arguments': args})
            pos = stop
        return result

    need(0, 8, len(data))
    pos, table = 4, []
    while True:
        need(pos, 4, len(data))
        if data[pos:pos + 4] == bytes(4):
            break
        table.append((word(pos, len(data)), word(pos + 2, len(data))))
        pos += 4
    base = pos + 4
    entries = [(f'{name:04x}', base + offset) for name, offset in table]
    entries.append(('main', 8 + int.from_bytes(data[:4], 'big')))
    procedures = []
    for name, pos in entries:
        need(pos, 3, len(data))
        if data[pos] != 0x40:
            raise ValueError(f'procedure {name} missing block header at {pos:#x}')
        end = pos + 1 + word(pos + 1, len(data))
        need(pos, end - pos, len(data))
        procedures.append({'procedure': name, 'offset': pos,
                           'commands': block(pos + 3, end, 0)})
    return procedures


def candidates(procedures, traps):
    result = []
    def walk(commands, proc, parents):
        for command in commands:
            name, args = command['name'], command['arguments']
            item = {'procedure': proc, 'offset': command['offset'], 'command': name,
                    'lexical_parents': parents}
            literals = [arg.get('value') for arg in args if arg['kind'] == 'literal']
            expressions = [arg['pretty'] for arg in args if arg['kind'] == 'expr']
            item['expressions'] = expressions
            if name == 'load':
                item['target_stages'] = [s.rstrip('\0') for s in literals if isinstance(s, str)]
                result.append(item)
            elif name in ('trap', 'ntrap'):
                item['trigger_hash'] = literals[0] if literals else None
                item['volumes'] = traps.get(item['trigger_hash'], [])
                result.append(item)
            elif name in ('radio', 'demo'):
                item['literals'] = literals
                result.append(item)
            elif name == 'eval':
                for arg in args:
                    ast = arg.get('ast')
                    if not ast or ast.get('operator') != 'set':
                        continue
                    left, right = ast['left'], ast['right']
                    if left.get('kind') == 'variable' and right.get('kind') == 'literal':
                        var = left['variable']
                        if var['scope'] == 'global' and var['offset'] == 10 and var['type'] == 'short':
                            result.append({**item, 'story_value': right['value'],
                                           'observation': 'gcl_' + left['raw_hex']})
            context = {'offset': command['offset'], 'command': name, 'expressions': expressions}
            option = None
            for index, arg in enumerate(args):
                if arg['kind'] == 'option':
                    option = arg['name']
                if arg['kind'] == 'block':
                    # This distinguishes e.g. the -e block from the initial
                    # block of an if, without claiming the parent expression
                    # must be true in both branches.
                    walk(arg['commands'], proc, parents + [
                        {**context, 'block_argument': index, 'option': option}])
    for proc in procedures:
        walk(proc['commands'], proc['procedure'], [])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disc', type=Path, help='raw MODE2/Form1 disc BIN, not the CUE descriptor')
    parser.add_argument('output', type=Path)
    parser.add_argument('--decomp', type=Path, required=True, help='mgs_reversing/source directory')
    parser.add_argument('--movie', type=Path, required=True)
    parser.add_argument('--stages', help='comma-separated stage names; default all')
    args = parser.parse_args()
    names = {}
    sources = []
    for relative in ('game/script.c', 'libgcl/basic.c', 'libgcl/parse.c',
                     'libgcl/variable.c', 'libgcl/expr.c', 'include/linkvar.h',
                     'game/g_struct.h', 'enemy/enemy.h'):
        path = args.decomp / relative
        data = path.read_bytes()
        sources.append({'file': relative, 'sha256': hashlib.sha256(data).hexdigest()})
        if relative in ('game/script.c', 'libgcl/basic.c'):
            for match in re.finditer(r'\{ (0x[0-9a-f]+),[^\n]*GV_StrCode\("([^\"]+)"\)', data.decode()):
                names[int(match[1], 16)] = match[2]
    disc = Disc(args.disc)
    record = disc.files()['/MGS/STAGE.DIR']
    archive = disc.read(record['lba'], record['size'])
    selected = set(args.stages.split(',')) if args.stages else None
    report = {'format': 'mgs-tas-alignment-catalog-1', 'decomp_sources': sources,
              'stage_archive_sha256': hashlib.sha256(archive).hexdigest(),
              'status': 'structural candidates; no source-frame correspondence inferred', 'stages': []}
    for stage, _, payload in stages(archive):
        if selected is not None and stage not in selected:
            continue
        members = list(stage_members(payload))
        traps, hazards = {}, []
        for member, data in members:
            if member['extension'] != 'h':
                continue
            decoded = hazard(data)
            hazards.append({'resource': member['id'], 'sha256': decoded['sha256'],
                            'patrol_routes': decoded['routes']})
            for group in decoded['groups']:
                for trap in group['traps']:
                    if not trap['name'].isascii():
                        continue
                    traps.setdefault(hash16(trap['name']), []).append(
                        {'resource': member['id'], 'name': trap['name'], 'bounds': trap['bounds']})
        entry = {'stage': stage, 'scripts': [], 'hazards': hazards}
        for member, data in members:
            if member['extension'] != 'g':
                continue
            script = {'resource': member['id'], 'sha256': hashlib.sha256(data).hexdigest()}
            try:
                tree = script_tree(data, names)
                script.update(status='decoded', procedure_count=len(tree),
                              candidates=candidates(tree, traps))
            except ValueError as error:
                script.update(status='unsupported', error=str(error))
            entry['scripts'].append(script)
        report['stages'].append(entry)
    if selected is not None and selected - {stage['stage'] for stage in report['stages']}:
        parser.error('requested stage is absent from the disc')
    metadata, frames, _ = parse_pjm(args.movie.read_bytes())
    report['reference'] = metadata
    report['reference_runs'] = []
    cursor = 1
    for pads, group in itertools.groupby(frames):
        length = sum(1 for _ in group)
        report['reference_runs'].append({'first': cursor, 'last': cursor + length - 1,
            'pads': pads, 'buttons': [[b for bit, b in enumerate(BUTTONS) if mask & (1 << bit)] for mask in pads]})
        cursor += length
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('x') as stream:
        json.dump(report, stream, indent=2)
        stream.write('\n')
    scripts = [s for stage in report['stages'] for s in stage['scripts']]
    print(json.dumps({'stages': len(report['stages']), 'scripts': len(scripts),
                      'decoded': sum(s['status'] == 'decoded' for s in scripts),
                      'candidates': sum(len(s.get('candidates', [])) for s in scripts),
                      'reference_runs': len(report['reference_runs'])}))


if __name__ == '__main__':
    main()
