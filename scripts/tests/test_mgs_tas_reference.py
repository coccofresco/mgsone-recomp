from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_tas_reference import FIELDS, index_trace, matches, read_trace, sampled_runs


def sample(frame, stage='s02a', **changes):
    row = dict.fromkeys(FIELDS, 0)
    row.update(frame=frame, stage=stage, story=18, difficulty=3, life=256, yaw=1024)
    row.update(changes)
    return row


class ReferenceTests(unittest.TestCase):
    def test_one_tick_interaction_uses_last_pad_sample_of_update(self):
        # GV_Time advances before GV_PadData is refreshed. The first VBlank
        # can still expose the preceding pad, including a just-released button.
        rows = [sample(10, game_tick=5, pad0=0, pad1=0),
                sample(11, game_tick=6, pad0=0, pad1=0),
                sample(12, game_tick=6, pad0=8256, pad1=8),
                sample(13, game_tick=7, pad0=8256, pad1=8),
                sample(14, game_tick=7, pad0=0, pad1=0)]
        self.assertEqual(sampled_runs(rows, 10, 14), [[1, 8256, 8], [1, 0, 0]])

    def test_script_masking_cannot_silently_erase_ladder_interaction(self):
        rows = [sample(20453, game_tick=9675, pad0=16, pad1=0),
                sample(20454, game_tick=9676, pad0=8208, pad1=0),
                sample(20455, game_tick=9676, pad0=0, pad1=0, game_status=273285120)]
        with self.assertRaisesRegex(ValueError, 'filtered interaction at source frame 20454'):
            sampled_runs(rows, 20453, 20455)

    def test_retime_uses_consumed_pads_and_rejects_missing_updates(self):
        rows = [sample(10, game_tick=5, pad0=0, pad1=0),
                sample(11, game_tick=5, pad0=0, pad1=0),
                sample(12, game_tick=6, pad0=16, pad1=32768),
                sample(13, game_tick=6, pad0=16, pad1=32768),
                sample(14, game_tick=7, pad0=16, pad1=32768),
                sample(15, game_tick=8, pad0=0, pad1=8)]
        self.assertEqual(sampled_runs(rows, 10, 15), [[2, 16, 32768], [1, 0, 8]])
        rows[-1]['game_tick'] = 9
        with self.assertRaisesRegex(ValueError, 'missing/reset source game tick'):
            sampled_runs(rows, 10, 15)

    def trace(self, rows):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        path = Path(folder.name) / 'trace.tsv'
        path.write_text('\t'.join(FIELDS) + '\n' + ''.join(
            '\t'.join(str(r[k]) for k in FIELDS) + '\n' for r in rows))
        return path

    def test_boot_reset_deduplicates_frame_labels_not_row_numbers(self):
        rows, duplicates = read_trace(self.trace([
            sample(1, ''), sample(2, ''), sample(1, ''), sample(2, ''), sample(3)]))
        self.assertEqual([r['frame'] for r in rows], [1, 2, 3])
        self.assertEqual(duplicates, 2)
        with self.assertRaisesRegex(ValueError, 'non-monotone gameplay'):
            read_trace(self.trace([sample(20), sample(19)]))

    def test_stage_entry_does_not_imply_control_ready_and_revisits_stay_separate(self):
        result = index_trace([sample(1, yaw=-1), sample(2, game_status=0x10000000),
                              sample(3), sample(4, 's03a'), sample(5)])
        self.assertEqual(len(result), 3)
        self.assertEqual(result[0]['first_ready_frame'], 3)
        self.assertEqual(result[2]['first_frame'], 5)

    def test_matching_preserves_ambiguity_and_requires_story_and_stance(self):
        rows = [sample(10, x=10), sample(11, x=12), sample(12, x=1000),
                sample(13, x=10), sample(14, x=10, stance=2), sample(15, story=19)]
        state = dict(stage='s02a', story=18, difficulty=3, stance=0,
                     pause_level=0, control=0x80100000, x=10, y=0, z=0)
        found = matches(rows, state, 1, 20, 20)
        self.assertEqual([(r['first_frame'], r['last_frame']) for r in found], [(10, 11), (13, 13)])

    def test_incomplete_stream_is_not_silently_certified(self):
        path = self.trace([sample(1)])
        with path.open('a') as stream:
            stream.write('2\ts02a\t18')
        with self.assertRaisesRegex(ValueError, 'incomplete reference row'):
            read_trace(path)


if __name__ == '__main__':
    unittest.main()
