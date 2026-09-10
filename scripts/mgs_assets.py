#!/usr/bin/env python3
"""Read-only PSX MGS archive/geometry audit. See docs/mgs-3d-formats.md."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct


class FormatError(ValueError):
    pass


def require(ok, message):
    if not ok:
        raise FormatError(message)


def span(data, offset, size):
    require(0 <= offset <= len(data) and 0 <= size <= len(data) - offset,
            f'range {offset:#x}+{size:#x} exceeds {len(data):#x}')
    return data[offset:offset + size]


def unpack(fmt, data, offset=0):
    return struct.unpack(fmt, span(data, offset, struct.calcsize(fmt)))


def sha(data):
    return hashlib.sha256(data).hexdigest()


def align(value):
    return (value + 2047) & ~2047


class Disc:
    """ISO9660 directories over raw MODE2/Form1 sectors, no image writes."""
    def __init__(self, path):
        self.path = Path(path)

    def read(self, lba, size):
        require(lba >= 0 and size >= 0, 'negative disc extent')
        result = bytearray()
        with self.path.open('rb') as stream:
            stream.seek(lba * 2352)
            for _ in range((size + 2047) // 2048):
                sector = stream.read(2352)
                require(len(sector) == 2352 and sector[:12] == b'\x00' + b'\xff' * 10 + b'\x00', 'bad raw sector')
                require(sector[15] == 2 and not sector[18] & 0x20, 'expected MODE2/Form1 sector')
                result.extend(sector[24:2072])
        return bytes(result[:size])

    def files(self):
        pvd = self.read(16, 2048)
        require(pvd[:7] == b'\x01CD001\x01', 'missing ISO9660 primary descriptor')
        found = {}
        visited = set()

        def directory(lba, size, prefix):
            require((lba, size) not in visited, 'directory cycle')
            visited.add((lba, size))
            data = self.read(lba, size)
            offset = 0
            while offset < len(data):
                length = data[offset]
                if not length:
                    offset = align(offset + 1)
                    continue
                row = span(data, offset, length)
                require(length >= 34, 'short directory record')
                name = span(row, 33, row[32])
                if name not in (b'\0', b'\1'):
                    name = name.decode('ascii').split(';')[0]
                    require('/' not in name and name not in ('.', '..'), 'unsafe ISO name')
                    path = prefix + '/' + name
                    extent, = unpack('<I', row, 2)
                    count, = unpack('<I', row, 10)
                    if row[25] & 2:
                        directory(extent, count, path)
                    else:
                        require(path not in found, 'duplicate ISO file')
                        found[path] = {'lba': extent, 'size': count}
                offset += length

        directory(unpack('<I', pvd, 158)[0], unpack('<I', pvd, 166)[0], '')
        return found


def stages(data):
    table_size, = unpack('<I', data)
    require(table_size % 12 == 0 and table_size > 0, 'invalid stage table size')
    span(data, 4, table_size)
    entries = []
    for offset in range(4, 4 + table_size, 12):
        name, sector = unpack('<8sI', data, offset)
        name = name.split(b'\0')[0].decode('ascii')
        require(re.fullmatch(r'[A-Za-z0-9_]+', name), 'unsafe stage name')
        entries.append((name, sector * 2048))
    require(len(set(n for n, _ in entries)) == len(entries), 'duplicate stage name')
    for i, (name, start) in enumerate(entries):
        end = entries[i + 1][1] if i + 1 < len(entries) else len(data)
        require(start >= align(4 + table_size) and end > start, 'unordered stage extents')
        yield name, start, span(data, start, end - start)


def stage_members(data):
    version, padding, declared_sectors = unpack('<BBH', data)
    require(version == 1 and padding == 0, 'unsupported stage config version/padding')
    cursor = 2048
    span(data, 0, cursor)
    tags = []
    for pos in range(4, cursor - 7, 8):
        tag = unpack('<HBBI', data, pos)
        if tag[1] == 0:
            break
        require(tag[1] in b'ncrs', f'unknown stage mode {tag[1]:#x}')
        tags.append(tag)
    else:
        raise FormatError('unterminated stage config')
    cache_start = None
    for i, (identity, mode, ext, value) in enumerate(tags):
        if mode == ord('c'):
            if cache_start is None:
                require(value == 0, 'cache group must start at offset zero')
                cache_start = cursor
            if ext == 255:
                require(value >= cursor - cache_start, 'backwards cache end')
                span(data, cache_start, value)
                cursor = align(cache_start + value)
                cache_start = None
                continue
            require(i + 1 < len(tags) and tags[i + 1][1] == mode, 'unterminated cached member')
            require(value == cursor - cache_start, 'noncontiguous cache member')
            size = tags[i + 1][3] - value
            start = cache_start + value
            cursor = start + size
        else:
            require(cache_start is None, 'cache group missing end marker')
            start, size = cursor, value
            cursor = align(cursor + value)
        payload = span(data, start, size)
        yield {'config_index': i, 'id': f'{identity:04x}', 'extension': chr(ext),
               'mode': chr(mode), 'offset': start, 'size': size}, payload
    require(cache_start is None, 'open cached section')
    require(cursor <= len(data), 'stage payload exceeds extent')


def dar_members(data):
    offset = 0
    while offset < len(data):
        identity, extension, size = unpack('<HHI', data, offset)
        require(32 <= extension < 127, f'invalid DAR extension {extension:#x}')
        yield {'id': f'{identity:04x}', 'extension': chr(extension),
               'offset': offset + 8, 'size': size}, span(data, offset + 8, size)
        offset += 8 + size


def kmd(data, geometry=False, header_offset=0, pointer_base=0):
    primary, total = unpack('<II', data, header_offset)
    require(0 < primary <= total <= 4096, 'invalid model counts')
    span(data, header_offset + 32, total * 88)
    models = []
    all_textures = set()
    for i in range(total):
        h = unpack('<22i', data, header_offset + 32 + i * 88)
        flags, faces = h[:2]
        parent, extend, nv, vo, vi, nn, no, ni, uv, tex, pad = h[11:]
        vo += pointer_base
        vi, no, ni, uv, tex = [v + pointer_base if v else 0 for v in (vi, no, ni, uv, tex)]
        require(parent == -1 or 0 <= parent < total and parent != i, 'invalid parent')
        require(extend == -1 or 0 <= extend < total and extend != i, 'invalid extension')
        require(0 <= faces <= 65535 and 0 <= nv <= 256 and 0 <= nn <= 256, 'invalid geometry counts')
        for table_offset, count in ((vo, nv), (vi, faces), (no, nn),
                                    (ni, faces if ni else 0),
                                    (uv, faces if uv else 0), (tex, faces if tex else 0)):
            require(not count or table_offset >= header_offset + 32 + total * 88,
                    'geometry table overlaps model header')
        vertices = list(struct.iter_unpack('<4h', span(data, vo, nv * 8)))
        indices = list(span(data, vi, faces * 4))
        normals = list(struct.iter_unpack('<4h', span(data, no, nn * 8)))
        nindices = list(span(data, ni, faces * 4)) if ni else []
        coords = list(struct.iter_unpack('<2B', span(data, uv, faces * 8))) if uv else []
        textures = [v[0] for v in struct.iter_unpack('<H', span(data, tex, faces * 2))] if tex else []
        require(all(v < nv for v in indices), 'vertex index outside model')
        require(all((v & 127) < nn for v in nindices), 'normal index outside model')
        for vertex in vertices:
            require(vertex[3] == -1 or parent >= 0, 'seam vertex without parent')
            if vertex[3] != -1:
                pnv, = unpack('<i', data, header_offset + 32 + parent * 88 + 52)
                require(0 <= vertex[3] < pnv, 'seam parent vertex outside model')
        all_textures.update(textures)
        model = {'index': i, 'flags': f'0x{flags & 0xffffffff:08x}', 'faces': faces,
                 'vertices': nv, 'normals': nn, 'parent': parent, 'extend': extend,
                 'translation': h[8:11], 'bounds_raw': h[2:8],
                 'seam_vertices': sum(v[3] != -1 for v in vertices),
                 'degenerate_last_corners': sum(indices[j+2] == indices[j+3] for j in range(0, len(indices), 4)),
                 'texture_ids': [f'{t:04x}' for t in sorted(set(textures))]}
        if geometry:
            model.update(positions_i16=vertices, corner_vertex_indices=indices,
                         normals_i16=normals, corner_normal_indices_raw=nindices,
                         corner_uv_u8=coords, face_texture_ids=textures)
        models.append(model)
    for field in ('parent', 'extend'):
        for index in range(total):
            seen = set()
            while index != -1:
                require(index not in seen, f'{field} cycle')
                seen.add(index)
                index = models[index][field]
    return {'primary_models': primary, 'total_models': total,
            'bounds_raw': unpack('<6i', data, header_offset + 8),
            'source_to_psx_packet_corner_order': [0, 1, 3, 2], 'models': models,
            'texture_ids': [f'{t:04x}' for t in sorted(all_textures)]}


def zmd(data, geometry=False):
    magic, count, vertex_offset, body_size = unpack('<4I', data)
    require(magic == 0x61444d4b, 'invalid ZMD magic')
    require(0 < count <= 4096, 'invalid ZMD count')
    shared = 16 + vertex_offset
    span(data, shared, body_size)
    cursor = 16
    models = []
    for _ in range(count):
        identity, = unpack('<I', data, cursor)
        parsed = kmd(data, geometry=geometry, header_offset=cursor + 4, pointer_base=shared)
        models.append({'id': f'{identity:04x}', 'parsed': parsed})
        cursor += 4 + 32 + parsed['total_models'] * 88
    require(cursor <= shared, 'ZMD headers overlap shared geometry')
    return {'count': count, 'shared_data_offset': shared, 'models': models}


def lit(data):
    count, = unpack('<I', data)
    span(data, 4, count * 16)
    return {'lights': count, 'validation': 'record extents only'}


PLANE_LUT = [[sum(((value >> (7 - bit)) & 1) << (bit * 8 + plane)
                   for bit in range(8)) for value in range(256)] for plane in range(4)]

def pcx(data, pixels_out=False):
    span(data, 0, 128)
    require(data[0] == 10 and data[2] == 1, 'unsupported PCX header')
    xmin, ymin, xmax, ymax = unpack('<4H', data, 4)
    stamp, flags, px, py, cx, cy, colors = unpack('<7H', data, 74)
    require(xmax >= xmin and ymax >= ymin, 'invalid PCX dimensions')
    require(stamp == 12345, 'missing MGS PCX metadata')
    width, height = xmax-xmin+1, ymax-ymin+1
    stride = unpack('<H', data, 66)[0]
    require(width <= 4096 and height <= 4096, 'oversized PCX')
    eight_bit = bool(flags & 1)
    require((data[3], data[65]) == ((8, 1) if eight_bit else (1, 4)), 'PCX flags/planes disagree')
    require(0 < colors <= (256 if eight_bit else 16), 'invalid palette size')
    expected = width * height if eight_bit else stride * 4 * height
    require(0 < stride <= 4096 and (eight_bit or stride * 8 >= width), 'invalid PCX stride')
    decoded = bytearray()
    cursor = 128
    while len(decoded) < expected:
        code = span(data, cursor, 1)[0]
        cursor += 1
        if code > 192:  # game loader treats 0xc0 itself as a literal
            count = code - 192
            value = span(data, cursor, 1)[0]
            cursor += 1
        else:
            count, value = 1, code
        require(len(decoded) + count <= expected, 'PCX run exceeds decoded size')
        decoded.extend([value] * count)
    if eight_bit:
        require(span(data, cursor, 1) == b'\x0c', 'missing 8-bit PCX palette marker')
        palette = span(data, cursor + 1, colors * 3)
        pixels = decoded
    else:
        palette = span(data, 16, colors * 3)
        pixels = bytearray()
        for y in range(height):
            row = y * stride * 4
            line = bytearray()
            for x in range((width + 7) // 8):
                value = (PLANE_LUT[0][decoded[row + x]] |
                         PLANE_LUT[1][decoded[row + stride + x]] |
                         PLANE_LUT[2][decoded[row + stride * 2 + x]] |
                         PLANE_LUT[3][decoded[row + stride * 3 + x]])
                line.extend(value.to_bytes(8, 'little'))
            pixels.extend(line[:width])
    # RGB low bits carry PS1 STP semantics; retain originals separately.
    palette_words = []
    for r, g, b in struct.iter_unpack('3B', palette):
        word = ((r >> 3) | (g >> 3) << 5 | (b >> 3) << 10 | bool((r | g | b) & 7) << 15) if r or g or b else 0
        palette_words.append(word)
    result = {'width': width, 'height': height,
            'bits_per_plane': data[3], 'planes': data[65],
            'bytes_per_line': stride, 'flags': flags,
            'texture_vram': [px, py], 'clut_vram': [cx, cy], 'colors': colors,
            'indices_sha256': sha(pixels), 'palette_rgb_sha256': sha(palette),
            'palette_ps1_sha256': sha(struct.pack('<' + 'H' * colors, *palette_words)),
            'indices_outside_declared_palette': len(pixels.translate(None, bytes(range(colors)))),
            'validation': 'RLE and palette decoded; dynamic CLUT state not resolved'}
    if pixels_out:
        result.update(pixels=bytes(pixels), palette_rgb=palette, palette_words=palette_words)
    return result


def oar(data):
    magic, joints, motions, words = unpack('<4I', data)
    require(magic == 0x6152414f, 'invalid OAR magic')
    require(0 < joints <= 4096 and 0 < motions <= 65535, 'invalid OAR counts')
    table_words = (joints + 2) * motions
    span(data, 16, table_words * 2)
    archive_start = 16 + table_words * 2
    rows = []
    for i in range(motions):
        row = unpack('<' + 'H' * (joints + 2), data, 16 + i * (joints + 2) * 2)
        for offset in row[1:]:
            require(archive_start + offset * 2 < len(data), 'OAR stream offset out of bounds')
        rows.append(row[0])
    return {'joints': joints, 'motions': motions, 'declared_archive_words': words,
            'archive_offset': archive_start, 'frame_counts': rows,
            'validation': 'header/table only; animation bitstreams not decoded'}


def audit(disc_path, out):
    out.mkdir(parents=True, exist_ok=True)
    disc = Disc(disc_path)
    files = disc.files()
    stage_file = files['/MGS/STAGE.DIR']
    data = disc.read(stage_file['lba'], stage_file['size'])
    (out / 'STAGE.DIR').write_bytes(data)
    exe_paths = [p for p in files if re.fullmatch(r'/MGS/(?:MGS[12]\.EXE|SLUS_00[57]\.(?:94|76))', p)]
    require(len(exe_paths) == 1, 'expected one internal game executable')
    exe_path = exe_paths[0]
    exe_file = files[exe_path]
    exe = disc.read(exe_file['lba'], exe_file['size'])
    (out / Path(exe_path).name).write_bytes(exe)
    records, errors, stage_rows = [], [], []
    parsed_cache = {}
    parsers = {'k': kmd, 'p': pcx, 'o': oar, 'z': zmd, 'l': lit}
    for name, base, stage in stages(data):
        start_count = len(records)
        stage_rows.append({'name': name, 'offset': base, 'size': len(stage),
                           'header': unpack('<HH', stage)})
        for member, payload in stage_members(stage):
            children = dar_members(payload) if member['extension'] == 'd' else [(None, payload)]
            for child, content in children:
                meta = child or member
                offset = base + member['offset'] + (child['offset'] if child else 0)
                record = {'stage': name, 'config_index': member['config_index'],
                          'mode': member['mode'], 'container_id': member['id'] if child else None,
                          'id': meta['id'], 'extension': meta['extension'],
                          'stage_dir_offset': offset, 'size': len(content), 'sha256': sha(content)}
                if record['extension'] in parsers:
                    try:
                        key = (record['extension'], record['sha256'])
                        if key not in parsed_cache:
                            parsed_cache[key] = parsers[record['extension']](content)
                        record['parsed'] = parsed_cache[key]
                    except (FormatError, struct.error) as error:
                        record['error'] = str(error)
                        errors.append({k: record[k] for k in ('stage', 'id', 'extension', 'error')})
                records.append(record)
        stage_rows[-1]['resources'] = len(records) - start_count
    summary = {'stage_count': len(stage_rows), 'resource_count': len(records),
               'counts': dict(sorted(Counter(r['extension'] for r in records).items())),
               'unique_by_type': {ext: len({r['sha256'] for r in records if r['extension'] == ext})
                                  for ext in sorted({r['extension'] for r in records})},
               'parse_errors': errors}
    report = {'schema': 'mgs-assets-audit-v1', 'stage_dir_sha256': sha(data),
              'main_executable': {'path': exe_path, 'size': len(exe), 'sha256': sha(exe),
                                  'magic': exe[:8].decode('ascii', errors='replace'),
                                  'entry_pc': f'0x{unpack("<I", exe, 16)[0]:08x}'},
              'iso_files': files, 'summary': summary, 'stages': stage_rows, 'resources': records}
    (out / 'inventory.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    return bool(errors)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subs = parser.add_subparsers(dest='command', required=True)
    a = subs.add_parser('audit')
    a.add_argument('--disc', type=Path, required=True)
    a.add_argument('--out', type=Path, required=True)
    k = subs.add_parser('kmd')
    k.add_argument('input', type=Path)
    k.add_argument('--out', type=Path, required=True)
    e = subs.add_parser('extract')
    e.add_argument('--inventory', type=Path, required=True)
    e.add_argument('--stage', required=True)
    e.add_argument('--id', required=True)
    e.add_argument('--type', required=True, help='One-letter resource type, e.g. k')
    e.add_argument('--config-index', type=int)
    e.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'extract':
        report = json.loads(args.inventory.read_text())
        candidates = [r for r in report['resources'] if r['stage'] == args.stage
                      and int(r['id'], 16) == int(args.id, 16) and r['extension'] == args.type
                      and (args.config_index is None or r['config_index'] == args.config_index)]
        require(len(candidates) == 1, 'resource absent or ambiguous; specify config index')
        record = candidates[0]
        archive = args.inventory.parent / 'STAGE.DIR'
        with archive.open('rb') as stream:
            stream.seek(record['stage_dir_offset'])
            payload = stream.read(record['size'])
        require(len(payload) == record['size'] and sha(payload) == record['sha256'],
                'archive does not match inventory')
        args.out.write_bytes(payload)
        return 0
    if args.command == 'audit':
        return int(audit(args.disc, args.out))
    args.out.write_text(json.dumps(kmd(args.input.read_bytes(), geometry=True), indent=2) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
