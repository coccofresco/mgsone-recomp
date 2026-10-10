#!/usr/bin/env python3
"""Repeat verified native segments in ONE process with isolated saves/cards.

Verified adaptive routes (or recorded digital inputs) use the ordinary latch. This is an
endpoint/resource regression, not an exact TAS replay or campaign certificate.
--vulkan explicitly enables the original-asset presentation (physical or software Vulkan).
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import time
import tomllib

from mgs_tas import ROOT, sha256, write_json
from mgs_presentation_check import audit_metrics
from mgs_tas_import import BUTTONS
from mgs_tas_route import compile_route, snapshot


def vulkan_environment(cache, headless_surface=False, require_hardware=True):
    if not cache.is_file():
        raise ValueError(f'original-asset cache missing: {cache}')
    env = dict(PS1_MGS_ASSET_CACHE=str(cache.resolve()), PS1_MGS_MESH_REPLAY='1',
               PS1_MGS_ASSET_VERIFY='1', PS1_PRESENT_WINDOWED='1',
               PS1_VULKAN_HIDDEN_WINDOW='1')
    if headless_surface:
        env.update(PS1_VULKAN_HEADLESS_SURFACE='1', SDL_VIDEODRIVER='dummy')
    if require_hardware:
        env.update(PS1_VULKAN_REQUIRE_HARDWARE='1', PS1_VULKAN_GPU_TIMINGS='1',
                   PS1_VULKAN_REQUIRE_VALIDATION='1')
    return env


def movie_inputs(text):
    samples = [line.split() for line in text.splitlines() if line.startswith('F ')]
    if not samples:
        raise ValueError('movie has no input frames')
    ticks = [int(row[2]) for row in samples]
    if ticks != list(range(ticks[0], ticks[0] + len(ticks))):
        raise ValueError('movie VBlanks are not contiguous')
    intervals = []
    for port, column in ((0, 5), (1, 11)):
        if any(len(row) != 17 or row[column+5] != '65' or
               any(int(v) != 128 for v in row[column+1:column+5]) for row in samples):
            raise ValueError('soak replay currently requires neutral-axis digital pads')
        for bit, button in enumerate(BUTTONS):
            start = None
            for index, row in enumerate(samples + [None], 1):
                held = row is not None and not (int(row[column]) & (1 << bit))
                if held and start is None:
                    start = index
                elif not held and start is not None:
                    intervals.append(f'{start} {index-1} {button} {port}')
                    start = None
    return len(samples), '\n'.join(intervals)


def trend(values, allowance, slope_limit):
    """Ignore bounded warm-up; require both sustained slope and total growth."""
    first, last = statistics.median(values[:3]), statistics.median(values[-3:])
    slopes = [(values[j]-values[i])/(j-i) for i in range(len(values)) for j in range(i+1,len(values))]
    slope = statistics.median(slopes) if slopes else 0
    return dict(first=first, last=last, growth=last-first, slope_per_cycle=slope,
                passed=not (last-first > allowance and slope > slope_limit))


def analyze(samples, names, cycles, warmup, vulkan=False):
    rows, failures = [], []
    for index, name in enumerate(names):
        selected = [s for s in samples if s['state']['soak.case'] == index]
        observed = [s['state']['soak.cycle'] for s in selected]
        if sorted(observed) != list(range(cycles)):
            failures.append(f'{name}: incomplete or duplicate cycles')
            continue
        selected.sort(key=lambda s:s['state']['soak.cycle'])
        steady = [s['state'] for s in selected[warmup:]]
        if vulkan and any(s.get('soak.present_cpu_p95_us', -1) < 0 or
                          'host.vulkan_memory.live_bytes' not in s or
                          'host.vulkan_memory.live_allocations' not in s for s in steady):
            failures.append(f'{name}: missing Vulkan measurements')
        rss = trend([s['soak.rss_bytes'] for s in steady],32*1024**2,1024**2)
        resource = 'soak.open_handles' if all('soak.open_handles' in s for s in steady) else 'soak.open_fds'
        # Wine currently returns zero for GetProcessHandleCount. A process
        # running these probes has open handles; zero is not leak evidence.
        fds = ({'available':False,'passed':None,'reason':'Windows handle count unavailable (zero returned by host)'}
               if resource.endswith('handles') and any(s[resource]<=0 for s in steady)
               else trend([s[resource] for s in steady],3,.25))
        record = dict(name=name,rss=rss,**{'windows_handles' if resource.endswith('handles') else 'file_descriptors':fds})
        if not rss['passed'] or fds['passed'] is False:
            failures.append(f'{name}: sustained resource growth')
        render = [s['soak.present_cpu_p95_us'] for s in steady]
        record['presentation_cpu_p95_us'] = render
        if all(v >= 0 for v in render):
            baseline, ending = statistics.median(render[:3]), statistics.median(render[-3:])
            if ending > max(baseline*1.5,baseline+1000):
                failures.append(f'{name}: presentation CPU time degraded')
        for sample in selected:
            counters = sample['counters']
            if counters.get('host.savestate.failed',0):
                failures.append(f'{name}: failed save/load')
            if any(value for key,value in counters.items() if key.startswith('mips.')):
                failures.append(f'{name}: unexpected interpreter use')
            if any(counters.get('host.vulkan_memory.'+key,0) for key in
                   ('allocate_failed','duplicate_handle','untracked_free')):
                failures.append(f'{name}: Vulkan allocation error')
        record['host_interval_p95_us'] = [s['soak.host_interval_p95_us'] for s in steady]
        for field,allowance,slope in [('host.vulkan_memory.live_bytes',4*1024**2,65536),
                                      ('host.vulkan_memory.live_allocations',2,.1)]:
            if all(field in s for s in steady):
                record[field]=trend([s[field] for s in steady],allowance,slope)
                if not record[field]['passed']:failures.append(f'{name}: {field} increased')
        rows.append(record)
    return dict(passed=not failures,failures=sorted(set(failures)),cases=rows)


def run(args):
    output=args.output.resolve(); output.mkdir(parents=True,exist_ok=False)
    samples=output/'samples'; samples.mkdir()
    profile=output/'profile'; profile.mkdir()
    (profile/'cards').mkdir(); (profile/'states').mkdir()
    config=args.config.resolve(); template=config.read_text(); settings=tomllib.loads(template)
    if not all(k in settings.get('paths',{}) for k in ('memcard_dir','savestate_dir')):
        raise ValueError('config must declare save and card paths')
    for key,folder in (('memcard_dir','cards'),('savestate_dir','states')):
        template=re.sub(r'(?m)^'+key+r'\s*=.*$',lambda _:key+' = '+json.dumps(str(profile/folder)),template)
    runtime_config=profile/'runtime.toml';runtime_config.write_text(template)
    manifest=[f'cycles = {args.cycles}',f'output = {json.dumps(str(samples))}']
    sources={}; names=[]
    for index,session in enumerate(args.session):
        session=session.resolve()
        result=json.loads((session/'result.json').read_text())
        identity=json.loads((session/'manifest.json').read_text())
        if result.get('status')!='complete' or identity.get('status')!='complete' or result.get('game_errors') or result.get('interpreter_counters'):
            raise ValueError(f'not a successful native segment: {session}')
        for name,digest in identity['artifacts'].items():
            if sha256(session/name)!=digest: raise ValueError(f'changed segment artifact: {name}')
        state=session/'initial.ps1s';movie=session/'movie.mgst';ram=session/'final-ram.bin'
        for path in (state,movie,ram,session/'manifest.json',session/'result.json'):
            sources[str(path)]=sha256(path)
        frames,inputs=movie_inputs(movie.read_text())
        final=snapshot(ram.read_bytes())
        goal={k:final[k] for k in ('stage','current_stage','story','disc','difficulty','pause_level','game_over_timer')}
        goal['life']={'min':1}
        route=dict(format='mgs-tas-route-1',steps=[dict(name='endpoint',until=goal,max_frames=1)])
        endpoint=compile_route(route,[],{})
        name=f'segment-{index:02d}';names.append(name)
        manifest+=['[[cases]]',f'name = {json.dumps(name)}',f'state = {json.dumps(str(state))}',
                   f'frames = {frames}',f'inputs = {json.dumps(inputs)}',f'endpoint = {json.dumps(endpoint)}']
        route_file=session/'route.mgsr'
        if route_file.is_file():
            sources[str(route_file)]=sha256(route_file)
            manifest.append(f'route = {json.dumps(route_file.read_text())}')
        for slot in (1,2):
            card=session/f'initial_cards/card{slot}.mcd'
            sources[str(card)]=sha256(card)
            if index==0:shutil.copy2(card,profile/f'cards/card{slot}.mcd')
    manifest_path=output/'soak.toml';manifest_path.write_text('\n'.join(manifest)+'\n')
    runtime=args.runtime.resolve()
    env={k:v for k,v in os.environ.items() if not k.startswith(('PS1_','PS1RECOMP_'))}
    env.update(PS1_SOAK_MANIFEST=str(manifest_path),PS1_METRICS=str(output/'metrics.json'),
               PS1_DIAGNOSTIC_IGNORE_INPUT='1',PS1_VBLANK_PERIOD_US=str(args.period_us),
               PS1_MEMORY_CARD_DIR=str(profile/'cards'),MGS_CONTROLLER_CONFIG=str(profile/'controllers.toml'),
               SDL_AUDIODRIVER='dummy')
    if args.vulkan:
        env.update(vulkan_environment(args.asset_cache, args.headless_surface,
                                      require_hardware=not args.allow_software))
        sources[str(args.asset_cache.resolve())] = sha256(args.asset_cache)
    else:
        env.update(PS1_DIAGNOSTIC_HEADLESS='1',SDL_VIDEODRIVER='dummy')
    report=dict(format='mgs-soak-1',runtime=dict(path=str(runtime),sha256=sha256(runtime)),
                sources=sources,cycles=args.cycles,warmup=args.warmup,sessions=[str(s.resolve()) for s in args.session],
                vulkan=args.vulkan,acceptance=('diagnostic-only' if args.allow_software else 'hardware') if args.vulkan else 'gameplay-only',
                scope='One process; adaptive routes or recorded digital inputs, endpoint contracts and paused save/restore. No exact TAS or continuous campaign claim.')
    write_json(output/'report.json',report)
    started=time.monotonic();timed_out=False
    with (output/'runtime.log').open('w') as log:
        process=subprocess.Popen([str(runtime),'--config',str(runtime_config)],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT)
        try:code=process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out=True;process.terminate()
            try:code=process.wait(timeout=8)
            except subprocess.TimeoutExpired:process.kill();code=process.wait()
    measured=[json.loads(p.read_text()) for p in sorted(samples.glob('sample-*.json'))]
    report.update(returncode=code,timed_out=timed_out,elapsed_seconds=time.monotonic()-started)
    write_json(output/'report.json',report)
    analysis=analyze(measured,names,args.cycles,args.warmup,args.vulkan)
    text=(output/'runtime.log').read_text(errors='replace')
    game_errors=[line for line in text.splitlines() if
                 ('[Game]' in line and 'exception' in line.lower()) or '[DISPATCH] FATAL:' in line]
    passes=len(re.findall(r'^\[Soak\] PASS ',text,re.M))
    unchanged=all(sha256(Path(p))==digest for p,digest in sources.items())
    report.update(analysis,returncode=code,timed_out=timed_out,elapsed_seconds=time.monotonic()-started,
                  pause_save_restore_passes=passes,source_files_unchanged=unchanged,game_errors=game_errors)
    report['passed'] &= code==0 and not timed_out and unchanged and not game_errors and passes==args.cycles*len(names) and '[Shutdown] guest and ticker joined cleanly' in text
    if args.vulkan:
        metrics_path=output/'metrics.json'
        metrics=json.loads(metrics_path.read_text()) if metrics_path.exists() else {}
        counts=metrics.get('counters',{});state=metrics.get('state',{})
        report['asset_presentation_observed'] = (
            state.get('mgs.asset_cache.readback_verified') == 1 and
            counts.get('mgs.source.gpu_original_format_packets',0) > 0 and
            counts.get('mgs.source.gpu_full_faces',0) > 0)
        report['passed'] &= report['asset_presentation_observed']
        if not report['asset_presentation_observed']:
            report['failures'].append('no verified original-asset Vulkan submissions')
        presentation_failures = audit_metrics(metrics, require_hardware=not args.allow_software)
        if re.search(r'Validation Error|\(ERROR /|invalid_depth=[1-9]', text):
            presentation_failures.append('Vulkan validation/depth errors')
        report['failures'].extend(presentation_failures)
        report['passed'] &= not presentation_failures
    report['limits']=['RSS includes allocator/cache high-water marks; thresholds are regression budgets, not proof of zero leaks.',
                      'Presentation CPU duration is not GPU execution time. Allocation ledger covers renderer and asset cache, excluding driver/swapchain/ImGui private allocations.',
                      'Headless runs do not measure Vulkan resources or physical frame pacing.' if not args.vulkan else 'Actual device/driver acceptance must also inspect visual output.']
    write_json(output/'report.json',report)
    print(f"soak: {'PASS' if report['passed'] else 'FAIL'}; {passes}/{args.cycles*len(names)} segments; {output}",flush=True)
    return 0 if report['passed'] else 1


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--session',type=Path,action='append',required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--config',type=Path,required=True)
    p.add_argument('--runtime',type=Path,default=ROOT/'build/ps1Runtime/ps1Runtime')
    p.add_argument('--cycles',type=int,default=12)
    p.add_argument('--warmup',type=int,default=2)
    p.add_argument('--period-us',type=int,default=2000)
    p.add_argument('--timeout',type=int,default=1800)
    p.add_argument('--vulkan',action='store_true')
    p.add_argument('--asset-cache',type=Path,default=ROOT/'data/presentation/usa-source/assets.mgsc')
    p.add_argument('--headless-surface',action='store_true')
    p.add_argument('--allow-software',action='store_true',help='Vulkan diagnostics only; waive hardware acceptance')
    args=p.parse_args()
    if args.headless_surface and not args.vulkan:
        p.error('--headless-surface requires --vulkan')
    if args.allow_software and not args.vulkan:
        p.error('--allow-software requires --vulkan')
    if not 0<=args.warmup<=args.cycles-6 or not 6<=args.cycles<=1000:
        p.error('require at least six measured cycles after warm-up')
    return run(args)

if __name__=='__main__':raise SystemExit(main())
