#!/usr/bin/env python3
"""Inventory retail progression and prepare explicitly synthetic phase checks.

Static edges are candidates, never proof of reachability. Runtime evidence is
kept separate from load/control smoke tests and from synthetic preconditions.
"""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re
import sys
import subprocess
import tomllib

from mgs_assets import Disc, stages, stage_members
from mgs_fixture import phase_card
from mgs_scenarios import card_entries, compare, config_with_cue, isolated_card, load_recipe
from mgs_tas import CONFIG, ROOT, sha256, write_json
from mgs_tas_align import candidates, script_tree

DECOMP = ROOT / '.local/effects-research/mgs-upstream/source'


def identity(path):
    resolved = path.resolve()
    return dict(path=str(resolved.relative_to(ROOT) if resolved.is_relative_to(ROOT) else resolved), sha256=sha256(path))


def catalog(discs, decomp):
    names, sources = {}, []
    for relative in ('game/script.c', 'libgcl/basic.c', 'libgcl/variable.c'):
        path = decomp / relative
        sources.append(dict(file=relative, sha256=sha256(path)))
        for match in re.finditer(r'\{ (0x[0-9a-f]+),[^\n]*GV_StrCode\("([^"]+)"\)', path.read_text()):
            names[int(match[1], 16)] = match[2]
    result = dict(format='mgs-completion-catalog-1', sources=sources, discs=[],
                  scope='Syntactic GCL load edges and story assignments, including conditional and unused branches. Not a reachability proof.')
    all_stages, edges, errors = set(), [], []
    for number, image in enumerate(discs, 1):
        disc = Disc(image)
        record = disc.files()['/MGS/STAGE.DIR']
        archive = disc.read(record['lba'], record['size'])
        entry = dict(disc=number, archive_sha256=hashlib.sha256(archive).hexdigest(), stages=[])
        for stage, _, payload in stages(archive):
            all_stages.add(stage)
            stage_row = dict(stage=stage, scripts=[])
            for member, data in stage_members(payload):
                if member['extension'] != 'g':
                    continue
                row = dict(resource=member['id'], sha256=hashlib.sha256(data).hexdigest())
                try:
                    tree = script_tree(data, names)
                    found = candidates(tree, {})
                    row.update(status='decoded', procedures=len(tree),
                               transitions=[c for c in found if c['command'] == 'load' or 'story_value' in c])
                    for c in row['transitions']:
                        for target in c.get('target_stages', []):
                            edges.append(dict(disc=number, stage=stage, target=target,
                                              resource=member['id'], procedure=c['procedure'],
                                              offset=c['offset'], conditions=c['lexical_parents']))
                except ValueError as error:
                    row.update(status='unsupported', error=str(error))
                    errors.append(dict(disc=number, stage=stage, resource=member['id'], error=str(error)))
                stage_row['scripts'].append(row)
            entry['stages'].append(stage_row)
        result['discs'].append(entry)
    scripts = [r for d in result['discs'] for s in d['stages'] for r in s['scripts']]
    result.update(load_edges=edges, parse_errors=errors,
                  unresolved_stage_targets=sorted({e['target'] for e in edges} - all_stages),
                  counts=dict(stage_records=sum(len(d['stages']) for d in result['discs']),
                              distinct_stage_names=len(all_stages), scripts=len(scripts),
                              procedures=sum(s.get('procedures', 0) for s in scripts),
                              load_edges=len(edges),
                              story_assignments=sum('story_value' in c for s in scripts for c in s.get('transitions', []))))
    return result


