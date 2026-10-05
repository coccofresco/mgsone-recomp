from pathlib import Path
import json
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_campaign import intervals, terminal_checkpoint
from mgs_tas import sha256


class CampaignTests(unittest.TestCase):
    def test_relative_actions_cover_inclusive_samples_without_overlap(self):
        frames, script = intervals('right:3,wait:2,up+left:2', 100)
        self.assertEqual(frames, 7)
        self.assertEqual(script, '100 102 right\n105 106 up\n105 106 left\n')
        for bad in ('right:0', 'wait:-2', 'teleport:1', 'up+up:2'):
            with self.assertRaises(ValueError):
                intervals(bad, 100)

    def test_ancestry_requires_untampered_exact_terminal_checkpoint(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'states').mkdir()
            state = root / 'states/checkpoint_000000040_vblank_400.ps1s'
            state.write_bytes(b'checkpoint fixture')
            movie = root / 'movie.mgst'
            movie.write_text('MGSTAS 1\nE 40 123 400 456 789\n')
            (root / 'manifest.json').write_text(json.dumps({
                'status': 'complete', 'artifacts': {'movie.mgst': sha256(movie)}}))
            result = {'status': 'complete', 'checkpoints': [
                {'file': str(state.relative_to(root)), 'sha256': sha256(state)}]}
            (root / 'result.json').write_text(json.dumps(result))
            self.assertEqual(terminal_checkpoint(root), (state, 400))
            state.write_bytes(b'modified state')
            with self.assertRaisesRegex(ValueError, 'checkpoint changed'):
                terminal_checkpoint(root)
            result['checkpoints'] = []
            (root / 'result.json').write_text(json.dumps(result))
            with self.assertRaisesRegex(ValueError, 'exact terminal witness'):
                terminal_checkpoint(root)
            movie.write_text('MGSTAS 1\nE 40 123 401 456 789\n')
            with self.assertRaisesRegex(ValueError, 'movie changed'):
                terminal_checkpoint(root)
