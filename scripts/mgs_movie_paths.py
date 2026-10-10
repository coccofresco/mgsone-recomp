#!/usr/bin/env python3
"""Find original movie branches and prepare isolated, source-derived phase cards.

Eligibility is intentionally narrow: direct main-procedure if branches whose
conditions depend only on the story value and assign a literal next story.
Other calls stay inventoried, never guessed into runnable fixtures.
"""
import argparse
import itertools
import json
import operator
from pathlib import Path

from mgs_fixture import phase_card
from mgs_scenarios import card_entries, isolated_card, load_recipe
from mgs_tas import sha256, write_json


def identity(path):
    return dict(path=str(path.resolve()), sha256=sha256(path))


def is_story(node):
    var = node.get('variable', {})
    return node.get('kind') == 'variable' and (var.get('scope'), var.get('offset'), var.get('type')) == ('global', 10, 'short')


def predicate(node):
    if node.get('kind') == 'literal' and type(node.get('value')) is int:
        return lambda story: node['value']
    if is_story(node):
        return lambda story: story
    operations = dict(eq=operator.eq, ne=operator.ne, lt=operator.lt, le=operator.le,
                      gt=operator.gt, ge=operator.ge, logical_and=lambda a, b: bool(a and b),
                      logical_or=lambda a, b: bool(a or b))
    if node.get('kind') != 'binary' or node.get('operator') not in operations:
        raise ValueError('condition is not a supported pure story predicate')
    left, right, op = predicate(node['left']), predicate(node['right']), operations[node['operator']]
    return lambda story: op(left(story), right(story))


def if_blocks(command):
    """GCL_Command_if: first true branch wins; -i is else-if, -e is else."""
    args, previous, pos = command['arguments'], [], 0
    while pos < len(args):
        if pos:
            option = args[pos]
            if option.get('kind') != 'option' or option.get('name') not in ('i', 'e'):
                raise ValueError('unsupported if branch encoding')
            pos += 1
            if option['name'] == 'e':
                if pos != len(args) - 1 or args[pos].get('kind') != 'block':
                    raise ValueError('malformed else block')
                yield args[pos]['commands'], list(previous)
                return
        if pos + 1 >= len(args) or args[pos].get('kind') != 'expr' or args[pos + 1].get('kind') != 'block':
            raise ValueError('malformed if condition/block')
        expression = args[pos]
        current = dict(offset=expression['offset'], ast=expression['ast'], pretty=expression['pretty'], truth=True)
        yield args[pos + 1]['commands'], previous + [current]
        previous = previous + [dict(current, truth=False)]
        pos += 2


def paths(scripts, movie_codes):
    rows = []
    for script in scripts:
        def walk(commands, proc, guards, direct):
            story_after = None
            for command in commands:
                args = command['arguments']
                if command['name'] == 'eval':
                    for arg in args:
                        ast = arg.get('ast', {})
                        if ast.get('operator') == 'set' and is_story(ast.get('left', {})):
                            right = ast.get('right', {})
                            story_after = right.get('value') if right.get('kind') == 'literal' and type(right.get('value')) is int else None
                literals = [a.get('value') for a in args if a.get('kind') == 'literal']
                if command['name'] == 'chara' and len(literals) >= 2 and literals[0] == 0x3453 and literals[1] in movie_codes:
                    row = dict(stage=script['stage'], resource=script['resource'], procedure=proc,
                        offset=command['offset'], code=f'{literals[1]:04x}', guards=guards,
                        story_after=story_after, eligible=False)
                    if direct and proc == 'main' and guards and type(story_after) is int:
                        try:
                            tests = [(predicate(g['ast']), g['truth']) for g in guards]
                            values = [s for s in range(32768) if all(bool(fn(s)) == truth for fn, truth in tests)]
                            row['story_ranges'] = [[group[0][1], group[-1][1]] for _, pairs in
                                itertools.groupby(enumerate(values), lambda p: p[1] - p[0]) if (group := list(pairs))]
                            row['eligible'] = bool(values)
                        except ValueError as error:
                            row['unsupported'] = str(error)
                    rows.append(row)
                if command['name'] == 'if':
                    for block, conditions in if_blocks(command):
                        walk(block, proc, guards + conditions, direct and not guards)
                else:
                    for arg in args:
                        if arg.get('kind') == 'block':
                            walk(arg['commands'], proc, guards, False)
        for procedure in script['procedures']:
            walk(procedure['commands'], procedure['procedure'], [], True)
    return rows