def static_entries(directory, config):
    """Fail closed on literal dispatches into unregistered main-text entries.

    Indirect calls and generated instructions outside main text are outside
    this audit. In particular, embedded tables must not become guessed code.
    """
    registry = directory / 'recompiled_out.cpp'
    known = {int(pc, 16) for pc in re.findall(
        r'recomp_(?:func|entry_alias)_table\[(0x[0-9A-F]+)\]', registry.read_text())}
    parsed = tomllib.loads(config.read_text())
    ranges = [(int(f['address'], 16), int(f['address'], 16) + f['size']) for f in parsed['functions']]
    if not ranges or not known:
        raise ValueError('static entry audit requires a main registry and function ranges')
    lower, upper = min(a for a, _ in ranges), max(b for _, b in ranges)
    unresolved, outside, sources, sites, targets = [], [], [], 0, set()
    for path in sorted(directory.glob('recompiled_out.part*.cpp')):
        text = path.read_text()
        sources.append(identity(path))
        for match in re.finditer(r'recomp_dispatch\(rdram, ctx, (0x[0-9A-F]+)\)', text):
            target = int(match[1], 16)
            owner = text.rfind('\nvoid ', 0, match.start())
            function = text[owner + 6:text.find('(', owner)]
            row = dict(function=function, target=f'0x{target:08x}')
            if lower <= target < upper:
                sites += 1
                targets.add(target)
                if target not in known:
                    unresolved.append(row)
            elif target < 0x800c0000:
                outside.append(row)
    if not sources or not sites:
        raise ValueError('static entry audit found no compiled main dispatch sites')
    return dict(format='mgs-static-main-entry-audit-1', status='complete' if not unresolved else 'needs-review',
                scope='Literal dispatches into main executable text across all compiled overlays. Not a proof of indirect-call coverage or playability.',
                registry=identity(registry), config=identity(config), translation_units=sources,
                literal_main_dispatch_sites=sites, distinct_targets=len(targets),
                unresolved=unresolved, outside_text_not_certified=outside)


def prepare_phase(source, block, branch, output):
    """Only isolated cards; all subsequent execution goes through retail Load."""
    raw = source.read_bytes()
    entries = card_entries(raw)
    original = next(e for e in entries if e['block'] == block)
    if original['disc'] != 2:
        raise ValueError('ending phases require a disc-two USA save')
    credits = branch == 'credits'
    stage, story = ('roll', 294) if credits else ('s20a', 286)
    # Keep unrelated bits in this packed story byte.
    offset = block * 8192 + 0x108 + 0x38 + 190 + 0x5c
    value = (raw[offset] & ~8) | (8 if branch == 'otacon' else 0)
    fields = [] if credits else [dict(offset=0x5c, width=1, value=value,
        source='USA s20a/ea54 and ending/ea54: global005c bit8 selects ending branch')]
    card = phase_card(raw, block, stage, story, fields)
    entry = card_entries(card)[0]
    route = load_recipe(entry, False)
    route['steps'] = route['steps'][:4]
    route['steps'][-1]['until'] = dict(stage=stage)
    if credits:
        # USA names this actor ending.c; the Integral decomp calls it ending2.c.
        route['steps'] += [dict(name='original-credits-start', until=dict(actor_ending_count=1),
            action=dict(sequence='wait:30,circle:6,wait:60'), attempts=60, max_frames=6000),
            dict(name='credits-and-final-phone', until=dict(gcl_120003d7=3), max_frames=40000),
            dict(name='original-credits-to-rank', until=dict(stage='rank',
            load_request=0, actor_loader_count=0), max_frames=18000, stable_frames=90)]
    else:
        route['steps'] += [
            dict(name='escape-aftermath-film', until=dict(stage='s20a', story=290),
                 action=dict(sequence='wait:60,circle:6,wait:60,start:6,wait:60'), attempts=70, max_frames=10000),
            dict(name='original-final-codec', until=dict(stage='s20a', story=292),
                 when=dict(pause_level=dict(mask=1, eq=1)),
                 action=dict(sequence='cross:6,wait:6'), attempts=2000, max_frames=24000),
            dict(name='ending-stage', until=dict(stage='ending'),
                 action=dict(sequence='wait:60,start:6,wait:90'), attempts=100, max_frames=15000),
            dict(name='credits-entry', until=dict(stage='roll', story=294, actor_loader_count=0, load_request=0),
                 action=dict(sequence='wait:60,start:6,wait:90'), attempts=100, max_frames=15000, stable_frames=30)]
    output.mkdir(parents=True, exist_ok=False)
    (output / 'cards').mkdir()
    (output / 'cards/card1.mcd').write_bytes(card)
    (output / 'cards/card2.mcd').write_bytes(isolated_card(raw))
    entry.update(format='mgs-scene-case-1', source_card=str(source.resolve()), source_sha256=sha256(source),
                 card_sha256={f'card{n}.mcd': sha256(output / f'cards/card{n}.mcd') for n in (1, 2)},
                 synthetic_phase=dict(source='USA retail GCL, Foxdie libgcl/variable.c, validated USA 5f save layout',
                     scope=('Synthetic credits entry: original complete roll and rank callback.' if credits else
                            f'Synthetic post-escape {branch} branch: original films, Codec, ending and credits entry.') +
                           ' No proof of reachability from torture or escape.',
                     changes=dict(stage=stage, story=story, variables=fields), route=route))
    write_json(output / 'scenario.json', entry)
    return entry


