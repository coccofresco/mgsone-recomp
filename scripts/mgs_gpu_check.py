#!/usr/bin/env python3
"""Run the captured asset matrix on a real Vulkan GPU, with no CPU fallback.

Run from the desktop terminal with access to the GPU and display. Uses isolated
saves/cards, records selected device/driver, GPU timestamps and 4K captures.
An environment failure remains a failure; there is no software retry.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import tomllib

from mgs_tas import ROOT, sha256, write_json
from mgs_presentation_check import audit


def desktop_environment(source):
    env = {k:v for k,v in source.items() if not k.startswith('PS1_')}
    # CPU ICD overrides left over from diagnostics must not select the driver.
    for key in ('VK_DRIVER_FILES', 'VK_ICD_FILENAMES', 'VK_ADD_DRIVER_FILES',
                'VK_LOADER_DRIVERS_SELECT', 'VK_LOADER_DRIVERS_DISABLE',
                'SDL_VIDEODRIVER', 'SDL_AUDIODRIVER'):
        env.pop(key, None)
    env['VK_INSTANCE_LAYERS'] = 'VK_LAYER_KHRONOS_validation'
    layers = ROOT/'data/validation/presentation/asset-path-20261007/layers'
    if 'VK_LAYER_PATH' not in env and layers.is_dir():
        env['VK_LAYER_PATH'] = str(layers)
    return env


def presentation_config(text, height):
    config = tomllib.loads(text)
    if config.get('game', {}).get('profile') != 'mgs_usa_v1_0':
        raise ValueError('hardware matrix requires the USA profile')
    if config.get('video', {}).get('backend') != 'vulkan':
        raise ValueError('hardware matrix requires Vulkan')
    for key, value in (('presentation_height', str(height)), ('fullscreen', 'false')):
        text, count = re.subn(r'(?m)^'+key+r'\s*=.*$', key+' = '+value, text)
        if count != 1:
            raise ValueError('expected one explicit setting: '+key)
    return text


def run(args):
    source = json.loads(args.evidence.read_text())
    prepared = []
    cache = ROOT/source['asset_cache']['path']
    if sha256(cache) != source['asset_cache']['sha256']:
        raise ValueError('source cache differs from the tested fixture matrix')
    # Prepare and verify all fixtures before starting any game process.
    for index, scene in enumerate(source['scenes']):
        receipt_path = ROOT/scene['session']['path']
        if sha256(receipt_path) != scene['session']['sha256']:
            raise ValueError('source receipt changed: '+str(receipt_path))
        session = receipt_path.parent
        manifest = json.loads((session/'manifest.json').read_text())
        initial = session/'initial.ps1s'
        if sha256(initial) != manifest['artifacts']['initial.ps1s']:
            raise ValueError('source checkpoint changed: '+str(initial))
        for name in ('initial_cards/card1.mcd', 'initial_cards/card2.mcd'):
            if sha256(session/name) != manifest['artifacts'][name]:
                raise ValueError('source memory card changed: '+str(session/name))
        identity = json.loads(receipt_path.read_text())['identity']['config']
        config = Path(identity['path'])
        if sha256(config) != identity['sha256']:
            raise ValueError('source configuration changed: '+str(config))
        prepared.append((f'{index+1:02d}-{session.name}', scene['label'], session,
                         presentation_config(config.read_text(), args.height)))
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    env = desktop_environment(os.environ)
    report = dict(format='mgs-hardware-presentation-1', status='running',
                  acceptance='hardware-required', source_evidence_sha256=sha256(args.evidence),
                  height=args.height, frames_per_scene=args.frames, scenes=[],
                  limits=['Short sampled windows, not continuous gameplay or complete visual acceptance.',
                          'GPU timestamps include diagnostic work; not an uninstrumented FPS benchmark.'])
    write_json(output/'report.json', report)
    for name, label, source_session, text in prepared:
        config = output/(name+'.toml')
        config.write_text(text)
        destination = output/name
        command = [sys.executable, str(ROOT/'scripts/mgs_tas.py'), 'record', str(destination),
                   '--state', str(source_session/'initial.ps1s'),
                   '--cards', str(source_session/'initial_cards'),
                   '--config', str(config), '--require-hardware', '--asset-cache', str(cache),
                   '--frames', str(args.frames), '--min-game-ticks', '3',
                   '--period-us', '16667', '--capture-every', str(max(1,args.frames//3)),
                   '--native-every', str(max(1,args.frames//3)), '--trace-presentation',
                   '--timeout', '240']
        if not args.show:
            command.append('--hidden')
        row = dict(label=label, command=command, source_checkpoint_sha256=sha256(source_session/'initial.ps1s'))
        report['scenes'].append(row)
        write_json(output/'report.json', report)
        print('GPU: '+label, flush=True)
        with (output/(name+'.log')).open('w') as log:
            result = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        row['returncode'] = result.returncode
        try:
            result_audit = audit(destination, args.height)
            write_json(output/(name+'-audit.json'), result_audit)
            row.update(passed=result_audit['passed'], failures=result_audit['failures'],
                       device=result_audit['device'], selected_device=result_audit['selected_device'],
                       gpu_timestamps=result_audit['gpu_timestamps'])
            if not result_audit['gpu_timestamps'].get('host.renderer.vulkan_gpu.total_samples'):
                row['passed'] = False
                row['failures'].append('missing hardware GPU timestamp samples')
        except (OSError, ValueError, KeyError) as error:
            row.update(passed=False, failures=[str(error)])
        if result.returncode or not row['passed']:
            report['status'] = 'failed'
            write_json(output/'report.json', report)
            print('GPU: FAIL — '+str(output/'report.json'), flush=True)
            return 1
        write_json(output/'report.json', report)
    # Eight save/restore cycles in one process; two warm-up, six measured.
    first = output/prepared[0][0]
    command = [sys.executable, str(ROOT/'scripts/mgs_soak.py'), '--session', str(first),
               '--output', str(output/'soak'), '--config', str(output/(prepared[0][0]+'.toml')),
               '--vulkan', '--asset-cache', str(cache), '--cycles', '8', '--warmup', '2',
               '--period-us', '16667', '--timeout', '600']
    with (output/'soak.log').open('w') as log:
        result = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    report['soak'] = dict(command=command, returncode=result.returncode)
    report['status'] = 'passed' if result.returncode == 0 else 'failed'
    write_json(output/'report.json', report)
    print('GPU: '+report['status'].upper()+' — '+str(output/'report.json'), flush=True)
    return 0 if report['status'] == 'passed' else 1


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--evidence', type=Path, default=ROOT/'docs/mgs-asset-presentation-evidence.json')
    p.add_argument('--output', type=Path, default=ROOT/'data/validation/presentation'/time.strftime('physical-gpu-%Y%m%d-%H%M%S'))
    p.add_argument('--height', type=int, choices=(720,1080,1440,2160), default=2160)
    p.add_argument('--frames', type=int, default=180)
    p.add_argument('--show', action='store_true', help='show the window during scene captures')
    args = p.parse_args()
    if args.frames < 12:
        p.error('at least 12 VBlanks per scene')
    try:
        return run(args)
    except (OSError, ValueError, KeyError) as error:
        print('GPU test: '+str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