def prepare(catalog_path, source, block, output, stage, code, resource='a242'):
    catalog = json.loads(catalog_path.read_text())
    if catalog.get('format') != 'mgs-movie-paths-1':
        raise ValueError('requires movie path catalog')
    # Do not use a stale script analysis to write preconditions.
    for record in catalog['inputs']:
        if sha256(Path(record['path'])) != record['sha256']:
            raise ValueError('source evidence changed')
    matches = [r for r in catalog['paths'] if r['stage'] == stage and r['code'] == code and r['resource'] == resource and r['eligible']]
    if len(matches) != 1:
        raise ValueError('requires exactly one eligible original movie branch')
    row = matches[0]
    story = row['story_ranges'][0][0]
    raw = source.read_bytes()
    original = next(e for e in card_entries(raw) if e['block'] == block)
    if original['disc'] != catalog['disc_number']:
        raise ValueError('save/disc mismatch')
    card = phase_card(raw, block, stage, story)
    entry = card_entries(card)[0]
    route = load_recipe(entry, False)
    route['steps'] = route['steps'][:4]
    # Loading the card changes saved_stage while abst.c still waits for Circle.
    route['steps'][-1]['until'] = dict(current_stage=stage)
    # abst.c can be recreated after the stage-name transition. Confirm only
    # while that original UI actor is present; never press into the movie.
    route['steps'] += [dict(name='original-movie-start', until=dict(stage=stage, actor_movie_count=1),
        when=dict(actor_abst_count=1), action=dict(sequence='wait:30,circle:6,wait:90'),
        attempts=55, max_frames=7000),
        # ad2d restarts the original script, which may immediately advance its
        # story again. The pre-movie assignment is a lower bound, not an exit
        # equality; full decode counts are checked separately from this goal.
        dict(name='original-movie-callback', until=dict(stage=stage, current_stage=stage, story=dict(min=row['story_after']),
            actor_movie_count=0, actor_loader_count=0, load_request=0), stable_frames=60,
            max_frames=int(catalog['movies'][code]['drive_seconds_2x'] * 60) + 1800)]
    output.mkdir(parents=True, exist_ok=False)
    (output / 'cards').mkdir()
    (output / 'cards/card1.mcd').write_bytes(card)
    (output / 'cards/card2.mcd').write_bytes(isolated_card(raw))
    entry.update(format='mgs-scene-case-1', source_card=str(source.resolve()), source_sha256=sha256(source),
        card_sha256={f'card{n}.mcd': sha256(output / f'cards/card{n}.mcd') for n in (1, 2)},
        synthetic_phase=dict(source='USA retail GCL direct story branch; isolated card stage/story only',
            scope=f'Synthetic entry to {stage}/{code}; original movie and callback. Does not establish prior campaign reachability.',
            changes=dict(stage=stage, story=story, variables=[]), route=route),
        movie_evidence=dict(catalog=identity(catalog_path), branch=row, movie=catalog['movies'][code]))
    write_json(output / 'scenario.json', entry)
    return entry


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    scan = sub.add_parser('catalog')
    scan.add_argument('scripts', type=Path)
    scan.add_argument('movies', type=Path)
    scan.add_argument('output', type=Path)
    scan.add_argument('--disc', type=int, choices=(1, 2), required=True)
    prep = sub.add_parser('prepare')
    prep.add_argument('catalog', type=Path)
    prep.add_argument('card', type=Path)
    prep.add_argument('block', type=int)
    prep.add_argument('output', type=Path)
    prep.add_argument('--stage', required=True)
    prep.add_argument('--movie', required=True)
    prep.add_argument('--resource', default='a242')
    args = parser.parse_args()
    if args.command == 'prepare':
        prepare(args.catalog, args.card, args.block, args.output, args.stage, args.movie, args.resource)
        return
    if args.output.exists():
        parser.error('refusing to overwrite evidence')
    movies = json.loads(args.movies.read_text())['discs'][args.disc - 1]
    codes = {int(m['code'], 16) for m in movies['movies']}
    found = paths(json.loads(args.scripts.read_text()), codes)
    report = dict(format='mgs-movie-paths-1', disc_number=args.disc, disc=movies['disc'],
        inputs=[identity(args.scripts), identity(args.movies)], paths=found,
        movies={m['code']: {k:v for k,v in m.items() if k != 'frames'} for m in movies['movies']},
        scope=__doc__.strip())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_json(args.output, report)
    print(json.dumps(dict(paths=len(found), eligible=sum(r['eligible'] for r in found),
        cases=[(r['stage'], r['code'], r.get('story_ranges'), r['story_after']) for r in found if r['resource']=='a242'])))


if __name__ == '__main__':
    main()
