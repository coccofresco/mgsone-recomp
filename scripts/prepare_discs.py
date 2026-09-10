#!/usr/bin/env python3
"""Analyze local Italian MGS discs using the inherited PS1Recomp analyzer."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disc1', required=True, type=Path, help='Raw MODE2/2352 IMG or BIN')
    parser.add_argument('--disc2', required=True, type=Path, help='Raw MODE2/2352 IMG or BIN')
    parser.add_argument('--analyzer', type=Path, default=ROOT / 'build/ps1Analyzer/ps1Analyzer')
    args = parser.parse_args()
    analyzer = args.analyzer.resolve(strict=True)
    paths = [args.disc1.resolve(strict=True), args.disc2.resolve(strict=True)]
    for path in paths:
        if path.stat().st_size % 2352:
            parser.error(f'Not a whole-sector raw 2352-byte image: {path}')
        with path.open('rb') as stream:
            header = stream.read(16)
        if header[:12] != b'\x00' + b'\xff' * 10 + b'\x00' or header[15] != 2:
            parser.error(f'Expected a MODE2/2352 image: {path}')
    out = ROOT / 'data/discs'
    out.mkdir(parents=True, exist_ok=True)
    reports = []
    for number, path in enumerate(paths, 1):
        alias = out / f'disc{number}.bin'
        if alias.is_symlink():
            if alias.resolve() != path:
                parser.error(f'{alias} already points to a different disc')
        elif alias.exists():
            parser.error(f'Refusing to replace {alias}')
        else:
            alias.symlink_to(path)
        config = ROOT / f'data/disc{number}.analysis.toml'
        log = ROOT / f'data/disc{number}.analysis.log'
        with log.open('w') as stream:
            subprocess.run([str(analyzer), str(alias), str(config)], cwd=ROOT,
                           stdout=stream, stderr=subprocess.STDOUT, check=True)
        exe = Path(str(alias) + '.boot.exe')
        contents = exe.read_bytes()
        if contents[:8] != b'PS-X EXE':
            raise ValueError(f'Invalid extracted executable: {exe}')
        boot_lines = [line for line in log.read_text().splitlines()
                      if line.startswith('Boot executable:')]
        if len(boot_lines) != 1:
            raise ValueError(f'Cannot identify boot executable in {log}')
        with path.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        reports.append(dict(disc=number, boot_executable=boot_lines[0].split(':', 1)[1].strip(),
                            image_size=path.stat().st_size, image_sha256=digest,
                            exe_sha256=hashlib.sha256(contents).hexdigest(),
                            entry_pc=f'0x{struct.unpack_from("<I", contents, 0x10)[0]:08X}',
                            load_address=f'0x{struct.unpack_from("<I", contents, 0x18)[0]:08X}',
                            text_size=struct.unpack_from('<I', contents, 0x1c)[0],
                            stack_base=f'0x{struct.unpack_from("<I", contents, 0x30)[0]:08X}'))
        print(json.dumps(reports[-1], indent=2))
    (ROOT / 'data/disc-manifest.json').write_text(json.dumps({'discs': reports}, indent=2) + '\n')


if __name__ == '__main__':
    main()
