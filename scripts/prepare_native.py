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
from mgs_overlay_catalog import describe

ROOT = Path(__file__).resolve().parents[1]
EXE_SHA = '3342f75717c5134aec832547691c2354e67a253abfcfa9e0e823e2133bb1fc32'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, default=ROOT / 'data/assets/usa/disc1/SLUS_005.94')
    parser.add_argument('--cue', type=Path, required=True)
    parser.add_argument('--disc2-cue', type=Path, help='USA second disc for seamless transitions')
    parser.add_argument('--campaign', action='store_true', help='compile all story-stage overlay candidates from the original USA archive')
    args = parser.parse_args()
    exe = args.exe.resolve(strict=True)
    cue = args.cue.resolve(strict=True)
    data = exe.read_bytes()
    if hashlib.sha256(data).hexdigest() != EXE_SHA:
        parser.error('Expected the verified USA v1.0 executable; refusing address patches')
    # CD_sync poll: the original delay slot sets a0=-1 before VSync.
    assert struct.unpack_from('<I', data, 0x8ff54 - 0x10000 + 2048)[0] == 0x2404ffff
    assert struct.unpack_from('<I', data, 0x17b38 - 0x10000 + 2048)[0] == 0x84830006
    # DG_SortChanlSystem dispatch, including channel/buffer argument setup.
    # Observe the stage of the pipeline, even when fog replaces its callback.
    assert struct.unpack_from('<7I', data, 0x18774 - 0x10000 + 2048) == (
        0x3c04800b, 0x24844164, 0x02602821, 0x8e420000,
        0x26520004, 0x0040f809, 0x2631ffff)
    for pc, instruction in ((0x8001A618, 0x27bdffd0), (0x80019F60, 0x27bdffc8),
                            (0x80019CDC, 0x27bdffe8), (0x80019F48, 0xac860000)):
        assert struct.unpack_from('<I', data, (pc & 0x1fffff) - 0x10000 + 2048)[0] == instruction
    # Ocelot's loader calls this leaf entry directly (USA s04b: JAL at
    # 0x800D8A00). The analyzer includes it after the preceding routine's JR.
    assert struct.unpack_from('<4I', data, 0x3b090 - 0x10000 + 2048) == (
        0x3c02800a, 0x24420a1c, 0x03e00008, 0)
    # USA DG_GetDisplayEnv leaf used by the credits overlay.
    assert struct.unpack_from('<4I', data, 0x18008 - 0x10000 + 2048) == (
        0x3c02800b, 0x24422d78, 0x03e00008, 0)
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
    # Direct overlay JALs into verified main-function interiors.
    # Register native entries rather than taking the private interpreter path.
    main_entries = {
        # Credits/ending2 call DG_GetDisplayEnv directly at the leaf after
        # DG_BackGroundBlack. USA JALs return to 800C9AFC and 800C9BF8.
        0x80017f64: [0x80018008],
        0x8001c65c: [0x8001c680],
        # s04c post-Ocelot demo, direct JAL at USA 0x800C777C.
        0x80024b0c: [0x80024c64],
        0x8002778c: [0x80027838],
        0x800279ec: [0x80027a44, 0x80027a94],
        # Direct calls in tower geometry/rope, Meryl and escape overlays.
        0x80027ed4: [0x80027f58],
        0x80032380: [0x80032498],
        0x8002f610: [0x8002f6c0],
        0x8003189c: [0x80031940],
        0x80033428: [0x800334d4],
        0x80034b64: [0x80034bd8],
        0x8003a01c: [0x8003a074],
        0x8003a918: [0x8003a928],
        0x8003b028: [0x8003b090],
        0x8003f088: [0x8003f09c],
        0x80041100: [0x80041114],
        # Retail memory-card load menu (abst: caller return 800C6CEC).
        0x80044c5c: [0x80044d2c],
        0x80052254: [0x80052288, 0x8005229c],
        0x80062744: [0x800627b0],
        0x800723f8: [0x800724f4],
        0x80081aa4: [0x80081be0, 0x80081c20, 0x80081cf8],
        0x8008b168: [0x8008b180],
        0x8008f7d0: [0x8008f7f0, 0x8008f8f4],
        0x80093134: [0x80093174],
        0x80095244: [0x80095360],
        0x80095ee8: [0x80095f40],
    }
    for owner, entries in main_entries.items():
        aliases = ", ".join(f'"0x{x:08X}"' for x in entries)
        s = s.replace(f'address = "0x{owner:08X}"',
                      f'address = "0x{owner:08X}"\nentry_aliases = [{aliases}]', 1)
    for name, address in [('ResetCallback', 0x8009ae54), ('InterruptCallback', 0x8009ae84),
                          ('DMACallback', 0x8009aeb4), ('CheckCallback', 0x8009afa8), ('VSync', 0x8009ac44)]:
        s += f'\n[[hle_functions]]\nsubsystem = "Other"\nhle = true\nstub_type = "stub"\nlibrary = "libetc"\nname = "libetc_{name}"\naddress = "0x{address:08X}"\n'
    # RecompOne MIT PsyQ signatures, verified against this fingerprinted EXE.
    for name, address in [('PadInitDirect', 0x8009ee48), ('PadStartCom', 0x8009d524),
                          ('PadStopCom', 0x8009d5f0), ('PadGetState', 0x8009cdf4),
                          ('PadInfoMode', 0x8009ceb4), ('PadInfoAct', 0x8009cfac),
                          ('PadSetAct', 0x8009d1a8), ('PadSetActAlign', 0x8009d128),
                          ('PadSetMainMode', 0x8009d160)]:
        s += f'\n[[hle_functions]]\nsubsystem = "Other"\nhle = true\nstub_type = "stub"\nlibrary = "libpad"\nname = "libpad_{name}"\naddress = "0x{address:08X}"\n'
    # The analyzer omitted the bodies of these initially HLE-matched wrappers.
    for start in sorted(native_sdk - existing):
        end = next(address for address in boundaries if address > start)
        s += f'\n[[functions]]\nname = "func_{start:08X}"\naddress = "0x{start:08X}"\nsize = {end-start}\nleaf = false\nsource = "verified_sdk_boundary"\n'
    # JSON quoting is valid TOML basic-string quoting (not shell interpolation).
    import json
    s += f'\n[recompiler]\ntranslation_units = {160 if args.campaign else 32}\n'
    s += f'\n[disc]\nexe_path = {json.dumps(str(exe))}\ncue_path = {json.dumps(str(cue))}\n'
    second = args.disc2_cue
    if second is None:
        candidates = sorted((ROOT / 'data/discs/usa/disc2').glob('*.cue'))
        if len(candidates) == 1:
            second = candidates[0]
    if second is not None:
        s += f'additional_cues = [{json.dumps(str(second.resolve(strict=True)))}]\n'
    s += '\n[video]\nbackend = "vulkan"\nvsync = false\npgxp = false\ntrue_depth = false\ntrue_color = false\n'
    s += '\n[game]\nprofile = "mgs_usa_v1_0"\nwidescreen_16_9 = true\nfast_loading = true\n\n[timing]\nvideo_standard = "ntsc"\n'
    s += '\n[instruction_overrides]\n"0x8008FF54" = "if (ctx->bios) ps1::runtimeBiosDrainPendingCallbacks(ctx->bios, 0x8008FF50u); pgxp_addiu(ctx, 4, 0, -1);"\n'
    s += '\n"0x80017B38" = "extern void mgsAdjustProjection(recomp_context*); mgsAdjustProjection(ctx); pgxp_invalidate_gpr(ctx, 3); ctx->r3 = (int16_t)MEM_READ16(ctx, ctx->r4 + 6);"\n'
    # Observe DG_ChanlUnits[1] before bounds and [6] before sorting. Hooks at
    # the default callback entries were bypassed by s12c's fog replacements.
    s += '\n"0x80018780" = "extern void mgsBeforeChannelUnit(recomp_context*); mgsBeforeChannelUnit(ctx); pgxp_invalidate_gpr(ctx, 2); ctx->r2 = pgxp_load_word(ctx, 2, ctx->r18);"\n'
    # Observe subdivision lineage without modifying original subdivision or RAM.
    for pc, name, original in (
        (0x8001A618, 'mgsBeginMeshDivide', 'pgxp_addiu(ctx, 29, 29, -48);'),
        (0x80019F60, 'mgsMeshDivideGrid', 'pgxp_addiu(ctx, 29, 29, -56);'),
        (0x80019CDC, 'mgsMeshDivideCopy', 'pgxp_addiu(ctx, 29, 29, -24);'),
        (0x80019F48, 'mgsMeshDividePacket', 'pgxp_store_word(ctx, 6, ctx->r4, ctx->r6);')):
        code = f'extern void {name}(recomp_context*); {name}(ctx); {original}'
        s += f'\n"0x{pc:08X}" = {json.dumps(code)}\n'
    image = next(cue.parent.glob('*.bin'))
    disc = Disc(image)
    record = disc.files()['/MGS/STAGE.DIR']
    archive = disc.read(record['lba'], record['size'])
    specifications = [
        ('d00a', '8d69b08b378fd6124a1390e82097477fde4b71c9161a7d27cdfdb93b422dfb38', 0x800e3a8c, 64),
        ('s00a', '0162b192109171aa249343e10340f0756efe2f03694f51b5844cbb20797be96f', 0x800e3688, 64),
        ('s01a', 'bd046ff36bc34bb60f845d6139be04ac0c000e0f526746209acbeb4fa354e84b', 0x800e74b4, 64),
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
    # Verified callback references in USA overlay code/actor tables. These
    # entries follow JR RA/NOP but lack prologues or direct JAL callers.
    callbacks = {
        # USA GV_SetNamedActor third argument: title destructor at 800D6070
        # stores 2 to 800AFBA8; opening's destructor is an empty JR RA/NOP.
        # Neither leaf has a stack prologue or a direct JAL caller.
        'title': [0x800d5f30],
        'opening': [0x800c7f6c],
        'd00a': [0x800c850c, 0x800c8bb4, 0x800c9abc, 0x800d29ac,
                 0x800d4210, 0x800d634c, 0x800db2c0, 0x800db7c8,
                 0x800dd940, 0x800e38e4],
        's01a': [0x800c7574, 0x800cb6c0, 0x800cbd68, 0x800ccc70,
                 0x800d5b60, 0x800d73c4, 0x800de990, 0x800dec84,
                 0x800e0804, 0x800e5b08, 0x800e63bc, 0x800e6f0c,
                 0x800e7110],
        's00a': [0x800c6120, 0x800c896c, 0x800c9014, 0x800c9f1c,
                 0x800d2e0c, 0x800d4670, 0x800d6328, 0x800d661c,
                 0x800daf60, 0x800db464, 0x800ddc40, 0x800e22dc,
                 0x800e2db8, 0x800e3060, 0x800e3264],
    }
    for stage, addresses in callbacks.items():
        payload = (ROOT / f'data/overlays/usa/{stage}.bin').read_bytes()
        for address in addresses:
            offset = address - 0x800c5968
            if payload[offset-8:offset] != bytes.fromhex('0800e00300000000'):
                parser.error(f'Unexpected callback boundary {stage}: {address:08x}')
        s = s.replace(f'name = "usa_{stage}"\n',
                      f'name = "usa_{stage}"\nadditional_functions = {addresses}\n')
    if args.campaign:
        known = {item[0]: item for item in specifications}
        catalogue = []
        images = {}
        for name, _, stage in stages(archive):
            if name.startswith(('vr', 'select')) or name in ('camera', 'sound', 'demosel'):
                continue
            for member, payload in stage_members(stage):
                if member['extension'] != 'b':
                    continue
                entry = describe(name, payload)
                images[name] = payload
                if name in known:
                    if entry['code_size'] != known[name][2] - 0x800c5968:
                        parser.error(f'Candidate extent disagrees with verified {name}')
                    # Verified boot/menu stages still need newly discovered
                    # callback boundaries. Preserve explicit historical entries
                    # and merge the same scan used for every campaign overlay.
                    addresses = sorted(set(callbacks[name] + entry['additional_functions']))
                    pattern = rf'(name = "usa_{re.escape(name)}"\n(?:[^\n]*\n)*?additional_functions = )\[[^\n]*\]'
                    s, count = re.subn(pattern, lambda m: m[1] + str(addresses), s, count=1)
                    if count != 1:
                        parser.error(f'Missing callback section for verified {name}')
                    entry['additional_functions'] = addresses
                    entry['derivation'] = 'previously donor-verified extent; historical entries plus current callback scan'
                    catalogue.append(entry)
                    continue
                overlay = ROOT / f'data/overlays/usa/{name}.bin'
                overlay.write_bytes(payload)
                # Match immutable constructor addresses without including the
                # mutable globals after short actor tables (e.g. change.b).
                identity_size = min(64, entry['actor_table_size'])
                s += (f'\n[[overlays]]\nname = "usa_{name}"\nram_base = {0x800c5968}\n'
                      f'size = {entry["code_size"]}\nbinary_path = "overlays/usa/{name}.bin"\n'
                      f'identity_size = {identity_size}\n'
                      f'additional_functions = {entry["additional_functions"]}\n')
                entry['identity_size'] = identity_size
                catalogue.append(entry)
        for i, entry in enumerate(catalogue):
            for other in catalogue[i + 1:]:
                left, right = images[entry['stage']], images[other['stage']]
                prefix = min(entry.get('identity_size', known.get(entry['stage'], (None, None, None, 64))[3]),
                             other.get('identity_size', known.get(other['stage'], (None, None, None, 64))[3]))
                if left[:prefix] != right[:prefix] or left[:entry['code_size']] == right[:other['code_size']]:
                    continue
                start = max(entry['actor_table_size'], other['actor_table_size']) + 8
                offset = next((j for j in range(start, min(entry['code_size'], other['code_size']) - 3, 4)
                               if left[j:j + 4] != right[j:j + 4]), None)
                if offset is None:
                    parser.error(f'No immutable code identity separates {entry["stage"]} and {other["stage"]}')
                for candidate in (entry, other):
                    candidate.setdefault('additional_identity_offsets', []).append(offset)
        for entry in catalogue:
            if entry.get('additional_identity_offsets'):
                offsets = sorted(set(entry['additional_identity_offsets']))
                entry['additional_identity_offsets'] = offsets
                s = s.replace(f'name = "usa_{entry["stage"]}"\n',
                              f'name = "usa_{entry["stage"]}"\nadditional_identity_offsets = {offsets}\n')
        report = {'archive_sha256': hashlib.sha256(archive).hexdigest(),
                  'note': 'Candidate static coverage only; reached-stage TAS validation is required.',
                  'overlays': catalogue}
        (ROOT / 'data/overlays/usa/campaign-catalog.json').write_text(json.dumps(report, indent=2) + '\n')
    output = ROOT / 'data/mgs.usa.native.toml'
    output.write_text(s)
    print(output)


if __name__ == '__main__':
    main()
