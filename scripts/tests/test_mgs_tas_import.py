import gzip
import contextlib
import io
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_tas_import import card_image, input_intervals, main, parse_pjm


def fixture(rows, text=True, cards=False):
    header = bytearray(52)
    header[:4] = b'PJM '
    struct.pack_into('<IIHBBII', header, 4, 2, 20, (64 if text else 0) | (8 if cards else 0),
                     4, 4, len(rows), 0)
    image = b'MC' + bytes(131070)
    card = struct.pack('<I', len(image)) + gzip.compress(image) if cards else bytes(4)
    chunks = [bytes(4), card, card, bytes(4), b'\x02slus00594slus00776']
    payload = bytearray()
    for index, chunk in enumerate(chunks):
        struct.pack_into('<I', header, 24 + index * 4, len(header) + len(payload))
        payload.extend(chunk)
    struct.pack_into('<I', header, 44, len(header) + len(payload))
    # Reproduce the historical oversized author-length field.
    struct.pack_into('<I', header, 48, 512)
    return bytes(header + payload) + b''.join(rows)


class PjmTests(unittest.TestCase):
    def test_text_physical_bit_order_and_second_port(self):
        # Deliberately independent expected bit positions, including shoulders.
        bits = [15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 0]
        rows = []
        for index in range(14):
            pad = bytearray(b'..............')
            pad[index] = ord('x')
            rows.append(bytes(pad) + b'|' + bytes(pad) + b'|0|\r\n')
        meta, frames, _ = parse_pjm(fixture(rows))
        self.assertEqual(frames, [(1 << bit, 1 << bit) for bit in bits])
        self.assertEqual(meta['events'], [])
        self.assertEqual(meta['disc_ids'], ['slus00594', 'slus00776'])

    def test_text_command_is_exponent_binary_command_is_mask(self):
        text, _, _ = parse_pjm(fixture([b'..............|..............|2|\r\n']))
        binary, pads, _ = parse_pjm(fixture([struct.pack('<HHB', 0x4080, 0x8, 4)], text=False))
        self.assertEqual(text['events'], binary['events'])
        self.assertEqual(text['events'], [{'frame': 1, 'mask': 4, 'kind': 'disc_lid_toggle'}])
        self.assertEqual(pads, [(0x4080, 0x8)])

    def test_intervals_preserve_held_edges_ports_and_absolute_offset(self):
        frames = [(0, 0), (0x4010, 8), (0x10, 8), (0x10, 0), (0, 0)]
        result = input_intervals(frames, 2, 4, 100)
        self.assertEqual(result, '100 100 cross 0\n100 101 start 1\n100 102 up 0\n')
        for first, last, target in [(0, 3, 1), (1, 6, 1), (3, 1, 1), (1, 2, 0)]:
            with self.assertRaises(ValueError):
                input_intervals(frames, first, last, target)

    def test_embedded_cards_are_bounded_and_untampered(self):
        _, _, cards = parse_pjm(fixture([bytes(5)], text=False, cards=True))
        self.assertEqual(cards, [b'MC' + bytes(131070)] * 2)
        for bad in [bytes(4), struct.pack('<I', 131072) + gzip.compress(bytes(131073)),
                    struct.pack('<I', 131072) + gzip.compress(bytes(131072)) + b'extra']:
            with self.assertRaises(ValueError):
                card_image(bad)

    def test_corrupt_and_unsupported_movies_are_rejected(self):
        valid = fixture([b'..............|..............|0|\r\n'])
        bad_offset = bytearray(valid)
        struct.pack_into('<I', bad_offset, 44, 0xfffffff0)
        for bad in [valid[:-1], valid + b'x', b'PJM ', bytes(bad_offset),
                    valid[:-4] + b'9|\r\n']:
            with self.assertRaises(ValueError):
                parse_pjm(bad)
        for flag in [2, 4, 16, 32, 128, 256, 512, 1024]:
            bad = bytearray(valid)
            struct.pack_into('<H', bad, 12, 64 | flag)
            with self.assertRaisesRegex(ValueError, 'flags'):
                parse_pjm(bad)

    def test_conversion_refuses_to_silently_drop_disc_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            movie = root / 'movie.pjm'
            movie.write_bytes(fixture([bytes(5), struct.pack('<HHB', 0, 0, 4)], text=False))
            target = root / 'converted'
            argv = ['import', 'convert', str(movie), str(target), '--first-vblank', '50']
            with patch.object(sys, 'argv', argv), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(main(), 1)
            self.assertFalse(target.exists())
            with patch.object(sys, 'argv', argv + ['--last-frame', '1']), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(main(), 0)
            self.assertTrue((target / 'import.json').is_file())


if __name__ == '__main__':
    unittest.main()
