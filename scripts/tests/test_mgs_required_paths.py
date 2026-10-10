from copy import deepcopy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_required_paths import entry_assignments, entry_map, literal


def assignment(scope, offset, value, kind='short'):
    right = dict(kind='literal', value=abs(value))
    if value < 0:
        right = dict(kind='unary', operator='negate', operand=right)
    return dict(name='eval', offset=4500 + offset, arguments=[dict(ast=dict(
        operator='set', left=dict(kind='variable', variable=dict(scope=scope, offset=offset, type=kind)),
        right=right))])


class RequiredPathsTests(unittest.TestCase):
    def test_map_prerequisite_comes_from_unique_retail_load_operand(self):
        proc = dict(procedure='f9db', commands=[dict(name='load', offset=467, arguments=[
            dict(kind='literal', type='string', value='s11h\0'),
            dict(kind='option', name='m'),
            dict(kind='literal', type='strid', value=32249),
            dict(kind='option', name='p'), dict(kind='literal', value=-5500)])])
        field = entry_map(proc, 's11h', 's11c/ea54')
        self.assertEqual((field['offset'], field['value']), (14, 32249))
        self.assertIn('s11c/ea54 procedure f9db load @467', field['source'])
        with self.assertRaises(ValueError):
            entry_map(proc, 's07b', 's11c/ea54')
        bad = []
        p = deepcopy(proc); p['commands'] *= 2; bad.append(p)
        p = deepcopy(proc); p['commands'][0]['arguments'][2]['kind'] = 'variable'; bad.append(p)
        p = deepcopy(proc); p['commands'][0]['arguments'][2]['value'] = 65536; bad.append(p)
        p = deepcopy(proc); p['commands'][0]['arguments'].extend(p['commands'][0]['arguments'][1:3]); bad.append(p)
        p = deepcopy(proc); p['commands'][0]['arguments'][3] = dict(kind='literal', value=1); bad.append(p)
        p = deepcopy(proc); p['commands'][0]['arguments'] = p['commands'][0]['arguments'][:2]; bad.append(p)
        for item in bad:
            with self.subTest(item=item), self.assertRaises(ValueError):
                entry_map(item, 's11h', 's11c/ea54')

    def setUp(self):
        self.procedure = dict(procedure='a4ad', commands=[
            assignment('global', 10, 66), assignment('global', 84, 2),
            assignment('global', 86, 640), assignment('link', 14, 65, 'strid'),
            assignment('link', 16, -1000), assignment('link', 20, 18000),
            dict(name='restart', arguments=[])])

    def test_complete_constant_entry_keeps_boss_counters_and_spawn(self):
        globals_, linked = entry_assignments(self.procedure, 's05a', 66)
        self.assertEqual([(x['offset'], x['value']) for x in globals_], [(84, 2), (86, 640)])
        self.assertEqual([(x['offset'], x['value']) for x in linked], [(14, 65), (16, -1000), (20, 18000)])
        self.assertTrue(linked[1]['signed'])
        self.assertTrue(all('a4ad eval @' in x['source'] for x in globals_ + linked))

    def test_cannot_silently_ignore_dynamic_effects_or_partial_procedures(self):
        bad = []
        p = deepcopy(self.procedure); p['commands'][1]['name'] = 'if'; bad.append(p)
        p = deepcopy(self.procedure); p['commands'][1]['arguments'][0]['ast']['right'] = dict(kind='opaque'); bad.append(p)
        p = deepcopy(self.procedure); p['commands'].pop(); bad.append(p)
        p = deepcopy(self.procedure); p['commands'].insert(1, dict(name='restart', arguments=[])); bad.append(p)
        p = deepcopy(self.procedure); p['commands'].pop(0); bad.append(p)
        p = deepcopy(self.procedure); p['commands'][0] = assignment('global', 10, 68); bad.append(p)
        p = deepcopy(self.procedure); p['commands'].insert(1, assignment('global', 10, 66)); bad.append(p)
        p = deepcopy(self.procedure); p['commands'][1] = assignment('global', 84, 2, 'bool'); bad.append(p)
        for procedure in bad:
            with self.subTest(procedure=procedure), self.assertRaises(ValueError):
                entry_assignments(procedure, 's05a', 66)
        for value in (True, '66', 1.0):
            with self.assertRaises(ValueError):
                literal(dict(kind='literal', value=value))
