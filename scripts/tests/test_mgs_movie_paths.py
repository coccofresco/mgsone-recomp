from pathlib import Path
import json
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_movie_paths import if_blocks, paths, predicate, prepare, identity
from test_mgs_scenarios import fixture as card_fixture


STORY = dict(kind='variable', variable=dict(scope='global', offset=10, type='short'))


def literal(n):
    return dict(kind='literal', value=n)


def expr(op, n):
    return dict(kind='expr', offset=1, pretty=f'story {op} {n}',
                ast=dict(kind='binary', operator=op, left=STORY, right=literal(n)))


def command(name, *args):
    return dict(name=name, offset=10, arguments=list(args))


def block(*commands):
    return dict(kind='block', commands=list(commands))


def movie(code, next_story):
    return [command('eval', dict(kind='expr', ast=dict(operator='set', left=STORY, right=literal(next_story)))),
            command('chara', literal(0x3453), literal(code))]


def catalog(*commands):
    return paths([dict(stage='s01a', resource='a242', procedures=[dict(procedure='main', commands=commands)])], {1, 2, 3})


class MoviePathTests(unittest.TestCase):
    def test_else_if_conditions_include_exclusion_of_previous_branches(self):
        branch = command('if', expr('lt', 20), block(*movie(1, 20)),
            dict(kind='option', name='i'), expr('lt', 30), block(*movie(2, 30)),
            dict(kind='option', name='e'), block(*movie(3, 40)))
        rows = catalog(branch)
        self.assertEqual([r['story_ranges'] for r in rows], [[[0, 19]], [[20, 29]], [[30, 32767]]])
        self.assertEqual([g['truth'] for g in rows[1]['guards']], [False, True])

    def test_unreachable_branch_cannot_be_prepared(self):
        rows = catalog(command('if', expr('lt', 20), block(*movie(1, 20)),
            dict(kind='option', name='i'), expr('lt', 10), block(*movie(2, 10))))
        self.assertFalse(rows[1]['eligible'])
        self.assertEqual(rows[1]['story_ranges'], [])

    def test_unknown_state_or_nested_execution_stays_inventory_only(self):
        condition = expr('eq', 5)
        condition['ast']['left'] = dict(kind='variable', variable=dict(scope='global', offset=20, type='short'))
        self.assertFalse(catalog(command('if', condition, block(*movie(1, 6))))[0]['eligible'])
        nested = command('if', expr('lt', 30), block(command('if', expr('lt', 10), block(*movie(1, 10)))))
        self.assertFalse(catalog(nested)[0]['eligible'])
        loop = command('foreach', block(command('if', expr('lt', 30), block(*movie(1, 30)))))
        self.assertFalse(catalog(loop)[0]['eligible'])

    def test_assignment_after_call_cannot_supply_a_fake_exit(self):
        commands = movie(1, 20)
        commands.reverse()
        row = catalog(command('if', expr('lt', 20), block(*commands)))[0]
        self.assertFalse(row['eligible'])
        self.assertIsNone(row['story_after'])

    def test_impure_predicates_and_malformed_branches_fail_closed(self):
        with self.assertRaises(ValueError):
            predicate(dict(kind='binary', operator='set', left=STORY, right=literal(20)))
        for args in ((expr('lt', 20),), (expr('lt', 20), block(), dict(kind='option', name='q'))):
            with self.assertRaises(ValueError):
                list(if_blocks(command('if', *args)))

    def test_fixture_confirms_live_stage_and_preserves_its_source_card(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            card = root / 'source.mcd'
            card.write_bytes(card_fixture())
            original = card.read_bytes()
            source = root / 'script.json'
            source.write_text('{}')
            index = root / 'paths.json'
            rows = catalog(command('if', expr('lt', 20), block(*movie(1, 20))))
            index.write_text(json.dumps(dict(format='mgs-movie-paths-1', disc_number=1,
                inputs=[identity(source)], paths=rows, movies={'0001': dict(drive_seconds_2x=30)})))
            entry = prepare(index, card, 4, root/'case', 's01a', '0001')
            steps = entry['synthetic_phase']['route']['steps']
            self.assertEqual(steps[3]['until'], dict(current_stage='s01a'))
            self.assertEqual(steps[4]['when'], dict(actor_abst_count=1))
            self.assertEqual(steps[4]['until']['actor_movie_count'], 1)
            self.assertEqual(steps[-1]['until']['story'], dict(min=20))
            self.assertEqual(steps[-1]['until']['actor_movie_count'], 0)
            self.assertEqual(card.read_bytes(), original)
            source.write_text('changed')
            with self.assertRaisesRegex(ValueError, 'source evidence changed'):
                prepare(index, card, 4, root/'stale', 's01a', '0001')
            self.assertFalse((root/'stale').exists())


if __name__ == '__main__':
    unittest.main()
