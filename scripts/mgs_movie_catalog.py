#!/usr/bin/env python3
"""Inventory every retail ZMOVIE extent and validate complete interleaved STR frames.

Counts encoded assets, not runtime playback. Uses the original FS_MOVIE_FILE
index; never guesses movie boundaries from a magic byte scan.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct

from mgs_assets import Disc, require
from mgs_tas import sha256, write_json

SECTOR = 2352
SYNC = b'\0' + b'\xff' * 10 + b'\0'


def movie_index(table, start, count, disc_sectors):
    require(len(table) >= 4, 'truncated movie table')
    n, = struct.unpack_from('<I', table)
    require(1 <= n <= 8 and len(table) >= 4 + n * 8, 'invalid movie count/table')
    require(start >= 0 and count > 1 and start + count <= disc_sectors, 'movie archive outside disc')
    entries = [struct.unpack_from('<HHI', table, 4 + i * 8) for i in range(n)]
    require(len({r[0] for r in entries}) == n, 'duplicate movie identity')
    result = []
    for i, (code, frames, offset) in enumerate(entries):
        end = entries[i + 1][2] if i + 1 < n else count
        require(0 < offset < end <= count and frames >= 3, 'invalid movie extent/frame count')
        result.append(dict(code=f'{code:04x}', table_frames=frames,
                           start_lba=start + offset, sectors=end - offset))
    return result


def scan_movie(sectors, entry):
    frames, streams, types, padding, n = [], Counter(), Counter(), 0, 0
    active = None
    extent_hash = hashlib.sha256()
    for n, raw in enumerate(sectors, 1):
        require(len(raw) == SECTOR and raw[:12] == SYNC and raw[15] == 2, 'invalid/truncated raw mode2 sector')
        require(raw[16:20] == raw[20:24], 'mismatched XA subheader copies')
        extent_hash.update(raw)
        if raw[18] & 0x44 == 0x44:
            require(raw[18] & 0x20, 'XA audio must use Form2')
            coding = raw[19]
            require(all(((coding >> shift) & 3) in (0, 1) for shift in (0, 2, 4)), 'unsupported XA coding')
            streams[(raw[16], raw[17], coding)] += 1
            continue
        header = raw[24:56]
        magic, kind, chunk, chunks, frame, size, width, height = struct.unpack_from('<HHHHIIHH', header)
        if magic == 0 and kind == 0:
            require(not any(raw[24:2072]), 'unrecognized nonempty data sector')
            padding += 1
            continue
        require(magic == 0x160, 'invalid STR magic')
        types[f'{kind:04x}'] += 1
        # Retail subtitle/font packet: not a video frame and not arbitrary padding.
        if kind != 0x8001:
            require(kind == 1 and not frames and active is None, 'unexpected nonvideo STR packet')
            require(chunk < chunks and 0 < size <= chunks * 2016, 'invalid font packet')
            continue
        require(0 < width <= 1024 and 0 < height <= 512, 'invalid dimensions')
        require(0 < chunks <= 32 and 0 <= chunk < chunks, 'invalid chunk count')
        # STR allocates fixed-rate chunk slots, including whole padding sectors.
        require(0 < size <= chunks * 2016, 'inconsistent encoded frame length')
        if active is None:
            require(chunk == 0 and frame == len(frames) + 1, 'missing/duplicate/out-of-order frame')
            active = dict(frame=frame, chunks=chunks, size=size, width=width, height=height,
                          next_chunk=0, data=bytearray())
        require(all(active[k] == v for k, v in dict(frame=frame, chunks=chunks, size=size, width=width, height=height).items()),
                'frame metadata changed between chunks')
        require(chunk == active['next_chunk'], 'missing/duplicate/out-of-order chunk')
        active['next_chunk'] += 1
        active['data'].extend(raw[56:2072])
        if active['next_chunk'] == chunks:
            payload = bytes(active['data'][:size])
            frames.append(dict(frame=frame, chunks=chunks, encoded_bytes=size,
                               width=width, height=height, sha256=hashlib.sha256(payload).hexdigest(),
                               extent_sector=n - 1, preceding_xa_sectors=sum(streams.values())))
            active = None
    require(n == entry['sectors'], 'truncated/extended movie extent')
    require(active is None, 'incomplete final video frame')
    require(len(frames) == entry['table_frames'], 'movie table and complete frame count disagree')
    audio = []
    total_pcm = 0
    for (file, channel, coding), count in sorted(streams.items()):
        channels = 2 if coding & 3 == 1 else 1
        bits = 8 if (coding >> 4) & 3 == 1 else 4
        rate = 18900 if (coding >> 2) & 3 == 1 else 37800
        pcm = count * (2016 if bits == 8 else 4032) // channels * 44100 // rate
        total_pcm += pcm
        audio.append(dict(file=file, channel=channel, coding=coding, channels=channels,
                          bits=bits, rate=rate, sectors=count, stereo_frames=pcm))
    return dict(entry, status='complete', sha256=extent_hash.hexdigest(), encoded_frames=len(frames),
                standard_actor_playable_frames=entry['table_frames'] - 2,
                dimensions=[dict(width=w, height=h, frames=n) for (w, h), n in sorted(Counter((r['width'], r['height']) for r in frames).items())],
                data_types=dict(types), padding_sectors=padding, audio=audio, stereo_frames=total_pcm,
                drive_seconds_2x=entry['sectors'] / 150, frames=frames)


def catalog(path):
    disc = Disc(path)
    archive = disc.files()['/MGS/ZMOVIE.STR']
    entries = movie_index(disc.read(archive['lba'], 2048), archive['lba'],
                          (archive['size'] + 2047) // 2048, path.stat().st_size // SECTOR)
    movies = []
    with path.open('rb') as source:
        for entry in entries:
            source.seek(entry['start_lba'] * SECTOR)
            movies.append(scan_movie((source.read(SECTOR) for _ in range(entry['sectors'])), entry))
    return dict(disc=dict(path=str(path.resolve()), sha256=sha256(path)), archive=archive, movies=movies)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('discs', type=Path, nargs='+')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    require(not args.output.exists(), 'refusing to overwrite catalog evidence')
    discs = [catalog(p.resolve()) for p in args.discs]
    movies = [m for d in discs for m in d['movies']]
    report = dict(format='mgs-movie-catalog-1', status='complete', discs=discs,
                  counts=dict(discs=len(discs), movies=len(movies), encoded_frames=sum(m['encoded_frames'] for m in movies),
                              xa_sectors=sum(a['sectors'] for m in movies for a in m['audio']),
                              stereo_frames=sum(m['stereo_frames'] for m in movies)),
                  scope='Complete source-indexed STR structure and XA duration inventory. Standard movie.c excludes the last two video frames; runtime coverage and PCM decoding are separate obligations.',
                  source='Foxdie libfs/movie.c FS_MOVIE_FILE index; game/movie.c frame-2 cutoff; raw retail STR chunk headers.')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_json(args.output, report)
    print(json.dumps(report['counts']))


if __name__ == '__main__':
    main()
