"""Create explicitly synthetic, isolated checkpoint fixtures for USA diagnostics.

Only RAM bytes named by a recipe may change. Native payload checksums and the
donor's complete RAM witness are checked before copying. Original states/cards
are never modified. These fixtures do not establish campaign reachability.
"""
import struct
import re
import zlib

from mgs_tas_route import actor_counts

RAM_SIZE = 2097152


def phase_card(card, block, stage, story, variables=(), linked=()):
    """Synthetic USA save preconditions, always on an isolated copy.

    Retail USA SAVE_DATA has 95 linked shorts, versus Integral's 96. Derive
    and validate the USA layout through the existing card decoder first.
    """
    from mgs_scenarios import card_entries, isolated_card
    if not re.fullmatch(r'[a-z0-9_]{1,15}', stage) or not 0 <= story <= 32767:
        raise ValueError('invalid phase stage/story')
    result = bytearray(isolated_card(card, block))
    base = block * 8192 + 0x108
    result[base + 24:base + 40] = stage.encode().ljust(16, b'\0')
    variables_base = base + 0x38 + 190
    struct.pack_into('<h', result, variables_base + 10, story)
    touched = {10, 11}
    for field in variables:
        offset, width = integer(field['offset']), field['width']
        if width not in (1, 2) or not 0 <= offset <= 2048 - width or not field.get('source'):
            raise ValueError('phase variable must name bounded USA GCL data and source')
        if touched.intersection(range(offset, offset + width)):
            raise ValueError('overlapping phase fields')
        touched.update(range(offset, offset + width))
        result[variables_base + offset:variables_base + offset + width] = integer(field['value']).to_bytes(width, 'little')
    # Source-derived entry procedures also set the active map and player spawn
    # in the USA 95-short linked bank. Keep its bounds separate from GCL globals.
    touched = set()
    for field in linked:
        offset, width = integer(field['offset']), field['width']
        if width not in (1, 2) or not 0 <= offset <= 190 - width or not field.get('source'):
            raise ValueError('phase linked field must name bounded USA linked data and source')
        if touched.intersection(range(offset, offset + width)):
            raise ValueError('overlapping linked phase fields')
        touched.update(range(offset, offset + width))
        start = base + 0x38 + offset
        result[start:start + width] = integer(field['value']).to_bytes(width, 'little', signed=field.get('signed', False))
    struct.pack_into('<I', result, base - 4, zlib.crc32(result[base:base + 0xa38]))
    card_entries(result)
    return bytes(result)


def integer(value):
    return int(value, 0) if isinstance(value, str) else int(value)


def actors(ram):
    if actor_counts(ram) is None:
        raise ValueError('invalid actor lists')
    word = lambda p: struct.unpack_from('<I', ram, p & 0x1fffffff)[0]
    found = {}
    for level in range(9):
        head = 0x800af390 + level * 68
        actor = word(head + 4)
        while actor != head + 32:
            name = word(actor + 20) & 0x1fffffff
            if name and (word(actor + 8) or word(actor + 12)):
                label = ram[name:ram.index(b'\0', name)].decode('ascii')
                found.setdefault(label, []).append(actor)
            actor = word(actor + 4)
    return found


