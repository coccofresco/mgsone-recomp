#!/usr/bin/env python3
"""Import PSXjin PJM digital input, without claiming cross-emulator sync.

Format: https://tasvideos.org/EmulatorResources/PSXjin/PJM
Text bit order and command exponent: PSXjin movie.cpp MOV_ReadJoy/ReadControl.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import zlib

BUTTONS = ('select', 'l3', 'r3', 'start', 'up', 'right', 'down', 'left',
           'l2', 'r2', 'l1', 'r1', 'triangle', 'circle', 'cross', 'square')
TEXT_BITS = tuple(range(15, 2, -1)) + (0,)


def card_image(chunk):
    if len(chunk) < 4 or struct.unpack_from('<I', chunk)[0] != 131072:
        raise ValueError('invalid embedded memory card length')
    decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
    card = decoder.decompress(chunk[4:], 131073)
    if len(card) != 131072 or not decoder.eof or decoder.unused_data:
        raise ValueError('invalid or oversized embedded memory card stream')
    return card


def parse_pjm(data):
    if len(data) < 52 or data[:4] != b'PJM ':
        raise ValueError('not a PJM movie')
    version, emulator = struct.unpack_from('<II', data, 4)
    flags, pad1, pad2, count, rerecords = struct.unpack_from('<HBBII', data, 12)
    if version != 2:
        raise ValueError('only PJM version 2 is supported')
    # Explicitly exclude states, PAL, cheats, hacks, multitaps and analog hacks.
    if flags & ~(8 | 64):
        raise ValueError(f'unsupported PJM flags: 0x{flags:x}')
    if (pad1, pad2) != (4, 4):
        raise ValueError('this importer requires two standard digital controllers')
    offsets = struct.unpack_from('<6I', data, 24)
    if offsets[0] < 52 or any(a >= b for a, b in zip(offsets, offsets[1:])) or offsets[-1] > len(data):
        raise ValueError('invalid PJM chunk offsets')
    chunks = [data[a:b] for a, b in zip(offsets, offsets[1:])]
    state, card1, card2, cheats, cd_ids = chunks
    if state != b'\0' * 4 or cheats != b'\0' * 4:
        raise ValueError('embedded savestates and cheats are not supported')
    cards = [card_image(card1), card_image(card2)] if flags & 8 else []
    if not cards and (card1 != b'\0' * 4 or card2 != b'\0' * 4):
        raise ValueError('memory card data contradicts header flags')
    if not cd_ids or len(cd_ids) != 1 + cd_ids[0] * 9:
        raise ValueError('invalid disc ID chunk')
    ids = [cd_ids[i:i + 9].decode('ascii') for i in range(1, len(cd_ids), 9)]
    # Some historical files claim a 512-byte author string overlapping chunks.
    # PSXjin seeks to chunk offsets explicitly; only the actual metadata is text.
    author_len = struct.unpack_from('<I', data, 48)[0]
    author = data[52:min(52 + author_len, offsets[0])].split(b'\0')[0].decode('utf-8', 'replace')
    payload = data[offsets[-1]:]
    stride = 34 if flags & 64 else 5
    if count == 0 or len(payload) != count * stride:
        raise ValueError('PJM input length does not match frame count')
    frames = []
    events = []
    for index in range(count):
        row = payload[index * stride:(index + 1) * stride]
        if flags & 64:
            if row[14:15] != b'|' or row[29:30] != b'|' or row[31:] != b'|\r\n' or row[30] not in range(48, 56):
                raise ValueError(f'malformed text input at frame {index + 1}')
            pads = []
            for field in (row[:14], row[15:29]):
                if any(byte < 33 or byte > 126 or byte == 124 for byte in field):
                    raise ValueError(f'malformed pad field at frame {index + 1}')
                pads.append(sum(1 << bit for byte, bit in zip(field, TEXT_BITS) if byte != 46))
            # The text command is an exponent, unlike the binary bitmask.
            control = (1 << (row[30] - 48)) & 0xfe
        else:
            first, second, control = struct.unpack('<HHB', row)
            if (first | second) & 6:
                raise ValueError('unsupported digital controller reserved bits')
            pads = [first, second]
            control &= 0xfe
        frames.append(tuple(pads))
        if control:
            events.append({'frame': index + 1, 'mask': control,
                           'kind': 'disc_lid_toggle' if control == 4 else 'unsupported_system_command'})
    metadata = {'format': 'psxjin-pjm-2', 'emulator_version': emulator,
                'author': author, 'frames': count, 'rerecords': rerecords,
                'flags': flags, 'disc_ids': ids, 'controller_types': [pad1, pad2],
                'embedded_card_sha256': [hashlib.sha256(card).hexdigest() for card in cards],
                'events': events, 'source_sha256': hashlib.sha256(data).hexdigest()}
    return metadata, frames, cards


def input_intervals(frames, first, last, vblank):
    if not 1 <= first <= last <= len(frames) or vblank < 1:
        raise ValueError('invalid source frame range or target VBlank')
    intervals = []
    opened = {}
    for index in range(first - 1, last + 1):
        pads = frames[index] if index < last else (0, 0)
        target = vblank + index - first + 1
        for port in range(2):
            for bit, name in enumerate(BUTTONS):
                key = port, bit
                if pads[port] & (1 << bit):
                    opened.setdefault(key, target)
                elif key in opened:
                    intervals.append((opened.pop(key), target - 1, name, port))
    return ''.join(f'{start} {end} {name} {port}\n' for start, end, name, port in sorted(intervals))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    inspect = commands.add_parser('inspect')
    inspect.add_argument('movie', type=Path)
    convert = commands.add_parser('convert')
    convert.add_argument('movie', type=Path)
    convert.add_argument('destination', type=Path)
    convert.add_argument('--first-frame', type=int, default=1)
    convert.add_argument('--last-frame', type=int)
    convert.add_argument('--first-vblank', type=int, required=True,
                         help='absolute native VBlank receiving --first-frame; never inferred from emulator power-on')
    convert.add_argument('--source-url', default='')
    args = parser.parse_args()
    try:
        if args.movie.stat().st_size > 128 * 1024 * 1024:
            raise ValueError('PJM exceeds 128 MiB input limit')
        metadata, frames, cards = parse_pjm(args.movie.read_bytes())
        if args.command == 'inspect':
            print(json.dumps(metadata, indent=2))
            return 0
        last = args.last_frame if args.last_frame is not None else len(frames)
        script = input_intervals(frames, args.first_frame, last, args.first_vblank)
        events = [event for event in metadata['events'] if args.first_frame <= event['frame'] <= last]
        if events:
            raise ValueError(f'system commands require runtime support; selected range includes {events[0]}; choose an explicit range before that event')
        output = args.destination.resolve()
        output.mkdir(parents=True, exist_ok=False)
        (output / 'source.input').write_text('# Imported input only; synchronization unverified.\n' + script)
        if cards:
            (output / 'cards').mkdir()
            for port, card in enumerate(cards, 1):
                (output / 'cards' / f'card{port}.mcd').write_bytes(card)
        metadata.update(source_file=str(args.movie.resolve()), source_url=args.source_url,
                        selected_frames=[args.first_frame, last], first_vblank=args.first_vblank,
                        last_vblank=args.first_vblank + last - args.first_frame,
                        synchronization='unverified: source emulator power-on differs from native checkpoint',
                        input_sha256=hashlib.sha256((output / 'source.input').read_bytes()).hexdigest())
        (output / 'import.json').write_text(json.dumps(metadata, indent=2) + '\n')
        print(f'Imported {last - args.first_frame + 1} frames into {output}; synchronization unverified')
        return 0
    except (OSError, ValueError, zlib.error) as error:
        print(f'PJM: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
