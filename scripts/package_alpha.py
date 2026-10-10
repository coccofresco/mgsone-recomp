#!/usr/bin/env python3
"""Assemble a reviewable Linux alpha candidate from explicit files only."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile

from mgs_alpha import ROOT, atomic_json, digest

SCRIPTS = ('mgs_alpha.py', 'mgs_couch.py', 'mgs_windows.py', 'mgs_assets.py', 'build_mgs_asset_cache.py')
LICENSES = {'LICENSE': 'LICENSE', 'engine/third_party/ELFIO/LICENSE.txt': 'licenses/ELFIO.txt',
            'engine/third_party/fmt/LICENSE': 'licenses/fmt.txt',
            'engine/third_party/imgui/LICENSE.txt': 'licenses/ImGui.txt',
            'engine/third_party/godot_ssao/LICENSE.txt': 'licenses/Godot.txt',
            'engine/third_party/godot_ssao/ASSAO-LICENSE.txt': 'licenses/Intel-ASSAO.txt',
            'engine/third_party/n8ao_radius/LICENSE': 'licenses/N8AO-CC0.txt',
            'engine/third_party/SDL_GameControllerDB/LICENSE.txt': 'licenses/SDL_GameControllerDB.txt',
            'engine/third_party/toml11/LICENSE': 'licenses/toml11.txt'}
PRIVATE_SUFFIXES = {'.bin','.cue','.iso','.img','.chd','.sub','.ps1s','.mcd','.mgsc','.xex','.rom'}
SOURCE_ROOTS = {'engine','scripts','config','docs','packaging','.github'}


def command(*args):
    return subprocess.check_output(args,cwd=ROOT,text=True,env=dict(os.environ,LC_ALL='C')).strip()


def abi_requirements(runtime):
    text=command('readelf','--version-info',str(runtime))
    text=text.split('Version needs section',1)[1] if 'Version needs section' in text else ''
    tags=set(re.findall(r'Name: ((?:GLIBC|GLIBCXX|CXXABI)_[0-9.]+)',text))
    def version(v):return tuple(map(int,v.split('_',1)[1].split('.')))
    return {prefix:max((t for t in tags if t.startswith(prefix+'_')),key=version,default=None)
            for prefix in ('GLIBC','GLIBCXX','CXXABI')}


def source_files():
    result=[]
    output=subprocess.check_output(['git','ls-files','-c','-o','--exclude-standard','-z'],cwd=ROOT)
    for name in sorted(set(output.decode().split('\0'))- {''}):
        p=Path(name)
        if p.parts[0] not in SOURCE_ROOTS and name not in ('README.md','LICENSE','.gitignore','.gitattributes'):
            continue
        path=ROOT/p
        if not path.is_file():continue
        generated = p.name in {'recompiled_out.cpp', 'recompiled_out.generated.h'} or p.name.startswith('recompiled_out.part')
        if (path.is_symlink() or p.suffix.lower() in PRIVATE_SUFFIXES or generated
                or any(x in p.parts for x in ('.local','savestates','__pycache__'))):
            raise ValueError('Unexpected private/generated source candidate: '+name)
        result.append(p)
    return result


def audit(folder):
    """Reject extra files, symlinks or content changes after manifest creation."""
    if any(p.is_symlink() for p in folder.rglob('*')):
        raise ValueError('Package contains a symlink.')
    data=json.loads((folder/'package-manifest.json').read_text())
    actual={p.relative_to(folder).as_posix() for p in folder.rglob('*') if p.is_file()}
    if actual != set(data['files'])|{'package-manifest.json'}:
        raise ValueError('Package contains missing or unlisted files.')
    for name,sha in data['files'].items():
        p=folder/name
        if p.is_symlink() or p.suffix.lower() in PRIVATE_SUFFIXES or digest(p)!=sha:
            raise ValueError('Package audit failed: '+name)
    return data


def build(output, build_dir=None):
    output=output.resolve();output.mkdir(parents=True,exist_ok=True)
    release=json.loads((ROOT/'packaging/alpha.json').read_text())
    build_dir=(build_dir or ROOT/'build').resolve()
    runtime=build_dir/'ps1Runtime/ps1Runtime'
    cache=(build_dir/'CMakeCache.txt').read_text()
    if 'PS1RECOMP_USE_STUB:BOOL=OFF' not in cache or not runtime.is_file():
        raise ValueError('Build a native (non-stub) runtime first.')
    with runtime.open('rb') as stream:
        elf_identity = stream.read(20)
    if elf_identity[:6] != b'\x7fELF\x02\x01' or elf_identity[18:20] != b'\x3e\x00':
        raise ValueError('Expected a 64-bit Linux ELF runtime.')
    name='mgs-recomp-'+release['version']+'-linux-x86_64'
    folder=output/name
    if folder.exists() or (output/(name+'.tar.gz')).exists():
        raise ValueError('Output already exists; use a new release staging directory.')
    folder.mkdir()
    def copy(source,dest):
        src=ROOT/source;target=folder/dest
        if src.is_symlink():raise ValueError('Refusing symlink package input: '+str(source))
        target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,target)
    copy(runtime,'bin/mgs-runtime')
    subprocess.run(['strip','--strip-debug',str(folder/'bin/mgs-runtime')],check=True)
    copy(build_dir/'ps1Runtime/mgsFrontend','bin/mgs-frontend')
    subprocess.run(['strip','--strip-debug',str(folder/'bin/mgs-frontend')],check=True)
    copy('engine/third_party/SDL_GameControllerDB/gamecontrollerdb.txt','bin/gamecontrollerdb.txt')
    shaders=build_dir/'ps1Runtime/shaders'
    shader_files=sorted(shaders.glob('*.spv'))
    if not shader_files:raise ValueError('Compiled SPIR-V shaders missing.')
    for p in shader_files:copy(p,'bin/shaders/'+p.name)
    for script in SCRIPTS:copy('scripts/'+script,'scripts/'+script)
    for src,dest in LICENSES.items():copy(src,dest)
    copy('docs/mgs-alpha-install.md','README.md')
    copy('docs/mgs-steamos.md','mgs-steamos.md')
    copy('docs/mgs-windows.md','mgs-windows.md')
    copy('docs/mgs-controllers.md','mgs-controllers.md')
    copy('docs/mgs-savestates.md','mgs-savestates.md')
    copy('docs/mgs-input-memory-card.md','mgs-input-memory-card.md')
    copy('docs/mgs-shutdown-saves-evidence.json','mgs-shutdown-saves-evidence.json')
    copy('packaging/THIRD_PARTY_NOTICES.md','THIRD_PARTY_NOTICES.md')
    copy('packaging/RELEASE_NOTES.md','RELEASE_NOTES.md')
    launcher=folder/'mgs-recomp'
    launcher.write_text('#!/usr/bin/env bash\nset -euo pipefail\napp_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"\nexec python3 "$app_root/scripts/mgs_alpha.py" "$@"\n')
    launcher.chmod(0o755)
    sources=source_files()
    source_hashes={p.as_posix():digest(ROOT/p) for p in sources}
    source_name='mgs-recomp-'+release['version']+'-source'
    source_archive=output/(source_name+'.tar.gz')
    if source_archive.exists():raise ValueError('Source archive already exists.')
    with tarfile.open(source_archive,'w:gz') as out:
        for p in sources:out.add(ROOT/p,arcname=source_name+'/'+p.as_posix(),recursive=False)
    release.update(git_commit=command('git','rev-parse','HEAD'),
                   dirty_worktree=bool(command('git','status','--porcelain')),
                   abi_requirements=abi_requirements(folder/'bin/mgs-runtime'),
                   runtime_sha256=digest(folder/'bin/mgs-runtime'),
                   input_runtime_sha256=digest(runtime),
                   source_archive=dict(name=source_archive.name,sha256=digest(source_archive)),
                   source_files_sha256=__import__('hashlib').sha256(json.dumps(source_hashes,sort_keys=True).encode()).hexdigest())
    atomic_json(folder/'build-info.json',release)
    files={p.relative_to(folder).as_posix():digest(p) for p in sorted(folder.rglob('*')) if p.is_file()}
    atomic_json(folder/'package-manifest.json',dict(schema=1,game_data_included=False,files=files))
    audit(folder)
    archive=output/(name+'.tar.gz')
    with tarfile.open(archive,'w:gz') as out:out.add(folder,arcname=name)
    (output/'SHA256SUMS').write_text(''.join(f'{digest(p)}  {p.name}\n' for p in (archive,source_archive)))
    atomic_json(output/'package-report.json',dict(schema=1,status='candidate-assembled',
      public_release_ready=release['public_release_ready'],
      validation_status=release.get('validation_status','unqualified'),archive=archive.name,
      source_archive=source_archive.name,files=len(files),shaders=len(shader_files),
      source_files=len(sources),abi_requirements=release['abi_requirements'],
      input_runtime_sha256=release['input_runtime_sha256'],runtime_sha256=release['runtime_sha256']))
    print(json.dumps(json.loads((output/'package-report.json').read_text()),indent=2))
    return folder


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output',type=Path)
    parser.add_argument('--audit',action='store_true',help='audit an already assembled package folder')
    parser.add_argument('--build-dir',type=Path,help='native build directory (default build)')
    args=parser.parse_args()
    if args.audit:audit(args.output);print('Package inventory and hashes verified.')
    else:build(args.output,args.build_dir)


if __name__=='__main__':main()
