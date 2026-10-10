from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_loading_check import analyze, compare_textures


class LoadingCheckTest(unittest.TestCase):
    def test_equal_game_progress_and_sector_counts_cannot_hide_missing_textures(self):
        ram = bytearray(2 * 1024 * 1024)
        ram[0xb46c8:0xb46c8+12] = bytes((1, 2, 1, 16, 0, 1, 0, 2, 0, 0, 31, 31))
        self.assertEqual(compare_textures(ram, ram)['count'], 1)
        other = bytearray(ram)
        other[0xb46c8+2] = 0 # residency bookkeeping is not a texture layout
        self.assertEqual(compare_textures(ram, other)['count'], 1)
        other[0xb46c8+7] ^= 1
        with self.assertRaisesRegex(ValueError, 'texture content differs'):
            compare_textures(ram, other)
        with self.assertRaisesRegex(ValueError, 'texture content differs'):
            compare_textures(ram, bytearray(len(ram)))

    files = {'/MGS/STAGE.DIR': dict(lba=100, size=4096),
             '/MGS/DEMO.DAT': dict(lba=102, size=4096)}

    def rows(self):
        return [dict(event='mgs.loading_sample', frame=10, loader=1),
                dict(event='cdrom.read_begin', frame=11, target_lba=100),
                dict(event='cdrom.sector_read', frame=11, lba=100, unpaced=1),
                dict(event='cdrom.sector_read', frame=11, lba=101, unpaced=1),
                dict(event='mgs.loading_sample', frame=14, loader=0),
                dict(event='mgs.loading_sample', frame=900, loader=0)]

    def test_excludes_scripted_wait_and_counts_partial_last_sector(self):
        result = analyze(self.rows(), self.files, {100: 's00a'})
        self.assertEqual(result['loads'][0]['vblanks'], 4)
        self.assertEqual(result['loads'][0]['stages'], ['s00a'])
        self.assertEqual(result['loads'][0]['sectors'], [2])
        self.assertEqual(result['unpaced_sectors'], 2)

    def test_rejects_incomplete_or_unidentified_loading_and_corrupt_actor_lists(self):
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            analyze(self.rows()[:4], self.files, {100: 's00a'})
        with self.assertRaisesRegex(ValueError, 'identified stage'):
            analyze(self.rows(), self.files, {})
        rows = self.rows()
        rows[4]['loader'] = -1
        with self.assertRaisesRegex(ValueError, 'invalid actor'):
            analyze(rows, self.files, {100: 's00a'})

    def test_rejects_unpaced_stream_and_unknown_or_overlapping_extent(self):
        for lba in (102, 999):
            rows = self.rows()
            rows[3]['lba'] = lba
            with self.assertRaisesRegex(ValueError, 'outside explicit'):
                analyze(rows, self.files, {100: 's00a'})
        files = dict(self.files, overlap=dict(lba=100, size=2048))
        with self.assertRaisesRegex(ValueError, 'outside explicit'):
            analyze(self.rows(), files, {100: 's00a'})


if __name__ == '__main__':
    unittest.main()
