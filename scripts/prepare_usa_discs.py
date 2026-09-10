#!/usr/bin/env python3
"""Extract the user's USA v1.0 archives and fingerprint their executable copies."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from zipfile import ZipFile
from mgs_assets import Disc

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archives', type=Path, default=Path.home() / 'Scaricati')
    args = parser.parse_args()
    reports = []
    for number in (1, 2):
        archive = args.archives / f'Metal Gear Solid (USA) (Disc {number}) (v1.0).zip'
        target = ROOT / f'data/discs/usa/disc{number}'
        target.mkdir(parents=True, exist_ok=True)
        with ZipFile(archive) as source:
            for member in source.infolist():
                path = Path(member.filename)
                if path.is_absolute() or '..' in path.parts or len(path.parts) != 1:
                    raise ValueError(f'Unexpected archive path: {member.filename}')
                if path.suffix.lower() not in ('.bin', '.cue'):
                    raise ValueError(f'Unexpected archive member: {member.filename}')
                output = target / path
                if output.exists():
                    # Validate existing extracted data against archive CRC.
                    import zlib
                    crc = 0
                    with output.open('rb') as stream:
                        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                            crc = zlib.crc32(chunk, crc)
                    if output.stat().st_size != member.file_size or crc != member.CRC:
                        raise ValueError(f'Existing extraction differs: {output}')
                else:
                    source.extract(member, target)  # ZipFile validates CRC during extraction.
        image = next(target.glob('*.bin'))
        disc = Disc(image)
        for name, record in disc.files().items():
            if 'SLUS_' not in name and name != '/SYSTEM.CNF':
                continue
            contents = disc.read(record['lba'], record['size'])
            output = ROOT / f'data/assets/usa/disc{number}' / name.lstrip('/')
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(contents)
            reports.append(dict(disc=number, name=name, size=len(contents),
                                sha256=hashlib.sha256(contents).hexdigest(),
                                entry=hex(struct.unpack_from('<I', contents, 16)[0])
                                if contents.startswith(b'PS-X EXE') else None))
    (ROOT / 'data/discs/usa/manifest.json').write_text(json.dumps(reports, indent=2) + '\n')
    print('USA v1.0 discs and executable fingerprints ready under data/')


if __name__ == '__main__':
    main()
