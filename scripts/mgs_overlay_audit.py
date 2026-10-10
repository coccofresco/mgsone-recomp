#!/usr/bin/env python3
"""Audit USA overlay identity and callback registration, without executing code.

Resume labels are deliberately excluded from ordinary-call coverage. Candidate
callback discovery is conservative; passing this audit is not playability proof.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import tomllib

from mgs_assets import Disc, stages, stage_members
from mgs_completion import identity
from mgs_overlay_catalog import BASE, callback_entries
from mgs_tas import CONFIG, ROOT, write_json


def normal_registry(text):
    """Separate generated native continuation PCs from callable function entries."""
    start = text.index('static recomp_func_t recomp_lookup_overlay(')
    start = text.index('if (resume) {', start) + len('if (resume) {')
    depth, end = 1, start
    while depth and end < len(text):
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    if depth:
        raise ValueError('unterminated generated resume registry')
    stop = text.index('\nstatic ', end)
    entries = {}
    for pc, stage in re.findall(r'case (0x[0-9A-F]+)u: return overlay_([a-z0-9_]+)__[0-9A-F]+;', text[end:stop]):
        entries.setdefault(stage, set()).add(int(pc, 16))
    main = {int(pc, 16) for pc in re.findall(r'recomp_(?:func|entry_alias)_table\[(0x[0-9A-F]+)\]', text)}
    if not entries or not main:
        raise ValueError('empty generated function registry')
    return main, entries


def signature_matches(spec, expected, actual):
    size = spec.get('identity_size', 64)
    offsets = spec.get('additional_identity_offsets', [])
    if not 0 < size <= len(expected) or any(o < 0 or o + 4 > len(expected) for o in offsets):
        raise ValueError('invalid overlay identity bounds')
    return (expected[:size] == actual[:size] and
            all(expected[o:o + 4] == actual[o:o + 4] for o in offsets))


def analyze(specs, payloads, main, entries):
    rows, missing, collisions = [], [], []
    if not specs or {o['name'] for o in specs} != set(entries):
        raise ValueError('configured and generated overlay sets differ')
    for spec in specs:
        name, size = spec['name'], spec['size']
        payload = payloads[name]
        if spec['ram_base'] != BASE or size > len(payload):
            raise ValueError('unsupported USA overlay layout')
        callbacks, table = callback_entries(payload, size)
        ctors = [struct.unpack_from('<II', payload, offset)[1] for offset in range(0, table, 8)]
        matches = [other for other in specs if signature_matches(other, payloads[other['name']], payload)]
        # Runtime returns from the FIRST matching identity, even on a lookup
        # miss; unioning different overlays could hide an actual dispatch bug.
        if not matches:
            raise ValueError('overlay does not match its own signature')
        for other in matches:
            theirs = payloads[other['name']][:other['size']]
            if theirs != payload[:size]:
                collisions.append(dict(stage=name, matches=other['name']))
        selected = matches[0]['name']
        for target in sorted(set(ctors + callbacks)):
            if target not in main and target not in entries[selected]:
                offset = target - BASE
                words = (struct.unpack_from('<2I', payload, offset)
                         if 0 <= offset <= len(payload) - 8 else None)
                missing.append(dict(stage=name, selected=selected, target=f'0x{target:08x}',
                    kind='constructor' if target in ctors else 'callback-candidate',
                    empty_return=words == (0x03e00008, 0) if words is not None else None))
        rows.append(dict(stage=name, sha256=hashlib.sha256(payload).hexdigest(),
            code_size=size, constructors=len(ctors), callback_candidates=len(callbacks),
            normal_entries=len(entries[name]), matching_identities=[o['name'] for o in matches]))
    return dict(format='mgs-overlay-audit-1', status='complete' if not missing and not collisions else 'needs-review',
        scope='Configured USA constructors, conservative callback references and immutable overlay identities. '
              'Resume PCs do not count as call entries. Does not cover arbitrary computed callbacks or playability.',
        overlays=rows, missing_entries=missing, identity_collisions=collisions,
        counts=dict(overlays=len(rows), constructors=sum(r['constructors'] for r in rows),
                    callback_candidates=sum(r['callback_candidates'] for r in rows),
                    normal_entries=sum(r['normal_entries'] for r in rows)))


def retail_overlays(path):
    disc = Disc(path)
    entry = disc.files()['/MGS/STAGE.DIR']
    archive = disc.read(entry['lba'], entry['size'])
    found = {}
    for stage, _, payload in stages(archive):
        for member, raw in stage_members(payload):
            if member['extension'] == 'b':
                if stage in found:
                    raise ValueError('duplicate stage overlay')
                found[stage] = hashlib.sha256(raw).hexdigest()
    return hashlib.sha256(archive).hexdigest(), found


def audit(config, registry, discs):
    specs = tomllib.loads(config.read_text())['overlays']
    payloads = {o['name']: (config.parent / o['binary_path']).read_bytes() for o in specs}
    main, entries = normal_registry(registry.read_text())
    result = analyze(specs, payloads, main, entries)
    result.update(config=identity(config), registry=identity(registry), discs=[], retail_mismatches=[])
    configured = {r['stage'].removeprefix('usa_'): r['sha256'] for r in result['overlays']}
    for index, path in enumerate(discs, 1):
        digest, retail = retail_overlays(path)
        mismatches = [name for name, sha in configured.items() if retail.get(name) != sha]
        result['discs'].append(dict(disc=index, archive_sha256=digest, overlays=len(retail),
            uncompiled=sorted(set(retail) - configured.keys())))
        result['retail_mismatches'] += [dict(disc=index, stage=name) for name in mismatches]
    if result['retail_mismatches']:
        result['status'] = 'needs-review'
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--config', type=Path, default=CONFIG)
    parser.add_argument('--registry', type=Path, default=ROOT / 'engine/ps1Runtime/src/recompiled_out.cpp')
    parser.add_argument('--discs', type=Path, nargs=2,
        default=[ROOT / f'data/discs/usa/disc{n}/Metal Gear Solid (USA) (Disc {n}) (v1.0).bin' for n in (1, 2)])
    args = parser.parse_args()
    result = audit(args.config, args.registry, args.discs)
    write_json(args.output, result)
    print(json.dumps(dict(status=result['status'], **result['counts'], missing=len(result['missing_entries']))))
    return 0 if result['status'] == 'complete' else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError) as error:
        print(f'Overlay audit: {error}', file=sys.stderr)
        sys.exit(1)
