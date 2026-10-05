#!/usr/bin/env python3
"""Read-only HZD/GCL extraction for diagnosing a recorded campaign route."""
import argparse
import hashlib
import html
import json
from pathlib import Path
import re

from mgs_assets import Disc, stages, stage_members, unpack, span


def hazard(data):
    header = unpack('<8h3I', data)
    groups, routes = [], []
    for i in range(header[5]):
        nt, nw, nf, flat, walls, floors, traps, flags = unpack('<4h4I', data, header[8] + i * 24)
        group = {'walls': [], 'floors': [], 'traps': []}
        for j in range(nw):
            group['walls'].append(list(unpack('<8h', data, walls + j * 16)))
        for j in range(nf):
            group['floors'].append(list(unpack('<24h', data, floors + j * 48)))
        for j in range(nt):
            raw = span(data, traps + j * 32, 32)
            if raw[29] == 255:  # cameras follow named triggers
                break
            group['traps'].append({'bounds': list(unpack('<8h', raw)),
                                   'name': raw[16:28].split(b'\0')[0].decode('ascii', errors='replace'),
                                   'ids': list(raw[28:30]), 'name_hash': unpack('<H', raw, 30)[0]})
        groups.append(group)
    for i in range(header[7]):
        count, initial, pointer = unpack('<hhI', data, header[10] + i * 8)
        routes.append({'initial': initial, 'points': [list(unpack('<4h', data, pointer + j * 8))
                                                     for j in range(count)]})
    return {'sha256': hashlib.sha256(data).hexdigest(), 'groups': groups, 'routes': routes}


def draw(hzd, height, trap_pattern):
    walls = [w for g in hzd['groups'] for w in g['walls'] if min(w[2], w[6]) <= height]
    xs = [w[k] for w in walls for k in (0, 4)]
    zs = [w[k] for w in walls for k in (1, 5)]
    if not walls:
        return ''
    x0, z0, width, depth = min(xs)-500, min(zs)-500, max(xs)-min(xs)+1000, max(zs)-min(zs)+1000
    unit = max(width, depth) / 1000
    lines = [f'<svg xmlns="http://www.w3.org/2000/svg" width="1000" height="1000" viewBox="{x0} {z0} {width} {depth}">',
             f'<rect x="{x0}" y="{z0}" width="{width}" height="{depth}" fill="white"/>']
    for w in walls:
        lines.append(f'<line x1="{w[0]}" y1="{w[1]}" x2="{w[4]}" y2="{w[5]}" stroke="#333" stroke-width="{unit*2}"/>')
    for group in hzd['groups']:
        for trap in group['traps']:
            if not re.search(trap_pattern, trap['name']):
                continue
            x,z,y,_,xx,zz,yy,_ = trap['bounds']
            lines.append(f'<rect x="{x}" y="{z}" width="{xx-x}" height="{zz-z}" fill="none" stroke="blue" stroke-width="{unit}"/>')
            lines.append(f'<text x="{x}" y="{z}" font-size="{unit*10}">{html.escape(trap["name"])} y{y}:{yy}</text>')
    for x in range((x0//5000)*5000, x0+width, 5000):
        lines.append(f'<text x="{x}" y="{z0+depth-100}" font-size="{unit*12}">{x}</text>')
    return '\n'.join(lines + ['</svg>'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disc', type=Path)
    parser.add_argument('stage')
    parser.add_argument('output', type=Path)
    parser.add_argument('--height', type=int, default=1200)
    parser.add_argument('--traps', default='(?i)move|lift|duct|exit')
    args = parser.parse_args()
    disc = Disc(args.disc)
    record = disc.files()['/MGS/STAGE.DIR']
    archive = disc.read(record['lba'], record['size'])
    stage = next(data for name, _, data in stages(archive) if name == args.stage)
    args.output.mkdir(parents=True, exist_ok=True)
    for member, data in stage_members(stage):
        extension = member['extension']
        if extension not in ('h', 'g'):
            continue
        identity = str(member['id'])
        path = args.output / f'{identity}.{extension}'
        path.write_bytes(data)
        if extension == 'h':
            hzd = hazard(data)
            path.with_suffix('.hzd.json').write_text(json.dumps(hzd, indent=2)+'\n')
            drawing = draw(hzd, args.height, args.traps)
            if drawing:
                path.with_suffix('.svg').write_text(drawing)
        print(path)


if __name__ == '__main__':
    main()
