#!/usr/bin/env python3
"""Compare a recompiler P6 frame against a software-emulator oracle.

The normalization pipeline is intentionally small and deterministic:

1. decode P6 samples to RGB8;
2. crop in source-image coordinates;
3. downsample by an integer factor;
4. compare either RGB8 or canonical PS1 RGB5 (``component >> 3``).

No image library is required.  The JSON report separates color differences
from black/non-black coverage differences so missing geometry is not hidden by
an aggregate image score.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


SCHEMA = "ps1-frame-comparison/v1"
_WHITESPACE = b" \t\r\n\v\f"


@dataclass(frozen=True)
class Image:
    width: int
    height: int
    pixels: bytes

    def __post_init__(self) -> None:
        if self.width <= 0 or self.height <= 0:
            raise ValueError("image dimensions must be positive")
        expected = self.width * self.height * 3
        if len(self.pixels) != expected:
            raise ValueError(
                f"RGB payload has {len(self.pixels)} bytes, expected {expected}"
            )


@dataclass(frozen=True)
class LoadedImage:
    image: Image
    max_value: int


@dataclass(frozen=True)
class Crop:
    x: int
    y: int
    width: int
    height: int


def _skip_header_space(data: bytes, offset: int) -> int:
    """Skip token-separating whitespace and PNM comments."""

    while offset < len(data):
        if data[offset] in _WHITESPACE:
            offset += 1
            continue
        if data[offset] == ord("#"):
            newline = data.find(b"\n", offset + 1)
            if newline < 0:
                raise ValueError("unterminated PPM header comment")
            offset = newline + 1
            continue
        break
    return offset


def _header_token(data: bytes, offset: int) -> tuple[bytes, int]:
    offset = _skip_header_space(data, offset)
    start = offset
    while offset < len(data):
        value = data[offset]
        if value in _WHITESPACE or value == ord("#"):
            break
        offset += 1
    if offset == start:
        raise ValueError("truncated PPM header")
    return data[start:offset], offset


def _parse_decimal(token: bytes, label: str) -> int:
    try:
        text = token.decode("ascii")
    except UnicodeDecodeError as exc:
        raise ValueError(f"non-ASCII PPM {label}") from exc
    if not text.isdecimal():
        raise ValueError(f"invalid PPM {label}: {text!r}")
    return int(text)


def read_p6(path: Path) -> LoadedImage:
    """Read a binary P6 PPM and normalize its samples to RGB8.

    Both one-byte and big-endian two-byte PNM samples are accepted.  The one
    required whitespace separator after ``maxval`` is consumed exactly, so a
    first raster byte that happens to be whitespace is never lost.
    """

    data = path.read_bytes()
    magic, offset = _header_token(data, 0)
    if magic != b"P6":
        raise ValueError(f"{path}: expected binary P6 PPM, found {magic!r}")
    width_token, offset = _header_token(data, offset)
    height_token, offset = _header_token(data, offset)
    max_token, offset = _header_token(data, offset)
    width = _parse_decimal(width_token, "width")
    height = _parse_decimal(height_token, "height")
    max_value = _parse_decimal(max_token, "maxval")
    if width <= 0 or height <= 0:
        raise ValueError(f"{path}: PPM dimensions must be positive")
    if not 1 <= max_value <= 65535:
        raise ValueError(f"{path}: PPM maxval must be in [1, 65535]")
    if offset >= len(data) or data[offset] not in _WHITESPACE:
        raise ValueError(f"{path}: missing whitespace before PPM raster")
    if (
        data[offset] == ord("\r")
        and offset + 1 < len(data)
        and data[offset + 1] == ord("\n")
    ):
        offset += 2
    else:
        offset += 1

    sample_bytes = 1 if max_value < 256 else 2
    sample_count = width * height * 3
    expected_bytes = sample_count * sample_bytes
    payload = data[offset:]
    if len(payload) != expected_bytes:
        raise ValueError(
            f"{path}: PPM raster has {len(payload)} bytes, expected {expected_bytes}"
        )

    if sample_bytes == 1 and max_value == 255:
        pixels = payload
    else:
        normalized = bytearray(sample_count)
        if sample_bytes == 1:
            samples = payload
        else:
            samples = (
                (payload[index] << 8) | payload[index + 1]
                for index in range(0, len(payload), 2)
            )
        rounding = max_value // 2
        for index, value in enumerate(samples):
            if value > max_value:
                raise ValueError(
                    f"{path}: raster sample {value} exceeds maxval {max_value}"
                )
            normalized[index] = (value * 255 + rounding) // max_value
        pixels = bytes(normalized)

    return LoadedImage(Image(width, height, pixels), max_value)


def crop_image(image: Image, crop: Crop | None) -> Image:
    if crop is None:
        return image
    if crop.x < 0 or crop.y < 0 or crop.width <= 0 or crop.height <= 0:
        raise ValueError("crop must have non-negative origin and positive size")
    if crop.x + crop.width > image.width or crop.y + crop.height > image.height:
        raise ValueError(
            f"crop {crop.x},{crop.y},{crop.width},{crop.height} exceeds "
            f"{image.width}x{image.height} image"
        )
    row_bytes = crop.width * 3
    result = bytearray(row_bytes * crop.height)
    for output_y, source_y in enumerate(range(crop.y, crop.y + crop.height)):
        source_start = (source_y * image.width + crop.x) * 3
        output_start = output_y * row_bytes
        result[output_start : output_start + row_bytes] = image.pixels[
            source_start : source_start + row_bytes
        ]
    return Image(crop.width, crop.height, bytes(result))


def downsample_image(image: Image, factor: int, mode: str = "box") -> Image:
    if factor < 1:
        raise ValueError("downsample factor must be at least 1")
    if factor == 1:
        return image
    if image.width % factor or image.height % factor:
        raise ValueError(
            f"{image.width}x{image.height} image is not divisible by "
            f"downsample factor {factor}"
        )
    if mode not in {"box", "nearest"}:
        raise ValueError(f"unknown downsample mode: {mode}")

    output_width = image.width // factor
    output_height = image.height // factor
    result = bytearray(output_width * output_height * 3)
    source = image.pixels

    if mode == "nearest":
        # Choose the lower of the two center pixels for an even-sized block.
        center = (factor - 1) // 2
        for output_y in range(output_height):
            source_y = output_y * factor + center
            for output_x in range(output_width):
                source_x = output_x * factor + center
                source_offset = (source_y * image.width + source_x) * 3
                output_offset = (output_y * output_width + output_x) * 3
                result[output_offset : output_offset + 3] = source[
                    source_offset : source_offset + 3
                ]
    else:
        samples = factor * factor
        rounding = samples // 2
        output_offset = 0
        for output_y in range(output_height):
            source_y = output_y * factor
            for output_x in range(output_width):
                source_x = output_x * factor
                red = green = blue = 0
                for dy in range(factor):
                    offset = ((source_y + dy) * image.width + source_x) * 3
                    for _ in range(factor):
                        red += source[offset]
                        green += source[offset + 1]
                        blue += source[offset + 2]
                        offset += 3
                result[output_offset] = (red + rounding) // samples
                result[output_offset + 1] = (green + rounding) // samples
                result[output_offset + 2] = (blue + rounding) // samples
                output_offset += 3

    return Image(output_width, output_height, bytes(result))


def quantized_pixels(image: Image, color_space: str) -> bytes:
    if color_space == "rgb8":
        return image.pixels
    if color_space == "rgb5":
        return bytes(component >> 3 for component in image.pixels)
    raise ValueError(f"unknown color space: {color_space}")


def nonblack_pixels(pixels: bytes) -> int:
    return sum(
        pixels[offset] != 0
        or pixels[offset + 1] != 0
        or pixels[offset + 2] != 0
        for offset in range(0, len(pixels), 3)
    )


def _extend_bounds(
    bounds: tuple[int, int, int, int] | None, x: int, y: int
) -> tuple[int, int, int, int]:
    if bounds is None:
        return (x, y, x, y)
    return (
        min(bounds[0], x),
        min(bounds[1], y),
        max(bounds[2], x),
        max(bounds[3], y),
    )


def _bounds_json(bounds: tuple[int, int, int, int] | None) -> dict | None:
    if bounds is None:
        return None
    return {
        "x": bounds[0],
        "y": bounds[1],
        "width": bounds[2] - bounds[0] + 1,
        "height": bounds[3] - bounds[1] + 1,
    }


def compare_images(reference: Image, candidate: Image, color_space: str) -> dict:
    if (reference.width, reference.height) != (candidate.width, candidate.height):
        raise ValueError(
            "normalized dimensions differ: "
            f"reference={reference.width}x{reference.height}, "
            f"candidate={candidate.width}x{candidate.height}"
        )

    reference_pixels = quantized_pixels(reference, color_space)
    candidate_pixels = quantized_pixels(candidate, color_space)
    total_pixels = reference.width * reference.height
    mismatches = 0
    coverage_mismatches = 0
    reference_only = 0
    candidate_only = 0
    max_channel_delta = 0
    max_pixel_delta = 0
    absolute_delta = 0
    reference_nonblack = 0
    candidate_nonblack = 0
    mismatch_bounds = None
    coverage_bounds = None
    reference_only_bounds = None
    candidate_only_bounds = None

    for pixel_index, offset in enumerate(range(0, len(reference_pixels), 3)):
        x = pixel_index % reference.width
        y = pixel_index // reference.width
        rr, rg, rb = reference_pixels[offset : offset + 3]
        cr, cg, cb = candidate_pixels[offset : offset + 3]
        reference_visible = rr != 0 or rg != 0 or rb != 0
        candidate_visible = cr != 0 or cg != 0 or cb != 0
        reference_nonblack += reference_visible
        candidate_nonblack += candidate_visible
        if reference_visible != candidate_visible:
            coverage_mismatches += 1
            coverage_bounds = _extend_bounds(coverage_bounds, x, y)
            reference_only += reference_visible
            candidate_only += candidate_visible
            if reference_visible:
                reference_only_bounds = _extend_bounds(reference_only_bounds, x, y)
            else:
                candidate_only_bounds = _extend_bounds(candidate_only_bounds, x, y)

        red_delta = abs(rr - cr)
        green_delta = abs(rg - cg)
        blue_delta = abs(rb - cb)
        pixel_delta = red_delta + green_delta + blue_delta
        if pixel_delta:
            mismatches += 1
            mismatch_bounds = _extend_bounds(mismatch_bounds, x, y)
            max_channel_delta = max(
                max_channel_delta, red_delta, green_delta, blue_delta
            )
            max_pixel_delta = max(max_pixel_delta, pixel_delta)
            absolute_delta += pixel_delta

    return {
        "width": reference.width,
        "height": reference.height,
        "pixels": total_pixels,
        "matching_pixels": total_pixels - mismatches,
        "mismatch_pixels": mismatches,
        "mismatch_fraction": mismatches / total_pixels,
        "mismatch_bounds": _bounds_json(mismatch_bounds),
        "max_channel_delta": max_channel_delta,
        "max_pixel_l1_delta": max_pixel_delta,
        "mean_absolute_channel_delta": absolute_delta / (total_pixels * 3),
        "coverage": {
            "reference_nonblack_pixels": reference_nonblack,
            "reference_nonblack_fraction": reference_nonblack / total_pixels,
            "candidate_nonblack_pixels": candidate_nonblack,
            "candidate_nonblack_fraction": candidate_nonblack / total_pixels,
            "mismatch_pixels": coverage_mismatches,
            "mismatch_fraction": coverage_mismatches / total_pixels,
            "mismatch_bounds": _bounds_json(coverage_bounds),
            "reference_only_pixels": reference_only,
            "reference_only_bounds": _bounds_json(reference_only_bounds),
            "candidate_only_pixels": candidate_only,
            "candidate_only_bounds": _bounds_json(candidate_only_bounds),
        },
    }


def diagnostic_diff_image(
    reference: Image,
    candidate: Image,
    color_space: str,
    mode: str = "all",
) -> Image:
    """Build a deterministic mismatch map in normalized image coordinates.

    Exact pixels retain a dim copy of the reference. Reference-only coverage
    is magenta and candidate-only coverage is cyan. In ``all`` mode,
    color-only disagreement is yellow; ``coverage`` mode leaves those pixels
    as a dim reference. Visibility and equality use the selected color space.
    """

    if (reference.width, reference.height) != (candidate.width, candidate.height):
        raise ValueError(
            "normalized dimensions differ: "
            f"reference={reference.width}x{reference.height}, "
            f"candidate={candidate.width}x{candidate.height}"
        )
    if mode not in {"all", "coverage"}:
        raise ValueError(f"unknown diagnostic diff mode: {mode}")
    reference_units = quantized_pixels(reference, color_space)
    candidate_units = quantized_pixels(candidate, color_space)
    result = bytearray(len(reference.pixels))
    for offset in range(0, len(result), 3):
        reference_pixel = reference_units[offset : offset + 3]
        candidate_pixel = candidate_units[offset : offset + 3]
        reference_visible = any(reference_pixel)
        candidate_visible = any(candidate_pixel)
        if reference_visible and not candidate_visible:
            result[offset : offset + 3] = bytes((255, 0, 255))
        elif candidate_visible and not reference_visible:
            result[offset : offset + 3] = bytes((0, 255, 255))
        elif mode == "all" and reference_pixel != candidate_pixel:
            result[offset : offset + 3] = bytes((255, 255, 0))
        else:
            result[offset : offset + 3] = bytes(
                component // 4 for component in reference.pixels[offset : offset + 3]
            )
    return Image(reference.width, reference.height, bytes(result))


def write_p6(path: Path, image: Image) -> None:
    path.write_bytes(
        f"P6\n{image.width} {image.height}\n255\n".encode("ascii") + image.pixels
    )


def _crop_json(crop: Crop | None) -> dict | None:
    if crop is None:
        return None
    return {
        "x": crop.x,
        "y": crop.y,
        "width": crop.width,
        "height": crop.height,
    }


def prepare_image(
    loaded: LoadedImage,
    crop: Crop | None,
    downsample: int,
    downsample_mode: str,
) -> Image:
    return downsample_image(
        crop_image(loaded.image, crop), downsample, downsample_mode
    )


def image_report(
    path: Path,
    loaded: LoadedImage,
    normalized: Image,
    crop: Crop | None,
    downsample: int,
    downsample_mode: str,
    color_space: str,
) -> dict:
    comparison_pixels = quantized_pixels(normalized, color_space)
    total_pixels = normalized.width * normalized.height
    rgb8_nonblack = nonblack_pixels(normalized.pixels)
    comparison_nonblack = nonblack_pixels(comparison_pixels)
    return {
        "path": str(path),
        "source": {
            "width": loaded.image.width,
            "height": loaded.image.height,
            "max_value": loaded.max_value,
        },
        "normalization": {
            "crop": _crop_json(crop),
            "downsample": downsample,
            "downsample_mode": downsample_mode,
        },
        "width": normalized.width,
        "height": normalized.height,
        "pixels": total_pixels,
        "rgb8_nonblack_pixels": rgb8_nonblack,
        "rgb8_nonblack_fraction": rgb8_nonblack / total_pixels,
        "comparison_nonblack_pixels": comparison_nonblack,
        "comparison_nonblack_fraction": comparison_nonblack / total_pixels,
    }


def build_report(
    reference_path: Path,
    candidate_path: Path,
    reference_crop: Crop | None = None,
    candidate_crop: Crop | None = None,
    reference_downsample: int = 1,
    candidate_downsample: int = 1,
    downsample_mode: str = "box",
    color_space: str = "rgb5",
) -> dict:
    reference_loaded = read_p6(reference_path)
    candidate_loaded = read_p6(candidate_path)
    reference = prepare_image(
        reference_loaded,
        reference_crop,
        reference_downsample,
        downsample_mode,
    )
    candidate = prepare_image(
        candidate_loaded,
        candidate_crop,
        candidate_downsample,
        downsample_mode,
    )
    return {
        "schema": SCHEMA,
        "color_space": color_space,
        "reference": image_report(
            reference_path,
            reference_loaded,
            reference,
            reference_crop,
            reference_downsample,
            downsample_mode,
            color_space,
        ),
        "candidate": image_report(
            candidate_path,
            candidate_loaded,
            candidate,
            candidate_crop,
            candidate_downsample,
            downsample_mode,
            color_space,
        ),
        "comparison": compare_images(reference, candidate, color_space),
    }


def parse_crop(value: str) -> Crop:
    try:
        fields = [int(field, 10) for field in value.split(",")]
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "crop must be X,Y,WIDTH,HEIGHT"
        ) from exc
    if len(fields) != 4:
        raise argparse.ArgumentTypeError("crop must be X,Y,WIDTH,HEIGHT")
    crop = Crop(*fields)
    if crop.x < 0 or crop.y < 0 or crop.width <= 0 or crop.height <= 0:
        raise argparse.ArgumentTypeError(
            "crop needs a non-negative origin and positive size"
        )
    return crop


def positive_integer(value: str) -> int:
    try:
        result = int(value, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("expected a positive integer") from exc
    if result < 1:
        raise argparse.ArgumentTypeError("expected a positive integer")
    return result


def argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Compare a P6 donor frame with a normalized recompiler frame. "
            "RGB5 comparison is the default."
        )
    )
    parser.add_argument("reference", type=Path, help="donor/oracle P6 PPM")
    parser.add_argument("candidate", type=Path, help="recompiler P6 PPM")
    parser.add_argument(
        "--reference-crop",
        type=parse_crop,
        metavar="X,Y,W,H",
        help="crop the reference before downsampling",
    )
    parser.add_argument(
        "--candidate-crop",
        type=parse_crop,
        metavar="X,Y,W,H",
        help="crop the candidate before downsampling",
    )
    parser.add_argument(
        "--reference-downsample",
        type=positive_integer,
        default=1,
        metavar="N",
    )
    parser.add_argument(
        "--candidate-downsample",
        "--downsample",
        dest="candidate_downsample",
        type=positive_integer,
        default=1,
        metavar="N",
        help="integer candidate reduction (for example 4 for a 4x render)",
    )
    parser.add_argument(
        "--downsample-mode",
        choices=("box", "nearest"),
        default="box",
        help="reduction filter used for either image (default: box)",
    )
    parser.add_argument(
        "--color-space",
        choices=("rgb5", "rgb8"),
        default="rgb5",
        help="comparison units (default: canonical PS1 RGB5)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        metavar="PATH",
        help="write JSON to PATH instead of standard output",
    )
    parser.add_argument(
        "--diff-output",
        type=Path,
        metavar="PATH",
        help=(
            "write a normalized P6 mismatch map: magenta=reference only, "
            "cyan=candidate only, yellow=color mismatch"
        ),
    )
    parser.add_argument(
        "--diff-mode",
        choices=("all", "coverage"),
        default="all",
        help="show all color differences or only missing/extra coverage",
    )
    parser.add_argument(
        "--compact", action="store_true", help="emit compact single-line JSON"
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = argument_parser()
    args = parser.parse_args(argv)
    try:
        report = build_report(
            args.reference,
            args.candidate,
            reference_crop=args.reference_crop,
            candidate_crop=args.candidate_crop,
            reference_downsample=args.reference_downsample,
            candidate_downsample=args.candidate_downsample,
            downsample_mode=args.downsample_mode,
            color_space=args.color_space,
        )
        if args.diff_output is not None:
            if args.diff_output in (args.reference, args.candidate):
                raise ValueError("diff output must not overwrite an input image")
            reference = prepare_image(
                read_p6(args.reference),
                args.reference_crop,
                args.reference_downsample,
                args.downsample_mode,
            )
            candidate = prepare_image(
                read_p6(args.candidate),
                args.candidate_crop,
                args.candidate_downsample,
                args.downsample_mode,
            )
            write_p6(
                args.diff_output,
                diagnostic_diff_image(
                    reference, candidate, args.color_space, args.diff_mode
                ),
            )
            report["diagnostic_diff"] = {
                "path": str(args.diff_output),
                "mode": args.diff_mode,
                "exact": "dim reference",
                "reference_only": "#ff00ff",
                "candidate_only": "#00ffff",
                "color_mismatch": "#ffff00",
            }
        if args.compact:
            output = json.dumps(report, separators=(",", ":"), sort_keys=True) + "\n"
        else:
            output = json.dumps(report, indent=2, sort_keys=True) + "\n"
        if args.output is None:
            sys.stdout.write(output)
        else:
            args.output.write_text(output, encoding="utf-8")
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