def evidence(manifest):
    report = dict(format='mgs-completion-evidence-1',
                  scope='Independent transitions and phase exits. Full campaign completion remains unproven.',
                  date=manifest.get('date'), reference_documents=manifest.get('reference_documents', []),
                  open_findings=manifest.get('open_findings', []),
                  resolved_findings=manifest.get('resolved_findings', []),
                  static_audits=[], cases=[], counts={})
    for audit in manifest.get('static_audits', []):
        path = ROOT / audit['artifact']['path']
        item = dict(audit, status='missing')
        if path.is_file():
            item['status'] = 'unchanged' if sha256(path) == audit['artifact']['sha256'] else 'changed'
        report['static_audits'].append(item)
    for case in manifest['cases']:
        row = dict(case, evidence=[])
        for test in case.get('tests', []):
            references = test['results']
            paths = [ROOT / (p['path'] if isinstance(p, dict) else p) for p in references]
            result = dict(test, artifacts=[], status='missing')
            if paths and all(p.is_file() for p in paths):
                result['artifacts'] = [identity(p) for p in paths]
                if any(isinstance(ref, dict) and ref['sha256'] != artifact['sha256']
                       for ref, artifact in zip(references, result['artifacts'])):
                    result.update(status='changed', error='result differs from manifest fingerprint')
                    row['evidence'].append(result)
                    continue
                records = [json.loads(p.read_text()) for p in paths]
                if len(records) == 2 and all(r.get('format') == 'mgs-scene-result-1' for r in records):
                    result['status'] = compare(*records)['status']
                else:
                    result['status'] = 'complete' if all(r.get('status') == 'complete' for r in records) else 'needs-review'
                result['synthetic'] = any(r.get('synthetic_fixture') or r.get('synthetic_ancestry') or
                    r.get('scenario', {}).get('synthetic_phase') for r in records)
                # A passed child certifies its own transition, not a timed-out
                # parent. Keep the failed ancestry visible in derived reports.
                result['diagnostic_ancestry'] = [parent for record in records
                    for parent in record.get('diagnostic_ancestry', [])]
                observations = []
                for record in records:
                    engines = record.get('engines', {record.get('engine', 'native'): record})
                    for engine, run in engines.items():
                        final = run.get('final_state', {})
                        binaries = run.get('identity') or run.get('binaries', {})
                        observations.append(dict(engine=engine, status=run.get('status'),
                            final={k: final[k] for k in ('stage', 'story', 'disc', 'difficulty', 'life', 'max_life') if k in final},
                            runtime=binaries.get('runtime'), frames=run.get('frames', run.get('completed_frames'))))
                result['observations'] = observations
            row['evidence'].append(result)
        passed = [r for r in row['evidence'] if r['status'] == 'complete']
        # A list of passed partial tests never turns an entire chapter green.
        row['coverage'] = 'partial' if passed else 'unverified'
        row.pop('tests', None)
        report['cases'].append(row)
    report['counts'] = dict(campaign_cases=len(report['cases']),
        partially_exercised=sum(r['coverage'] == 'partial' for r in report['cases']),
        fully_certified=0,
        passed_test_contracts=sum(t['status'] == 'complete' for r in report['cases'] for t in r['evidence']))
    report['evidence_integrity'] = ('valid' if
        all(t['status'] == 'complete' for r in report['cases'] for t in r['evidence']) and
        all(a['status'] == 'unchanged' for a in report['static_audits']) else 'invalid')
    return report


