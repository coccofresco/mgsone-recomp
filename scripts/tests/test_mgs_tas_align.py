from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_gcl_values import decode_gcl_value
from mgs_tas_align import candidates, hash16, script_tree


def word(value):
    return value.to_bytes(2, 'big')


def block(body):
    return b'\x40' + word(len(body) + 3) + body + b'\0'


def command(code, arguments):
    body = word(code) + b'\0' + arguments + b'\0'
    return b'\x60' + word(len(body) + 2) + body


def expression(variable, literal, operator):
    body = bytes.fromhex(variable) + b'\x01' + word(literal) + bytes([0x31, operator, 0x31, 0])
    return b'\x30' + bytes([len(body) + 1]) + body


def script(main, procedures=()):
    table, bodies = b'', b''
    for code, body in procedures:
        table += word(code) + word(len(bodies))
        bodies += block(body)
    table += bytes(4)
    base = 4 + len(table)
    return (base + len(bodies) - 8).to_bytes(4, 'big') + table + bodies + block(main)


class AlignmentTests(unittest.TestCase):
    def test_story_trigger_and_load_keep_original_offsets_and_else_branch(self):
        names = {1: 'if', 2: 'eval', 3: 'trap', 4: 'load'}
        stage = b'\x07\x05s01a\0'
        load = command(4, stage)
        trigger = command(3, b'\x06' + word(hash16('liftup')) + block(load))
        set_story = command(2, expression('1100000a', 7, 20))
        main = command(1, expression('1100000a', 7, 11) + block(set_story)
                       + b'\x50e\x01' + block(trigger))
        data = script(main)
        tree = script_tree(data, names)
        volume = {'resource': 'synthetic', 'name': 'liftup', 'bounds': [0] * 8}
        result = candidates(tree, {hash16('liftup'): [volume]})
        self.assertEqual([item['command'] for item in result], ['eval', 'trap', 'load'])
        self.assertEqual(result[0]['story_value'], 7)
        self.assertEqual(result[0]['observation'], 'gcl_1100000a')
        self.assertIsNone(result[0]['lexical_parents'][0]['option'])
        self.assertEqual(result[1]['lexical_parents'][0]['option'], 'e')
        self.assertEqual(result[1]['volumes'], [volume])
        self.assertEqual(result[2]['target_stages'], ['s01a'])
        for item in result:
            self.assertEqual(data[item['offset']], 0x60)
        self.assertEqual(result[2]['lexical_parents'][-1]['command'], 'trap')

    def test_procedure_offsets_are_relative_to_body_table_end(self):
        call = b'\x70\x04\x12\x34\0'
        data = script(call, [(0x1234, command(2, expression('1100000a', 9, 20)))])
        tree = script_tree(data, {2: 'eval'})
        self.assertEqual([p['procedure'] for p in tree], ['1234', 'main'])
        self.assertEqual(tree[1]['commands'][0]['code'], 0x1234)
        self.assertEqual(candidates(tree, {})[0]['story_value'], 9)

    def test_truncated_lengths_and_unknown_commands_do_not_become_anchors(self):
        valid = script(command(4, b'\x07\x05s01a\0'))
        for data in (valid[:7], valid[:-3], script(b'\xff'),
                     script(command(4, b'\x07\xffshort'))):
            with self.subTest(data=data.hex()), self.assertRaises(ValueError):
                script_tree(data, {4: 'load'})

    def test_operand_semantics_match_retail_signed_short_and_flag_mask(self):
        value, end, errors = decode_gcl_value(bytes.fromhex('01ffff'))
        self.assertEqual((value['value'], end, errors), (-1, 3, []))
        value, end, errors = decode_gcl_value(bytes.fromhex('14040067'))
        self.assertEqual(value['variable']['flag_bit'], 16)
        self.assertEqual(value['variable']['offset'], 0x67)
        self.assertEqual((end, errors), (4, []))
        self.assertEqual(hash16('liftup'), 48597)
        self.assertEqual(hash16('move01A'), 27664)


if __name__ == '__main__':
    unittest.main()
