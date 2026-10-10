#!/usr/bin/env python3
"""Build real AppImage/Flatpak candidates from an audited native package.

AppImage dependencies come from an explicit SDK root, never a sweep of the
developer's home. Every shipped ELF must satisfy the declared ABI ceiling.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

from mgs_alpha import ROOT, atomic_json, digest
from package_alpha import abi_requirements, audit

APP_ID='io.github.mgsrecomp.MGSRecomp'
SYSTEM_LIBS={'libc.so.6','libm.so.6','libdl.so.2','libpthread.so.0','librt.so.1',
             'libresolv.so.2','libutil.so.1','libanl.so.1','ld-linux-x86-64.so.2'}


def version(tag):
    return tuple(map(int,tag.rsplit('_',1)[-1].split('.')))


def check_abi(paths, ceiling):
    requirements={}
    for path in paths:
        with path.open('rb') as f:elf=f.read(4)==b'\x7fELF'
        if not elf:continue
        abi=abi_requirements(path);requirements[str(path)]=abi
        if abi['GLIBC'] and version(abi['GLIBC']) > version(ceiling):
            raise ValueError(f'{path.name} requires {abi["GLIBC"]}; target ceiling is GLIBC_{ceiling}. Rebuild/link against the baseline SDK.')
    return requirements


def needed(path):
    result=subprocess.check_output(['readelf','-d',str(path)],text=True)
    return re.findall(r'\(NEEDED\).*\[([^\]]+)\]',result)


def dependencies(seeds, sdk):
    roots=[sdk/'usr/lib/x86_64-linux-gnu',sdk/'lib/x86_64-linux-gnu',sdk/'usr/lib',sdk/'lib']
    found={};queue=list(seeds)
    while queue:
        p=queue.pop()
        for name in needed(p):
            if name in SYSTEM_LIBS or name in found:continue
            path=next((r/name for r in roots if (r/name).is_file()),None)
            if path is None:raise ValueError('Missing SDK runtime dependency: '+name)
            resolved=path.resolve()
            if not resolved.is_relative_to(sdk):raise ValueError('SDK symlink escapes its root: '+str(path))
            found[name]=resolved;queue.append(resolved)
    return found


def dependency_notices(sdk, paths, output):
    """Copy SDK distro copyright files for the selected packages only."""
    selected={str(p.relative_to(sdk)) for p in paths}
    selected|={'usr/bin/python3.13'}
    owners=set()
    for listing in (sdk/'var/lib/dpkg/info').glob('*.list'):
        entries={line.lstrip('/') for line in listing.read_text(errors='replace').splitlines()}
        if entries & selected:owners.add(listing.name[:-5].split(':')[0])
    output.mkdir(parents=True,exist_ok=True)
    copied=[]
    for owner in sorted(owners):
        source=sdk/'usr/share/doc'/owner/'copyright'
        if source.is_file():shutil.copy2(source,output/(owner+'.txt'));copied.append(owner)
    if not copied:raise ValueError('SDK copyright metadata is missing; provide a complete SDK root.')
    return copied


def copy_linux_metadata(prefix):
    for suffix,directory in (('desktop','applications'),('svg','icons/hicolor/scalable/apps'),('metainfo.xml','metainfo')):
        dest=prefix/'share'/directory/(APP_ID+'.'+suffix)
        dest.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(ROOT/'packaging/linux'/(APP_ID+'.'+suffix),dest)


def finish_report(out, artifact, root, kind, extra, release):
    files={p.relative_to(root).as_posix():digest(p) for p in sorted(root.rglob('*')) if p.is_file()}
    report=dict(schema=1,format=kind,artifact=artifact.name,sha256=digest(artifact),
                public_release_ready=release['public_release_ready'],
                validation_status=release.get('validation_status','unqualified'),
                game_data_included=False,files=files,**extra)
    atomic_json(out/(kind+'-report.json'),report)
    (out/(artifact.name+'.sha256')).write_text(digest(artifact)+'  '+artifact.name+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='files'},indent=2))


def appimage(package, out, sdk, appimagetool, runtime):
    sdk=sdk.resolve(strict=True)
    # Validate before creating a misleading .AppImage artifact.
    check_abi([package/'bin/mgs-runtime',package/'bin/mgs-frontend'],'2.39')
    appdir=out/'MGSRecomp.AppDir'
    if appdir.exists():raise ValueError('AppDir already exists; use a new output directory.')
    payload=appdir/'usr/share/mgs-recomp';payload.parent.mkdir(parents=True)
    shutil.copytree(package,payload)
    copy_linux_metadata(appdir/'usr')
    for suffix in ('desktop','svg'):
        shutil.copy2(ROOT/'packaging/linux'/(APP_ID+'.'+suffix),appdir/(APP_ID+'.'+suffix))
    shutil.copy2(ROOT/'packaging/appimage/AppRun',appdir/'AppRun');(appdir/'AppRun').chmod(0o755)
    python=sdk/'usr/bin/python3.13'
    stdlib=sdk/'usr/lib/python3.13'
    if not python.is_file() or not stdlib.is_dir():raise ValueError('SDK must contain Python 3.13 and its standard library.')
    (appdir/'usr/bin').mkdir();shutil.copy2(python,appdir/'usr/bin/python3')
    shutil.copytree(stdlib,appdir/'usr/lib/python3.13',ignore=shutil.ignore_patterns(
        '__pycache__','site-packages','dist-packages','test','tests','tkinter','idlelib','ensurepip',
        '_tkinter*','sitecustomize.py','usercustomize.py'))
    seeds=[payload/'bin/mgs-runtime',payload/'bin/mgs-frontend',appdir/'usr/bin/python3']
    seeds+=list((appdir/'usr/lib/python3.13').rglob('*.so'))
    deps=dependencies(seeds,sdk)
    abi=check_abi(seeds+list(deps.values()),'2.39')
    for name,p in deps.items():shutil.copy2(p,appdir/'usr/lib'/name)
    notices=dependency_notices(sdk,list(deps.values())+[python],appdir/'usr/share/doc/mgs-recomp/dependencies')
    # Driver implementations and glibc are deliberately supplied by SteamOS;
    # the application bundles Python, SDL and its ordinary dependency closure.
    artifact=out/'MGS-Recomp-x86_64.AppImage'
    env=dict(os.environ,ARCH='x86_64',APPIMAGE_EXTRACT_AND_RUN='1')
    subprocess.run([str(appimagetool.resolve()),'--runtime-file',str(runtime.resolve()),str(appdir),str(artifact)],env=env,check=True)
    finish_report(out,artifact,appdir,'appimage',dict(glibc_ceiling='2.39',dependency_packages=notices,
        checked_elf_files=len(abi),tool_sha256=digest(appimagetool),appimage_runtime_sha256=digest(runtime)),
        json.loads((package/'build-info.json').read_text()))


def flatpak(package,out,stage_only=False):
    check_abi([package/'bin/mgs-runtime',package/'bin/mgs-frontend'],'2.39')
    spec=json.loads((ROOT/'packaging/flatpak/permissions.json').read_text())
    directory=out/'flatpak-build';repo=out/'flatpak-repository'
    if directory.exists():raise ValueError('Flatpak staging directory exists; use a new output directory.')
    subprocess.run(['flatpak','build-init','--arch=x86_64',str(directory),APP_ID,
        spec['sdk'],spec['runtime'],spec['runtime-version']],check=True)
    prefix=directory/'files'
    dest=prefix/'share/mgs-recomp';dest.parent.mkdir(parents=True,exist_ok=True)
    shutil.copytree(package,dest);copy_linux_metadata(prefix)
    (prefix/'bin').mkdir(exist_ok=True)
    wrapper=prefix/'bin/mgs-recomp'
    wrapper.write_text('#!/bin/sh\nexport SDL_VIDEO_X11_WMCLASS=MGSRecomp\nexport SDL_VIDEO_WAYLAND_WMCLASS='+APP_ID+'\nexec python3 /app/share/mgs-recomp/scripts/mgs_alpha.py "$@"\n')
    wrapper.chmod(0o755)
    subprocess.run(['flatpak','build-finish','--command=mgs-recomp',*spec['finish-args'],str(directory)],check=True)
    if stage_only:
        # Standard export remains separate when the current environment cannot
        # run Flatpak's sandbox. Never suppress its validators to claim a bundle.
        script=out/'export-flatpak.sh'
        script.write_text('#!/bin/sh\nset -eu\ncd -- "$(dirname -- "$0")"\n'
            'flatpak build-export --arch=x86_64 flatpak-repository flatpak-build alpha\n'
            'flatpak build-bundle --arch=x86_64 '
            '--runtime-repo=https://flathub.org/repo/flathub.flatpakrepo '
            'flatpak-repository MGS-Recomp-x86_64.flatpak '+APP_ID+' alpha\n'
            'sha256sum MGS-Recomp-x86_64.flatpak > MGS-Recomp-x86_64.flatpak.sha256\n')
        script.chmod(0o755)
        files={p.relative_to(directory).as_posix():digest(p) for p in sorted(directory.rglob('*')) if p.is_file()}
        report=dict(schema=1,format='flatpak',status='staged-export-pending',artifact=None,
            public_release_ready=False,game_data_included=False,app_id=APP_ID,
            runtime=spec['runtime']+'/'+spec['runtime-version'],permissions=spec['finish-args'],
            files=files,export_script=script.name,export_script_sha256=digest(script))
        atomic_json(out/'flatpak-staging-report.json',report)
        print(json.dumps({k:v for k,v in report.items() if k!='files'},indent=2))
        return
    subprocess.run(['flatpak','build-export','--arch=x86_64',str(repo),str(directory),'alpha'],check=True)
    artifact=out/'MGS-Recomp-x86_64.flatpak'
    subprocess.run(['flatpak','build-bundle','--arch=x86_64','--runtime-repo=https://flathub.org/repo/flathub.flatpakrepo',
                    str(repo),str(artifact),APP_ID,'alpha'],check=True)
    finish_report(out,artifact,directory,'flatpak',dict(runtime=spec['runtime']+'/'+spec['runtime-version'],
                  app_id=APP_ID,permissions=spec['finish-args']),
                  json.loads((package/'build-info.json').read_text()))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('format',choices=['appimage','flatpak'])
    parser.add_argument('package',type=Path);parser.add_argument('output',type=Path)
    parser.add_argument('--sdk-root',type=Path)
    parser.add_argument('--appimagetool',type=Path);parser.add_argument('--appimage-runtime',type=Path)
    parser.add_argument('--stage-only',action='store_true',help='Flatpak only: prepare payload and standard export script, without claiming a bundle')
    args=parser.parse_args()
    try:
        package=args.package.resolve(strict=True);out=args.output.resolve()
        audit(package);out.mkdir(parents=True,exist_ok=True)
        if args.format=='flatpak':flatpak(package,out,args.stage_only)
        else:
            if args.stage_only:raise ValueError('--stage-only is available only for Flatpak.')
            if not all((args.sdk_root,args.appimagetool,args.appimage_runtime)):
                raise ValueError('AppImage requires --sdk-root, --appimagetool and --appimage-runtime.')
            appimage(package,out,args.sdk_root,args.appimagetool,args.appimage_runtime)
    except (OSError,ValueError,subprocess.CalledProcessError) as e:
        print('Portable package: '+str(e),file=sys.stderr);return 1
    return 0


if __name__=='__main__':sys.exit(main())