def prepare_clear_reload(parent, output):
    result = json.loads(parent.read_text())
    if (result.get('format') != 'mgs-situation-result-1' or result.get('status') != 'complete'
            or not result.get('seed_states_and_cards_unchanged')
            or any(not result['engines'][e].get('card_goal', {}).get('passed') for e in ('native', 'donor'))):
        raise ValueError('requires a passed paired clear-game save test')
    record = result['engines']['native']['card_goal']['after']
    source = Path(record['path'])
    if sha256(source) != record['sha256']:
        raise ValueError('completed-game memory card changed')
    raw = source.read_bytes()
    entry = next(e for e in card_entries(raw) if (e['stage'], e['story'], e['disc']) == ('rank', 0, 1))
    route = load_recipe(entry, False)
    route['steps'] = route['steps'][:4]
    route['steps'][-1].update(until=dict(stage='d00a'), max_frames=14000, attempts=110)
    ready = dict(stage='s00a', difficulty=entry['difficulty'], disc=1,
        control={'ne': 0}, life=256, max_life=256, pause_level=0, load_request=0,
        actor_loader_count=0, game_over_timer=0,
        game_status={'mask': 0xdc080000, 'eq': 0}, player_status={'mask': 0x20000000, 'eq': 0})
    route['steps'] += [
        dict(name='original-clear-game-opening', until=dict(stage='s00a'),
            action=dict(sequence='wait:30,start:6,wait:30'), max_frames=8000, attempts=100),
        dict(name='original-clear-game-codec', until=ready,
            when=dict(pause_level={'mask': 1, 'eq': 1}),
            action=dict(sequence='cross:6,wait:6'), max_frames=12000, attempts=1000, stable_frames=30)]
    output.mkdir(parents=True, exist_ok=False)
    (output / 'cards').mkdir()
    (output / 'cards/card1.mcd').write_bytes(isolated_card(raw, entry['block']))
    (output / 'cards/card2.mcd').write_bytes(isolated_card(raw))
    entry.update(format='mgs-scene-case-1', source_card=str(source), source_sha256=sha256(source),
        parent=identity(parent), card_sha256={f'card{n}.mcd': sha256(output / f'cards/card{n}.mcd') for n in (1, 2)},
        synthetic_phase=dict(source='Original rank save, then original title Load; inherited synthetic credits entry',
            scope='Read the actual completed-game save from a new boot on Disc 1 and reach controllable dock. '
                  'No new RAM/card payload edits; does not verify all unlocks or a complete campaign.', route=route))
    write_json(output / 'scenario.json', entry)
    return entry



def prepare_movie(source, block, code, output):
    """Isolated source-bound USA disc-two card; no host writes during playback."""
    phases = {'e80c': (263, 265, 482, '913c'), '9cd7': (267, 269, 1112, '913e')}
    if code not in phases:
        raise ValueError('unknown verified movie')
    raw = source.read_bytes()
    original = next(e for e in card_entries(raw) if e['block'] == block)
    if original['disc'] != 2:
        raise ValueError('Liquid movie fixture requires a USA disc-two seed')
    before, during, total, proc = phases[code]
    card = phase_card(raw, block, 'd18a', before)
    entry = card_entries(card)[0]
    route = load_recipe(entry, False)
    route['steps'] = route['steps'][:4]
    route['steps'][-1]['until'] = dict(stage='d18a')
    route['steps'].append(dict(name='retail-movie-created',
        until=dict(current_stage='d18a', story=during, actor_movie_count=1,
                   movie_total_frames=total),
        action=dict(sequence='wait:30,circle:6,wait:30'), attempts=100, max_frames=10000))
    output.mkdir(parents=True, exist_ok=False)
    (output / 'cards').mkdir()
    (output / 'cards/card1.mcd').write_bytes(card)
    (output / 'cards/card2.mcd').write_bytes(isolated_card(raw))
    entry.update(format='mgs-scene-case-1', source_card=str(source.resolve()), source_sha256=sha256(source),
        card_sha256={f'card{n}.mcd': sha256(output / f'cards/card{n}.mcd') for n in (1, 2)},
        synthetic_phase=dict(source=f'USA d18a/ea54 main/proc{proc}, chara3453/{code}; game/movie.c',
            scope='Synthetic pre-movie card loaded through retail Load. Constructor presence does not prove playback or campaign reachability.',
            changes=dict(stage='d18a', story=before, variables=[]), route=route))
    write_json(output / 'scenario.json', entry)
    return entry

