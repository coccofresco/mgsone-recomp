#!/usr/bin/env python3
"""Prepare source-bounded entries for mandatory progression probes.

An isolated entry is a synthetic precondition, never proof of its preceding
combat. Literal entry-procedure assignments are imported together so boss
counters, map and player position cannot silently retain another save's state.
"""
import argparse
import json
from pathlib import Path

from mgs_fixture import phase_card
from mgs_scenarios import card_entries, isolated_card, load_recipe
from mgs_tas import ROOT, sha256, write_json

SCRIPTS = ROOT / 'data/validation/completion/disc1-scripts.json'
PROFILES = {
    'tank': dict(seed='d1-03-s04b-36', stage='s05a', story=66, ready=66, procedure='a4ad'),
    'mantis': dict(seed='d1-06-s07c-119', stage='s07b', story=124, ready=125),
    # Start before the original Meryl/Mantis films: their callbacks establish
    # the spawn and phase. The older story124 probe inherited an unrelated
    # bathroom spawn and must not be used as evidence of combat readiness.
    'mantis-intro': dict(seed='d1-06-s07c-119', stage='s07b', story=120, ready=123),
    'hind': dict(seed='d1-12-s11c-190', stage='s11h', story=185, ready=186,
                 map_from_load=('s11c', 'ea54', 'f9db')),
}


def literal(node):
    if node.get('kind') == 'literal' and type(node.get('value')) is int:
        return node['value']
    if node.get('kind') == 'unary' and node.get('operator') == 'negate':
        return -literal(node['operand'])
    raise ValueError('entry assignment is not a constant')


def entry_assignments(procedure, stage, story):
    """Only unconditional literal assignments and a final retail restart."""
    commands = procedure['commands']
    if not commands or commands[-1]['name'] != 'restart' or commands[-1].get('arguments'):
        raise ValueError('entry must end with a plain restart')
    globals_, linked = [], []
    found_story = False
    for command in commands[:-1]:
        if command['name'] != 'eval' or len(command['arguments']) != 1:
            raise ValueError('entry procedure has unmodeled effects')
        ast = command['arguments'][0].get('ast', {})
        if ast.get('operator') != 'set' or ast.get('left', {}).get('kind') != 'variable':
            raise ValueError('entry procedure is not a simple assignment')
        var = ast['left']['variable']
        if var['scope'] not in ('global', 'link') or var['type'] not in ('short', 'strid', 'byte'):
            raise ValueError('unsupported entry variable')
        value = literal(ast['right'])
        if var['scope'] == 'global' and var['offset'] == 10:
            if found_story or value != story or var['type'] != 'short':
                raise ValueError('entry story disagrees with contract')
            found_story = True
            continue
        width = 1 if var['type'] == 'byte' else 2
        field = dict(offset=var['offset'], width=width, value=value,
                     source=f'USA {stage}/ea54 procedure {procedure["procedure"]} eval @{command["offset"]}')
        if value < 0:
            if var['scope'] == 'link':
                field['signed'] = True
            else:
                field['value'] = value % (1 << (width * 8))
        (linked if var['scope'] == 'link' else globals_).append(field)
    if not found_story:
        raise ValueError('entry procedure does not establish required story')
    return globals_, linked


def entry_map(procedure, target_stage, source):
    """Extract only the map operand of one unconditional original load.

    This is a synthetic map prerequisite, not execution of the procedure:
    position, flags and other load effects are deliberately not inferred.
    """
    loads = [c for c in procedure['commands'] if c['name'] == 'load']
    if len(loads) != 1:
        raise ValueError('map prerequisite requires one unconditional load')
    command = loads[0]
    args = command['arguments']
    if (not args or args[0].get('kind') != 'literal' or
            args[0].get('type') != 'string' or args[0].get('value') != target_stage + '\0'):
        raise ValueError('map prerequisite load targets a different stage')
    positions = [i for i, a in enumerate(args) if a.get('kind') == 'option' and a.get('name') == 'm']
    if len(positions) != 1:
        raise ValueError('load must name exactly one map')
    pos = positions[0] + 1
    if (pos >= len(args) or args[pos].get('kind') != 'literal' or
            args[pos].get('type') != 'strid' or
            (pos + 1 < len(args) and args[pos + 1].get('kind') != 'option')):
        raise ValueError('load map must be one literal strid')
    value = literal(args[pos])
    if not 0 <= value <= 65535:
        raise ValueError('load map is outside strid range')
    return dict(offset=14, width=2, value=value,
                source=f'USA {source} procedure {procedure["procedure"]} load @{command["offset"]} -m')


def prepare(name, output):
    profile = PROFILES[name]
    source = ROOT / 'data/scenarios/usa-normal/cases' / profile['seed'] / 'cards/card1.mcd'
    raw = source.read_bytes()
    original = card_entries(raw)[0]
    stage, story = profile['stage'], profile['story']
    globals_, linked = [], []
    scripts = json.loads(SCRIPTS.read_text())
    script = next(s for s in scripts if s['stage'] == stage and s['resource'] == 'ea54')
    if profile.get('procedure'):
        proc = next(p for p in script['procedures'] if p['procedure'] == profile['procedure'])
        globals_, linked = entry_assignments(proc, stage, story)
    if profile.get('map_from_load'):
        from_stage, resource, procedure = profile['map_from_load']
        origin = next(s for s in scripts if s['stage'] == from_stage and s['resource'] == resource)
        proc = next(p for p in origin['procedures'] if p['procedure'] == procedure)
        linked.append(entry_map(proc, stage, f'{from_stage}/{resource}'))
    card = phase_card(raw, original['block'], stage, story, globals_, linked)
    entry = card_entries(card)[0]
    route = load_recipe(entry, False)
    route['steps'] = route['steps'][:4]
    route['steps'][-1]['until'] = dict(stage=stage)
    route['steps'] += [
        dict(name='confirm-original-mission-log', until=dict(control={'ne': 0}),
             action=dict(sequence='wait:30,circle:6,wait:90'), attempts=60, max_frames=9000),
        dict(name='original-boss-entry', until=dict(stage=stage, story=profile['ready'],
             control={'ne': 0}, pause_level=0, load_request=0, actor_loader_count=0,
             life={'min': 1}, game_over_timer=0), stable_frames=30, max_frames=18000),
    ]
    output.mkdir(parents=True, exist_ok=False)
    (output / 'cards').mkdir()
    (output / 'cards/card1.mcd').write_bytes(card)
    (output / 'cards/card2.mcd').write_bytes(isolated_card(raw))
    entry.update(format='mgs-scene-case-1', source_card=str(source), source_sha256=sha256(source),
        card_sha256={f'card{n}.mcd': sha256(output / f'cards/card{n}.mcd') for n in (1, 2)},
        synthetic_phase=dict(source=dict(path=str(SCRIPTS), sha256=sha256(SCRIPTS), stage=stage,
            resource='ea54', procedure=profile.get('procedure'), profile=name,
            map_from_load=profile.get('map_from_load')),
            scope='Synthetic source-derived pre-boss entry; original load/intro and actor initialization. Neither boss victory nor preceding route is established by this probe.',
            changes=dict(stage=stage, story=story, globals=globals_, linked=linked), route=route))
    write_json(output / 'scenario.json', entry)
    return entry


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('profile', choices=PROFILES)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    prepare(args.profile, args.output)
