#!/usr/bin/env python3
"""Prepare pinned Windows cross-build dependencies without system installation."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def prepare(sdk, jobs=4):
    sdk = sdk.resolve()
    downloads = sdk/'downloads'
    downloads.mkdir(parents=True, exist_ok=True)
    lock = json.loads((ROOT/'packaging/windows/dependencies.json').read_text())
    for key, item in lock.items():
        archive = downloads/item['archive']
        if not archive.exists():
            pending = archive.with_suffix(archive.suffix+'.part')
            with urllib.request.urlopen(item['url']) as src, pending.open('wb') as out:
                shutil.copyfileobj(src, out)
            pending.replace(archive)
        with archive.open('rb') as stream:
            actual = hashlib.file_digest(stream, 'sha256').hexdigest()
        if actual != item['sha256']:
            raise ValueError('Dependency hash mismatch: '+str(archive))
        destination = sdk/key
        if key != 'vulkan_def' and not destination.exists():
            destination.mkdir()
            if key == 'python':
                with zipfile.ZipFile(archive) as z:
                    z.extractall(destination)
            else:
                with tarfile.open(archive) as t:
                    t.extractall(destination, filter='data')
    toolchain = next((sdk/'toolchain').glob('*/bin'))
    target = sdk/'target'
    include = target/'include'
    include.mkdir(parents=True, exist_ok=True)
    headers = next((sdk/'vulkan_headers').glob('*/include'))
    for name in ('vulkan', 'vk_video'):
        shutil.copytree(headers/name, include/name, dirs_exist_ok=True)
    (target/'lib').mkdir(exist_ok=True)
    subprocess.run([str(toolchain/'llvm-dlltool'), '-m', 'i386:x86-64', '-d',
                    str(downloads/lock['vulkan_def']['archive']), '-l',
                    str(target/'lib/libvulkan-1.dll.a')], check=True)
    source = next((sdk/'sdl').glob('SDL2-*'))
    subprocess.run(['cmake', '-S', str(source), '-B', str(sdk/'sdl-build'), '-G', 'Ninja',
                    '-DCMAKE_TOOLCHAIN_FILE='+str(ROOT/'packaging/windows/toolchain.cmake'),
                    '-DMGS_WINDOWS_SDK='+str(sdk), '-DCMAKE_INSTALL_PREFIX='+str(target),
                    '-DCMAKE_BUILD_TYPE=Release', '-DSDL_SHARED=ON', '-DSDL_STATIC=OFF',
                    '-DSDL_TEST=OFF'], check=True)
    subprocess.run(['cmake', '--build', str(sdk/'sdl-build'), '--target', 'install',
                    '--parallel', str(jobs)], check=True)
    return sdk


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk', type=Path, default=ROOT/'.local/windows-sdk')
    p.add_argument('--jobs', type=int, default=4)
    a = p.parse_args()
    prepare(a.sdk, a.jobs)
