from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_tas_route import action_runs, actor_counts, actor_variable, check, compile_route, drive_step, gcl_variable, render_input, snapshot


class RouteTests(unittest.TestCase):
    def test_effect_actor_counts_require_complete_lists_and_ignore_stale_heap(self):
        ram = bytearray(2097152)
        word = lambda p, v: struct.pack_into('<I', ram, p & 0x1fffffff, v)
        for level in range(9):
            head = 0x800af390 + level * 68
            word(head + 4, head + 32); word(head + 32, head)
        head = 0x800af390 + 6 * 68
        a, b = 0x80010000, 0x80010100
        word(head + 4, a); word(a, head); word(a + 4, b)
        word(b, a); word(b + 4, head + 32); word(head + 32, b)
        for p in (a, b, 0x80010200):
            word(p + 8, 0x80070000); word(p + 20, 0x80020000)
        ram[0x20000:0x2000a] = b'bakudan.c\0'
        original = bytes(ram)
        fields = ['actor_bakudan_count', 'actor_kogaku2_count']
        self.assertEqual([snapshot(ram, fields)[f] for f in fields], [2, 0])
        self.assertEqual(ram, original)
        recipe = {'format': 'mgs-tas-route-1', 'steps': [
            {'name': 'planted', 'until': {'actor_bakudan_count': {'min': 1}}}]}
        self.assertIn('UNTIL actor_bakudan_count min 1', compile_route(recipe, [], {}))
        for p, value in ((b, 0), (b + 4, a), (head + 32, head), (a + 20, 0x801fffff)):
            ram[:] = original; word(p, value); ram[-1] = ord('x')
            self.assertIsNone(actor_counts(ram))
            self.assertFalse(check({'actor_kogaku2_count': 0}, snapshot(ram, fields)))
            self.assertFalse(check({'actor_bakudan_count': {'ne': 0}}, snapshot(ram, fields)))
        ram[:] = original; word(head + 4, head + 32); word(head + 32, head)
        self.assertEqual(actor_counts(ram).get('bakudan.c', 0), 0)

    def test_actor_observation_has_bounded_filename_only(self):
        for field in ('actor__count', 'actor_kogaku2_timer', 'actor_../foo_count',
                      'actor_RedAlert_count', 'actor_' + 'a' * 49 + '_count'):
            self.assertIsNone(actor_variable(field))
        self.assertEqual(actor_variable('actor_red_alrt_count'), 'red_alrt.c')

    def test_game_over_can_occur_with_snake_alive(self):
        ram = bytearray(2097152)
        struct.pack_into('<h', ram, 0xb7526, 256)
        struct.pack_into('<i', ram, 0xadb44, -2)
        state = snapshot(ram)
        self.assertEqual(state['life'], 256)
        self.assertEqual(state['game_over_timer'], -2)
        self.assertFalse(check({'life': {'min': 1}, 'game_over_timer': 0}, state))

    def test_ocelot_layout_is_separate_from_guard_and_torture_actor(self):
        ram = bytearray(2097152)
        base = 0x10000
        ram[0xb7500:0xb7505] = b's04b\0'
        ram[0x20000:0x2000b] = b'revolver.c\0'
        struct.pack_into('<3I', ram, base, 0x80010000, 0x80010000, 0x800d811c)
        struct.pack_into('<I', ram, base+20, 0x80020000)
        struct.pack_into('<H', ram, base+0x54, 0x09fb)
        struct.pack_into('<h', ram, base+0x24, -5826)
        struct.pack_into('<i', ram, base+0x854, 657)
        struct.pack_into('<2i', ram, base+0x8fc, 2, 65540)
        fields = ['revolver_09fb_' + k for k in ('x', 'life', 'phase', 'timer')]
        self.assertEqual([snapshot(ram, fields)[k] for k in fields], [-5826, 657, 2, 65540])
        ram[0xb7500:0xb7505] = b's11e\0'
        self.assertIsNone(snapshot(ram, fields)[fields[1]])
        ram[0xb7500:0xb7505] = b's04b\0'
        struct.pack_into('<I', ram, base, 0)
        self.assertIsNone(snapshot(ram, fields)[fields[1]])

    def test_prison_guard_life_dereferences_target_and_rejects_other_overlays(self):
        ram = bytearray(2097152)
        ram[0xb7500:0xb7505] = b's03d\0'
        struct.pack_into('<i', ram, 0xdfb70, 12)
        struct.pack_into('<i', ram, 0xdfbe8, 15)
        ram[0x20000:0x20007] = b'zako.c\0'
        base = 0x10000
        struct.pack_into('<3I', ram, base, base | 0x80000000, base | 0x80000000, 0x800c1000)
        struct.pack_into('<I', ram, base+20, 0x80020000)
        struct.pack_into('<H', ram, base+0x50, 0x41e2)
        field = 'zako_41e2_life'
        self.assertIsNone(snapshot(ram, [field])[field])
        struct.pack_into('<I', ram, base+0x8fc, 0x80030000)
        struct.pack_into('<h', ram, 0x30000+38, -4)
        self.assertEqual(snapshot(ram, [field])[field], -4)
        self.assertEqual(snapshot(ram)['ambush_phase'], 12)
        self.assertEqual(snapshot(ram)['ambush_kills'], 15)
        ram[0xb7500:0xb7505] = b's11e\0'
        self.assertIsNone(snapshot(ram, [field])[field])
        self.assertIsNone(snapshot(ram)['ambush_phase'])

    def test_elevator_arrival_uses_named_actor_and_signed_short_timer(self):
        ram = bytearray(2097152)
        base = 0x10000
        ram[0x20000:0x2000a] = b'evpanel.c\0'
        struct.pack_into('<3I', ram, base, base | 0x80000000, base | 0x80000000, 0x800c1000)
        struct.pack_into('<I', ram, base+20, 0x80020000)
        struct.pack_into('<2H', ram, base+0x2c, 0x7858, 2)
        struct.pack_into('<2h', ram, base+0x36, -1, 600)
        struct.pack_into('<h', ram, base+0x42, 2)
        fields = ['evpanel_7858_' + k for k in ('phase', 'timer', 'door_phase')]
        self.assertEqual([snapshot(ram, fields)[k] for k in fields], [2, -1, 2])
        struct.pack_into('<I', ram, base, 0)  # detached/stale actor
        self.assertIsNone(snapshot(ram, fields)[fields[0]])

    def test_sampled_game_clock_requires_bounded_runs_and_no_mixed_domains(self):
        action = {'sampled_game_runs': [[1, 16, 8], [2, 0, 0]]}
        r = {'format': 'mgs-tas-route-1', 'steps': [{'name': 'sampled',
             'until': {'stage': 's03a'}, 'action': action}]}
        self.assertIn('CLOCK GAME\nINPUT 1 16 8\nINPUT 2 0 0', compile_route(r, [], {}))
        for bad in ({'sampled_game_runs': [[0, 1, 0]]},
                    {'sampled_game_runs': [[1, 65536, 0]]},
                    {'sampled_game_runs': [[1, 16, 8]], 'source_frames': [1, 2]}):
            r['steps'][0]['action'] = bad
            with self.assertRaises(ValueError):
                compile_route(r, [], {})

    def test_local_timing_adjustment_preserves_source_edges_and_ports(self):
        movie = [(16, 0)] * 3 + [(128, 8)] * 2 + [(0, 0)] * 4
        action = {'source_frames': [1, 9], 'run_adjustments': [
            {'first_frame': 4, 'extra_frames': 20}, {'first_frame': 6, 'extra_frames': -2}]}
        self.assertEqual(action_runs(action, movie, {'events': []}),
                         [(3, 16, 0), (22, 128, 8), (2, 0, 0)])
        for edits in ([{'first_frame': 5, 'extra_frames': 1}],
                      [{'first_frame': 4, 'extra_frames': -2}],
                      [{'first_frame': 4, 'extra_frames': 1}] * 2):
            action['run_adjustments'] = edits
            with self.assertRaises(ValueError):
                action_runs(action, movie, {'events': []})

    def test_movement_compiles_bounded_world_coordinates_without_timed_keys(self):
        r={'format':'mgs-tas-route-1','steps':[{'name':'go','until':{'x':{'min':900}},
            'action':{'move_to':{'x':1000,'z':-500}}}]}
        self.assertIn('MOVE 1000 -500',compile_route(r,[],{}))
        for action in ({'move_to':{'x':32768,'z':0}}, {'move_to':{'x':0}},
                       {'move_to':{'x':0,'z':0},'sequence':'up:5'}):
            r['steps'][0]['action']=action
            with self.assertRaises(ValueError):compile_route(r,[],{})

    def test_named_patrol_requires_live_unique_actor_and_signed_coordinates(self):
        ram = bytearray(2097152)
        ram[0x20000:0x2000a] = b'watcher.c\0'
        field = 'watcher_1466_target_z'
        self.assertIsNone(snapshot(ram, [field])[field])
        for base in (0x10000, 0x12000):
            struct.pack_into('<3I', ram, base, base | 0x80000000, base | 0x80000000, 0x800c1000)
            struct.pack_into('<I', ram, base + 20, 0x80020000)
            struct.pack_into('<H', ram, base + 0x50, 0x1466)
            struct.pack_into('<h', ram, base + 0xbe4, -16000)
            state = snapshot(ram, [field])
            self.assertEqual(state[field], -16000 if base == 0x10000 else None)
        # A stale actor with a broken list link must not conceal the live one.
        struct.pack_into('<I', ram, 0x12000, 0)
        self.assertEqual(snapshot(ram, [field])[field], -16000)
        struct.pack_into('<4h', ram, 0x10000 + 0xb8c, 2048, 512, 8000, 2)
        sight = ['watcher_1466_vision_' + member for member in ('facing', 'angle', 'range')]
        sight.append('watcher_1466_visible')
        observed = snapshot(ram, sight)
        self.assertEqual([observed[name] for name in sight], [2048, 512, 8000, 2])
        recipe = {'format': 'mgs-tas-route-1', 'steps': [
            {'name': 'patrol', 'until': {field: -16000}}]}
        self.assertIn('UNTIL watcher_1466_target_z eq -16000', compile_route(recipe, [], {}))

    def test_camera_and_guard_ids_are_separate_and_camera_has_no_patrol_layout(self):
        ram = bytearray(2097152)
        for base, name in ((0x10000, b'watcher.c\0'), (0x12000, b'camera.c\0')):
            ram[base+0x100:base+0x100+len(name)] = name
            struct.pack_into('<3I', ram, base, base | 0x80000000, base | 0x80000000, 0x800c1000)
            struct.pack_into('<I', ram, base+20, (base+0x100) | 0x80000000)
            struct.pack_into('<H', ram, base+0x50, 0xdd6d)
            struct.pack_into('<h', ram, base+0x2a, -30 if base == 0x12000 else 99)
        struct.pack_into('<2i', ram, 0x12000+0x1e8, 2, 65542)
        observed = snapshot(ram, ['camera_dd6d_facing', 'watcher_dd6d_facing', 'camera_dd6d_timer'])
        self.assertEqual(observed['camera_dd6d_timer'], 65542)
        self.assertEqual(observed['camera_dd6d_facing'], -30)
        self.assertEqual(observed['watcher_dd6d_facing'], 99)
        self.assertTrue(check({'camera_dd6d_facing': {'mask': 4095, 'eq': 4066}}, observed))
        recipe = {'format': 'mgs-tas-route-1', 'steps': [{'name': 'camera',
            'until': {'camera_dd6d_target_x': 0}}]}
        with self.assertRaises(ValueError):
            compile_route(recipe, [], {})

    def test_original_gcl_operands_preserve_types_and_reject_invalid_ranges(self):
        fields = ['gcl_11800002', 'gcl_1402002d', 'gcl_1100000a', 'gcl_1600000a']
        ram = bytearray(2097152)
        ram[0xB7512] = 3
        ram[0xB646D] = 4
        struct.pack_into('<h', ram, 0xB644A, -1)
        state = snapshot(ram, fields)
        self.assertEqual([state[f] for f in fields], [3, 1, -1, 65535])
        self.assertTrue(check({'gcl_11800002': 3, 'gcl_1402002d': 1}, state))
        for invalid in ('gcl_118000c0', 'gcl_110007ff', 'gcl_19000000',
                        'gcl_14880000', 'gcl_11100000'):
            self.assertIsNone(gcl_variable(invalid))

    def test_compiler_preserves_state_gates_and_imported_edges(self):
        recipe = {'format': 'mgs-tas-route-1', 'steps': [
            {'name': 'fire', 'when': {'guards_in_room': {'min': 1}},
             'guard': {'life': {'min': 1}}, 'until': {'story': 29},
             'abort_when': {'socom_ammo': 0},
             'action': {'source_frames': [1, 2], 'minimum_hold': 4, 'minimum_release': 4},
             'max_frames': 80, 'attempts': 3},
            {'name': 'wait-for-transition', 'until': {'stage': 's03b'}}]}
        program = compile_route(recipe, [(32768, 8), (0, 0)], {'events': []})
        self.assertIn('WHEN guards_in_room min 1 4294967295', program)
        self.assertIn('ABORT socom_ammo eq 0 4294967295', program)
        self.assertIn('INPUT 4 32768 8\nINPUT 4 0 0\nEND', program)
        self.assertTrue(program.endswith('UNTIL stage eq s03b 4294967295\nEND\n'))
        with self.assertRaisesRegex(ValueError, 'system event'):
            compile_route(recipe, [(32768, 8), (0, 0)], {'events': [{'frame': 2}]})

    def test_compiler_rejects_unsupported_observations_and_malformed_goals(self):
        for goal in ({'weapons': 12}, {'stage': 's03d\nINPUT 10 8 0'},
                     {'life': '256'}, {'life': {'mask': 255}},
                     {'life': {'mask': -1, 'eq': 1}}):
            recipe = {'format': 'mgs-tas-route-1', 'steps': [{'name': 'bad', 'until': goal}]}
            with self.subTest(goal=goal), self.assertRaises(ValueError):
                compile_route(recipe, [], {'events': []})

    def test_import_stretches_short_edges_and_preserves_both_ports(self):
        movie = [(8, 0), (0, 0), (0x20, 0x4000), (0x20, 0x4000)]
        action = {'source_frames': [1, 4], 'minimum_hold': 6, 'minimum_release': 4, 'settle_frames': 10}
        runs = action_runs(action, movie, {'events': []})
        self.assertEqual(runs, [(6, 8, 0), (4, 0, 0), (6, 0x20, 0x4000), (10, 0, 0)])
        duration, script = render_input(runs, 100)
        self.assertEqual(duration, 26)
        self.assertEqual(script, '100 105 start 0\n110 115 right 0\n110 115 cross 1\n')
        with self.assertRaisesRegex(ValueError, 'system event'):
            action_runs(action, movie, {'events': [{'frame': 2}]})

    def test_waits_for_gate_and_checks_result_before_next_attempt(self):
        states = [{'stage': 'd00a'}, {'stage': 'title'}, {'stage': 's00a'}]
        cursor = [0]
        seen = []
        def execute(runs, kind):
            seen.append((runs, kind))
            cursor[0] += 1
            return sum(r[0] for r in runs)
        step = {'name': 'menu', 'when': {'stage': 'title'}, 'until': {'stage': 's00a'},
                'action': {'sequence': 'circle:6,wait:4'}, 'poll_frames': 20}
        drive_step(step, lambda: states[cursor[0]], execute, [], {}, lambda *args: None)
        self.assertEqual(seen, [([(20, 0, 0)], 'wait'), ([(6, 8192, 0), (4, 0, 0)], 'action')])

    def test_timeout_includes_extended_checkpoint_tail(self):
        step = {'name': 'absent', 'until': {'stage': 's04b'}, 'max_frames': 30, 'poll_frames': 20}
        calls = []
        def execute(runs, kind):
            calls.append(kind)
            return 35  # requested 20, but the first portable boundary was later
        with self.assertRaisesRegex(ValueError, 'timeout after 35'):
            drive_step(step, lambda: {'stage': 's03d'}, execute, [], {}, lambda *args: None)
        self.assertEqual(calls, ['wait'])

    def test_guard_prevents_wrong_stage_inputs_and_missing_actor_is_not_ne(self):
        step = {'name': 'ambush', 'guard': {'stage': 's03d'}, 'until': {'story': 30},
                'action': {'sequence': 'square:10'}}
        with self.assertRaisesRegex(ValueError, 'guard failed'):
            drive_step(step, lambda: {'stage': 's00a'}, lambda *args: self.fail('input leaked'),
                       [], {}, lambda *args: None)
        self.assertFalse(check({'title_state': {'ne': 6}}, {'title_state': None}))
        self.assertTrue(check({'game_status': {'mask': 0x80000, 'eq': 0}}, {'game_status': 4}))
        with self.assertRaises(ValueError):
            check({'stage': {'equals': 's00a'}}, {'stage': 'title'})

    def test_source_is_not_repeated_without_explicit_attempt_limit(self):
        calls = []
        step = {'name': 'unaccepted', 'when': {'stage': 'title'}, 'until': {'stage': 's00a'},
                'action': {'sequence': 'circle:2'}, 'poll_frames': 2, 'max_frames': 6}
        def execute(runs, kind):
            calls.append(kind)
            return 2
        with self.assertRaises(ValueError):
            drive_step(step, lambda: {'stage': 'title'}, execute, [], {}, lambda *args: None)
        self.assertEqual(calls, ['action', 'wait', 'wait'])

    def test_title_actor_uses_usa_offsets_and_requires_unique_live_actor(self):
        ram = bytearray(2097152)
        ram[0xB7500:0xB7506] = b'title\0'
        ram[0x10000:0x10007] = b'open.c\0'
        for base in (0x20000,):
            struct.pack_into('<I', ram, base + 8, 0x800D26E0)
            struct.pack_into('<I', ram, base + 20, 0x80010000)
            struct.pack_into('<i', ram, base + 0xA48, 21)
            struct.pack_into('<i', ram, base + 0xA4C, 0)
            struct.pack_into('<i', ram, base + 0x24B8, 1)
        observed = snapshot(ram)
        self.assertEqual(observed['title_state'], 21)
        self.assertEqual(observed['title_level'], 1)
        ram[0x30000:0x32500] = ram[0x20000:0x22500]
        self.assertIsNone(snapshot(ram)['title_state'])


if __name__ == '__main__':
    unittest.main()
