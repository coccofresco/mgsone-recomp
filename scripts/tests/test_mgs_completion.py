import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_completion import evidence, prepare_phase, prepare_torture, result_status, static_entries
from mgs_tas import sha256
from mgs_scenarios import card_entries
from mgs_tas_route import compile_route
from test_mgs_scenarios import fixture


class CompletionTest(unittest.TestCase):
    def test_torture_seed_keeps_other_flags_and_valid_crc_without_touching_source(self):
        card = fixture()
        base = 4 * 8192 + 0x108
        variables = base + 0x38 + 190
        for offset in (0x5c, 0x64, 0x65, 0xa9):
            card[variables + offset] = 0xff
        struct.pack_into('<I', card, base - 4, zlib.crc32(card[base:base + 0xa38]))
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / 'source.mcd'
            source.write_bytes(card)
            for intro in (False, True):
                output = root / str(intro)
                report = prepare_torture(source, 4, output, intro)
                raw = (output / 'cards/card1.mcd').read_bytes()
                entry, = card_entries(raw)  # validates the rebuilt retail CRC
                self.assertEqual((entry['stage'], entry['story'], entry['disc']), ('s03b', 157, 1))
                self.assertEqual([raw[variables + o] for o in (0x5c, 0x64, 0x65, 0xa9)],
                                 [0xf7, 0xfd if intro else 0xff, 0, 0xfd])
                self.assertEqual(source.read_bytes(), card)
                route = compile_route(report['synthetic_phase']['route'], [], {})
                self.assertIn('UNTIL gcl_14010064 eq ' + str(int(not intro)), route)
                self.assertIn('No proof of capture or intro continuity', report['synthetic_phase']['scope'])
            struct.pack_into('<h', card, base + 0x38 + 8, 2)
            struct.pack_into('<I', card, base - 4, zlib.crc32(card[base:base + 0xa38]))
            source.write_bytes(card)
            with self.assertRaisesRegex(ValueError, 'disc-one'):
                prepare_torture(source, 4, root / 'wrong-disc')

    def test_partial_or_interrupted_reports_never_pass(self):
        for record, code in (({}, 0), ({'status': None}, 0), ({'status': 'running'}, 0),
                             ({'status': 'complete'}, 1), ({'status': 'not-applicable'}, 0)):
            self.assertEqual(result_status(record, code), 'needs-review')
        self.assertEqual(result_status({'status': 'complete'}, 0), 'complete')
        self.assertEqual(result_status({'status': 'not-applicable'}, 2), 'not-applicable')

    def test_missing_evidence_does_not_certify_a_chapter(self):
        report = evidence({'cases': [dict(id='tank', tests=[dict(results=['no-such-evidence.json']), dict(results=[])])]})
        self.assertEqual(report['cases'][0]['coverage'], 'unverified')
        self.assertEqual(report['counts']['fully_certified'], 0)
        self.assertEqual(report['counts']['passed_test_contracts'], 0)

    def test_report_rejects_changed_results_and_labels_synthetic_ancestry(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'result.json'
            path.write_text(json.dumps(dict(format='mgs-situation-result-1', status='complete', synthetic_ancestry=True)))
            manifest = dict(cases=[dict(id='rex', tests=[dict(results=[dict(path=str(path), sha256=sha256(path))])])])
            report = evidence(manifest)
            self.assertEqual(report['cases'][0]['coverage'], 'partial')
            self.assertTrue(report['cases'][0]['evidence'][0]['synthetic'])
            path.write_text(json.dumps(dict(status='complete')))
            report = evidence(manifest)
            self.assertEqual(report['cases'][0]['coverage'], 'unverified')
            self.assertEqual(report['cases'][0]['evidence'][0]['status'], 'changed')
            self.assertEqual(report['evidence_integrity'], 'invalid')

    def test_static_audit_detects_missing_entry_and_rejects_empty_scan(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            config = root / 'config.toml'
            config.write_text('[[functions]]\naddress="0x80010000"\nsize=256\n')
            registry = root / 'recompiled_out.cpp'
            registry.write_text('recomp_func_table[0x80010000] = func_80010000;\n')
            with self.assertRaisesRegex(ValueError, 'no compiled'):
                static_entries(root, config)
            (root / 'recompiled_out.part000.cpp').write_text('\nvoid caller() { recomp_dispatch(rdram, ctx, 0x80010040); }\n')
            self.assertEqual(static_entries(root, config)['status'], 'needs-review')
            registry.write_text(registry.read_text() + 'recomp_entry_alias_table[0x80010040] = entry;\n')
            self.assertEqual(static_entries(root, config)['status'], 'complete')

    def test_passed_child_retains_failed_parent_in_derived_evidence(self):
        ancestry = [dict(parent=dict(path='parent.json', sha256='a' * 64),
                         status='needs-review', reason='Explicit clean-timeout resume')]
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'result.json'
            path.write_text(json.dumps(dict(format='mgs-situation-result-1', status='complete',
                synthetic_ancestry=True, diagnostic_ancestry=ancestry)))
            report = evidence(dict(cases=[dict(id='prison-escape', tests=[dict(
                results=[dict(path=str(path), sha256=sha256(path))])])]))
            result = report['cases'][0]['evidence'][0]
            self.assertEqual(result['status'], 'complete')
            self.assertEqual(result['diagnostic_ancestry'], ancestry)
            self.assertEqual(report['counts']['fully_certified'], 0)

    def test_synthetic_phases_preserve_source_and_unrelated_branch_flags(self):
        card = fixture()
        for block in (4, 14):
            base = block * 8192 + 0x108
            struct.pack_into('<h', card, base + 0x38 + 8, 2)
            card[base + 0x38 + 190 + 0x5c] = 0x40
            struct.pack_into('<I', card, base - 4, zlib.crc32(card[base:base + 0xa38]))
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / 'source.mcd'
            source.write_bytes(card)
            for branch in ('meryl', 'otacon', 'credits'):
                out = root / branch
                report = prepare_phase(source, 4, branch, out)
                self.assertEqual(source.read_bytes(), card)
                self.assertIn('Synthetic', report['synthetic_phase']['scope'])
                compiled = compile_route(report['synthetic_phase']['route'], [], {})
                self.assertIn('UNTIL stage eq ' + ('rank' if branch == 'credits' else 'roll') + ' ', compiled)
                raw = (out / 'cards/card1.mcd').read_bytes()
                self.assertEqual(raw[4 * 8192 + 0x108 + 0x38 + 190 + 0x5c],
                                 0x48 if branch == 'otacon' else 0x40)
                self.assertEqual(len(card_entries(raw)), 1)
                self.assertEqual(json.loads((out / 'scenario.json').read_text()), report)


if __name__ == '__main__':
    unittest.main()
