#!/usr/bin/env python3
"""Compare all retail ISO file extents with the native parser under ASan/UBSan.

This checks directory walking and path lookup, not file contents or gameplay.
The independent Python reader supplies expected names, LBAs and lengths.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from mgs_assets import Disc

ROOT = Path(__file__).resolve().parents[1]


def identity(path):
    with path.open('rb') as stream:
        return dict(path=str(path.resolve()), sha256=hashlib.file_digest(stream, 'sha256').hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--discs', type=Path, nargs='+', default=[
        ROOT / f'data/discs/usa/disc{n}/Metal Gear Solid (USA) (Disc {n}) (v1.0).bin'
        for n in (1, 2)])
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    sources = [ROOT / 'engine' / path for path in (
        'tools/iso_catalog.cpp', 'ps1Runtime/src/cdrom/bin_reader.cpp',
        'ps1Runtime/src/cdrom/iso9660.cpp')]
    binary = output / 'iso_catalog'
    command = ['c++', '-std=c++20', '-O1', '-g', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', '-I' + str(ROOT / 'engine/ps1Runtime/include'),
               *map(str, sources), '-o', str(binary)]
    with (output / 'build.log').open('w') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    report = dict(format='mgs-native-iso-audit-1', status='complete',
                  scope=__doc__, command=command, sources=[identity(p) for p in sources],
                  binary=identity(binary), discs=[])
    for n, disc in enumerate(args.discs, 1):
        expected = Disc(disc).files()
        result = subprocess.run([str(binary), str(disc.resolve())], env=env, capture_output=True, text=True)
        (output / f'disc{n}.tsv').write_text(result.stdout)
        (output / f'disc{n}.log').write_text(result.stderr)
        observed = {}
        for line in result.stdout.splitlines():
            name, lba, size = line.split('\t')
            if name in observed: raise ValueError('duplicate native path')
            observed[name] = dict(lba=int(lba), size=int(size))
        passed = result.returncode == 0 and not result.stderr and observed == expected
        report['discs'].append(dict(source=identity(disc), passed=passed,
            returncode=result.returncode, files=len(expected), expected=expected, observed=observed))
        if not passed: report['status'] = 'needs-review'
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(status=report['status'], files=sum(d['files'] for d in report['discs']))))
    return 0 if report['status'] == 'complete' else 1


if __name__ == '__main__':
    raise SystemExit(main())
