#!/usr/bin/env python3
"""Check full-movie playback totals against retail STR data, not observed targets.

This certifies aggregate decode/mix counts for a declared movie sequence. It
does not identify displayed frames, prove AV sync or certify Vulkan visuals.
"""
import argparse
import json
from pathlib import Path

from mgs_scenarios import compare
from mgs_tas import sha256, write_json


def fingerprint(path):
    return dict(path=str(path.resolve()), sha256=sha256(path))


def expected(catalog, disc_sha256, codes):
    if catalog.get('format') != 'mgs-movie-catalog-1' or catalog.get('status') != 'complete' or not codes:
        raise ValueError('complete catalog and nonempty declared movie sequence required')
    discs = [d for d in catalog['discs'] if d['disc']['sha256'] == disc_sha256]
    if len(discs) != 1:
        raise ValueError('runtime disc does not uniquely match catalog')
    movies = {m['code']: m for m in discs[0]['movies']}
    result = dict(frames=0, macroblocks=0, xa_min=0, xa_max=0, pcm_per_sector=2352)
    for code in codes:
        if code not in movies:
            raise ValueError(f'unknown movie: {code}')
        movie = movies[code]
        if movie['table_frames'] != len(movie['frames']) or len(movie['audio']) != 1 or movie['audio'][0]['coding'] != 1:
            raise ValueError('requires complete movie frames and single-stream stereo 37800-Hz 4-bit XA')
        played = movie['frames'][:-2]  # Both movie.c and ending2.c stop at frame < total - 1.
        if not played:
            raise ValueError('no playable source frames')
        result['frames'] += len(played)
        result['macroblocks'] += sum(((f['width'] + 15) // 16) * ((f['height'] + 15) // 16) for f in played)
        # To submit the last frame the CD must have crossed these audio sectors.
        # Buffered read-ahead may reach the end of the allocated extent.
        result['xa_min'] += played[-1]['preceding_xa_sectors']
        result['xa_max'] += movie['audio'][0]['sectors']
    return result


def assess(target, metrics):
    counters = metrics.get('counters', {})
    checks = []
    def check(name, lo, hi, absent_zero=False):
        actual = counters.get(name, 0 if absent_zero else None)
        passed = type(actual) is int and lo <= actual <= hi
        checks.append(dict(counter=name, min=lo, max=hi, actual=actual, passed=passed))
        return actual if type(actual) is int else -1
    check('mdec.decoded_frame', target['frames'], target['frames'])
    check('mdec.decoded_macroblock', target['macroblocks'], target['macroblocks'])
    # Presence distinguishes the stricter decoder from historical telemetry.
    check('mdec.decode_command', target['frames'], target['frames'])
    check('mdec.truncated_command', 0, 0, True)
    check('cdrom.discarded_clock_cycles', 0, 0, True)
    xa = check('cdrom.xa_decoded_sector', target['xa_min'], target['xa_max'])
    pcm = xa * target['pcm_per_sector']
    check('cdrom.xa_output_frame', pcm, pcm)
    check('spu.xa_consumed_frame', pcm, pcm)
    check('spu.cd_nonzero_frame', 1, max(pcm, 1))
    return dict(passed=all(c['passed'] for c in checks), expected=target, checks=checks)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('native', type=Path)
    parser.add_argument('donor', type=Path)
    parser.add_argument('--catalog', type=Path, required=True)
    parser.add_argument('--movies', nargs='+', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('refusing to overwrite evidence')
    native, donor, catalog = [json.loads(p.read_text()) for p in (args.native, args.donor, args.catalog)]
    comparison = compare(native, donor)
    session = Path(native['session'])
    session_result = json.loads((session / 'result.json').read_text())
    target = expected(catalog, native['binaries']['track_0']['sha256'], args.movies)
    metrics = session / 'metrics.json'
    checked = assess(target, json.loads(metrics.read_text()))
    passed = comparison['status'] == 'complete' and session_result['status'] == 'complete' and checked['passed']
    report = dict(format='mgs-movie-playback-check-1', status='complete' if passed else 'needs-review',
        movies=args.movies, comparison=comparison, media=checked,
        inputs=[fingerprint(p) for p in (args.native, args.donor, args.catalog, metrics, session / 'result.json')],
        runtime=native['binaries']['runtime'], scope=__doc__.strip(),
        completed_frames=dict(native=session_result['completed_frames'],
            donor=json.loads((Path(donor['session']) / 'result.json').read_text())['frames']))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_json(args.output, report)
    print(json.dumps(dict(status=report['status'], movies=args.movies, expected=target)))
    return 0 if passed else 2


if __name__ == '__main__':
    raise SystemExit(main())