def prepare_torture(source, block, output, include_intro=False):
    """Source-bound capture fixture; original script creates the torture actor."""
    raw = source.read_bytes()
    original = next(e for e in card_entries(raw) if e['block'] == block)
    if original['disc'] != 1:
        raise ValueError('torture requires a USA disc-one seed')
    base = block * 8192 + 0x108 + 0x38 + 190
    fields = [dict(offset=offset, width=1, value=value,
                   source='USA s03b/ea54 main @13945..14520, constructor7197, survival c48b, surrender4742')
              for offset, value in ((0x65, 0),
                  (0x64, raw[base + 0x64] & ~2 if include_intro else raw[base + 0x64] | 2),
                  (0xa9, raw[base + 0xa9] & ~2), (0x5c, raw[base + 0x5c] & ~8))]
    card = phase_card(raw, block, 's03b', 157, fields)
    entry = card_entries(card)[0]
    route = load_recipe(entry, False)
    route['steps'] = route['steps'][:4]
    route['steps'][-1]['until'] = dict(stage='s03b')
    route['steps'].append(dict(name='original-torture-actor',
        until=dict(actor_torture_count=1, actor_loader_count=0, load_request=0,
                   gcl_14010064=0 if include_intro else 1),
        action=dict(sequence='wait:30,circle:6,wait:60'), attempts=100, max_frames=10000, stable_frames=30))
    output.mkdir(parents=True, exist_ok=False)
    (output / 'cards').mkdir()
    (output / 'cards/card1.mcd').write_bytes(card)
    (output / 'cards/card2.mcd').write_bytes(isolated_card(raw))
    entry.update(format='mgs-scene-case-1', source_card=str(source.resolve()), source_sha256=sha256(source),
        card_sha256={f'card{n}.mcd': sha256(output / f'cards/card{n}.mcd') for n in (1, 2)},
        synthetic_phase=dict(source='USA s03b/ea54 and Foxdie chara/torture/torture.c; USA actor callback800C9194',
            scope='Synthetic capture with ' + ('intro pending' if include_intro else 'intro-seen flag') +
                  '; original actor startup via retail Load. No proof of capture or intro continuity.',
            changes=fields, route=route))
    write_json(output / 'scenario.json', entry)
    return entry


def result_status(record, returncode):
    """Interrupted/partial JSON is a failed check, never an implicit pass."""
    status = record.get('status')
    if status not in ('complete', 'not-applicable') or returncode not in (0, 2):
        return 'needs-review'
    return status if (status == 'complete') == (returncode == 0) else 'needs-review'


