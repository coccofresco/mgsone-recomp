import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_tas import recording_extent_error


class RecordingExtentTests(unittest.TestCase):
    def test_completed_route_can_finish_before_budget(self):
        for portable in (False, True):
            self.assertIsNone(recording_extent_error(447, 1000, portable, 'complete'))

    def test_budget_does_not_validate_missing_or_failed_route(self):
        for status in ('failed', 'incomplete', 'missing'):
            for frames in (447, 1000, 1008):
                self.assertIsNotNone(recording_extent_error(frames, 1000, True, status))

    def test_fixed_recording_cannot_end_early(self):
        for portable in (False, True):
            self.assertIsNotNone(recording_extent_error(447, 1000, portable))
            self.assertIsNone(recording_extent_error(1000, 1000, portable))

    def test_portable_checkpoint_only_extends_fixed_tail(self):
        self.assertIsNone(recording_extent_error(1008, 1000, True))
        self.assertIsNotNone(recording_extent_error(1008, 1000, False))
        self.assertIsNone(recording_extent_error(447, 0, False))


if __name__ == '__main__':
    unittest.main()
