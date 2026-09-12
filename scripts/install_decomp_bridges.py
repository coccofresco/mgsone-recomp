#!/usr/bin/env python3
"""Bind compiled decomp modules at generated USA function entry boundaries.

Leaves original bodies available for unported behavior and savestate interior
continuations. Does not rewrite calls or identify functions heuristically.
"""
import argparse
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
USA_SHA256 = '3342f75717c5134aec832547691c2354e67a253abfcfa9e0e823e2133bb1fc32'
ENTRIES = (0x8001E254, 0x8001E2E8, 0x8001E310, 0x8001E378)


def install(directory, executable):
    if hashlib.sha256(executable.read_bytes()).hexdigest() != USA_SHA256:
        raise ValueError('decomp ABI manifest requires USA v1.0 SLUS_005.94')
    sources = {p: p.read_text() for p in directory.glob('recompiled_out*.cpp')}
    edits = {}
    for entry in ENTRIES:
        marker = f'    PS1_FUNCTION_TRACE_HOOK(ctx, 0x{entry:08X}u);'
        owners = [p for p, text in sources.items() if marker in text]
        if len(owners) != 1:
            raise ValueError(f'expected exactly one generated entry for {entry:08X}')
        p = owners[0]
        hook = (f'\n    extern bool mgsTryDecompiled(recomp_context*, uint32_t);'
                f'\n    if (mgsTryDecompiled(ctx, 0x{entry:08X}u)) return;')
        current = edits.get(p, sources[p])
        if marker + hook not in current:
            edits[p] = current.replace(marker, marker + hook, 1)
    # Validate all entries before changing any output file.
    for path, text in edits.items():
        path.write_text(text)
    return len(edits)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=ROOT / 'engine/ps1Runtime/src')
    parser.add_argument('--executable', type=Path, default=ROOT / 'data/assets/usa/disc1/SLUS_005.94')
    args = parser.parse_args()
    print(f'Installed {len(ENTRIES)} decomp entry bindings; changed {install(args.directory, args.executable)} files')
