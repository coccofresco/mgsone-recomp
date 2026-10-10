from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_fixture import RAM_SIZE, actors, checkpoint_bytes, checksum, patch_ram, phase_card
from test_mgs_scenarios import fixture as card_fixture
from mgs_scenarios import card_entries


def actor_ram(count=1):
    ram = bytearray(RAM_SIZE)
    put = lambda p, v: struct.pack_into('<I', ram, p & 0x1fffffff, v)
    for level in range(9):
        head = 0x800af390 + level * 68
        put(head + 4, head + 32)
        put(head + 32, head)
    head, previous = 0x800af390, 0x800af390
    for index in range(count):
        actor = 0x80120000 + index * 4096
        put(previous + 4, actor)
        put(actor, previous)
        put(actor + 8, 0x8005ce1c)
        put(actor + 20, 0x80130000)
        previous = actor
    put(previous + 4, head + 32)
    put(head + 32, previous)
    ram[0x130000:0x130008] = b'snake.c\0'
    return ram


def patch(**kwargs):
    return dict(address=0x80010000, width=2, expect=0, value=42,
                source='test verified field', reason='test precondition', **kwargs)


class FixtureTest(unittest.TestCase):
    def test_linked_phase_negative_spawn_exact_bounds_and_crc(self):
        original = card_fixture()
        before = bytes(original)
        fields = [dict(offset=16, width=2, value=-1000, signed=True, source='entry spawn'),
                  dict(offset=188, width=2, value=0x1234, source='last USA linked short')]
        baseline = phase_card(original, 4, 's05a', 66)
        changed = phase_card(original, 4, 's05a', 66, linked=fields)
        base = 4 * 8192 + 0x108
        linked = base + 0x38
        self.assertEqual(original, before)
        self.assertEqual(struct.unpack_from('<h', changed, linked + 16)[0], -1000)
        self.assertEqual(changed[linked + 188:linked + 190], b'\x34\x12')
        self.assertEqual(card_entries(changed)[0]['story'], 66)  # also verifies CRC
        allowed = set(range(base - 4, base)) | set(range(linked + 16, linked + 18)) | set(range(linked + 188, linked + 190))
        self.assertTrue(all(a == b or i in allowed for i, (a, b) in enumerate(zip(baseline, changed))))
        for bad in (dict(offset=189, width=2), dict(offset=190, width=1),
                    dict(offset=-1, width=1), dict(offset=0, width=4), dict(source='')):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                phase_card(original, 4, 's05a', 66, linked=[dict(fields[0], **bad)])
        with self.assertRaisesRegex(ValueError, 'overlapping'):
            phase_card(original, 4, 's05a', 66, linked=fields + fields[:1])

    def test_exact_writes_and_overlap_rejection(self):
        ram = bytes(RAM_SIZE)
        result, changes = patch_ram(ram, [patch()])
        self.assertEqual(result[0x10000:0x10002], b'\x2a\0')
        self.assertEqual(result[:0x10000] + result[0x10002:], ram[:0x10000] + ram[0x10002:])
        self.assertEqual(changes[0]['before'], 0)
        with self.assertRaisesRegex(ValueError, 'overlapping'):
            patch_ram(ram, [patch(), patch()])

    def test_reject_invalid_preconditions_and_unbounded_writes(self):
        for changes in ({'expect': 5}, {'source': ''}, {'reason': ''},
                        {'width': 8}, {'address': 0x80000020}, {'address': 0x80200000}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                patch_ram(bytes(RAM_SIZE), [dict(patch(), **changes)])

    def test_authored_actor_selector_is_unique_and_independent_of_list_order(self):
        ram = actor_ram(2)
        struct.pack_into('<h', ram, 0x120020, -1750)
        struct.pack_into('<h', ram, 0x121020, 1850)
        p = dict(actor='snake.c', act='0x8005ce1c', offset=0x30, width=2,
                 expect=0, value=42, source='test authored position', reason='test boundary',
                 where=[dict(offset=32, width=2, value=-1750, signed=True)])
        patched, changes = patch_ram(ram, [p])
        self.assertEqual(changes[0]['address'], '0x80120030')
        self.assertEqual(patched[0x121030:0x121032], b'\0\0')
        for selectors in ([dict(offset=8,width=4,value=0)],
                          [dict(offset=33,width=2,value=0)],
                          [dict(offset=32,width=2,value=1)],
                          [dict(offset=32,width=2,value=0,bogus=True)], []):
            with self.assertRaises(ValueError):patch_ram(ram,[dict(p,where=selectors)])
        struct.pack_into('<h', ram, 0x121020, -1750)
        with self.assertRaisesRegex(ValueError,'requires one'):patch_ram(ram,[p])

    def test_actor_identity_links_and_ambiguity(self):
        p = dict(actor='snake.c', act='0x8005ce1c', offset=32, width=2,
                 expect=0, value=1, source='USA binding', reason='test')
        ram = actor_ram()
        self.assertEqual(actors(ram)['snake.c'], [0x80120000])
        result, _ = patch_ram(ram, [p])
        self.assertEqual(result[0x120020], 1)
        for bad in (dict(p, act='0x80010000'), dict(p, offset=8)):
            with self.assertRaises(ValueError):
                patch_ram(ram, [bad])
        with self.assertRaisesRegex(ValueError, 'found 2'):
            patch_ram(actor_ram(2), [p])
        ram[0x120000] = 0
        with self.assertRaisesRegex(ValueError, 'invalid actor'):
            patch_ram(ram, [p])

    def test_indirect_target_is_bounded(self):
        ram = actor_ram()
        struct.pack_into('<I', ram, 0x120898, 0x80140000)
        p = dict(actor='snake.c', act='0x8005ce1c', offset=0x898,
                 indirect_offset=38, width=2, expect=0, value=1, source='TARGET', reason='test')
        result, _ = patch_ram(ram, [p])
        self.assertEqual(result[0x140026], 1)
        with self.assertRaises(ValueError):
            patch_ram(ram, [dict(p, indirect_offset=256)])
        struct.pack_into('<I', ram, 0x120898, 0)
        with self.assertRaises(ValueError):
            patch_ram(ram, [p])

    def test_native_version_nine_preserves_non_ram_trailer(self):
        ram = bytes(RAM_SIZE)
        changed, _ = patch_ram(ram, [patch()])
        state = bytearray(476) + ram + b'NSV9 scale table and media identity'
        state[:4] = b'PS1S'
        struct.pack_into('<III', state, 4, 9, 0x3956534e, checksum(state[16:]))
        output = checkpoint_bytes(state, ram, changed, 'native')
        self.assertEqual(output[476 + RAM_SIZE:], state[476 + RAM_SIZE:])
        self.assertEqual(struct.unpack_from('<I', output, 12)[0], checksum(output[16:]))

    def test_native_version_ten_keeps_xa_state_and_media(self):
        ram = bytes(RAM_SIZE)
        changed, _ = patch_ram(ram, [patch()])
        state = bytearray(476) + ram + b'NSVA XA history and gain' + bytes(64)
        state[:4] = b'PS1S'
        struct.pack_into('<III', state, 4, 10, 0x4156534e, checksum(state[16:]))
        output = checkpoint_bytes(state, ram, changed, 'native')
        self.assertEqual(output[476 + RAM_SIZE:], state[476 + RAM_SIZE:])
        self.assertEqual(struct.unpack_from('<I', output, 12)[0], checksum(output[16:]))

    def test_native_checksum_and_terminal_ram_witness(self):
        ram = bytes(RAM_SIZE)
        changed, _ = patch_ram(ram, [patch()])
        state = bytearray(476) + ram + b'unchanged GPU and continuation'
        state[:4] = b'PS1S'
        struct.pack_into('<III', state, 4, 8, 0x3856534e, checksum(state[16:]))
        output = checkpoint_bytes(state, ram, changed, 'native')
        self.assertEqual(output[476:476 + RAM_SIZE], changed)
        self.assertEqual(output[16:476], state[16:476])
        self.assertEqual(output[476 + RAM_SIZE:], state[476 + RAM_SIZE:])
        self.assertEqual(struct.unpack_from('<I', output, 12)[0], checksum(output[16:]))
        with self.assertRaisesRegex(ValueError, 'witness'):
            checkpoint_bytes(state, changed, ram, 'native')
        state[-1] ^= 1
        with self.assertRaisesRegex(ValueError, 'checksum'):
            checkpoint_bytes(state, ram, changed, 'native')

    def test_native_version_eleven_preserves_timer_phase_and_media(self):
        ram = bytes(RAM_SIZE)
        changed, _ = patch_ram(ram, [patch()])
        state = bytearray(476) + ram + bytes([1, 0, 1, 7]) + bytes(64)
        state[:4] = b'PS1S'
        struct.pack_into('<III', state, 4, 11, 0x4256534e, checksum(state[16:]))
        output = checkpoint_bytes(state, ram, changed, 'native')
        self.assertEqual(output[-68:], state[-68:])
        self.assertEqual(struct.unpack_from('<I', output, 12)[0], checksum(output[16:]))
        struct.pack_into('<I', state, 8, 0x4156534e)  # old tag must not masquerade as v11
        with self.assertRaisesRegex(ValueError, 'format'):
            checkpoint_bytes(state, ram, changed, 'native')

    def test_donor_requires_unique_full_ram(self):
        ram = bytes(range(256)) * 8192
        changed = b'Z' + ram[1:]
        state = b'DUCC\xff' + ram + b'\xffkeep all devices'
        self.assertEqual(checkpoint_bytes(state, ram, changed, 'donor'),
                         b'DUCC\xff' + changed + b'\xffkeep all devices')
        for invalid in (b'DUCCnothing', state + ram):
            with self.assertRaisesRegex(ValueError, 'exactly one'):
                checkpoint_bytes(invalid, ram, changed, 'donor')

    def test_native_version_twelve_keeps_bios_resources_and_media(self):
        ram = bytes(RAM_SIZE)
        changed, _ = patch_ram(ram, [patch()])
        tail = b'NSVC variable-length BIOS files and allocator metadata' + bytes(64)
        state = bytearray(476) + ram + tail
        state[:4] = b'PS1S'
        struct.pack_into('<III', state, 4, 12, 0x4356534e, checksum(state[16:]))
        output = checkpoint_bytes(state, ram, changed, 'native')
        self.assertEqual(output[476:476 + RAM_SIZE], changed)
        self.assertEqual(output[476 + RAM_SIZE:], tail)
        self.assertEqual(struct.unpack_from('<I', output, 12)[0], checksum(output[16:]))
        struct.pack_into('<I', state, 8, 0x4256534e)
        with self.assertRaisesRegex(ValueError, 'format'):
            checkpoint_bytes(state, ram, changed, 'native')

    def test_phase_card_recomputes_crc_and_preserves_source(self):
        source = card_fixture()
        before = bytes(source)
        output = phase_card(source, 4, 's20a', 286,
                            [dict(offset=0x5c, width=1, value=8, source='USA ending branch')])
        self.assertEqual(source, before)
        entries = card_entries(output)
        self.assertEqual(len(entries), 1)
        self.assertEqual((entries[0]['stage'], entries[0]['story']), ('s20a', 286))
        self.assertEqual(entries[0]['difficulty'], 1)
        self.assertEqual(output[4 * 8192 + 0x108 + 0x38 + 190 + 0x5c], 8)
        for bad in (dict(offset=10, width=2, value=0, source='x'),
                    dict(offset=2048, width=1, value=0, source='x')):
            with self.assertRaises(ValueError):
                phase_card(source, 4, 's20a', 286, [bad])


if __name__ == '__main__':
    unittest.main()
