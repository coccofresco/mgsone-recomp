#!/usr/bin/env python3
"""Assemble the Windows x64 ZIP with a pinned private Python and SDL runtime."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import zipfile
from mgs_alpha import ROOT, atomic_json, digest
from package_alpha import SCRIPTS, LICENSES, audit, source_files, command


def build(output, build_dir, sdk):
    output, build_dir, sdk = output.resolve(), build_dir.resolve(), sdk.resolve()
    if 'PS1RECOMP_USE_STUB:BOOL=OFF' not in (build_dir/'CMakeCache.txt').read_text():
        raise ValueError('A non-stub Windows game build is required.')
    release = json.loads((ROOT/'packaging/alpha.json').read_text())
    release.update(platform='windows-x86_64', target_system='Windows 10 1903+ / Windows 11 x64; Steam desktop and Big Picture',
                   required_distribution_formats=['ZIP'], minimum_python='bundled 3.14.8')
    release['known_limits'] = [
        'Continuous completion and full boss fights are not certified.',
        'Windows binaries are exercised on physical Vulkan hardware through Wine/Proton; native Windows acceptance remains required.',
        'Physical controller hotplug, Steam Input and audio-device acceptance on Windows remains required.',
        'The optional legacy Tk editor is not bundled; the native controller frontend and CLI are included.']
    name = 'mgs-recomp-'+release['version']+'-windows-x86_64'
    folder = output/name
    output.mkdir(parents=True, exist_ok=True)
    folder.mkdir(exist_ok=False)
    llvm = next((sdk/'toolchain').glob('*/bin')).parent
    inspector = llvm/'bin/llvm-objdump'
    dependencies = {}

    def copy(source, destination):
        source = ROOT/source
        if source.is_symlink(): raise ValueError('Symlink package input: '+str(source))
        target = folder/destination
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        return target

    def imports(path):
        text = subprocess.check_output([str(inspector), '-p', str(path)], text=True)
        if 'file format coff-x86-64' not in text:
            raise ValueError('Not a Windows x64 PE: '+str(path))
        return sorted(set(re.findall(r'DLL Name: (\S+)', text)), key=str.lower)

    for source, dest in [('ps1Runtime.exe', 'bin/mgs-runtime.exe'), ('mgsFrontend.exe', 'bin/mgs-frontend.exe'),
                         ('MGSRecomp.exe', 'MGSRecomp.exe')]:
        path = copy(build_dir/'ps1Runtime'/source, dest)
        subprocess.run([str(llvm/'bin/llvm-strip'), '--strip-debug', str(path)], check=True)
    copy(sdk/'target/bin/SDL2.dll', 'bin/SDL2.dll')
    shutil.copytree(sdk/'python', folder/'python')
    for pth in (folder/'python').glob('*._pth'):
        pth.write_text(pth.read_text()+'\n../scripts\n', encoding='utf-8')
    # Resolve all non-system PE dependencies, including Python extension DLLs.
    # UCRT/API-set and Vulkan come from the OS / user's graphics driver.
    system = set('advapi32 bcrypt cabinet cfgmgr32 comctl32 comdlg32 crypt32 dbghelp dinput8 dwmapi gdi32 hid imm32 iphlpapi kernel32 mpr netapi32 ntdll ole32 oleaut32 powrprof propsys psapi rpcrt4 secur32 setupapi shell32 shlwapi user32 userenv usp10 uxtheme version winhttp winmm winspool ws2_32 wtsapi32 vulkan-1 ucrtbase'.split())
    queue = [p for p in folder.rglob('*') if p.suffix.lower() in ('.exe', '.dll', '.pyd')]
    seen = set()
    while queue:
        path = queue.pop()
        if path in seen: continue
        seen.add(path)
        names = imports(path)
        dependencies[path.relative_to(folder).as_posix()] = names
        for dep in names:
            stem = dep.lower().removesuffix('.dll')
            if stem in system or stem.startswith(('api-ms-', 'ext-ms-')): continue
            present = next((p for p in path.parent.iterdir() if p.name.lower() == dep.lower()), None)
            if present: continue
            candidate = next((p for p in (llvm/'x86_64-w64-mingw32/bin').glob('*.dll') if p.name.lower() == dep.lower()), None)
            if candidate is None: raise ValueError(f'Unresolved Windows import: {path.name}: {dep}')
            queue.append(copy(candidate, path.parent.relative_to(folder)/candidate.name))
    copy('engine/third_party/SDL_GameControllerDB/gamecontrollerdb.txt', 'bin/gamecontrollerdb.txt')
    shaders = sorted((build_dir/'ps1Runtime/shaders').glob('*.spv'))
    if not shaders: raise ValueError('Missing Vulkan shaders')
    for path in shaders: copy(path, 'bin/shaders/'+path.name)
    for script in SCRIPTS: copy('scripts/'+script, 'scripts/'+script)
    for src, dest in LICENSES.items(): copy(src, dest)
    copy(sdk/'target/share/licenses/SDL2/LICENSE.txt', 'licenses/SDL2.txt')
    copy(llvm/'LICENSE.TXT', 'licenses/LLVM.txt')
    for license in (llvm/'x86_64-w64-mingw32/share/mingw32').glob('COPYING*'):
        copy(license, 'licenses/MinGW-'+license.name+'.txt')
    for src, dest in [('docs/mgs-windows.md', 'README.md'), ('docs/mgs-controllers.md', 'mgs-controllers.md'),
                      ('docs/mgs-savestates.md', 'mgs-savestates.md'), ('packaging/THIRD_PARTY_NOTICES.md', 'THIRD_PARTY_NOTICES.md'),
                      ('packaging/RELEASE_NOTES.md', 'RELEASE_NOTES.md'),
                      ('packaging/windows/dependencies.json', 'windows-dependencies.json')]:
        copy(src, dest)
    sources = source_files()
    source_name = name+'-source'
    source_archive = output/(source_name+'.tar.gz')
    with tarfile.open(source_archive, 'w:gz') as out:
        for path in sources: out.add(ROOT/path, arcname=source_name+'/'+path.as_posix(), recursive=False)
    source_hashes = {p.as_posix():digest(ROOT/p) for p in sources}
    release.update(git_commit=command('git','rev-parse','HEAD'), dirty_worktree=bool(command('git','status','--porcelain')),
        input_runtime_sha256=digest(build_dir/'ps1Runtime/ps1Runtime.exe'), runtime_sha256=digest(folder/'bin/mgs-runtime.exe'),
        windows_dependencies=json.loads((ROOT/'packaging/windows/dependencies.json').read_text()),
        pe_imports=dependencies, source_archive=dict(name=source_archive.name, sha256=digest(source_archive)),
        source_files_sha256=hashlib.sha256(json.dumps(source_hashes,sort_keys=True).encode()).hexdigest())
    atomic_json(folder/'build-info.json', release)
    files = {p.relative_to(folder).as_posix():digest(p) for p in sorted(folder.rglob('*')) if p.is_file()}
    atomic_json(folder/'package-manifest.json', dict(schema=1, game_data_included=False, files=files))
    audit(folder)
    archive = output/(name+'.zip')
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6, strict_timestamps=False) as out:
        for path in sorted(folder.rglob('*')):
            if path.is_file(): out.write(path, name+'/'+path.relative_to(folder).as_posix())
    (output/'SHA256SUMS').write_text(''.join(f'{digest(p)}  {p.name}\n' for p in (archive,source_archive)))
    atomic_json(output/'package-report.json', dict(schema=1, status='candidate-assembled', public_release_ready=False,
        archive=archive.name, source_archive=source_archive.name, files=len(files), shaders=len(shaders),
        runtime_sha256=release['runtime_sha256']))
    print(folder)
    return folder


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output', type=Path)
    p.add_argument('--build-dir', type=Path, default=ROOT/'build-windows')
    p.add_argument('--sdk', type=Path, default=ROOT/'.local/windows-sdk')
    a = p.parse_args()
    build(a.output, a.build_dir, a.sdk)
