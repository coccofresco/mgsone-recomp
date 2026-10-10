from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_movie_check import assess, expected


class MoviePlaybackTests(unittest.TestCase):
    def setUp(self):
        self.catalog = dict(format='mgs-movie-catalog-1', status='complete', discs=[dict(
            disc=dict(sha256='disc'), movies=[dict(code='abcd', table_frames=5,
                frames=[dict(width=320, height=160, preceding_xa_sectors=i) for i in range(1, 6)],
                audio=[dict(coding=1, sectors=6)])])])
        self.target = expected(self.catalog, 'disc', ['abcd'])
        self.counters = {'mdec.decoded_frame': 3, 'mdec.decoded_macroblock': 600,
            'mdec.decode_command': 3, 'cdrom.xa_decoded_sector': 4,
            'cdrom.xa_output_frame': 9408, 'spu.xa_consumed_frame': 9408,
            'spu.cd_nonzero_frame': 9000}

    def test_targets_come_from_source_and_preserve_repeated_movies(self):
        self.assertEqual(self.target, dict(frames=3, macroblocks=600, xa_min=3, xa_max=6, pcm_per_sector=2352))
        repeated = expected(self.catalog, 'disc', ['abcd', 'abcd'])
        self.assertEqual(repeated['frames'], 6)
        self.assertEqual(repeated['xa_min'], 6)

    def test_rejects_wrong_disc_unknown_movie_and_incomplete_index(self):
        for disc, movies in (('wrong', ['abcd']), ('disc', ['xxxx']), ('disc', [])):
            with self.assertRaises(ValueError):
                expected(self.catalog, disc, movies)
        self.catalog['discs'][0]['movies'][0]['frames'].pop()
        with self.assertRaises(ValueError):
            expected(self.catalog, 'disc', ['abcd'])

    def test_all_counter_obligations_fail_closed(self):
        self.assertTrue(assess(self.target, dict(counters=self.counters))['passed'])
        for key in self.counters:
            for value in (None, True, '3', -1, 0, 1000000):
                with self.subTest(counter=key, value=value):
                    broken = dict(self.counters, **{key: value})
                    self.assertFalse(assess(self.target, dict(counters=broken))['passed'])
            broken = dict(self.counters)
            del broken[key]
            self.assertFalse(assess(self.target, dict(counters=broken))['passed'])

    def test_catches_truncation_clock_loss_and_audio_not_consumed(self):
        for key in ('mdec.truncated_command', 'cdrom.discarded_clock_cycles'):
            counters = dict(self.counters, **{key: 1})
            self.assertFalse(assess(self.target, dict(counters=counters))['passed'])
        for sectors in (2, 7):
            counters = dict(self.counters, **{'cdrom.xa_decoded_sector': sectors,
                'cdrom.xa_output_frame': sectors * 2352, 'spu.xa_consumed_frame': sectors * 2352,
                'spu.cd_nonzero_frame': 1000})
            self.assertFalse(assess(self.target, dict(counters=counters))['passed'])


if __name__ == '__main__':
    unittest.main()
