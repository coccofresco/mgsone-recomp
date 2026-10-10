#!/usr/bin/env python3
"""Bind compiled decomp modules at generated USA function entry boundaries.

Leaves original bodies available for unported behavior and savestate interior
continuations. Does not rewrite calls or identify functions heuristically.
"""
import argparse
import hashlib
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
USA_SHA256 = '3342f75717c5134aec832547691c2354e67a253abfcfa9e0e823e2133bb1fc32'
ENTRIES = (0x8001E254, 0x8001E2E8, 0x8001E310, 0x8001E378)
DISC_ENTRIES = (0x800C699C, 0x800C63A0, 0x800C5FD0, 0x800C76A0,
                0x800C6E18, 0x800C7324)
CHANGE_SHA256 = 'd06543015baf02c8b4e6f30fe39e6bcd4786d7a0b6e6e2c80e2b482e1ead2844'


# Retail DecDCTin returns after a synchronous host DMA has produced its pixels.
# Yield at its existing portable return label so the already completed output
# IRQ chain reaches Movie before its non-volatile completion check. This keeps
# every retail instruction and avoids patching Movie's deliberate panic loop.
MOVIE_DMA_ENTRIES = dict(d18a=0x800DC554, roll=0x800CB740, s04b=0x800E1020,
                         s04c=0x800DD3D0, s08b=0x800E5E60, s12a=0x800DEF64,
                         s04br=0x800E1020, s08br=0x800E5E58, ending=0x800CE960)
DCT_IN_WORDS = (0x27bdffe8,0x30a20001,0x10400006,0xafbf0010,0x3c03f7ff,
    0x8c820000,0x3463ffff,0,0x00431024,0x8c820000,0x3c030800,0x00431025,
    0xac820000,0x30a20002,0x10400004,0x3c030200,0x8c820000,0,0x00431025,
    0x3c03fdff,0x8c820000,0x3463ffff,0x00431024,0xac820000,0x94850000,
    0,0,0x8fbf0010,0x27bd0018,0x03e00008,0)


def validate_dct_in(data, entry):
    offset = entry - 0x800C5968
    if offset < 0 or offset + 124 > len(data):
        raise ValueError('DecDCTin SDK boundary outside overlay')
    words = struct.unpack_from('<31I', data, offset)
    for index, (actual, expected) in enumerate(zip(words, DCT_IN_WORDS)):
        if index in (7, 17):
            expected = 0x08000000 | (((entry + (0x30 if index == 7 else 0x5c)) >> 2) & 0x3ffffff)
        elif index == 25:
            target = 0x80000000 | ((actual & 0x3ffffff) << 2)
            if actual >> 26 != 3 or not 0x800C5968 <= target < 0x800C5968 + len(data):
                raise ValueError('DecDCTin DMA helper outside overlay')
            continue
        if actual != expected:
            raise ValueError('DecDCTin SDK instruction identity mismatch')


