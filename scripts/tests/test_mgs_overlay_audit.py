from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_overlay_audit import analyze, normal_registry, signature_matches
from mgs_overlay_catalog import BASE


class OverlayAuditTest(unittest.TestCase):
    def test_resume_labels_do_not_count_as_callback_registration(self):
        text = '''static recomp_func_t recomp_lookup_overlay() {
    if (resume) { case 0x800C5980u: return overlay_usa_a__800C5978; }
    case 0x800C5978u: return overlay_usa_a__800C5978;
}
static int next;
recomp_func_table[0x80010000] = main;
'''
        main, entries = normal_registry(text)
        self.assertEqual(entries, {'usa_a': {BASE + 16}})
        self.assertEqual(main, {0x80010000})

    def test_signature_collision_and_missing_first_match_entry_fail(self):
        payload = struct.pack('<9I', 1, BASE+16, 0, 0, 0x03e00008, 0, 0x24020001, 0x03e00008, 0)
        payload += struct.pack('<I', BASE+24)
        specs = [dict(name=n, ram_base=BASE, size=36, identity_size=8) for n in ('usa_a','usa_b')]
        entries = {'usa_a':{BASE+16}, 'usa_b':{BASE+16,BASE+24}}
        result = analyze(specs, {n:payload for n in entries}, set(), entries)
        self.assertEqual(result['status'], 'needs-review')
        self.assertEqual([r['stage'] for r in result['missing_entries']], ['usa_a','usa_b'])
        entries['usa_a'].add(BASE+24)
        changed = bytearray(payload)
        changed[24] ^= 1
        result = analyze(specs, {'usa_a':payload,'usa_b':bytes(changed)}, set(), entries)
        self.assertTrue(result['identity_collisions'])
        for spec in specs:
            spec['additional_identity_offsets'] = [24]
        self.assertEqual(analyze(specs, {'usa_a':payload,'usa_b':bytes(changed)}, set(), entries)['status'], 'complete')

    def test_empty_and_bad_identity_audits_are_rejected(self):
        with self.assertRaises(ValueError):
            analyze([], {}, set(), {})
        for spec in (dict(identity_size=0), dict(identity_size=8, additional_identity_offsets=[10])):
            with self.assertRaises(ValueError):
                signature_matches(spec, bytes(8), bytes(8))

    def test_missing_main_constructor_is_reported_without_reading_overlay_at_negative_offset(self):
        payload = struct.pack('<6I', 1, 0x80010000, 0, 0, 0x03e00008, 0)
        spec = dict(name='usa_a', ram_base=BASE, size=len(payload), identity_size=8)
        result = analyze([spec], {'usa_a': payload}, set(), {'usa_a': {BASE + 16}})
        self.assertEqual(result['status'], 'needs-review')
        self.assertEqual(result['missing_entries'][0]['target'], '0x80010000')
        self.assertIsNone(result['missing_entries'][0]['empty_return'])


if __name__ == '__main__':
    unittest.main()
