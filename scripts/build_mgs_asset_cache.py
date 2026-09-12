#!/usr/bin/env python3
"""Build immutable GPU storage from original USA discs; never select by hash alone."""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct
import zlib
from mgs_assets import Disc, stages, stage_members, dar_members, kmd, zmd, pcx, sha, require

MAGIC = b'MGSAC003'
SECTIONS = [('ASST', 64), ('MODL', 32), ('VRTX', 48), ('TEXR', 32),
            ('TEXI', 1), ('PLTE', 4), ('SRCE', 1), ('NAME', 8), ('TMPL', 16)]


def pack_sections(sections):
    """16-byte header, 9x16-byte directory, 16-byte-aligned sections, CRC32."""
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
    logical_corners = 0
    def add_asset(ext, data, context, parsed=None, embedded_id=None):
        nonlocal logical_corners
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
                    first_corner = logical_corners
                    logical_corners += len(corners)
                    parent = model['parent']
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
    # Original file IDs are provenance hints, always checked against geometry.
    names = set()
    for row in occurrences:
        name = row['embedded_id']
        if name is None:
            name = int(row['id'], 16)
        names.add((name, row['asset']))
    for name, asset in sorted(names):
        sections['NAME'].extend(struct.pack('<2I', name, asset))
    # DG_PRIM quads have no disk model. Keep their canonical topology resident;
    # the native actor supplies the current anchor/shape, not projected pixels.
    for x, y in ((0, 0), (1, 0), (0, 1), (1, 1)):
        sections['TMPL'].extend(struct.pack('<4f', x, y, 0, 1))
    binary = pack_sections(sections)
    manifest = dict(schema='mgs-asset-cache/v3', discs=disc_rows, assets=assets,
        occurrences=occurrences, sections={name: dict(count=len(sections[name]) // stride, stride=stride)
                                         for name, stride in SECTIONS},
        binary_sha256=sha(binary), binary_size=len(binary), errors=errors,
        other_resource_occurrences=dict(ignored),
        lod_policy='retain every distinct source variant; no unverified cross-asset replacement',
        lod_families=[],
        runtime_status='Original KMD/ZMD streams and decoded PCX indices; no expanded geometry copies',
        logical_corners=logical_corners)
    output.mkdir(parents=True, exist_ok=True)
    temp = output / 'assets.mgsc.tmp'
    temp.write_bytes(binary)
    temp.replace(output / 'assets.mgsc')
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({k: manifest[k] for k in ('sections', 'binary_size', 'errors')}, indent=2))
    return manifest


def compact_cache(data):
    """Remove expanded corners while preserving all original streams verbatim."""
    require(len(data) >= 160 and data[:8] in (b'MGSAC002', MAGIC), 'expected v2/v3 cache')
    require(struct.unpack_from('<I', data, 8)[0] == len(data), 'cache length mismatch')
    require(struct.unpack_from('<I', data, 12)[0] == zlib.crc32(data[16:]), 'cache checksum mismatch')
    sections = {}
    end = 160
    for i, (name, stride) in enumerate(SECTIONS):
        tag, offset, count, actual_stride = struct.unpack_from('<4sIII', data, 16+i*16)
        require(tag == name.encode() and stride == actual_stride, 'cache directory mismatch')
        require(offset % 16 == 0 and offset >= end, 'cache section overlap')
        end = offset + count * stride
        require(end <= len(data), 'cache section outside file')
        sections[name] = data[offset:end] if name != 'VRTX' else b''
    require(end == len(data), 'cache trailing bytes')
    return pack_sections(sections)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disc1', type=Path)
    parser.add_argument('--disc2', type=Path)
    parser.add_argument('--from-cache', type=Path)
    parser.add_argument('--output', type=Path, default=Path('data/presentation/usa-source'))
    args = parser.parse_args()
    if args.from_cache:
        if args.disc1 or args.disc2:
            parser.error('--from-cache cannot be combined with discs')
        original = args.from_cache.read_bytes()
        binary = compact_cache(original)
        args.output.mkdir(parents=True, exist_ok=True)
        temporary = args.output / 'assets.mgsc.tmp'
        temporary.write_bytes(binary)
        temporary.replace(args.output / 'assets.mgsc')
        manifest = dict(schema='mgs-asset-cache/v3', source_cache=str(args.from_cache),
                        source_sha256=sha(original), binary_sha256=sha(binary),
                        binary_size=len(binary), removed_expanded_bytes=len(original)-len(binary))
        (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
        print(json.dumps(manifest, indent=2))
    else:
        if not args.disc1 or not args.disc2:
            parser.error('supply --disc1 and --disc2, or --from-cache')
        build([args.disc1, args.disc2], args.output)
