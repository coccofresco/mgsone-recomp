#!/usr/bin/env python3
"""Compare native 4-bit stereo XA PCM against the unmodified SwanStation decoder.

Uses the donor's actual 6:7 interpolation code with only the host FIFO capacity
check removed for an offline full-file decode. Does not certify in-game AV sync.
"""
import argparse
import json
import subprocess
from pathlib import Path

from mgs_tas import ROOT, sha256, write_json


def fingerprint(path):
    return dict(path=str(path.resolve()), sha256=sha256(path))


def build_oracle(output, donor):
    """Compile once for any number of source-indexed movie extents."""
    drive = donor / 'src/core/cdrom.cpp'
    decoder = donor / 'src/common/cd_xa.cpp'
    source = drive.read_text()
    a = source.index('static std::array<std::array<int16_t, 29>, 7> s_zigzag_table')
    b = source.index('void CDROM::ResetCurrentXAFile()', a)
    reference = source[a:b].replace('void CDROM::ResampleXAADPCM', 'void Reference::ResampleXAADPCM')
    a = reference.index('  // Since the disc reads')
    b = reference.index('  int16_t* left_ringbuf', a)
    assert 'm_audio_fifo.GetSize() > AUDIO_FIFO_LOW_WATERMARK' in reference[a:b]
    reference = reference[:a] + reference[b:]
    template = ROOT / 'engine/tools/donor_trace/xa_oracle.cpp.in'
    source_path = output / 'oracle.cpp'
    source_path.write_text(template.read_text().replace('// @DONOR_RESAMPLER@', reference))
    native = ROOT / 'engine/ps1Runtime/src/cdrom/xa_adpcm.cpp'
    includes = [f'-I{donor / "src"}', f'-I{ROOT / "engine/third_party/fmt/include"}',
                f'-I{ROOT / "engine/ps1Runtime/include"}']
    commands = [
        ['c++', '-std=c++17', '-O2', *includes, '-c', str(decoder), '-o', str(output / 'donor.o')],
        ['c++', '-std=c++20', '-O2', *includes, str(source_path), str(output / 'donor.o'),
         str(native), '-o', str(output / 'oracle')]]
    with (output / 'build.log').open('w') as log:
        for command in commands:
            subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
    synthetic = json.loads(subprocess.check_output([str(output / 'oracle')], text=True))
    if synthetic['fnv1a64'] != '3aedf009aafb4ee0':
        raise ValueError('synthetic XA regression')
    return dict(synthetic=synthetic, inputs=[fingerprint(p) for p in (drive, decoder, native, template)],
                donor_commit=subprocess.check_output(['git', '-C', str(donor), 'rev-parse', 'HEAD'], text=True).strip(),
                commands=commands)


def audit_catalog(catalog, output, oracle):
    source = json.loads(catalog.read_text())
    if source.get('format') != 'mgs-movie-catalog-1' or source.get('status') != 'complete':
        raise ValueError('requires a complete source movie catalog')
    results = []
    for entry in source['discs']:
        disc = Path(entry['disc']['path'])
        if fingerprint(disc) != entry['disc']:
            raise ValueError(f'disc identity changed: {disc}')
        for movie in entry['movies']:
            audio = movie['audio']
            if len(audio) != 1 or audio[0]['coding'] != 1:
                raise ValueError('oracle requires a single 4-bit stereo 37800-Hz stream')
            pcm = json.loads(subprocess.check_output([str(output / 'oracle'), str(disc),
                str(movie['start_lba']), str(movie['sectors'])], text=True))
            if (pcm['sectors'] != audio[0]['sectors'] or pcm['stereo_frames'] != movie['stereo_frames']
                    or pcm['restore_boundaries'] != pcm['sectors'] or not pcm['sample_exact']):
                raise ValueError(f'catalog/decoder/restore disagreement: {movie["code"]}')
            results.append(dict(disc=entry['disc'], code=movie['code'], start_lba=movie['start_lba'],
                                sectors=movie['sectors'], pcm=pcm))
    if not results:
        raise ValueError('empty movie catalog')
    report = dict(format='mgs-xa-catalog-audit-1', status='complete', catalog=fingerprint(catalog),
        **oracle, movies=results, counts=dict(movies=len(results),
            xa_sectors=sum(r['pcm']['sectors'] for r in results),
            stereo_frames=sum(r['pcm']['stereo_frames'] for r in results),
            restore_boundaries=sum(r['pcm']['restore_boundaries'] for r in results)),
        scope='All indexed XA sectors: independent donor PCM comparison and native decoder snapshot restore at every sector boundary. Not NSV file loading, live SPU mix, movie playback or AV synchronization.')
    write_json(output / 'result.json', report)
    print(json.dumps(report['counts']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disc', type=Path, help='raw disc image, or movie catalog with --catalog')
    parser.add_argument('output', type=Path)
    parser.add_argument('--catalog', action='store_true')
    parser.add_argument('--start-lba', type=int)
    parser.add_argument('--sectors', type=int)
    parser.add_argument('--donor', type=Path, default=ROOT / '.local/swanstation')
    args = parser.parse_args()
    disc, output, donor = args.disc.resolve(), args.output.resolve(), args.donor.resolve()
    if args.catalog:
        if args.start_lba is not None or args.sectors is not None:
            parser.error('catalog supplies its own complete extents')
    elif args.start_lba is None or args.sectors is None or args.start_lba < 0 or args.sectors < 1:
        parser.error('invalid or missing sector range')
    elif disc.stat().st_size < (args.start_lba + args.sectors) * 2352:
        parser.error('range outside raw 2352-byte disc')
    output.mkdir(parents=True, exist_ok=False)
    oracle = build_oracle(output, donor)
    if args.catalog:
        audit_catalog(disc, output, oracle)
        return
    actual = json.loads(subprocess.check_output([str(output / 'oracle'), str(disc),
                       str(args.start_lba), str(args.sectors)], text=True))
    if actual['sectors'] <= 0:
        raise ValueError('no XA sectors decoded')
    report = dict(format='mgs-xa-audit-1', status='complete', disc=fingerprint(disc),
                  start_lba=args.start_lba, sectors=args.sectors, pcm=actual, **oracle,
                  scope='Sample-exact decoder and resampler comparison for 4-bit stereo 37800-Hz sectors; not live SPU output or AV synchronization.')
    write_json(output / 'result.json', report)
    print(json.dumps(actual))


if __name__ == '__main__':
    main()
