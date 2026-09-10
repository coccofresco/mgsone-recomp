#!/usr/bin/env python3
"""Generate the USA v1.0 native bring-up configuration from verified local data."""
import argparse
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import tomllib
from mgs_assets import Disc, stages, stage_members

ROOT = Path(__file__).resolve().parents[1]
EXE_SHA = '3342f75717c5134aec832547691c2354e67a253abfcfa9e0e823e2133bb1fc32'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, default=ROOT / 'data/assets/usa/disc1/SLUS_005.94')
    parser.add_argument('--cue', type=Path, required=True)
    args = parser.parse_args()
    exe = args.exe.resolve(strict=True)
    cue = args.cue.resolve(strict=True)
    data = exe.read_bytes()
    if hashlib.sha256(data).hexdigest() != EXE_SHA:
        parser.error('Expected the verified USA v1.0 executable; refusing address patches')
    # CD_sync poll: the original delay slot sets a0=-1 before VSync.
    assert struct.unpack_from('<I', data, 0x8ff54 - 0x10000 + 2048)[0] == 0x2404ffff
    assert struct.unpack_from('<I', data, 0x17b38 - 0x10000 + 2048)[0] == 0x84830006
    analysis = ROOT / 'data/mgs.usa.analysis.toml'
    with (ROOT / '.local/analyze-usa.log').open('w') as log:
        subprocess.run([str(ROOT / 'build/ps1Analyzer/ps1Analyzer'), str(exe), str(analysis)],
                       cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
    parsed = tomllib.loads(analysis.read_text())
    native_sdk = {int(x['address'], 16) for x in parsed['hle_functions']
                  if x['library'] in ('libcd', 'libgte') or x['name'] in ('libgpu_DrawOTagEnv', 'libgpu_ResetGraph')}
    existing = {int(x['address'], 16) for x in parsed['functions']}
    boundaries = sorted({int(x['address'], 16) for kind in ('functions', 'stubs', 'hle_functions')
                         for x in parsed.get(kind, [])})
    sections = re.split(r'(?=\[\[)', analysis.read_text())
    result = []
    for section in sections:
        if section.startswith('[[functions]]') and 'address = "0x8009FA28"' in section:
            continue  # data begins immediately after the final jr ra delay slot
        match = re.search(r'address = "(0x[0-9A-Fa-f]+)"', section)
        native = match is not None and int(match[1], 16) in native_sdk
        if section.startswith('[[stubs]]') and native:
            continue
        if section.startswith('[[hle_functions]]') and native:
            section = section.replace('hle = true', 'hle = false').replace('stub_type = "stub"', 'stub_type = "recompile"')
        result.append(section)
    s = ''.join(result)
    s = s.replace('address = "0x80093134"', 'address = "0x80093134"\nentry_aliases = ["0x80093174"]', 1)
    for name, address in [('ResetCallback', 0x8009ae54), ('InterruptCallback', 0x8009ae84),
                          ('DMACallback', 0x8009aeb4), ('CheckCallback', 0x8009afa8), ('VSync', 0x8009ac44)]:
        s += f'\n[[hle_functions]]\nsubsystem = "Other"\nhle = true\nstub_type = "stub"\nlibrary = "libetc"\nname = "libetc_{name}"\naddress = "0x{address:08X}"\n'
    # The analyzer omitted the bodies of these initially HLE-matched wrappers.
    for start in sorted(native_sdk - existing):
        end = next(address for address in boundaries if address > start)
        s += f'\n[[functions]]\nname = "func_{start:08X}"\naddress = "0x{start:08X}"\nsize = {end-start}\nleaf = false\nsource = "verified_sdk_boundary"\n'
    # JSON quoting is valid TOML basic-string quoting (not shell interpolation).
    import json
    s += '\n[recompiler]\ntranslation_units = 32\n'
    s += f'\n[disc]\nexe_path = {json.dumps(str(exe))}\ncue_path = {json.dumps(str(cue))}\n'
    s += '\n[video]\nbackend = "vulkan"\nvsync = false\npgxp = false\ntrue_depth = false\ntrue_color = false\n'
    s += '\n[game]\nprofile = "mgs_usa_v1_0"\nwidescreen_16_9 = true\n\n[timing]\nvideo_standard = "ntsc"\n'
    s += '\n[instruction_overrides]\n"0x8008FF54" = "if (ctx->bios) ps1::runtimeBiosDrainPendingCallbacks(ctx->bios); pgxp_addiu(ctx, 4, 0, -1);"\n'
    s += '\n"0x80017B38" = "extern void mgsAdjustProjection(recomp_context*); mgsAdjustProjection(ctx); pgxp_invalidate_gpr(ctx, 3); ctx->r3 = (int16_t)MEM_READ16(ctx, ctx->r4 + 6);"\n'
    image = next(cue.parent.glob('*.bin'))
    disc = Disc(image)
    record = disc.files()['/MGS/STAGE.DIR']
    archive = disc.read(record['lba'], record['size'])
    specifications = [
        ('title', '8a909480158d16c1f6849df915751ce927335d0d6df5a7849a5bc20fdd042c71', 0x800d60fc, 16),
        ('opening', '359a5fffcf6555bb40861beea2d576691770fbe015d524d1395b2101c1884db8', 0x800cff24, 64),
    ]
    for name, digest, code_end, identity_size in specifications:
        payload = next(payload for stage_name, _, stage in stages(archive) if stage_name == name
                       for member, payload in stage_members(stage) if member['extension'] == 'b')
        if hashlib.sha256(payload).hexdigest() != digest:
            parser.error(f'Unexpected USA {name} overlay')
        overlay = ROOT / f'data/overlays/usa/{name}.bin'
        overlay.parent.mkdir(parents=True, exist_ok=True)
        overlay.write_bytes(payload)
        s += f'\n[[overlays]]\nname = "usa_{name}"\nram_base = {0x800c5968}\nsize = {code_end-0x800c5968}\nbinary_path = "overlays/usa/{name}.bin"\nidentity_size = {identity_size}\n'
    output = ROOT / 'data/mgs.usa.native.toml'
    output.write_text(s)
    print(output)


if __name__ == '__main__':
    main()
