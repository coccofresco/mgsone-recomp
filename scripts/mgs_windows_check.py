#!/usr/bin/env python3
"""Exercise a Windows package on the physical GPU through Wine, with Linux fixtures.

Wine hides application Vulkan layers; this runner records that limitation rather
than calling the strict native-Windows validation-layer acceptance complete.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
from mgs_alpha import ROOT, digest, atomic_json
from mgs_presentation_check import audit_metrics
from mgs_tas import game_state


def win(path):
    return 'Z:'+str(Path(path).resolve()).replace('\\', '/')


def environment(prefix, host_validation=False):
    env = {k:v for k,v in os.environ.items() if not k.startswith(('PS1_', 'VK_', 'SDL_'))}
    env.update(WINEPREFIX=str(prefix.resolve()), WINEDEBUG='-all', SDL_AUDIODRIVER='dummy')
    # The host loader can use Linux validation, although Wine does not expose
    # those layers to vkEnumerateInstanceLayerProperties on the Windows side.
    layers = ROOT/'data/validation/presentation/asset-path-20261007/layers'
    if host_validation and layers.is_dir():
        env.update(VK_LAYER_PATH=str(layers), VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation',
                   VK_VALIDATION_VALIDATE_SYNC='1')
    return env


def configuration(source, destination, cards, states):
    text = source.read_text()
    text = re.sub(r'"(/[^"\n]*)"', lambda m:json.dumps(win(m[1])), text)
    for key,path in [('memcard_dir', cards), ('savestate_dir', states)]:
        text,n = re.subn(r'(?m)^'+key+r'\s*=.*$', lambda _:key+' = '+json.dumps(win(path)),text)
        if n != 1: raise ValueError('Missing explicit isolated path: '+key)
    destination.write_text(text)


def compare_reference(destination, reference):
    result=dict(compared=0,identical=0,mismatches=[],unmatched=[])
    for kind in ('native','gpu'):
        for path in sorted((destination/kind).glob('*.ppm')):
            original=reference/kind/path.name
            if not original.is_file():
                result['unmatched'].append(kind+'/'+path.name)
                continue
            result['compared']+=1
            if digest(path)==digest(original):result['identical']+=1
            else:result['mismatches'].append(kind+'/'+path.name)
    return result


def soak(args, output, runtime, env_base):
    from mgs_soak import analyze
    dest=output/'soak';dest.mkdir()
    (dest/'samples').mkdir();(dest/'states').mkdir()
    first=sorted(args.fixtures.glob('01-*/'))[0]
    shutil.copytree(first/'initial_cards',dest/'cards')
    config=dest/'runtime.toml'
    configuration(args.fixtures/(first.name+'.toml'),config,dest/'cards',dest/'states')
    template=(args.fixtures/'soak/soak.toml').read_text()
    template=re.sub(r'"(/[^"\n]*)"',lambda m:json.dumps(win(m[1])),template)
    template=re.sub(r'(?m)^output = .*$',lambda _: 'output = '+json.dumps(win(dest/'samples')),template)
    (dest/'soak.toml').write_text(template)
    env=dict(env_base, PS1_SOAK_MANIFEST=win(dest/'soak.toml'), PS1_METRICS=win(dest/'metrics.json'),
        PS1_VBLANK_PERIOD_US='16667', PS1_MGS_ASSET_CACHE=win(ROOT/'data/presentation/usa-source/assets.mgsc'),
        PS1_MGS_MESH_REPLAY='1',PS1_MGS_ASSET_VERIFY='1',PS1_VULKAN_HIDDEN_WINDOW='1',
        PS1_PRESENT_WINDOWED='1',PS1_VULKAN_REQUIRE_HARDWARE='1',PS1_VULKAN_GPU_TIMINGS='1')
    print('Windows: eight pause/save/restore cycles',flush=True)
    with (dest/'runtime.log').open('w') as log:
        result=subprocess.run([str(args.wine),str(runtime),'--config',win(config)],cwd=runtime.parent,
                              env=env,stdout=log,stderr=subprocess.STDOUT,timeout=600)
    samples=[json.loads(p.read_text()) for p in sorted((dest/'samples').glob('sample-*.json'))]
    report=analyze(samples,['segment-00'],8,2,True)
    text=(dest/'runtime.log').read_text(errors='replace')
    report['returncode']=result.returncode
    report['pause_save_restore_passes']=len(re.findall(r'^\[Soak\] PASS ',text,re.M))
    metrics=json.loads((dest/'metrics.json').read_text()) if (dest/'metrics.json').is_file() else {}
    report['failures']+=audit_metrics(metrics)
    report['passed'] &= (result.returncode==0 and report['pause_save_restore_passes']==8 and
        not report['failures'] and '[Shutdown] guest and ticker joined cleanly' in text)
    atomic_json(dest/'report.json',report)
    return report


def run(args):
    output = args.output.resolve()
    runtime = args.package.resolve()/'bin/mgs-runtime.exe'
    if args.soak_only:
        report=json.loads((output/'report.json').read_text())
        if report['runtime_sha256']!=digest(runtime):raise ValueError('Runtime changed since GPU matrix')
        report['soak']=soak(args,output,runtime,environment(args.prefix,args.host_validation))
        report['passed'] &= report['soak']['passed']
        atomic_json(output/'report.json',report)
        return 0 if report['passed'] else 1
    output.mkdir(parents=True, exist_ok=False)
    cache = ROOT/'data/presentation/usa-source/assets.mgsc'
    env_base = environment(args.prefix, args.host_validation)
    report = dict(schema=1, platform='windows-x86_64', execution='Wine on physical Vulkan GPU',
        native_windows_certified=False, application_validation_layer_available=False,
        host_validation_requested=args.host_validation,
        runtime_sha256=digest(runtime), asset_cache_sha256=digest(cache), scenes=[],
        limits=['Wine exposes no application Vulkan validation layers; native Windows validation remains required.',
                'Sampled scenarios and restores, not continuous campaign completion or physical-controller acceptance.'])
    for scene in sorted(args.fixtures.glob('[0-9][0-9]-*/')):
        if not (scene/'initial.ps1s').is_file(): continue
        dest = output/scene.name
        dest.mkdir()
        shutil.copytree(scene/'initial_cards', dest/'cards')
        (dest/'states').mkdir()
        shutil.copy2(scene/'initial.ps1s', dest/'states/quick_slot_0.ps1s')
        config = dest/'runtime.toml'
        configuration(args.fixtures/(scene.name+'.toml'), config, dest/'cards', dest/'states')
        env = dict(env_base, PS1_DIAGNOSTIC_SAVESTATE_DIR=win(dest/'states'),
            PS1_MEMORY_CARD_DIR=win(dest/'cards'), PS1_AUTOLOAD_VBLANK='1',
            PS1_TAS_RECORD=win(dest/'movie.mgst'), PS1_TAS_MAX_FRAMES=str(args.frames),
            PS1_TAS_CHECKPOINT_INTERVAL='60', PS1_METRICS=win(dest/'metrics.json'),
            PS1_RAM_DUMP_PATH=win(dest/'final-ram.bin'), PS1_VBLANK_PERIOD_US='16667',
            PS1_VULKAN_REQUIRE_HARDWARE='1', PS1_VULKAN_GPU_TIMINGS='1',
            PS1_MGS_ASSET_CACHE=win(cache), PS1_MGS_ASSET_VERIFY='1', PS1_MGS_MESH_REPLAY='1',
            PS1_VULKAN_HIDDEN_WINDOW='1', PS1_PRESENT_WINDOWED='1',
            PS1_VULKAN_REPLAY_READBACK='all', PS1_VULKAN_REPLAY_READBACK_INTERVAL=str(args.frames//3),
            PS1_VULKAN_REPLAY_READBACK_DUMP_DIRECTORY=win(dest/'gpu'),
            PS1_NATIVE_DISPLAY_DUMP_DIRECTORY=win(dest/'native'), PS1_NATIVE_DISPLAY_DUMP_FROM='0',
            PS1_NATIVE_DISPLAY_DUMP_TO=str(0xffffffff), PS1_NATIVE_DISPLAY_DUMP_STEP=str(args.frames//3))
        print('Windows GPU: '+scene.name, flush=True)
        started = time.monotonic()
        with (dest/'runtime.log').open('w') as log:
            result = subprocess.run([str(args.wine), str(runtime), '--config', win(config)],
                cwd=runtime.parent, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=240)
        text = (dest/'runtime.log').read_text(errors='replace')
        metrics = json.loads((dest/'metrics.json').read_text()) if (dest/'metrics.json').is_file() else {}
        failures = audit_metrics(metrics)
        counters, state = metrics.get('counters',{}), metrics.get('state',{})
        if result.returncode: failures.append('process return '+str(result.returncode))
        if state.get('host.renderer.vulkan_device_type') not in (1,2): failures.append('physical GPU not selected')
        if not counters.get('host.renderer.vulkan_gpu.total_samples'): failures.append('missing GPU timestamps')
        if re.search(r'Validation Error|\(ERROR /|invalid_depth=[1-9]|\[DISPATCH\] FATAL|\[Game\].*exception', text):
            failures.append('runtime/validation error')
        if '[Shutdown] guest and ticker joined cleanly' not in text: failures.append('missing clean shutdown')
        if any(v for k,v in counters.items() if k.startswith('mips.')): failures.append('interpreter fallback')
        movie = (dest/'movie.mgst').read_text() if (dest/'movie.mgst').is_file() else ''
        if len(re.findall(r'^F ',movie,re.M)) != args.frames or not re.search(r'^E ',movie,re.M):
            failures.append('incomplete native execution')
        captures=[]
        for path in sorted((dest/'gpu').glob('*-ordinary-scanout.ppm')):
            with path.open('rb') as stream:
                magic=stream.readline(); size=stream.readline(); maximum=stream.readline(); pixels=stream.read()
            if (magic,size,maximum) != (b'P6\n',b'3840 2160\n',b'255\n') or len(pixels)!=3840*2160*3 or not any(pixels):
                failures.append('invalid 4K capture '+path.name)
            captures.append(dict(path=str(path.relative_to(output)),sha256=digest(path)))
        if len(captures)!=3: failures.append('expected three 4K captures')
        ram = dest/'final-ram.bin'
        comparison=compare_reference(dest,scene)
        failures+=['Linux image mismatch: '+p for p in comparison['mismatches']]
        if args.frames==180 and (comparison['compared']!=10 or comparison['unmatched']):
            failures.append('missing Linux reference captures for the standard 180-frame matrix')
        row = dict(name=scene.name, passed=not failures, failures=failures, elapsed_seconds=time.monotonic()-started,
            linux_image_comparison=comparison,
            captures=captures, checkpoint_sha256=digest(scene/'initial.ps1s'), counters=counters,
            selected_device={k:v for k,v in state.items() if k.startswith('host.renderer.vulkan_')},
            game_state=game_state(ram.read_bytes()) if ram.is_file() else {})
        report['scenes'].append(row)
        report['passed']=len(report['scenes'])==8 and all(s['passed'] for s in report['scenes'])
        atomic_json(output/'report.json',report)
        if failures:
            print(json.dumps(failures));return 1
    report['soak']=soak(args,output,runtime,env_base)
    report['passed'] &= report['soak']['passed']
    atomic_json(output/'report.json',report)
    return 0 if report.get('passed') else 1


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--package',type=Path,required=True)
    p.add_argument('--wine',type=Path,required=True)
    p.add_argument('--prefix',type=Path,default=ROOT/'.local/wine-windows')
    p.add_argument('--fixtures',type=Path,default=ROOT/'data/validation/presentation/ssao-distance-20261008')
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--frames',type=int,default=180)
    p.add_argument('--host-validation',action='store_true',help='diagnostic: enable Linux layers below Wine; Wine internal calls may produce VUIDs')
    p.add_argument('--soak-only',action='store_true',help='append restore cycles to an existing matrix with the same runtime hash')
    a=p.parse_args()
    if a.frames<12 or a.frames%3:p.error('frames must be >=12 and divisible by three')
    raise SystemExit(run(a))