def death_matrix(evidence_paths, output, selected_stages=None):
    """One ordinary-Snake death/Continue per distinct available scene."""
    selected, seen = [], set()
    for path in evidence_paths:
        for case in json.loads(path.read_text())['scenarios']:
            stage = case['stage']
            if selected_stages and stage not in selected_stages:
                continue
            if stage in seen:
                continue
            seen.add(stage)
            paths = [ROOT / (case[k]['path'] if isinstance(case[k], dict) else case[k])
                     for k in ('native_result', 'donor_result')]
            selected.append((stage, paths))
    output.mkdir(parents=True, exist_ok=False)
    report = dict(format='mgs-completion-death-matrix-1', status='running', results=[],
                  scope='Synthetic lethal target event, followed by original death/Continue. Does not test success paths.')
    template = json.loads((ROOT / 'scripts/fixtures/completion/s13a-death-continue.json').read_text())
    def one(case):
        stage, paths = case
        native = json.loads(paths[0].read_text())
        initial = native['final_state']
        recipe = json.loads(json.dumps(template))
        recipe['name'] = stage + '-death-continue'
        recipe['requires'].update(stage=stage, story=initial['story'])
        recipe['route']['steps'][-1]['until'].update(stage=stage, story=initial['story'])
        recipe_path = output / (stage + '.json')
        write_json(recipe_path, recipe)
        config = output / (stage + '.toml')
        config.write_text(config_with_cue(CONFIG.read_text(), Path(native['binaries']['cue']['path'])))
        with (output / (stage + '.log')).open('w') as log:
            code = subprocess.run([sys.executable, str(ROOT / 'scripts/mgs_situations.py'),
                *(str(p) for p in paths), str(output / stage), '--recipe', str(recipe_path),
                '--donor-runner', str(ROOT / '.local/libretro_completion'), '--period-us', '1000',
                '--config', str(config), '--timeout', '90'], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT).returncode
        result = output / stage / 'result.json'
        status = result_status(json.loads(result.read_text()) if result.exists() else {}, code)
        return dict(stage=stage, status=status, returncode=code,
                    result=identity(result) if result.exists() else None)
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        for row in pool.map(one, selected):
            report['results'].append(row)
            write_json(output / 'result.json', report)
            print(f"{row['stage']}: {row['status']}", flush=True)
    report['counts'] = {s: sum(r['status'] == s for r in report['results'])
                        for s in ('complete', 'not-applicable', 'needs-review')}
    report['status'] = 'needs-review' if report['counts']['needs-review'] else 'complete'
    write_json(output / 'result.json', report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    scan = commands.add_parser('catalog')
    scan.add_argument('output', type=Path)
    scan.add_argument('--decomp', type=Path, default=DECOMP)
    scan.add_argument('--discs', type=Path, nargs=2, default=[ROOT / f'data/discs/usa/disc{n}/Metal Gear Solid (USA) (Disc {n}) (v1.0).bin' for n in (1, 2)])
    prep = commands.add_parser('prepare-phase')
    prep.add_argument('source', type=Path)
    prep.add_argument('block', type=int)
    prep.add_argument('branch', choices=('meryl', 'otacon', 'credits'))
    prep.add_argument('output', type=Path)
    reload_clear = commands.add_parser('prepare-clear-reload')
    reload_clear.add_argument('parent', type=Path)
    reload_clear.add_argument('output', type=Path)
    torture = commands.add_parser('prepare-torture')
    torture.add_argument('source', type=Path)
    torture.add_argument('block', type=int)
    torture.add_argument('output', type=Path)
    torture.add_argument('--include-intro', action='store_true')
    movie = commands.add_parser('prepare-movie')
    movie.add_argument('source', type=Path)
    movie.add_argument('block', type=int)
    movie.add_argument('code', choices=('e80c', '9cd7'))
    movie.add_argument('output', type=Path)
    review = commands.add_parser('report')
    review.add_argument('manifest', type=Path)
    review.add_argument('output', type=Path)
    static = commands.add_parser('static-entries')
    static.add_argument('output', type=Path)
    static.add_argument('--directory', type=Path, default=ROOT / 'engine/ps1Runtime/src')
    static.add_argument('--config', type=Path, default=CONFIG)
    death = commands.add_parser('death-matrix')
    death.add_argument('output', type=Path)
    death.add_argument('--evidence', type=Path, nargs='+', default=[ROOT / 'docs/mgs-scenario-evidence.json', ROOT / 'docs/mgs-disc2-scenario-evidence.json'])
    death.add_argument('--stages', nargs='+', help='only these distinct stages; omit for all supplied seeds')
    args = parser.parse_args()
    if args.command == 'static-entries':
        result = static_entries(args.directory, args.config)
        write_json(args.output, result)
    elif args.command == 'death-matrix':
        result = death_matrix(args.evidence, args.output, args.stages)
    elif args.command == 'prepare-phase':
        result = prepare_phase(args.source, args.block, args.branch, args.output)
    elif args.command == 'prepare-clear-reload':
        result = prepare_clear_reload(args.parent, args.output)
    elif args.command == 'prepare-torture':
        result = prepare_torture(args.source, args.block, args.output, args.include_intro)
    elif args.command == 'prepare-movie':
        result = prepare_movie(args.source, args.block, args.code, args.output)
    elif args.command == 'catalog':
        result = catalog(args.discs, args.decomp)
        write_json(args.output, result)
    else:
        result = evidence(json.loads(args.manifest.read_text()))
        write_json(args.output, result)
    summary = result.get('counts') or {k: result[k] for k in
        ('status', 'stage', 'story', 'literal_main_dispatch_sites', 'distinct_targets') if k in result}
    print(json.dumps(summary))
    return 1 if result.get('status') == 'needs-review' or result.get('evidence_integrity') == 'invalid' else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, StopIteration) as error:
        print(f'Completion: {error}', file=sys.stderr)
        sys.exit(1)
