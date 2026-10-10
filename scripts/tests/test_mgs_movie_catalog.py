import hashlib
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_movie_catalog import movie_index, scan_movie, SYNC


def sector(frame=1, chunk=0, chunks=1, size=8, audio=False):
    raw = bytearray(2352)
    raw[:12], raw[15] = SYNC, 2
    raw[16:20] = bytes((1, 1, 0x64 if audio else 0x48, 1))
    raw[20:24] = raw[16:20]
    if not audio:
        struct.pack_into('<HHHHIIHH', raw, 24, 0x160, 0x8001, chunk, chunks, frame, size, 320, 160)
        raw[56:64] = b'payload!'
    return raw


def scan(sectors, frames=1):
    return scan_movie(sectors, dict(code='0123', sectors=len(sectors), table_frames=frames))


class MovieCatalogTests(unittest.TestCase):
    def test_index_uses_original_offsets_and_archive_end(self):
        table = struct.pack('<IHHIHHI', 2, 0x123, 3, 1, 0x456, 4, 11)
        entries = movie_index(table, 100, 20, 200)
        self.assertEqual([(m['start_lba'], m['sectors']) for m in entries], [(101, 10), (111, 9)])
        for broken in (table[:8], struct.pack('<I', 9),
                       struct.pack('<IHHIHHI', 2, 1, 3, 10, 2, 3, 9),
                       struct.pack('<IHHIHHI', 2, 1, 3, 1, 1, 3, 11),
                       struct.pack('<IHHI', 1, 1, 2, 1)):
            with self.subTest(broken=broken), self.assertRaises(ValueError):
                movie_index(broken, 100, 20, 200)
        with self.assertRaises(ValueError):
            movie_index(table, 195, 20, 200)

    def test_interleaved_audio_and_whole_padding_chunks_are_legal(self):
        sectors = [sector(chunk=0, chunks=2), sector(audio=True), sector(chunk=1, chunks=2)]
        result = scan(sectors)
        self.assertEqual(result['frames'][0]['sha256'], hashlib.sha256(b'payload!').hexdigest())
        self.assertEqual(result['audio'][0]['stereo_frames'], 2352)
        self.assertEqual(result['encoded_frames'], 1)

    def test_rejects_missing_duplicate_reordered_or_changed_chunks(self):
        cases = ([sector(chunk=1, chunks=2)],
                 [sector(chunks=2), sector(chunks=2)],
                 [sector(chunks=2), sector(chunk=1, chunks=2, size=9)],
                 [sector(chunks=2)], [sector(frame=2)],
                 [sector(size=2017)], [sector(), sector()])
        for sectors in cases:
            with self.subTest(sectors=len(sectors)), self.assertRaises(ValueError):
                scan(sectors)

    def test_rejects_corrupt_raw_headers_and_unaccounted_data(self):
        for offset, value in ((0, 1), (15, 1), (20, 2), (24, 0), (26, 0x42)):
            raw = sector()
            raw[offset] = value
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                scan([raw])
        raw = sector(audio=True)
        raw[18] = raw[22] = 0x44  # Audio without Form2.
        with self.assertRaises(ValueError):
            scan([raw])

    def test_requires_complete_extent_and_index_frame_count(self):
        with self.assertRaises(ValueError):
            scan([sector()], frames=2)
        with self.assertRaises(ValueError):
            scan_movie([sector()], dict(sectors=2, table_frames=1))

    def test_all_valid_xa_depths_rates_channels_have_duration(self):
        for bits in (4, 8):
            for channels in (1, 2):
                for rate in (18900, 37800):
                    raw = sector(audio=True)
                    raw[19] = raw[23] = (16 if bits == 8 else 0) | (4 if rate == 18900 else 0) | (channels == 2)
                    result = scan([raw, sector()])
                    self.assertEqual(result['stereo_frames'], (4032 if bits == 4 else 2016) // channels * 44100 // rate)


if __name__ == '__main__':
    unittest.main()
