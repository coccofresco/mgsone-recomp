from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_tas import recover_prefix, sha256


class PrefixTests(unittest.TestCase):
    def test_bounded_prefix_uses_next_preinput_witness_and_preserves_initial_cards(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / 'source'
            source.mkdir()
            (source / 'initial.ps1s').write_bytes(b'initial state')
            (source / 'initial_cards').mkdir()
            (source / 'initial_cards/card1.mcd').write_bytes(b'initial card')
            # boundary, VBlank, RAM hash, PC, then two digital pad samples.
            frames = [[100+i*10, 500+i, 700+i, 0x80010000+i,
                       16, 128, 128, 128, 128, 65,
                       0, 128, 128, 128, 128, 65] for i in range(4)]
            (source / 'movie.mgst').write_text('MGSTAS 1\n' + ''.join(
                'F ' + ' '.join(map(str, row)) + '\n' for row in frames) + 'F truncated\n')
            (source / 'manifest.json').write_text(json.dumps({
                'format': 'mgs-native-tas-1', 'status': 'failed', 'artifacts': {
                    'initial.ps1s': sha256(source / 'initial.ps1s'),
                    'initial_cards/card1.mcd': sha256(source / 'initial_cards/card1.mcd')}}))
            destination = Path(folder) / 'prefix'
            with redirect_stdout(io.StringIO()):
                recover_prefix(SimpleNamespace(session=source, destination=destination, frames=2))
            lines = (destination / 'movie.mgst').read_text().splitlines()
            self.assertEqual(len(lines), 4)
            self.assertEqual(lines[-1], f'E 2 120 501 702 {0x80010002}')
            self.assertEqual((destination / 'initial_cards/card1.mcd').read_bytes(), b'initial card')
            with self.assertRaisesRegex(ValueError, 'testimone'):
                recover_prefix(SimpleNamespace(session=source, destination=Path(folder) / 'bad', frames=4))


if __name__ == '__main__':
    unittest.main()
