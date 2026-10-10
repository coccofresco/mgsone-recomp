#!/usr/bin/env python3
"""Run with bundled Windows Python against an imported, disposable profile."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def run(args):
    package=args.package.resolve();profile=args.profile.resolve()
    sys.path.insert(0,str(package/'scripts'))
    import mgs_alpha as alpha
    paths=alpha.user_paths(override=profile)
    # A real import/verify has already exercised the package with this profile.
    alpha.installed(paths,full=True)
    launcher=package/'MGSRecomp.exe'
    command=[str(launcher),'--user-dir',str(profile)]
    subprocess.run(command+['settings','--no-fullscreen','--height','720'],check=True)
    logs=profile/'logs'; logs.mkdir(exist_ok=True)
    receipt=logs/'last-session.json'
    receipt.unlink(missing_ok=True)
    before=set(logs.glob('session-*.log'))
    env=dict(os.environ,SDL_AUDIODRIVER='dummy')
    process=subprocess.Popen(command+['run'],env=env)
    started=time.monotonic()
    try:
        while True:
            new=set(logs.glob('session-*.log'))-before
            if new:
                log=next(iter(new))
                text=log.read_text(encoding='utf-8',errors='replace')
                if '[Input] keyboard + SDL GameController enabled' in text and 'Starting emulation' in text:
                    break
            if process.poll() is not None or time.monotonic()-started>90:
                raise RuntimeError('Packaged game did not reach live input')
            time.sleep(.1)
        # Terminate only the Steam-facing bootstrap, not its descendants.
        # Python must notice its parent exiting, signal the native game and
        # hold the profile lock until the native save flush completes.
        time.sleep(.5)
        process.kill();process.wait(timeout=5)
        deadline=time.monotonic()+15
        while not receipt.is_file() and time.monotonic()<deadline:time.sleep(.1)
        result=json.loads(receipt.read_text())
        assert result['returncode']==0 and result['termination_signal']==15,result
        text=log.read_text(encoding='utf-8',errors='replace')
        assert '[Shutdown] guest and ticker joined cleanly' in text,text[-3000:]
        with alpha.user_lock(paths):pass
        cards=list((profile/'memory_cards').glob('card[12].mcd'))
        assert len(cards)==2 and all(p.stat().st_size==131072 for p in cards)
        report=dict(passed=True,scope='actual packaged bootstrap death -> Python monitoring -> native stop -> card flush -> lock release',
                    session=result,cards={p.name:alpha.digest(p) for p in cards})
        alpha.atomic_json(args.report,report)
        print(json.dumps(report,indent=2))
    finally:
        if process.poll() is None:process.kill();process.wait()


if __name__=='__main__':
    if os.name!='nt':raise SystemExit('Run this check with the bundled Windows Python.')
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--package',type=Path,required=True)
    p.add_argument('--profile',type=Path,required=True)
    p.add_argument('--report',type=Path,required=True)
    run(p.parse_args())
