#!/usr/bin/env python3
"""Audit a captured mgs_tas session for actual original-asset Vulkan work.

Hardware acceptance requires the actual selected integrated/discrete GPU.
--allow-software is only for diagnostics, never hardware acceptance.
This checks execution and coverage, not artistic fidelity or physical GPU speed.
Run mgs_tas record with --asset-cache and --capture-every first. The software
WSI path additionally uses --headless-surface and a Vulkan CPU ICD selected by
VK_DRIVER_FILES. A successful headless gameplay session cannot pass this audit.
"""
import argparse
import json
from pathlib import Path
import re

from mgs_tas import sha256, write_json


GUARDS = ('mgs.material.missing_texture', 'mgs.pose.invalid_inputs',
          'mgs.source.unmapped_instances', 'mgs.source.invalid_ownership',
          'mgs.source.missing_geometry', 'mgs.source.missing_material',
          'mgs.source.changed_packet_faces', 'mgs.source.unknown_packet_faces',
          'mgs.source.material_bind_rejected', 'mgs.scene.recovery_material_rejected',
          'mgs.scene.recovery_invalid_list', 'mgs.screen_effect.incomplete_pass',
          'host.renderer.vulkan_replay_executor_downgrade',
          'host.renderer.vulkan_replay_route.m0_reanchor_incomplete',
          'host.renderer.vulkan_replay_route.m0_reanchor_unsupported',
          'host.renderer.vulkan_replay_route.m0_reanchor_precision',
          'host.renderer.vulkan_replay_readback.dump_failed',
          'host.renderer.vulkan_replay_readback.scanout_dump_failed',
          'host.renderer.vulkan_gpu_timing.query_failure',
          'host.vulkan_memory.allocate_failed', 'host.vulkan_memory.duplicate_handle',
          'host.vulkan_memory.untracked_free')


def hardware_failures(metrics):
    # VkPhysicalDeviceType: integrated=1, discrete=2, virtual=3, CPU=4.
    # Missing telemetry cannot certify old software-only receipts as hardware.
    state = metrics.get('state', {})
    failures = []
    if state.get('host.renderer.vulkan_device_type') not in (1, 2):
        failures.append('hardware GPU not verified: require selected integrated/discrete Vulkan device')
    if state.get('host.renderer.vulkan_validation_required') != 1:
        failures.append('hardware acceptance requires explicitly enabled Vulkan validation')
    return failures


def audit_metrics(metrics, require_hardware=False, require_mesh=True):
    c, s = metrics.get('counters', {}), metrics.get('state', {})
    failures = [name for name in GUARDS if c.get(name, 0)]
    if require_hardware:
        failures += hardware_failures(metrics)
    for name in ('mgs.source.gpu_original_format_packets', 'mgs.source.gpu_full_faces',
                 'mgs.source.visible_instance_captures'):
        if require_mesh and c.get(name, 0) <= 0:
            failures.append('missing ' + name)
    if s.get('mgs.asset_cache.readback_verified') != 1:
        failures.append('asset upload not readback-verified')
    if c.get('mgs.source.complete_instance_captures', 0) != c.get('mgs.source.visible_instance_captures', 0):
        failures.append('incomplete visible instances')
    if s.get('mgs.pose.invalid_inputs', 0):
        failures.append('invalid pose')
    if require_mesh and s.get('mgs.ssao.enabled') and c.get('mgs.ssao.resolves', 0) <= 0:
        failures.append('enabled ambient occlusion was never resolved on GPU')
    return failures


def ppm_info(path):
    # Renderer emits this exact binary PPM format. Reject partial writes.
    with path.open('rb') as f:
        if f.readline() != b'P6\n':
            raise ValueError('not a renderer PPM')
        width, height = map(int, f.readline().split())
        if f.readline() != b'255\n' or width <= 0 or height <= 0:
            raise ValueError('invalid PPM header')
        data = f.read()
    if len(data) != width * height * 3:
        raise ValueError('incomplete PPM pixels')
    return dict(width=width, height=height, nonblack=any(data), sha256=sha256(path))


def audit(session, height, require_hardware=True):
    failures = []
    receipt = json.loads((session/'result.json').read_text())
    manifest = json.loads((session/'manifest.json').read_text())
    if receipt.get('status') != 'complete' or manifest.get('status') != 'complete':
        failures.append('incomplete game session')
    if receipt.get('game_errors') or receipt.get('interpreter_counters'):
        failures.append('game errors or interpreter fallback')
    for name, digest in manifest.get('artifacts', {}).items():
        if not (session/name).is_file() or sha256(session/name) != digest:
            failures.append('modified session artifact: '+name)
    metrics_path = session/'metrics.json'
    metrics = json.loads(metrics_path.read_text()) if metrics_path.exists() else {}
    failures += audit_metrics(metrics, require_hardware=require_hardware)
    log = (session/'runtime.log').read_text(errors='replace')
    if re.search(r'Validation Error|\(ERROR /|invalid_depth=[1-9]', log):
        failures.append('Vulkan validation/depth errors')
    if '[Shutdown] guest and ticker joined cleanly' not in log:
        failures.append('missing clean shutdown')
    captures = []
    for path in sorted((session/'gpu').glob('*-scanout.ppm')):
        record = dict(path=str(path.resolve()), **ppm_info(path))
        if (record['width'], record['height']) != (height*16//9, height):
            failures.append('wrong presentation resolution: '+path.name)
        captures.append(record)
    if not captures or not any(p['nonblack'] for p in captures):
        failures.append('missing nonblack Vulkan scanout')
    c = metrics.get('counters', {})
    devices = re.findall(r'\[RendererVK\] Vulkan ([^\n]+)', log)
    state = metrics.get('state', {})
    return dict(format='mgs-asset-presentation-check-2', session=str(session.resolve()),
                passed=not failures, failures=failures, device=devices,
                acceptance='hardware' if require_hardware else 'diagnostic-only',
                selected_device={k:state.get('host.renderer.vulkan_'+k) for k in
                                 ('device_type','vendor_id','device_id','driver_version','api_version')},
                gpu_timestamps={k:v for k,v in c.items() if k.startswith('host.renderer.vulkan_gpu.')},
                runtime=receipt.get('identity', {}).get('runtime'),
                metrics_sha256=sha256(metrics_path) if metrics_path.exists() else None,
                game_state=receipt.get('game_state'), captures=captures,
                counters={k:v for k,v in c.items() if k.startswith(('mgs.', 'host.vulkan_memory.'))},
                guard_counts={k:c.get(k, 0) for k in GUARDS},
                reanchors=c.get('host.renderer.vulkan_replay_route.m0_reanchor', 0),
                limits=['Counter totals are face/instance occurrences across frames, not unique assets.',
                        'Complete CPU preparation and successful Vulkan submissions do not certify every pixel.',
                        'Short captured checkpoint run; no continuous campaign or physical GPU performance claim.'])


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('session',type=Path)
    p.add_argument('--height',type=int,choices=(720,1080,1440,2160),required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--allow-software',action='store_true',help='diagnostic result only; does not certify hardware acceptance')
    args=p.parse_args()
    result=audit(args.session,args.height,require_hardware=not args.allow_software)
    write_json(args.output,result)
    print(json.dumps({k:result[k] for k in ('passed','failures','device')},indent=2))
    return 0 if result['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