def patch_ram(ram, patches):
    if len(ram) != RAM_SIZE:
        raise ValueError('expected complete USA RAM witness')
    result, changes, touched = bytearray(ram), [], set()
    for patch in patches:
        if not patch.get('reason') or not patch.get('source') or 'expect' not in patch:
            raise ValueError('each fixture write requires source, reason and prior-value bounds')
        width = patch.get('width')
        if width not in (1, 2, 4):
            raise ValueError('fixture width must be 1, 2 or 4')
        if 'actor' in patch:
            matches = actors(ram).get(patch['actor'], [])
            # Authored fields (e.g. CONTROL.name or position) disambiguate
            # multiple live instances; never select by allocator/list order.
            selectors = patch.get('where', [])
            if not isinstance(selectors, list) or len(selectors) > 8:
                raise ValueError('actor selectors require at most eight bounded fields')
            for selector in selectors:
                if (not isinstance(selector, dict) or
                        set(selector) - {'offset', 'width', 'value', 'signed'} or
                        not {'offset', 'width', 'value'} <= set(selector)):
                    raise ValueError('invalid actor selector')
                offset, size = integer(selector['offset']), selector['width']
                if size not in (1, 2, 4) or not 32 <= offset <= 0x4000-size or offset % size:
                    raise ValueError('actor selector outside bounded aligned Work fields')
                selected = []
                for base in matches:
                    at = (base & 0x1fffffff) + offset
                    if at <= RAM_SIZE-size and int.from_bytes(ram[at:at+size], 'little',
                            signed=selector.get('signed', False)) == integer(selector['value']):
                        selected.append(base)
                matches = selected
            if len(matches) != 1:
                raise ValueError(f'fixture requires one {patch["actor"]}, found {len(matches)}')
            base = matches[0]
            act = struct.unpack_from('<I', ram, (base & 0x1fffffff) + 8)[0]
            if act != integer(patch['act']):
                raise ValueError('actor callback differs from the verified USA binding')
            offset = integer(patch['offset'])
            if not 32 <= offset < 0x8000:
                raise ValueError('fixture cannot edit actor links/callbacks')
            address = base + offset
            if 'indirect_offset' in patch:
                if (address & 0x1fffffff) > RAM_SIZE - 4:
                    raise ValueError('actor pointer field outside guest RAM')
                pointer = struct.unpack_from('<I', ram, address & 0x1fffffff)[0]
                if not 0x80010000 <= pointer <= 0x801fffb8:
                    raise ValueError('actor data pointer outside guest RAM')
                member = integer(patch['indirect_offset'])
                if not 0 <= member < 0x100:
                    raise ValueError('indirect fixture member outside bounded target data')
                address = pointer + member
        else:
            address = integer(patch['address'])
        pos = address & 0x1fffffff
        if not 0x10000 <= pos <= RAM_SIZE - width:
            raise ValueError('fixture write outside guest data RAM')
        if touched.intersection(range(pos, pos + width)):
            raise ValueError('overlapping fixture writes')
        touched.update(range(pos, pos + width))
        old = int.from_bytes(ram[pos:pos + width], 'little', signed=patch.get('signed', False))
        expected = patch['expect']
        if not isinstance(expected, dict):
            expected = {'eq': integer(expected)}
        if not expected or set(expected) - {'eq', 'min', 'max'}:
            raise ValueError('invalid fixture prior-value predicate')
        if any((op == 'eq' and old != integer(v)) or (op == 'min' and old < integer(v)) or
               (op == 'max' and old > integer(v)) for op, v in expected.items()):
            raise ValueError(f'fixture prior value mismatch at {address:#x}: {old}, expected {expected}')
        value = integer(patch['value'])
        result[pos:pos + width] = value.to_bytes(width, 'little', signed=patch.get('signed', False))
        changes.append(dict(address=f'0x{address:08x}', before=old, after=value, width=width,
                            reason=patch['reason'], source=patch['source']))
    return bytes(result), changes


def checksum(payload):
    value = 2166136261
    for byte in payload:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def checkpoint_bytes(state, ram, patched, engine):
    if len(ram) != RAM_SIZE or len(patched) != RAM_SIZE:
        raise ValueError('fixture RAM must be exactly 2 MiB')
    if engine == 'native':
        if state[:4] != b'PS1S' or len(state) < 476 + RAM_SIZE:
            raise ValueError('not a supported native state')
        version, flags, saved = struct.unpack_from('<III', state, 4)
        if version not in (6, 7, 8, 9, 10, 11, 12) or flags != (int.from_bytes(f"NSV{version:X}".encode(), "little")):
            raise ValueError('unsupported native fixture format')
        if checksum(state[16:]) != saved:
            raise ValueError('native input state has a bad checksum')
        pos = 476
    elif engine == 'donor':
        if state[:4] != b'DUCC':
            raise ValueError('expected raw SwanStation state')
        pos = state.find(ram)
        if pos < 0 or state.find(ram, pos + 1) >= 0:
            raise ValueError('donor state must contain exactly one complete RAM witness')
    else:
        raise ValueError('unknown fixture engine')
    if state[pos:pos + RAM_SIZE] != ram:
        raise ValueError('checkpoint differs from terminal RAM witness')
    result = bytearray(state)
    result[pos:pos + RAM_SIZE] = patched
    if engine == 'native':
        struct.pack_into('<I', result, 12, checksum(result[16:]))
    return bytes(result)
