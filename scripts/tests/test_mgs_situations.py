from pathlib import Path
import json
import sys
import tempfile
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_situations import assess_metrics, PROBES, assess, builtin, card_transition, run, timeout_boundary
from mgs_tas import sha256
from mgs_fixture import phase_card
from test_mgs_scenarios import fixture as card_fixture
from mgs_tas_route import check, compile_route, snapshot


def state():
    return dict(snapshot(bytes(2097152)), stage='s15a', story=219, difficulty=1, disc=2,
                control=0x80110000, life=467, max_life=768, item=8, weapon=0)


class SituationTest(unittest.TestCase):
    def test_only_clean_route_deadlines_are_diagnostic_seeds(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            execution, log = root / 'result.json', root / 'driver.log'
            execution.write_text(json.dumps(dict(status='failed',
                route_log=['[Route] status: failed condition timeout: gate'])))
            log.write_text('[donor-route] failed after 120 frames: condition timeout: gate\n')
            row = dict(status='failed', returncode=1, execution_result=dict(path=str(execution)),
                       event_log=dict(path=str(log)), invariant_changes={}, final_goal_satisfied=False)
            self.assertTrue(timeout_boundary(row, 'native'))
            self.assertTrue(timeout_boundary(dict(row, returncode=43), 'donor'))
            for bad in (dict(returncode=-9), dict(final_goal_satisfied=True),
                        dict(invariant_changes={'story': 0}), dict(game_errors=['panic']),
                        dict(interpreter_counters={'fallback': 1})):
                self.assertFalse(timeout_boundary(dict(row, **bad), 'native'))
            log.write_text('[donor-route] failed after 120 frames: guard/abort condition: gate\n')
            self.assertFalse(timeout_boundary(dict(row, returncode=43), 'donor'))

    def test_story_exit_does_not_replace_required_movie_decoding(self):
        required = {'mdec.decoded_frame': 480}
        self.assertFalse(assess_metrics(required, {})['passed'])
        for count in (0, 2, 479, None, '480', True):
            self.assertFalse(assess_metrics(required, {'counters': {'mdec.decoded_frame': count}})['passed'])
        self.assertTrue(assess_metrics(required, {'counters': {'mdec.decoded_frame': 480}})['passed'])
        for invalid in ([], {'x': 0}, {'x': True}, {'x': -1}, {'x': '1'}):
            with self.assertRaises(ValueError):
                assess_metrics(invalid, {})

    def test_dynamic_flags_can_be_compared_even_when_absent_from_route_goals(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            ram = root / 'final-ram.bin'
            ram.write_bytes(bytes(2097152))
            for engine in ('native', 'donor'):
                (root / (engine + '.json')).write_text(json.dumps(dict(
                    engine=engine, status='complete', format='mgs-scene-result-1',
                    scenario=dict(card_sha256={}, stage='s03b', story=0), route_sha256='same',
                    scope='Test eligibility only', session=str(root), final_ram_sha256=sha256(ram))))
            recipe = dict(format='mgs-situation-1', name='unarmed-branch-check', requires=dict(story=1),
                preserve=['gcl_12000065'], compare=['gcl_1200005c'],
                route=dict(format='mgs-tas-route-1', steps=[dict(name='goal', until=dict(story=2), max_frames=10)]))
            path = root / 'recipe.json'
            path.write_text(json.dumps(recipe))
            args = SimpleNamespace(native=root/'native.json', donor=root/'donor.json',
                                   output=root/'output', recipe=path)
            self.assertEqual(run(args), 2)  # Valid fields; unmet initial guard, so no execution.
            self.assertEqual(json.loads((args.output/'result.json').read_text())['status'], 'not-applicable')
            recipe['compare'] = ['invented_flag']
            path.write_text(json.dumps(recipe))
            with self.assertRaisesRegex(ValueError, 'unknown compare'):
                run(args)

    def test_save_requires_a_new_crc_valid_record(self):
        before = card_fixture()
        after = phase_card(before, 4, 'rank', 0)
        goal = dict(slot=1, fields=dict(stage='rank', story=0))
        self.assertTrue(card_transition(goal, before, after)['passed'])
        self.assertFalse(card_transition(goal, before, before)['passed'])
        self.assertFalse(card_transition(goal, after, after)['passed'])
        damaged = bytearray(after)
        damaged[4 * 8192 + 0x108 + 100] ^= 1
        with self.assertRaises(ValueError):
            card_transition(goal, before, damaged)

    def test_probes_require_an_observed_transition_not_a_noop(self):
        initial = state()
        for name in PROBES:
            with self.subTest(name=name):
                recipe = builtin(name, initial)
                self.assertTrue(check(recipe['requires'], initial))
                self.assertFalse(check(recipe['route']['steps'][0]['until'], initial))
                self.assertIn('INPUT ', compile_route(recipe['route'], [], {}))
                self.assertEqual(recipe['route']['steps'][-1]['stable_frames'], 30)

    def test_disabled_codec_and_unavailable_equipment_are_not_counted(self):
        initial = state()
        recipe = builtin('codec', initial)
        for change in ({'game_status': 0x2000}, {'player_status': 0x8000}, {'pause_level': 1}):
            self.assertFalse(check(recipe['requires'], dict(initial, **change)))
        self.assertFalse(check(builtin('quick-item', initial)['requires'], dict(initial, item=-1)))

    def test_health_loss_stage_change_or_failed_execution_cannot_pass(self):
        initial = state()
        recipe = builtin('pause', initial)
        self.assertEqual(assess(recipe, initial, initial, True)['status'], 'complete')
        for change in ({'life': 466}, {'stage': 's15b'}, {'pause_level': 2}):
            self.assertEqual(assess(recipe, initial, dict(initial, **change), True)['status'], 'failed')
        self.assertEqual(assess(recipe, initial, initial, False)['status'], 'failed')
        self.assertEqual(assess(recipe, initial, {}, False)['status'], 'failed')


if __name__ == '__main__':
    unittest.main()
