#!/usr/bin/env python3
"""Build immutable GPU storage from original USA discs; never select by hash alone."""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct
import zlib
from mgs_assets import Disc, stages, stage_members, dar_members, kmd, zmd, pcx, sha, require

MAGIC = b'MGSAC001'
SECTIONS = [('ASST', 64), ('MODL', 32), ('VRTX', 48), ('TEXR', 32),
            ('TEXI', 1), ('PLTE', 4), ('SRCE', 1)]


def pack_sections(sections):
    """16-byte header, 7x16-byte directory, 16-byte-aligned sections, CRC32."""
    cursor = 16 + len(SECTIONS) * 16
    directory = bytearray()
    body = bytearray()
    for name, stride in SECTIONS:
        content = sections[name]
        require(len(content) % stride == 0, 'section stride mismatch')
        pad = (-cursor) % 16
        body.extend(b'\0' * pad)
        cursor += pad
        directory.extend(struct.pack('<4sIII', name.encode(), cursor, len(content) // stride, stride))
        body.extend(content)
        cursor += len(content)
    payload = directory + body
    return struct.pack('<8sII', MAGIC, cursor, zlib.crc32(payload)) + payload


def build(discs, output):
    sections = {name: bytearray() for name, _ in SECTIONS}
    assets, occurrences, disc_rows, asset_by_key = [], [], [], {}
    source_pool, pixel_pool, palette_pool = {}, {}, {}
    errors = []
    ignored = Counter()
    def add_asset(ext, data, context, parsed=None, embedded_id=None):
        digest = sha(data)
        # Embedded ZMD models share the container bytes but have distinct IDs.
        key = (ext, digest, embedded_id)
        if key not in asset_by_key:
            index = len(assets)
            asset_by_key[key] = index
            first_model = len(sections['MODL']) // 32
            texture_index = 0xffffffff
            parsed = parsed if parsed is not None else (kmd(data, True) if ext == 'k' else pcx(data, True))
            if digest not in source_pool:
                source_pool[digest] = len(sections['SRCE'])
                sections['SRCE'].extend(data)
            source_offset = source_pool[digest]
            if ext == 'k':
                for model in parsed['models']:
                    corners = model['corner_vertex_indices']
                    first_corner = len(sections['VRTX']) // 48
                    parent = model['parent']
                    for corner, vi in enumerate(corners):
                        position = model['positions_i16'][vi]
                        ni = model['corner_normal_indices_raw'][corner] if model['corner_normal_indices_raw'] else 0
                        normal = model['normals_i16'][ni & 127][:3] if model['normals_i16'] and model['corner_normal_indices_raw'] else (0, 0, 0)
                        uv = model['corner_uv_u8'][corner] if model['corner_uv_u8'] else (0, 0)
                        texture = model['face_texture_ids'][corner // 4] if model['face_texture_ids'] else 0xffffffff
                        sections['VRTX'].extend(struct.pack('<8f4I', *position, *(v / 4096 for v in normal), ni >> 7,
                            uv[0] | uv[1] << 8, texture, first_model + model['index'],
                            first_model + parent if parent >= 0 else 0xffffffff))
                    # A parent seam references a SOURCE vertex, not an expanded
                    # corner. Original indexed streams remain available in SRCE.
                    sections['MODL'].extend(struct.pack('<8I', int(model['flags'], 16),
                        first_model + parent if parent >= 0 else 0xffffffff,
                        first_model + model['extend'] if model['extend'] >= 0 else 0xffffffff,
                        first_corner, len(corners), model['vertices'], index,
                        int(model['index'] < parsed['primary_models'])))
                total = parsed['total_models']
                primary = parsed['primary_models']
            else:
                total = primary = 0
                texture_index = len(sections['TEXR']) // 32
                pixels = parsed['pixels']
                pixel_hash = sha(pixels)
                if pixel_hash not in pixel_pool:
                    pixel_pool[pixel_hash] = len(sections['TEXI'])
                    sections['TEXI'].extend(pixels)
                first_pixel = pixel_pool[pixel_hash]
                # Keep STP and zero-transparent semantics in the native palette.
                palette = parsed['palette_words']
                palette_bytes = struct.pack('<' + 'I' * len(palette), *palette)
                palette_hash = sha(palette_bytes)
                if palette_hash not in palette_pool:
                    palette_pool[palette_hash] = len(sections['PLTE']) // 4
                    sections['PLTE'].extend(palette_bytes)
                first_palette = palette_pool[palette_hash]
                sections['TEXR'].extend(struct.pack('<8I', parsed['width'], parsed['height'],
                    first_pixel, len(pixels), first_palette, len(palette), parsed['flags'], index))
            sections['ASST'].extend(bytes.fromhex(digest) + struct.pack('<8I', ord(ext),
                first_model, total, texture_index, source_offset, len(data), primary, embedded_id or 0))
            assets.append({'index': index, 'type': ext, 'sha256': digest,
                           'embedded_id': embedded_id, 'first_model': first_model,
                           'models': total, 'texture_index': texture_index if ext == 'p' else None,
                           'faces': sum(m['faces'] for m in parsed['models']) if ext == 'k' else 0})
        occurrences.append(dict(context, asset=asset_by_key[key], embedded_id=embedded_id))
    for number, path in enumerate(discs, 1):
        disc = Disc(path)
        files = disc.files()
        exe_name = '/MGS/SLUS_005.94' if number == 1 else '/MGS/SLUS_007.76'
        require(exe_name in files, 'expected USA discs in disc 1, disc 2 order')
        exe = files[exe_name]
        require(sha(disc.read(exe['lba'], exe['size'])) ==
                '3342f75717c5134aec832547691c2354e67a253abfcfa9e0e823e2133bb1fc32', 'unsupported executable')
        stage_file = files['/MGS/STAGE.DIR']
        archive = disc.read(stage_file['lba'], stage_file['size'])
        stage_count = 0
        for stage_name, base, stage in stages(archive):
            stage_count += 1
            for member, payload in stage_members(stage):
                children = dar_members(payload) if member['extension'] == 'd' else [(None, payload)]
                for child, data in children:
                    meta = child or member
                    ext = meta['extension']
                    if ext not in ('k', 'z', 'p'):
                        ignored[ext] += 1
                        continue
                    context = dict(disc=number, stage=stage_name, id=meta['id'], type=ext,
                        config_index=member['config_index'], mode=member['mode'],
                        container_id=member['id'] if child else None,
                        stage_dir_offset=base + member['offset'] + (child['offset'] if child else 0))
                    try:
                        if ext == 'z':
                            for model in zmd(data, True)['models']:
                                add_asset('k', data, context, model['parsed'], int(model['id'], 16))
                        else:
                            add_asset(ext, data, context)
                    except (ValueError, struct.error) as e:
                        errors.append(dict(context, error=str(e)))
        disc_rows.append(dict(disc=number, image=str(Path(path).resolve()), stages=stage_count,
                              stage_dir_sha256=sha(archive)))
    require(not errors, f'asset validation failed: {errors[:5]}')
    binary = pack_sections(sections)
    manifest = dict(schema='mgs-asset-cache/v1', discs=disc_rows, assets=assets,
        occurrences=occurrences, sections={name: dict(count=len(sections[name]) // stride, stride=stride)
                                         for name, stride in SECTIONS},
        binary_sha256=sha(binary), binary_size=len(binary), errors=errors,
        other_resource_occurrences=dict(ignored),
        lod_policy='retain every distinct source variant; no unverified cross-asset replacement',
        lod_families=[],
        runtime_status='Immutable GPU storage; optional native-scene bridge replaces certified geometry, retaining runtime materials')
    output.mkdir(parents=True, exist_ok=True)
    temp = output / 'assets.mgsc.tmp'
    temp.write_bytes(binary)
    temp.replace(output / 'assets.mgsc')
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({k: manifest[k] for k in ('sections', 'binary_size', 'errors')}, indent=2))
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disc1', type=Path, required=True)
    parser.add_argument('--disc2', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=Path('data/presentation/usa'))
    args = parser.parse_args()
    build([args.disc1, args.disc2], args.output)
