from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_overlay_catalog import BASE, callback_entries, code_extent


class OverlayCatalogTest(unittest.TestCase):
    def test_signed_pointer_materialization_finds_leaf_but_not_interior(self):
        # Actor table, constructor, and callback after its return. Materialize
        # C59A8 with signed low half 0x59A8 and reject interior C59AC.
        words = [0x1234, BASE + 16, 0, 0,
                 0x3C02800C, 0x244459A8, 0x244559AC,
                 0x03E00008, 0, 0, 0, 0, 0, 0, 0x03E00008, 0,
                 0x24020001, 0x03E00008, 0]
        payload = struct.pack(f'<{len(words)}I', *words)
        entries, table = callback_entries(payload, code_extent(payload))
        self.assertEqual(table, 8)
        self.assertIn(BASE + 16, entries)
        self.assertIn(BASE + 64, entries)
        self.assertNotIn(BASE + 68, entries)

    def test_noncode_tail_does_not_extend_code(self):
        payload = struct.pack('<6I', 0x1234, BASE + 16, 0, 0, 0x03E00008, 0) + b'tail\0' * 8
        self.assertEqual(code_extent(payload), 24)
        with self.assertRaises(ValueError):
            code_extent(b'no complete return')
        with self.assertRaises(ValueError):
            callback_entries(struct.pack('<2I', 0x03E00008, 0), 8)

    def test_pointer_tables_find_standalone_leaves_only(self):
        words = [0x1234, BASE + 16, 0, 0,
                 0x03E00008, 0, 0x24020001, 0x03E00008, 0]
        # Table lives after text; neither an interior nor an out-of-text
        # pointer is a valid callback. No LUI materializes the leaf address.
        payload = struct.pack('<9I', *words)
        payload += struct.pack('<4I', BASE + 24, BASE + 28, BASE + 36, BASE + 25)
        entries, _ = callback_entries(payload, 36)
        self.assertEqual(entries, [BASE + 16, BASE + 24])


if __name__ == '__main__':
    unittest.main()
