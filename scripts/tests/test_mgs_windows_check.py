from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from mgs_windows_check import compare_reference


class WindowsReferenceTests(unittest.TestCase):
    def test_changed_and_missing_images_cannot_count_as_identical(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);actual=root/'actual';reference=root/'reference'
            for base in (actual,reference):
                (base/'gpu').mkdir(parents=True);(base/'native').mkdir()
                (base/'native/frame-0.ppm').write_bytes(b'P6\n1 1\n255\n\0\0\0')
            (actual/'gpu/frame-1.ppm').write_bytes(b'changed')
            (reference/'gpu/frame-1.ppm').write_bytes(b'original')
            (actual/'gpu/frame-2.ppm').write_bytes(b'no reference')
            result=compare_reference(actual,reference)
            self.assertEqual(result,dict(compared=2,identical=1,mismatches=['gpu/frame-1.ppm'],unmatched=['gpu/frame-2.ppm']))


if __name__=='__main__':unittest.main()
