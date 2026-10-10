#!/usr/bin/env python3
"""Cross-build the current native game and Vulkan presentation for Windows x64."""
import argparse
from pathlib import Path
import shutil
import subprocess
from prepare_windows_sdk import ROOT, prepare


def build(sdk, directory, jobs=6, tests=False):
    sdk, directory = sdk.resolve(), directory.resolve()
    if not list((ROOT/'engine/ps1Runtime/src').glob('recompiled_out.part*.cpp')):
        raise ValueError('Generate the native game from your verified USA discs first; no stub release is allowed.')
    glslc = shutil.which('glslc')
    if not glslc:
        raise ValueError('Install the host glslc shader compiler first.')
    prepare(sdk, jobs)
    subprocess.run(['cmake', '-S', str(ROOT/'engine'), '-B', str(directory), '-G', 'Ninja',
        '-DCMAKE_TOOLCHAIN_FILE='+str(ROOT/'packaging/windows/toolchain.cmake'),
        '-DMGS_WINDOWS_SDK='+str(sdk), '-DCMAKE_BUILD_TYPE=Release',
        '-DPS1RECOMP_USE_STUB=OFF', '-DPS1RECOMP_DIAGNOSTIC_FAST_COMPILE=ON',
        '-DPS1RECOMP_BUILD_TESTS='+('ON' if tests else 'OFF'),
        '-DPS1RECOMP_VULKAN_HEADERS_ROOT='+str(sdk/'target/include'),
        '-DPS1RECOMP_VULKAN_LIBRARY='+str(sdk/'target/lib/libvulkan-1.dll.a'),
        '-DPS1RECOMP_GLSLC_EXECUTABLE='+glslc], check=True)
    targets=['ps1Runtime', 'mgsFrontend', 'MGSRecomp']
    if tests:targets+=['mgsWindows_tests','mgsController_tests']
    subprocess.run(['cmake', '--build', str(directory), '--parallel', str(jobs),
                    '--target', *targets], check=True)
    llvm=next((sdk/'toolchain').glob('*/bin')).parent
    for subdir in ['ps1Runtime']+(['ps1Test'] if tests else []):
        for source in (sdk/'target/bin/SDL2.dll',llvm/'x86_64-w64-mingw32/bin/libunwind.dll'):
            shutil.copy2(source,directory/subdir/source.name)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk', type=Path, default=ROOT/'.local/windows-sdk')
    p.add_argument('--build-dir', type=Path, default=ROOT/'build-windows')
    p.add_argument('--jobs', type=int, default=6)
    p.add_argument('--tests', action='store_true')
    a = p.parse_args()
    build(a.sdk, a.build_dir, a.jobs, a.tests)
