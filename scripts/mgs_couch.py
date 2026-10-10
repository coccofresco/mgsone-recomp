#!/usr/bin/env python3
"""Controller-native launcher bridge. Private TOML IPC; no shell commands."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import tomllib

import mgs_alpha as alpha


def wait_for_controller_release(process, ready, timeout=3):
    deadline=time.monotonic()+timeout
    while process.poll() is None:
        if ready.is_file():return
        if time.monotonic()>=deadline:break
        time.sleep(.01)
    raise RuntimeError('Launcher did not release its controllers; game launch cancelled. Try again.')


def write_state(path, value):
    # Flat scalar TOML is also readable by the native launcher's toml11.
    text=''.join(k+' = '+json.dumps(v,ensure_ascii=True)+'\n' for k,v in value.items())
    fd,tmp=tempfile.mkstemp(dir=path.parent,prefix='.state-')
    try:
        with os.fdopen(fd,'w',encoding='utf-8') as out:out.write(text)
        os.replace(tmp,path)
    finally:Path(tmp).unlink(missing_ok=True)


def dispatch(paths, action, big_picture, progress):
    command=action.get('action')
    if command=='run':
        alpha.launch(paths,big_picture=big_picture,progress=progress)
    elif command=='import':
        alpha.import_discs(paths,[Path(action['disc1']),Path(action['disc2'])],progress)
    elif command=='settings':
        opts=alpha.settings(paths)
        for k in ('presentation_height','volume','fullscreen','vsync','mute','fast_loading','ambient_occlusion'):
            if k in action:opts[k]=action[k]
        alpha.atomic_json(paths.settings,alpha.validate_settings(opts))
        progress('Settings saved for the next launch.')
    elif command=='verify':
        alpha.installed(paths,full=True);progress('Installed discs and cache verified.')
    elif command in ('backup','diagnostics'):
        folder=paths.data/'exports';folder.mkdir(parents=True,exist_ok=True)
        output=folder/(command+'-'+time.strftime('%Y%m%d-%H%M%S')+'-'+os.urandom(3).hex()+'.zip')
        if command=='backup':alpha.backup_saves(paths,output)
        else:alpha.diagnostics(paths,output)
        progress('Created: '+str(output))
    else:
        raise ValueError('Unknown launcher action: '+str(command))


def run(paths, big_picture=False):
    root=alpha.ROOT
    frontend=root/('bin/mgs-frontend.exe' if os.name=='nt' else 'bin/mgs-frontend')
    if not frontend.exists():frontend=root/('build-windows/ps1Runtime/mgsFrontend.exe' if os.name=='nt' else 'build/ps1Runtime/mgsFrontend')
    if not frontend.is_file():raise RuntimeError('Build or install the native mgs-frontend launcher first.')
    paths.data.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.launcher-',dir=paths.data) as temporary:
        folder=Path(temporary);state=folder/'state.toml';request=folder/'action.toml'
        def update(message,busy=False,hide=False):
            opts=alpha.settings(paths)
            write_state(state,dict(status=message,busy=busy,hide=hide,installed=paths.installation.is_file(),
                data=str(paths.data),version=alpha.metadata()['version'],
                **{k:opts[k] for k in ('presentation_height','volume','fullscreen','vsync','mute','fast_loading','ambient_occlusion')}))
        update('Import both USA v1.0 discs, or play your installed game.')
        env=dict(os.environ,MGS_CONTROLLER_CONFIG=str(paths.config/'controllers.toml'))
        process=subprocess.Popen([str(frontend),str(state),str(request),'1' if big_picture else '0'],cwd=root,env=env,
                                 creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
        try:
            while process.poll() is None:
                if os.name=='nt':alpha.mgs_windows.check_parent()
                if not request.is_file():time.sleep(.05);continue
                # The native UI writes a temporary file and atomically renames
                # it; it remains disabled until this request is acknowledged.
                action=tomllib.loads(request.read_text(encoding='utf-8'));request.unlink()
                playing=action.get('action')=='run'
                ready=folder/'controllers-released'
                if playing:ready.unlink(missing_ok=True)
                update('Working...',True,playing)
                message=['Done.']
                def progress(text):
                    message[0]=text
                    update(text,True,playing)
                try:
                    if playing:wait_for_controller_release(process,ready)
                    dispatch(paths,action,big_picture,progress)
                except (OSError,ValueError,KeyError,RuntimeError) as e:message[0]=str(e)
                update(message[0])
            if process.returncode:raise RuntimeError(f'Native launcher exited with code {process.returncode}.')
        finally:
            if process.poll() is None:
                process.terminate()
                try:process.wait(timeout=3)
                except subprocess.TimeoutExpired:process.kill();process.wait()
