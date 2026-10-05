from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_situations import PROBES, assess, builtin
from mgs_tas_route import check, compile_route, snapshot


def state():
    return dict(snapshot(bytes(2097152)), stage='s15a', story=219, difficulty=1, disc=2,
                control=0x80110000, life=467, max_life=768, item=8, weapon=0)


class SituationTest(unittest.TestCase):
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
