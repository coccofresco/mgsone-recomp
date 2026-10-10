"""Reject the plausible-looking transition endpoints that are not gameplay."""
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_disc_change import require_gameplay


class DiscChangeAuditTests(unittest.TestCase):
    def setUp(self):
        self.state = dict(stage='s13a', disc=2, pause_level=0, control=0x8015ee00,
                          load_request=0, game_status=0, player_status=0,
                          player_position=[7727, 17758, 8904])
        self.actors = {'snake.c': 1}

    def test_stable_second_disc_gameplay(self):
        require_gameplay(self.state, self.actors)

    def test_pending_return_transition_is_not_gameplay(self):
        self.state['load_request'] = 145
        with self.assertRaises(AssertionError):
            require_gameplay(self.state, self.actors)

    def test_loader_fade_and_disc_prompt_are_not_gameplay(self):
        for name in ('loader.c', 'fadeio.c', 'change.c'):
            with self.subTest(actor=name), self.assertRaises(AssertionError):
                require_gameplay(self.state, {**self.actors, name: 1})

    def test_spawn_or_return_door_is_not_forward_movement(self):
        for pos in ([7724, 19786, 10932], [7399, 20104, 11254]):
            self.state['player_position'] = pos
            with self.subTest(position=pos), self.assertRaises(AssertionError):
                require_gameplay(self.state, self.actors)

    def test_missing_player_or_blocked_control(self):
        for actors in (None, {}, {'snake.c': 2}):
            with self.subTest(actors=actors), self.assertRaises(AssertionError):
                require_gameplay(self.state, actors)
        for field, value in (('control', 0), ('game_status', 0x80000000),
                             ('player_status', 0x20000000), ('pause_level', 1)):
            with self.subTest(field=field), self.assertRaises(AssertionError):
                require_gameplay({**self.state, field: value}, self.actors)


if __name__ == '__main__':
    unittest.main()
