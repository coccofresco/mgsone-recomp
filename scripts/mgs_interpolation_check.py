#!/usr/bin/env python3
"""Verify source interpolation against the same TAS at a safe guest boundary.

Records with interpolation off and replays with it on, on the physical GPU.
Private states/cards only. Checks CPU state, RAM, scratchpad and raw 16-bit VRAM
(including mask bits); shutdown dumps without a TAS boundary are not sufficient.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from mgs_fixture import checkpoint_bytes, RAM_SIZE
from mgs_presentation_check import audit_metrics
from mgs_promo_capture import prepare, BINARY, ROOT
from mgs_tas import sha256


def audit_pair(base):
    failures, evidence, witnesses = [], {}, []
    for branch in ('off', 'on'):
        folder=base/branch
        metrics=json.loads((folder/'metrics.json').read_text())
        failures += [branch+': '+f for f in audit_metrics(metrics,require_hardware=True)]
        log=(folder/'runtime.log').read_text()
        if 'status: complete' not in log or '[Shutdown] guest and ticker joined cleanly' not in log:
            failures.append(branch+': no clean, completed TAS witness')
        if 'Validation Error' in log or '(ERROR /' in log:
            failures.append(branch+': Vulkan validation errors')
        if metrics['state'].get('mgs.interpolation.enabled') != int(branch=='on'):
            failures.append(branch+': interpolation mode differs')
        intermediates=metrics['counters'].get('mgs.interpolation.presented_intermediates',0)
        if (branch=='on' and intermediates<=0) or (branch=='off' and intermediates):
            failures.append(branch+': unexpected intermediate count')
        checkpoint=sorted((folder/'states').glob('checkpoint_*.ps1s'))[-1]
        raw=checkpoint.read_bytes();ram=(folder/'final-ram.bin').read_bytes()
        # Reuse the checked native-state decoder without changing any bytes.
        checkpoint_bytes(raw,ram,ram,'native')
        witnesses.append(raw)
        evidence[branch]={'intermediates':intermediates,'checkpoint':str(checkpoint),
            'checkpoint_sha256':sha256(checkpoint),'metrics_sha256':sha256(folder/'metrics.json'),
            'log_sha256':sha256(folder/'runtime.log')}
    for name,start,size in [('cpu',16,460),('ram',476,RAM_SIZE),
                            ('scratchpad',476+RAM_SIZE,1024),
                            ('vram_raw16',476+RAM_SIZE+1024,1048576)]:
        values=[w[start:start+size] for w in witnesses]
        identical=values[0]==values[1] and all(len(v)==size for v in values)
        evidence[name]={'identical':identical,'bytes':size,
            'sha256':[hashlib.sha256(v).hexdigest() for v in values]}
        if not identical: failures.append('guest '+name+' changed')
    return dict(format='mgs-interpolation-check-1',passed=not failures,failures=failures,
                evidence=evidence,limits='One deterministic route; not full-game or realtime performance certification.')


def run(args):
    base=args.output.resolve()
    for branch,enabled in [('off','0'),('on','1')]:
        folder,env=prepare(base/branch,args.state,args.disc)
        env.update(PS1_MGS_FRAME_INTERPOLATION=enabled,PS1_TAS_MAX_FRAMES=str(args.frames),
                   PS1_TAS_END_AT_CHECKPOINT='1',PS1_TAS_CHECKPOINT_INTERVAL=str(args.frames),
                   PS1_RAM_DUMP_PATH=str(folder/'final-ram.bin'))
        env['PS1_TAS_RECORD' if branch=='off' else 'PS1_TAS_REPLAY']=str(base/'off/movie.mgst')
        with (folder/'runtime.log').open('w') as log:
            subprocess.run([str(BINARY),'--config',str(folder/'runtime.toml')],cwd=ROOT,
                           env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=args.timeout)
    report=audit_pair(base)
    report.update(state_sha256=sha256(args.state),runtime_sha256=sha256(BINARY),requested_frames=args.frames)
    (base/'interpolation-check.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    return 0 if report['passed'] else 1


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--state',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
    p.add_argument('--frames',type=int,default=180);p.add_argument('--disc',type=int,choices=(1,2),default=1)
    p.add_argument('--timeout',type=int,default=300)
    a=p.parse_args()
    if a.frames<1 or a.timeout<1:p.error('frames and timeout must be positive')
    raise SystemExit(run(a))
