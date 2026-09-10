#!/usr/bin/env python3
"""Validate visible Crash 3 Vulkan A/M/B presentation sequences.

The renderer's temporal readback names every presented scanout as ordinary,
midpoint or endpoint and gives it a monotonic presentation serial.  This tool
checks that the sequence really alternates M/B, that moving midpoint images
are not copies of either authentic endpoint, and (when a non-interpolated
control directory is supplied) that every B image is an exact control frame.

This is deliberately an image-level contract.  Renderer counters can prove
that compute work was dispatched, but only the resolved scanout can prove that
the user was shown the additional frame.

When ``--trace`` is supplied, the validator also proves the Vulkan page
causality of every captured pair: midpoint A/B must name B's render page and
the following deferred endpoint must resolve that same page.  This catches the
former double-buffer bug where the visual sequence was midpoint A/B -> A even
though presentation serials and phase labels were both monotonic.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable


SCHEMA = "crash3-temporal-phase-validation/v1"
PHASE_NAME = re.compile(
    r"^frame-(?P<frame>[0-9]+)-present-(?P<serial>[0-9]+)-"
    r"(?P<phase>ordinary|midpoint|endpoint)-scanout\.ppm$"
)


@dataclass(frozen=True)
class Presentation:
    path: Path
    frame: int
    serial: int
    phase: str
    width: int
    height: int
    pixels: bytes
    sha256: str


def _ppm_token(stream) -> bytes:
    while True:
        token = stream.readline(1)
        if not token:
            raise ValueError("unexpected EOF in PPM header")
        if token == b"#":
            stream.readline()
            continue
        if not token.isspace():
            break
    value = bytearray(token)
    while True:
        token = stream.read(1)
        if not token or token.isspace():
            return bytes(value)
        value.extend(token)


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    with path.open("rb") as stream:
        if _ppm_token(stream) != b"P6":
            raise ValueError(f"{path}: expected binary P6 PPM")
        width = int(_ppm_token(stream))
        height = int(_ppm_token(stream))
        maximum = int(_ppm_token(stream))
        if width <= 0 or height <= 0 or maximum != 255:
            raise ValueError(f"{path}: unsupported PPM dimensions/range")
        expected = width * height * 3
        pixels = stream.read()
    if len(pixels) != expected:
        raise ValueError(f"{path}: expected {expected} RGB bytes, got {len(pixels)}")
    return width, height, pixels


def load_presentations(directory: Path) -> list[Presentation]:
    presentations: list[Presentation] = []
    for path in directory.glob("*-scanout.ppm"):
        match = PHASE_NAME.match(path.name)
        if match is None:
            continue
        width, height, pixels = read_ppm(path)
        presentations.append(
            Presentation(
                path=path,
                frame=int(match.group("frame")),
                serial=int(match.group("serial")),
                phase=match.group("phase"),
                width=width,
                height=height,
                pixels=pixels,
                sha256=hashlib.sha256(pixels).hexdigest(),
            )
        )
    presentations.sort(key=lambda item: item.serial)
    if not presentations:
        raise ValueError(f"{directory}: no temporal scanout PPMs found")
    serials = [item.serial for item in presentations]
    if len(serials) != len(set(serials)):
        raise ValueError(f"{directory}: duplicate presentation serial")
    shape = (presentations[0].width, presentations[0].height)
    if any((item.width, item.height) != shape for item in presentations):
        raise ValueError(f"{directory}: scanout extent changed inside capture")
    return presentations


def image_difference(left: bytes, right: bytes) -> dict[str, int | float]:
    if len(left) != len(right):
        raise ValueError("cannot compare differently sized scanouts")
    # Exact equality is certified separately by SHA-256.  Difference magnitude
    # is diagnostic only, so sample a bounded, evenly spaced pixel set instead
    # of spending seconds rescanning multi-megapixel 4x readbacks three times
    # per triplet.
    pixels = len(left) // 3
    pixel_stride = max(1, pixels // 65_536)
    sampled_pixels = range(0, pixels, pixel_stride)
    changed_channels = 0
    changed_pixels = 0
    absolute_error = 0
    maximum_error = 0
    sample_count = 0
    for pixel in sampled_pixels:
        sample_count += 1
        first = pixel * 3
        pixel_changed = False
        for channel in range(first, first + 3):
            difference = abs(left[channel] - right[channel])
            if difference:
                pixel_changed = True
                changed_channels += 1
                absolute_error += difference
                maximum_error = max(maximum_error, difference)
        changed_pixels += pixel_changed
    return {
        "sampled_pixels": sample_count,
        "sample_stride_pixels": pixel_stride,
        "sampled_changed_pixels": changed_pixels,
        "sampled_changed_pixel_fraction": (
            changed_pixels / sample_count if sample_count else 0.0
        ),
        "sampled_changed_channels": changed_channels,
        "sampled_mean_absolute_channel_error": (
            absolute_error / (sample_count * 3) if sample_count else 0.0
        ),
        "sampled_maximum_channel_error": maximum_error,
    }


def midpoint_discontinuity_probe(
    endpoint_a: bytes,
    midpoint: bytes,
    endpoint_b: bytes,
    *,
    black_threshold: int = 4,
    stable_endpoint_delta: int = 12,
    midpoint_outlier_delta: int = 48,
    scanout_width: int | None = None,
) -> dict:
    """Measure transient midpoint holes without treating it as identity.

    Exact renderer provenance remains the admission authority. This bounded
    image probe is a regression alarm for the characteristic one-frame seams:
    a dark midpoint where both endpoints contain colour, or a large midpoint
    colour excursion where the two authentic endpoints locally agree.
    """
    if not (len(endpoint_a) == len(midpoint) == len(endpoint_b)):
        raise ValueError("cannot probe differently sized scanouts")
    if not 0 <= black_threshold <= 255:
        raise ValueError("black threshold must be in 0..255")
    if not 0 <= stable_endpoint_delta <= 255:
        raise ValueError("stable endpoint delta must be in 0..255")
    if not 0 <= midpoint_outlier_delta <= 255:
        raise ValueError("midpoint outlier delta must be in 0..255")

    pixels = len(endpoint_a) // 3
    if scanout_width is not None and (
        scanout_width <= 0 or pixels % scanout_width != 0
    ):
        raise ValueError("scanout width must divide the complete image")
    region_counts = [dict.fromkeys((
        "sampled_pixels", "endpoint_coloured_pixels", "midpoint_only_dark_pixels",
        "stable_endpoint_pixels", "stable_endpoint_outlier_pixels",
    ), 0) for _ in range(3)]
    pixel_stride = max(1, pixels // 65_536)
    sampled = 0
    endpoint_coloured = 0
    midpoint_only_dark = 0
    stable_endpoint_pixels = 0
    stable_endpoint_outliers = 0
    for pixel in range(0, pixels, pixel_stride):
        sampled += 1
        first = pixel * 3
        a = endpoint_a[first : first + 3]
        m = midpoint[first : first + 3]
        b = endpoint_b[first : first + 3]
        region = (
            region_counts[min(2, (pixel % scanout_width) * 3 // scanout_width)]
            if scanout_width else None
        )
        if region is not None:
            region["sampled_pixels"] += 1
        if max(a) > black_threshold and max(b) > black_threshold:
            endpoint_coloured += 1
            if region is not None:
                region["endpoint_coloured_pixels"] += 1
            if max(m) <= black_threshold:
                midpoint_only_dark += 1
                if region is not None:
                    region["midpoint_only_dark_pixels"] += 1
        endpoint_delta = max(abs(a[channel] - b[channel]) for channel in range(3))
        if endpoint_delta <= stable_endpoint_delta:
            stable_endpoint_pixels += 1
            if region is not None:
                region["stable_endpoint_pixels"] += 1
            midpoint_from_a = max(
                abs(m[channel] - a[channel]) for channel in range(3)
            )
            midpoint_from_b = max(
                abs(m[channel] - b[channel]) for channel in range(3)
            )
            if min(midpoint_from_a, midpoint_from_b) >= midpoint_outlier_delta:
                stable_endpoint_outliers += 1
                if region is not None:
                    region["stable_endpoint_outlier_pixels"] += 1
    result = {
        "sampled_pixels": sampled,
        "sample_stride_pixels": pixel_stride,
        "endpoint_coloured_pixels": endpoint_coloured,
        "midpoint_only_dark_pixels": midpoint_only_dark,
        "midpoint_only_dark_fraction": (
            midpoint_only_dark / endpoint_coloured if endpoint_coloured else 0.0
        ),
        "stable_endpoint_pixels": stable_endpoint_pixels,
        "stable_endpoint_outlier_pixels": stable_endpoint_outliers,
        "stable_endpoint_outlier_fraction": (
            stable_endpoint_outliers / stable_endpoint_pixels
            if stable_endpoint_pixels
            else 0.0
        ),
        "black_threshold": black_threshold,
        "stable_endpoint_delta": stable_endpoint_delta,
        "midpoint_outlier_delta": midpoint_outlier_delta,
    }
    if scanout_width is not None:
        # These are thirds of the complete scanout, not a claimed native-4:3
        # crop. An edge-only regression must not disappear in the global mean.
        result["vertical_thirds"] = {
            name: {
                **counts,
                "midpoint_only_dark_fraction": (
                    counts["midpoint_only_dark_pixels"] / counts["endpoint_coloured_pixels"]
                    if counts["endpoint_coloured_pixels"] else 0.0
                ),
                "stable_endpoint_outlier_fraction": (
                    counts["stable_endpoint_outlier_pixels"] / counts["stable_endpoint_pixels"]
                    if counts["stable_endpoint_pixels"] else 0.0
                ),
            }
            for name, counts in zip(("left", "centre", "right"), region_counts)
        }
    return result


def ordered_endpoint_control_matches(
    endpoints: Iterable[Presentation], controls: list[Presentation]
) -> tuple[list[dict[str, int | str]], list[int]]:
    matches: list[dict[str, int | str]] = []
    unmatched: list[int] = []
    control_index = 0
    for endpoint in endpoints:
        found = None
        for index in range(control_index, len(controls)):
            if controls[index].sha256 == endpoint.sha256:
                found = index
                break
        if found is None:
            unmatched.append(endpoint.serial)
            continue
        control = controls[found]
        matches.append(
            {
                "endpoint_serial": endpoint.serial,
                "endpoint_frame": endpoint.frame,
                "control_serial": control.serial,
                "control_frame": control.frame,
                "sha256": endpoint.sha256,
            }
        )
        control_index = found + 1
    return matches, unmatched


def validate_page_causality(trace_path: Path) -> dict:
    schedules: list[dict] = []
    scene_barriers: list[dict] = []
    with trace_path.open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            try:
                event = json.loads(line)
            except json.JSONDecodeError as error:
                raise ValueError(
                    f"{trace_path}:{line_number}: invalid JSON: {error}"
                ) from error
            if event.get("event") == "renderer.crash3.temporal_schedule":
                schedules.append(event)
            elif event.get("event") == "renderer.crash3.temporal_underlay_history_barrier":
                if int(event.get("history_discontinuity_mask", 0)) & 15:
                    scene_barriers.append({
                        "frame": int(event["frame"]),
                        "previous_frame": int(event["previous_frame"]),
                        "history_discontinuity_mask": int(event["history_discontinuity_mask"]),
                    })
    if not schedules:
        raise ValueError(f"{trace_path}: no temporal schedule events found")

    pending: list[dict[str, int | list[int]]] = []
    resolved: list[dict[str, int | list[int]]] = []
    errors: list[dict[str, int | str | list[int]]] = []
    distinct_endpoint_pages = 0
    same_page_transitions = 0
    direct_endpoints: list[dict] = []
    for event in schedules:
        frame = int(event.get("frame", -1))
        sequence = int(event.get("seq", -1))
        if int(event.get("current_endpoint_direct_resolve", 0)):
            page = [int(event.get("midpoint_source_x", -1)),
                    int(event.get("midpoint_source_y", -1))]
            direct_endpoints.append({"frame": frame, "seq": sequence, "page": page})
            if (any(value < 0 or value == 0xFFFFFFFF for value in page)
                    or int(event.get("midpoint_selected", 0))
                    or int(event.get("endpoint_resolve", 0))):
                errors.append({"frame": frame, "seq": sequence,
                               "error": "direct endpoint has no exclusive valid current page"})
        if int(event.get("midpoint_selected", 0)):
            midpoint_page = [
                int(event.get("midpoint_source_x", -1)),
                int(event.get("midpoint_source_y", -1)),
            ]
            previous_page = [
                int(event.get("previous_source_x", -1)),
                int(event.get("previous_source_y", -1)),
            ]
            if any(value < 0 or value == 0xFFFFFFFF for value in midpoint_page):
                errors.append(
                    {
                        "frame": frame,
                        "seq": sequence,
                        "error": "midpoint has no current endpoint page",
                        "midpoint_page": midpoint_page,
                    }
                )
            if midpoint_page == previous_page:
                same_page_transitions += 1
            else:
                distinct_endpoint_pages += 1
            if pending:
                errors.append(
                    {
                        "frame": frame,
                        "seq": sequence,
                        "error": "midpoint selected before the preceding endpoint",
                    }
                )
            pending.append(
                {
                    "midpoint_frame": frame,
                    "midpoint_seq": sequence,
                    "midpoint_page": midpoint_page,
                    "previous_page": previous_page,
                }
            )
        if int(event.get("endpoint_resolve", 0)):
            endpoint_page = [
                int(event.get("pending_source_x", -1)),
                int(event.get("pending_source_y", -1)),
            ]
            if not pending:
                errors.append(
                    {
                        "frame": frame,
                        "seq": sequence,
                        "error": "endpoint resolve has no preceding midpoint",
                        "endpoint_page": endpoint_page,
                    }
                )
                continue
            expected = pending.pop(0)
            record = {
                **expected,
                "endpoint_frame": frame,
                "endpoint_seq": sequence,
                "endpoint_page": endpoint_page,
            }
            resolved.append(record)
            if endpoint_page != expected["midpoint_page"]:
                errors.append(
                    {
                        "frame": frame,
                        "seq": sequence,
                        "error": "deferred endpoint resolved a page other than B",
                        "expected_page": expected["midpoint_page"],
                        "endpoint_page": endpoint_page,
                    }
                )
            if sequence <= int(expected["midpoint_seq"]):
                errors.append(
                    {
                        "frame": frame,
                        "seq": sequence,
                        "error": "endpoint did not follow its midpoint",
                    }
                )
            if frame != int(expected["midpoint_frame"]) + 1:
                errors.append(
                    {
                        "frame": frame,
                        "seq": sequence,
                        "error": "deferred endpoint was not presented on the next VBlank",
                        "midpoint_frame": expected["midpoint_frame"],
                    }
                )

    # A bounded trace can end immediately after a selected midpoint.  At most
    # that final unresolved pair is allowed; anything older is a real cadence
    # discontinuity.
    if len(pending) > 1:
        errors.append(
            {
                "frame": int(pending[0]["midpoint_frame"]),
                "seq": int(pending[0]["midpoint_seq"]),
                "error": "multiple midpoint pages were never resolved",
            }
        )
    return {
        "trace": str(trace_path),
        "scene_underlay_barriers": scene_barriers,
        "direct_current_endpoints": direct_endpoints,
        "schedule_events": len(schedules),
        "selected_midpoints": len(resolved) + len(pending),
        "resolved_endpoint_pairs": len(resolved),
        "trailing_pending_pairs": len(pending),
        "distinct_endpoint_page_pairs": distinct_endpoint_pages,
        "same_page_pairs": same_page_transitions,
        "page_route_errors": errors,
        "passed": not errors,
    }


def validate(
    capture_directory: Path,
    control_directory: Path | None = None,
    minimum_distinct_fraction: float = 0.5,
    maximum_midpoint_only_dark_fraction: float | None = None,
    maximum_stable_endpoint_outlier_fraction: float | None = None,
    trace_path: Path | None = None,
    maximum_regional_discontinuity_fraction: float | None = None,
) -> dict:
    if not 0.0 <= minimum_distinct_fraction <= 1.0:
        raise ValueError("minimum distinct fraction must be in 0..1")
    for name, value in (
        ("maximum regional discontinuity fraction", maximum_regional_discontinuity_fraction),
        ("maximum midpoint-only dark fraction", maximum_midpoint_only_dark_fraction),
        (
            "maximum stable-endpoint outlier fraction",
            maximum_stable_endpoint_outlier_fraction,
        ),
    ):
        if value is not None and not 0.0 <= value <= 1.0:
            raise ValueError(f"{name} must be in 0..1")
    sequence = load_presentations(capture_directory)
    failures: list[str] = []
    page_causality = validate_page_causality(trace_path) if trace_path else None
    scene_barrier_frames = {
        event["frame"] for event in page_causality["scene_underlay_barriers"]
    } if page_causality else set()
    direct_endpoint_frames = {
        event["frame"] for event in page_causality["direct_current_endpoints"]
    } if page_causality else set()
    accepted_scene_barrier_serials: list[int] = []

    serial_gaps = [
        [left.serial, right.serial]
        for left, right in zip(sequence, sequence[1:])
        if right.serial != left.serial + 1
    ]
    if serial_gaps:
        failures.append("presentation serial is not contiguous")

    first_midpoint = next(
        (index for index, item in enumerate(sequence) if item.phase == "midpoint"),
        None,
    )
    temporal_phase_errors: list[dict[str, int | str]] = []
    if first_midpoint is None:
        failures.append("capture contains no midpoint")
    else:
        expected = "midpoint"
        for item in sequence[first_midpoint:]:
            if (item.phase == "ordinary" and expected == "midpoint"
                    and item.frame in scene_barrier_frames
                    and item.frame in direct_endpoint_frames):
                accepted_scene_barrier_serials.append(item.serial)
                continue
            if item.phase != expected:
                temporal_phase_errors.append(
                    {"serial": item.serial, "expected": expected, "observed": item.phase}
                )
            expected = "endpoint" if item.phase == "midpoint" else "midpoint"
        if temporal_phase_errors:
            failures.append("midpoint/endpoint presentation phases do not alternate")

    triplets: list[dict] = []
    moving_triplets = 0
    distinct_triplets = 0
    midpoint_matches_a: list[int] = []
    midpoint_matches_b: list[int] = []
    midpoint_dark_failures: list[int] = []
    midpoint_outlier_failures: list[int] = []
    regional_failures: list[dict] = []
    for index, midpoint in enumerate(sequence):
        if midpoint.phase != "midpoint" or index == 0 or index + 1 >= len(sequence):
            continue
        endpoint_a = sequence[index - 1]
        endpoint_b = sequence[index + 1]
        if endpoint_b.phase != "endpoint":
            continue
        if endpoint_b.frame != midpoint.frame + 1:
            temporal_phase_errors.append(
                {
                    "serial": endpoint_b.serial,
                    "expected": f"endpoint frame {midpoint.frame + 1}",
                    "observed": f"endpoint frame {endpoint_b.frame}",
                }
            )
        a_to_b = image_difference(endpoint_a.pixels, endpoint_b.pixels)
        a_to_m = image_difference(endpoint_a.pixels, midpoint.pixels)
        m_to_b = image_difference(midpoint.pixels, endpoint_b.pixels)
        discontinuity = midpoint_discontinuity_probe(
            endpoint_a.pixels, midpoint.pixels, endpoint_b.pixels,
            scanout_width=midpoint.width,
        )
        if maximum_regional_discontinuity_fraction is not None:
            for name, region in discontinuity["vertical_thirds"].items():
                for metric in ("midpoint_only_dark_fraction", "stable_endpoint_outlier_fraction"):
                    if region[metric] > maximum_regional_discontinuity_fraction:
                        regional_failures.append({
                            "serial": midpoint.serial, "region": name,
                            "metric": metric, "fraction": region[metric],
                        })
        moving = endpoint_a.sha256 != endpoint_b.sha256
        distinct = midpoint.sha256 not in (endpoint_a.sha256, endpoint_b.sha256)
        if moving:
            moving_triplets += 1
            if distinct:
                distinct_triplets += 1
            if midpoint.sha256 == endpoint_a.sha256:
                midpoint_matches_a.append(midpoint.serial)
            if midpoint.sha256 == endpoint_b.sha256:
                midpoint_matches_b.append(midpoint.serial)
        if (
            maximum_midpoint_only_dark_fraction is not None
            and discontinuity["midpoint_only_dark_fraction"]
            > maximum_midpoint_only_dark_fraction
        ):
            midpoint_dark_failures.append(midpoint.serial)
        if (
            maximum_stable_endpoint_outlier_fraction is not None
            and discontinuity["stable_endpoint_outlier_fraction"]
            > maximum_stable_endpoint_outlier_fraction
        ):
            midpoint_outlier_failures.append(midpoint.serial)
        triplets.append(
            {
                "a": {"frame": endpoint_a.frame, "serial": endpoint_a.serial},
                "m": {"frame": midpoint.frame, "serial": midpoint.serial},
                "b": {"frame": endpoint_b.frame, "serial": endpoint_b.serial},
                "moving_endpoints": moving,
                "midpoint_distinct_from_both": distinct,
                "a_to_b": a_to_b,
                "a_to_m": a_to_m,
                "m_to_b": m_to_b,
                "discontinuity_probe": discontinuity,
            }
        )

    if temporal_phase_errors and (
        "midpoint/endpoint presentation phases do not alternate" not in failures
    ):
        failures.append("midpoint/endpoint presentation cadence is discontinuous")
    if not triplets:
        failures.append("capture contains no complete A/M/B triplet")
    if not moving_triplets:
        failures.append("capture contains no moving endpoint pair")
    distinct_fraction = (
        distinct_triplets / moving_triplets if moving_triplets else 0.0
    )
    if moving_triplets and distinct_fraction < minimum_distinct_fraction:
        failures.append(
            "too few moving midpoint scanouts differ from both authentic endpoints"
        )
    if midpoint_dark_failures:
        failures.append("one or more midpoints introduced excess dark holes")
    if midpoint_outlier_failures:
        failures.append(
            "one or more midpoints introduced excess stable-endpoint outliers"
        )
    if regional_failures:
        failures.append("one or more scanout regions have excess midpoint discontinuity")

    control_summary = None
    if control_directory is not None:
        controls = load_presentations(control_directory)
        endpoints = [item for item in sequence if item.phase == "endpoint"
                     or item.serial in accepted_scene_barrier_serials]
        matches, unmatched = ordered_endpoint_control_matches(endpoints, controls)
        control_summary = {
            "directory": str(control_directory),
            "control_presentations": len(controls),
            "endpoint_matches": matches,
            "unmatched_endpoint_serials": unmatched,
        }
        if unmatched:
            failures.append("one or more exact endpoints have no ordered control match")

    if page_causality is not None:
        if not page_causality["passed"]:
            failures.append("temporal Vulkan page causality is invalid")

    return {
        "schema": SCHEMA,
        "capture_directory": str(capture_directory),
        "scanout_extent": [sequence[0].width, sequence[0].height],
        "presentations": len(sequence),
        "ordinary_presentations": sum(item.phase == "ordinary" for item in sequence),
        "midpoint_presentations": sum(item.phase == "midpoint" for item in sequence),
        "endpoint_presentations": sum(item.phase == "endpoint" for item in sequence),
        "serial_gaps": serial_gaps,
        "temporal_phase_errors": temporal_phase_errors,
        "accepted_scene_barrier_serials": accepted_scene_barrier_serials,
        "triplets": triplets,
        "moving_triplets": moving_triplets,
        "distinct_moving_triplets": distinct_triplets,
        "distinct_moving_fraction": distinct_fraction,
        "midpoint_matches_a_serials": midpoint_matches_a,
        "midpoint_matches_b_serials": midpoint_matches_b,
        "midpoint_dark_failure_serials": midpoint_dark_failures,
        "midpoint_outlier_failure_serials": midpoint_outlier_failures,
        "regional_discontinuity_failures": regional_failures,
        "maximum_regional_discontinuity_fraction": maximum_regional_discontinuity_fraction,
        "maximum_midpoint_only_dark_fraction": (
            maximum_midpoint_only_dark_fraction
        ),
        "maximum_stable_endpoint_outlier_fraction": (
            maximum_stable_endpoint_outlier_fraction
        ),
        "control": control_summary,
        "page_causality": page_causality,
        "failures": failures,
        "passed": not failures,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_directory", type=Path)
    parser.add_argument("--control-directory", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--minimum-distinct-fraction", type=float, default=0.5)
    parser.add_argument("--maximum-midpoint-only-dark-fraction", type=float)
    parser.add_argument("--maximum-stable-endpoint-outlier-fraction", type=float)
    parser.add_argument("--maximum-regional-discontinuity-fraction", type=float,
                        help="alarm threshold for each vertical third of the complete scanout; not a visual-correctness oracle")
    parser.add_argument(
        "--trace",
        type=Path,
        help="JSONL trace containing renderer.crash3.temporal_schedule",
    )
    args = parser.parse_args(argv)
    try:
        report = validate(
            args.capture_directory.resolve(),
            args.control_directory.resolve() if args.control_directory else None,
            args.minimum_distinct_fraction,
            args.maximum_midpoint_only_dark_fraction,
            args.maximum_stable_endpoint_outlier_fraction,
            args.trace.resolve() if args.trace else None,
            args.maximum_regional_discontinuity_fraction,
        )
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
