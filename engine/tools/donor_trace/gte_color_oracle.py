#!/usr/bin/env python3
"""Run the actual local SwanStation colour GTE as a separate-process oracle.

Produces game-independent differential vectors (GTECOL1). --capture also
compares instructions observed in an actual recomp run, without running its
game code in the donor. Donor sources are read/compiled, never patched/copied
into the runtime. Reports bind the executable and inputs by SHA-256.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
import itertools
import random
import struct
import subprocess
from pathlib import Path

MAGIC = b"GTECOL1\0"
RECORD = struct.Struct("<130I")  # PC, instruction, before[64], after[64]
COMMANDS = (0x10, 0x11, 0x13, 0x14, 0x16, 0x1B, 0x1C, 0x1E,
            0x20, 0x29, 0x2A, 0x3F)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def generated_samples(per_command):
    rng = random.Random(0x47544520260906)
    edges = (0, 1, -1, 4096, -4096, 32767, -32768, 0x7FFFFFFF, -0x80000000)
    for command in COMMANDS:
        for sample in range(per_command):
            regs = [rng.getrandbits(32) for _ in range(64)]
            # Canonical sign-extended architectural input registers; other
            # inputs deliberately exercise all bits, including FC and BK.
            for reg in (1, 3, 5, 8, 9, 10, 11, 36, 44, 52, 58, 59, 61, 62):
                regs[reg] = rng.randrange(-32768, 32768) & 0xFFFFFFFF
            for reg in (7, 16, 17, 18, 19):
                regs[reg] &= 0xFFFF
            if sample < len(edges) * 4:
                for channel in range(3):
                    regs[53 + channel] = edges[sample // 4] & 0xFFFFFFFF
            inst = command | ((sample & 1) << 19) | (((sample >> 1) & 1) << 10)
            yield (0, inst, *regs), None


def captured_samples(path):
    with path.open("rb") as stream:
        if stream.read(len(MAGIC)) != MAGIC:
            raise ValueError("invalid GTECOL1 header")
        while data := stream.read(RECORD.size):
            if len(data) != RECORD.size:
                raise ValueError("truncated GTECOL1 record")
            record = RECORD.unpack(data)
            if record[1] & 63 not in COMMANDS:
                raise ValueError("unsupported captured colour command")
            yield record[:66], record[66:]


def verify_vulkan(samples, expected_bytes, output, headers):
    here = Path(__file__).resolve().parent
    shader = output.with_suffix(".spv")
    runner = output.with_suffix(".gpu")
    subprocess.run(["glslc", "--target-env=vulkan1.0", "-O",
                    str(here / "gte_color_oracle.comp"), "-o", str(shader)], check=True)
    subprocess.run(["c++", "-std=c++20", "-O2", f"-I{headers}",
                    str(here / "vulkan_compute_oracle.cpp"), "-lvulkan", "-o",
                    str(runner)], check=True)
    words, expected = [], []
    for index, (before, _) in enumerate(samples):
        instruction = before[1]
        if not instruction & (1 << 19) or instruction & 63 not in (0x10, 0x2A):
            continue  # The production GLSL helper implements DPCS/DPCT sf=1 RGB.
        regs = before[2:]
        after = struct.unpack_from("<64I", expected_bytes, index * 256)
        for source, destination in ([(6, 22)] if instruction & 63 == 0x10 else
                                    [(20, 20), (21, 21), (22, 22)]):
            words.extend((regs[source], *regs[53:56], regs[8], 0xDEADBEEF))
            expected.append(after[destination] & 0xFFFFFF)
    if not expected:
        raise ValueError("no supported sf=1 colour cases for Vulkan")
    result = subprocess.run([str(runner.resolve()), str(shader.resolve()), str(len(expected))],
                            input=struct.pack(f"<{len(words)}I", *words),
                            capture_output=True, check=True)
    if len(result.stdout) != len(words) * 4:
        raise ValueError("incomplete Vulkan result")
    actual = struct.unpack(f"<{len(words)}I", result.stdout)
    mismatches = sum(actual[i * 6 + 5] != value for i, value in enumerate(expected))
    if any(actual[i] != word for i, word in enumerate(words) if i % 6 != 5):
        raise ValueError("Vulkan kernel modified its input words")
    return dict(samples=len(expected), mismatches=mismatches,
                device=result.stderr.decode().strip(), shader_sha256=sha(shader),
                runner_sha256=sha(runner), production_glsl_sha256=sha(
                    here.parent.parent / "ps1Runtime/shaders/gte_color_math.glsl"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--swan-source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--capture", type=Path)
    parser.add_argument("--limit", type=int, help="explicit input-prefix bound")
    parser.add_argument("--samples-per-command", type=int, default=2000)
    parser.add_argument("--vulkan", action="store_true",
                        help="also execute the production colour GLSL on a Vulkan device")
    parser.add_argument("--vulkan-headers", type=Path,
                        help="Vulkan headers root (default: donor's loader includes)")
    args = parser.parse_args()
    if not 36 <= args.samples_per_command <= 100000:
        parser.error("samples-per-command must be in 36..100000")
    if args.limit is not None and args.limit <= 0:
        parser.error("limit must be positive")
    source = args.swan_source.resolve()
    if args.capture and args.capture.resolve() in {
        args.output.resolve(), args.output.with_suffix(".json").resolve(),
        args.output.with_suffix(".oracle").resolve(), args.output.with_suffix(".spv").resolve(),
        args.output.with_suffix(".gpu").resolve()
    }:
        parser.error("output must not overwrite the source capture")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    executable = args.output.with_suffix(".oracle")
    adapter = Path(__file__).with_name("swan_gte_oracle.cpp").resolve()
    command = ["c++", "-std=c++20", "-O2", "-ffunction-sections", "-fdata-sections",
               f"-I{source / 'src'}", f"-I{source / 'dep/fmt/include'}",
               f"-I{source / 'dep/libretro-common/include'}", str(adapter),
               "-Wl,--gc-sections", "-o", str(executable)]
    subprocess.run(command, check=True)
    stream = (captured_samples(args.capture) if args.capture else
              generated_samples(args.samples_per_command))
    samples = list(itertools.islice(stream, args.limit)) if args.limit else list(stream)
    if not samples:
        parser.error("empty capture")
    inputs = b"".join(struct.pack("<65I", *row[1:]) for row, _ in samples)
    result = subprocess.run([str(executable.resolve())], input=inputs,
                            capture_output=True, check=True)
    if len(result.stdout) != 256 * len(samples):
        raise ValueError("oracle returned an incomplete result")
    counts, mismatches, registers, pcs = (collections.Counter() for _ in range(4))
    examples = []
    with args.output.open("wb") as output:
        output.write(MAGIC)
        for index, (before, observed) in enumerate(samples):
            expected = struct.unpack_from("<64I", result.stdout, index * 256)
            output.write(RECORD.pack(*before, *expected))
            operation = f"{before[1] & 63:02x}"
            counts[operation] += 1
            if observed is not None and observed != expected:
                mismatches[operation] += 1
                pcs[f"{before[0]:08x}"] += 1
                changed = [i for i in range(64) if observed[i] != expected[i]]
                registers.update(changed)
                if len(examples) < 12:
                    examples.append(dict(pc=f"{before[0]:08x}", command=operation,
                        registers={str(i): [observed[i], expected[i]] for i in changed}))
    report = dict(schema="ps1-gte-colour-oracle/v1", instructions=len(samples),
                  explicit_prefix_limit=args.limit,
                  command_counts=counts, mismatching_instructions=sum(mismatches.values()),
                  mismatches_by_command=mismatches, mismatches_by_register=registers,
                  mismatches_by_pc=pcs, examples=examples,
                  capture_sha256=sha(args.capture) if args.capture else None,
                  output_sha256=sha(args.output), adapter_sha256=sha(adapter),
                  donor_gte_sha256=sha(source / "src/core/gte.cpp"),
                  oracle_sha256=sha(executable), compile_command=command,
                  donor_revision=subprocess.check_output(
                      ["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip(),
                  donor_dirty=subprocess.check_output(
                      ["git", "-C", str(source), "status", "--porcelain"], text=True),
                  interpretation="Independent donor instructions, not a whole-game image oracle.")
    if args.vulkan:
        report["vulkan"] = verify_vulkan(samples, result.stdout, args.output,
            args.vulkan_headers or source / "dep/vulkan-loader/include")
    args.output.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: report[k] for k in ("instructions", "command_counts",
                      "mismatching_instructions", "mismatches_by_command")}))
    if args.vulkan:
        print(json.dumps(report["vulkan"]))
    if sum(mismatches.values()) or report.get("vulkan", {}).get("mismatches", 0):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