def install(directory, executable, change_overlay=None, overlay_directory=None):
    if hashlib.sha256(executable.read_bytes()).hexdigest() != USA_SHA256:
        raise ValueError('decomp ABI manifest requires USA v1.0 SLUS_005.94')
    sources = {p: p.read_text() for p in directory.glob('recompiled_out*.cpp')}
    edits = {}
    # USA FS_LoadStageSync calls LoadStageFiles then CDBIOS_ReadSync. The
    # native adapter interleaves those original consumers with CD transactions.
    entry = 0x80024A64
    offset = entry - 0x80010000 + 2048
    words = struct.unpack_from('<18I', executable.read_bytes(), offset)
    if words[3] != 0x0c009228 or words[7] != 0x0c008e88:
        raise ValueError('FS_LoadStageSync retail call identity mismatch')
    marker = f'    PS1_FUNCTION_TRACE_HOOK(ctx, 0x{entry:08X}u);'
    owners = [p for p, text in sources.items() if marker in text]
    if len(owners) != 1:
        raise ValueError('expected one USA FS_LoadStageSync binding')
    hook = ('\n    extern bool mgsTryFastStageSync(recomp_context*);'
            '\n    if (mgsTryFastStageSync(ctx)) return;')
    if marker + hook not in sources[owners[0]]:
        edits[owners[0]] = sources[owners[0]].replace(marker, marker + hook, 1)
    # USA PCX uploads are synchronous HLE LoadImage calls. Retire precisely
    # their scratch allocation after the original deferred free in a burst.
    data = executable.read_bytes()
    for pc, target in ((0x8002079C, 0x8009259C), (0x800207A8, 0x8009259C),
                       (0x800207B8, 0x800165F8)):
        word, = struct.unpack_from('<I', data, pc - 0x80010000 + 2048)
        if word != (0x0c000000 | ((target >> 2) & 0x3ffffff)):
            raise ValueError('PCX upload/free instruction identity mismatch')
    marker = '    PS1_PC_TRACE_HOOK(ctx, 0x800207C0u);'
    owners = [p for p, text in sources.items() if marker in text]
    if len(owners) != 1:
        raise ValueError('expected one USA PCX scratch lifetime boundary')
    p = owners[0]
    current = edits.get(p, sources[p])
    hook = ('\n    extern void mgsReleaseFastPcxScratch(recomp_context*);'
            '\n    mgsReleaseFastPcxScratch(ctx);')
    if marker + hook not in current:
        edits[p] = current.replace(marker, marker + hook, 1)
    for entry in ENTRIES:
        marker = f'    PS1_FUNCTION_TRACE_HOOK(ctx, 0x{entry:08X}u);'
        owners = [p for p, text in sources.items() if marker in text]
        if len(owners) != 1:
            raise ValueError(f'expected exactly one generated entry for {entry:08X}')
        p = owners[0]
        hook = (f'\n    extern bool mgsTryDecompiled(recomp_context*, uint32_t);'
                f'\n    if (mgsTryDecompiled(ctx, 0x{entry:08X}u)) return;')
        current = edits.get(p, sources[p])
        if marker + hook not in current:
            edits[p] = current.replace(marker, marker + hook, 1)
    # Overlay addresses overlap other stages. Bind by the generated overlay
    # symbol and verified retail bytes, never by a global PC match.
    if any('void overlay_usa_change__800C699C(' in text for text in sources.values()):
        overlay = change_overlay or ROOT / 'data/overlays/usa/change.bin'
        if hashlib.sha256(overlay.read_bytes()).hexdigest() != CHANGE_SHA256:
            raise ValueError('disc-change ABI requires verified USA change.bin')
        for entry in DISC_ENTRIES:
            marker = (f'void overlay_usa_change__{entry:08X}(uint8_t* rdram, recomp_context* ctx) {{\n'
                      f'    PS1_FUNCTION_TRACE_HOOK(ctx, 0x{entry:08X}u);')
            owners = [p for p, text in sources.items() if marker in text]
            if len(owners) != 1:
                raise ValueError(f'expected one USA disc-change binding for {entry:08X}')
            p = owners[0]
            hook = ('\n    extern bool mgsTryDiscChange(recomp_context*, uint32_t);'
                    f'\n    if (mgsTryDiscChange(ctx, 0x{entry:08X}u)) return;')
            current = edits.get(p, sources[p])
            if marker + hook not in current:
                edits[p] = current.replace(marker, marker + hook, 1)
    overlay_directory = overlay_directory or ROOT / 'data/overlays/usa'
    for name, entry in MOVIE_DMA_ENTRIES.items():
        function = f'void overlay_usa_{name}__{entry:08X}(uint8_t* rdram, recomp_context* ctx) {{'
        owners = [p for p, text in sources.items() if function in text]
        if not owners:
            continue  # this overlay was not compiled in the selected campaign
        if len(owners) != 1:
            raise ValueError(f'ambiguous DecDCTin boundary in {name}')
        validate_dct_in((overlay_directory / f'{name}.bin').read_bytes(), entry)
        p = owners[0]
        current = edits.get(p, sources[p])
        start = current.index(function)
        end = current.index('\n}', start)
        body = current[start:end]
        pc = entry + 0x6c
        marker = f'    PS1_PC_TRACE_HOOK(ctx, 0x{pc:08X}u);'
        if body.count(marker) != 1:
            raise ValueError(f'missing portable DecDCTin return in {name}')
        hook = f'\n    if (ctx->bios) ps1::runtimeBiosDrainPendingCallbacks(ctx->bios, 0x{pc:08X}u); // completed SDK DMA IRQs'
        if marker + hook not in body:
            edits[p] = current[:start] + body.replace(marker, marker + hook, 1) + current[end:]
    # Validate all entries before changing any output file.
    for path, text in edits.items():
        path.write_text(text)
    return len(edits)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=ROOT / 'engine/ps1Runtime/src')
    parser.add_argument('--executable', type=Path, default=ROOT / 'data/assets/usa/disc1/SLUS_005.94')
    args = parser.parse_args()
    print(f'Installed verified decomp, disc and SDK boundaries; changed {install(args.directory, args.executable)} files')
