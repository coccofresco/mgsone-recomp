from pathlib import Path
import struct
import sys
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_scenarios import card_entries, compare, config_with_cue, isolated_card, load_recipe
from mgs_tas_route import compile_route


def checksum_directory(card, block):
    value = 0
    for byte in card[block * 128:block * 128 + 127]:
        value ^= byte
    card[block * 128 + 127] = value


def fixture():
    card = bytearray(131072)
    card[:2] = b'MC'
    for block in (4, 14):
        directory = block * 128
        card[directory] = 0x51
        struct.pack_into('<I', card, directory + 4, 8192)
        card[directory + 8:directory + 10] = b'\xff\xff'
        card[directory + 10:directory + 24] = b'BASLUS-00594G0'
        checksum_directory(card, block)
        data = bytearray(0xa38)
        struct.pack_into('<II', data, 0, 0x5f, 0x800)
        data[24:28] = b's08c'
        struct.pack_into('<h', data, 0x38 + 2, 1)
        struct.pack_into('<h', data, 0x38 + 8, 1)
        struct.pack_into('<hh', data, 0x38 + 22, 384, 384)
        struct.pack_into('<h', data, 0x100, 75)
        start = block * 8192 + 0x100
        struct.pack_into('<II', card, start, 0xa40, zlib.crc32(data))
        card[start + 8:start + 0xa40] = data
    return card


class ScenariosTest(unittest.TestCase):
    def test_disc_two_boot_preserves_seamless_disc_one(self):
        import tomllib
        source = '[disc]\ncue_path = "/tmp/disc1.cue"\nadditional_cues = ["/tmp/disc2.cue"]\n'
        parsed = tomllib.loads(config_with_cue(source, Path('/tmp/disc2.cue')))
        self.assertEqual(parsed['disc']['cue_path'], '/tmp/disc2.cue')
        self.assertEqual(parsed['disc']['additional_cues'], ['/tmp/disc1.cue'])
        self.assertEqual(tomllib.loads(config_with_cue(source, Path('/tmp/disc1.cue')))['disc'],
                         tomllib.loads(source)['disc'])

    def test_disc_selection_changes_only_disc_section(self):
        import tomllib
        source = '[disc]\nexe_path = "retail.exe"\ncue_path = "disc1.cue"\n[other]\ncue_path = "untouched"\n'
        changed = config_with_cue(source, Path('/tmp/disc 2.cue'))
        parsed = tomllib.loads(changed)
        self.assertEqual(parsed['disc'], {'exe_path': 'retail.exe', 'cue_path': '/tmp/disc 2.cue'})
        self.assertEqual(parsed['other']['cue_path'], 'untouched')
        with self.assertRaises(ValueError):
            config_with_cue('[disc]\nexe_path = "x"\n', Path('/tmp/disc2.cue'))

    def test_save_layout_and_isolation_preserve_retail_payload(self):
        raw = fixture()
        entry = card_entries(raw)[0]
        self.assertEqual((entry['stage'], entry['story'], entry['difficulty'], entry['life']),
                         ('s08c', 75, 1, 384))
        isolated = isolated_card(raw, 14)
        self.assertEqual([e['block'] for e in card_entries(isolated)], [14])
        self.assertEqual(isolated[14 * 8192:15 * 8192], raw[14 * 8192:15 * 8192])
        self.assertEqual(isolated[14 * 128:15 * 128], raw[14 * 128:15 * 128])
        self.assertEqual(isolated[4 * 8192:5 * 8192], bytes(8192))
        self.assertEqual(card_entries(isolated_card(raw)), [])

    def test_corrupted_directory_or_save_and_foreign_layout_are_rejected(self):
        for address in (4 * 128 + 20, 4 * 8192 + 0x200):
            raw = fixture()
            raw[address] ^= 1
            with self.assertRaises(ValueError):
                card_entries(raw)
        raw = fixture()
        start = 4 * 8192 + 0x100
        struct.pack_into('<I', raw, start + 8, 0x60)
        struct.pack_into('<I', raw, start + 4, zlib.crc32(raw[start + 8:start + 0xa40]))
        with self.assertRaisesRegex(ValueError, 'Integral'):
            card_entries(raw)

    def test_control_checks_require_consecutive_observations_and_real_inputs(self):
        recipe = load_recipe(card_entries(fixture())[0])
        route = compile_route(recipe, [], {})
        self.assertIn('STABLE 30', route)
        self.assertIn('INPUT 6 16384 0', route)  # Cross, no RAM editing.
        self.assertEqual([s['name'] for s in recipe['steps'][-2:]], ['crouch', 'stand'])
        for duration in (0, 3601, 1.5):
            recipe['steps'][-1]['stable_frames'] = duration
            with self.assertRaises(ValueError):
                compile_route(recipe, [], {})

    def test_saved_progress_and_post_cutscene_progress_are_distinct(self):
        entry = dict(card_entries(fixture())[0], stage='s09a', story=137)
        recipe = load_recipe(entry)
        self.assertEqual(recipe['steps'][3]['until']['story'], 137)
        self.assertEqual(recipe['steps'][5]['until']['story'], 141)
        self.assertNotIn('action', recipe['steps'][5])
        self.assertEqual(recipe['steps'][-1]['guard']['story'], 141)
        self.assertEqual(entry['story'], 137)

    def test_liquid_posture_uses_player_flag_instead_of_linked_stance(self):
        entry = dict(card_entries(fixture())[0], stage='s18a', story=277, disc=2)
        recipe = load_recipe(entry)
        duck, recover = recipe['steps'][-2:]
        self.assertEqual((duck['name'], recover['name']), ('duck', 'recover'))
        self.assertEqual(duck['until']['player_status'], {'mask': 0x20000020, 'eq': 0x20})
        self.assertNotIn('stance', duck['until'])
        self.assertIn('cross:', recover['action']['sequence'])
        self.assertNotIn('action', recipe['steps'][-3])

    def test_differential_check_rejects_different_contracts_and_failed_runs(self):
        native = dict(engine='native', status='complete', route_sha256='route',
                      textures=dict(count=123, sha256='same-materials'),
                      scenario=dict(stage='s08c', story=75, card_sha256={'card': 'digest'}),
                      final_state=dict(stage='s08c', story=75, life=384), scope='load/control')
        donor = dict(native, engine='donor')
        self.assertEqual(compare(native, donor)['status'], 'complete')
        self.assertEqual(compare(native, dict(donor, status='failed'))['status'], 'needs-review')
        different = dict(donor, final_state=dict(donor['final_state'], life=383))
        self.assertIn('life', compare(native, different)['differences'])
        with self.assertRaises(ValueError):
            compare(native, dict(donor, route_sha256='other'))
        for textures in (None, dict(count=122, sha256='missing'),
                         dict(count=123, sha256='wrong-palette')):
            changed = dict(donor, textures=textures)
            result = compare(native, changed)
            self.assertEqual(result['status'], 'needs-review')
            self.assertIn('textures', result['differences'])


if __name__ == '__main__':
    unittest.main()
