import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS / "donor_trace"))

from frame_compare import (  # noqa: E402
    Crop,
    Image,
    compare_images,
    crop_image,
    diagnostic_diff_image,
    downsample_image,
    read_p6,
)


def write_p6(path, width, height, pixels, max_value=255, comment=False):
    header = b"P6\n"
    if comment:
        header += b"# generated test image\n"
    header += f"{width} {height}\n{max_value}\n".encode("ascii")
    path.write_bytes(header + pixels)


class FrameCompareTest(unittest.TestCase):
    def test_p6_parser_preserves_whitespace_first_pixel_and_scales_samples(self):
        with tempfile.TemporaryDirectory() as directory:
            eight_bit = Path(directory) / "eight.ppm"
            sixteen_bit = Path(directory) / "sixteen.ppm"
            # The first sample is LF (10): consuming all post-maxval whitespace
            # would incorrectly discard it.
            write_p6(eight_bit, 1, 1, bytes((10, 20, 30)), comment=True)
            write_p6(
                sixteen_bit,
                1,
                1,
                bytes((0x00, 0x00, 0x01, 0xFF, 0x03, 0xFF)),
                max_value=1023,
            )

            decoded_eight = read_p6(eight_bit)
            decoded_sixteen = read_p6(sixteen_bit)

        self.assertEqual(decoded_eight.image.pixels, bytes((10, 20, 30)))
        self.assertEqual(decoded_sixteen.max_value, 1023)
        self.assertEqual(decoded_sixteen.image.pixels, bytes((0, 127, 255)))

    def test_crop_then_box_downsample_uses_integer_rounding(self):
        # Crop the middle 2x2 block from a 4x2 image.  Its RGB mean is
        # (30, 40, 50), so the result is one deterministic native pixel.
        pixels = bytes(
            (
                1, 2, 3,
                0, 10, 20,
                20, 30, 40,
                4, 5, 6,
                7, 8, 9,
                40, 50, 60,
                60, 70, 80,
                10, 11, 12,
            )
        )
        image = Image(4, 2, pixels)

        cropped = crop_image(image, Crop(1, 0, 2, 2))
        reduced = downsample_image(cropped, 2, "box")

        self.assertEqual((reduced.width, reduced.height), (1, 1))
        self.assertEqual(reduced.pixels, bytes((30, 40, 50)))

    def test_rgb5_metrics_separate_color_and_coverage_mismatch(self):
        reference = Image(3, 1, bytes((8, 0, 0, 16, 16, 16, 0, 0, 0)))
        candidate = Image(3, 1, bytes((15, 7, 7, 24, 16, 16, 0, 8, 0)))

        result = compare_images(reference, candidate, "rgb5")

        # Pixel zero differs in RGB8 but is identical after RGB5 conversion.
        self.assertEqual(result["mismatch_pixels"], 2)
        self.assertEqual(result["max_channel_delta"], 1)
        self.assertEqual(result["max_pixel_l1_delta"], 1)
        self.assertEqual(result["coverage"]["mismatch_pixels"], 1)
        self.assertEqual(result["coverage"]["mismatch_fraction"], 1 / 3)
        self.assertEqual(result["coverage"]["candidate_only_pixels"], 1)
        self.assertEqual(result["coverage"]["reference_only_pixels"], 0)
        self.assertEqual(
            result["mismatch_bounds"], {"x": 1, "y": 0, "width": 2, "height": 1}
        )
        self.assertEqual(
            result["coverage"]["mismatch_bounds"],
            {"x": 2, "y": 0, "width": 1, "height": 1},
        )
        self.assertIsNone(result["coverage"]["reference_only_bounds"])
        self.assertEqual(
            result["coverage"]["candidate_only_bounds"],
            {"x": 2, "y": 0, "width": 1, "height": 1},
        )

    def test_diagnostic_diff_separates_missing_extra_and_color_pixels(self):
        reference = Image(
            4,
            1,
            bytes((16, 0, 0, 0, 0, 0, 8, 8, 8, 32, 32, 32)),
        )
        candidate = Image(
            4,
            1,
            bytes((0, 0, 0, 0, 16, 0, 16, 8, 8, 39, 39, 39)),
        )

        diagnostic = diagnostic_diff_image(reference, candidate, "rgb5")

        self.assertEqual(
            diagnostic.pixels,
            bytes(
                (
                    255, 0, 255,
                    0, 255, 255,
                    255, 255, 0,
                    8, 8, 8,
                )
            ),
        )
        coverage = diagnostic_diff_image(
            reference, candidate, "rgb5", mode="coverage"
        )
        self.assertEqual(coverage.pixels[6:9], bytes((2, 2, 2)))

    def test_cli_normalizes_4x_candidate_and_emits_json(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            donor = directory / "swan.ppm"
            recomp = directory / "recomp-4x.ppm"
            report = directory / "comparison.json"
            diagnostic = directory / "comparison.ppm"
            write_p6(donor, 1, 1, bytes((248, 128, 8)))
            write_p6(recomp, 4, 4, bytes((248, 128, 8)) * 16)

            completed = subprocess.run(
                [
                    sys.executable,
                    str(TOOLS / "donor_trace" / "frame_compare.py"),
                    str(donor),
                    str(recomp),
                    "--downsample",
                    "4",
                    "--output",
                    str(report),
                    "--diff-output",
                    str(diagnostic),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            parsed = json.loads(report.read_text(encoding="utf-8"))
            diagnostic_image = read_p6(diagnostic).image

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(parsed["schema"], "ps1-frame-comparison/v1")
        self.assertEqual(parsed["color_space"], "rgb5")
        self.assertEqual(parsed["candidate"]["normalization"]["downsample"], 4)
        self.assertEqual(parsed["comparison"]["mismatch_pixels"], 0)
        self.assertEqual(parsed["comparison"]["coverage"]["mismatch_pixels"], 0)
        self.assertEqual(parsed["diagnostic_diff"]["path"], str(diagnostic))
        self.assertEqual(diagnostic_image.pixels, bytes((62, 32, 2)))


if __name__ == "__main__":
    unittest.main()
