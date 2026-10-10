#!/usr/bin/env python3
"""First-run import, user settings and launch for the MGS alpha candidate.

The distributable has no discs, executable image, asset cache or user saves.
All game data is verified and prepared locally from the user's USA v1.0 pair.
"""
import argparse
from contextlib import contextmanager
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import zipfile
# The couch frontend and Windows lifetime monitor import this same module.
# Keep exception and process ownership identical when invoked as a script.
if __name__ == '__main__':
    sys.modules['mgs_alpha'] = sys.modules[__name__]
if os.name == 'nt':
    import mgs_windows
else:
    import fcntl


class LauncherTermination(BaseException):
    def __init__(self, signum):self.signum=signum


_runtime_processes=set()


@contextmanager
def appimage_parent_lifetime():
    # Some type-2 extract-and-run runtimes wait in an outer process without
    # forwarding termination. Arm only when our actual parent is that exact
    # AppImage executable, never for an ordinary shell/desktop/Steam parent.
    if os.name == 'nt':
        with mgs_windows.parent_lifetime(): yield
        return
    parent=os.getppid(); image=os.environ.get('APPIMAGE')
    try:is_wrapper=bool(image) and os.path.samefile(f'/proc/{parent}/exe',image)
    except OSError:is_wrapper=False
    if not is_wrapper:
        yield;return
    import ctypes
    libc=ctypes.CDLL(None,use_errno=True)
    prctl=libc.prctl
    prctl.argtypes=[ctypes.c_int,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_ulong]
    prctl.restype=ctypes.c_int
    previous=ctypes.c_int()
    pointer=ctypes.cast(ctypes.byref(previous),ctypes.c_void_p).value
    if prctl(2,pointer,0,0,0) or prctl(1,int(signal.SIGTERM),0,0,0):
        raise OSError(ctypes.get_errno(),'Cannot supervise AppImage wrapper termination')
    try:
        if os.getppid()!=parent:raise LauncherTermination(signal.SIGTERM)
        yield
    finally:prctl(1,previous.value,0,0,0)


def stop_process(process):
    if process.poll() is None:
        if os.name == 'nt':mgs_windows.request_shutdown(process)
        else:process.terminate()
        try:process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill();process.wait()


@contextmanager
def termination_signals():
    # Entry-point scope includes the native frontend's own finally cleanup.
    # BaseException bypasses normal UI error reporting and closes the session.
    def terminate(signum, _frame):raise LauncherTermination(signum)
    previous={s:signal.signal(s,terminate) for s in (signal.SIGTERM,signal.SIGINT)}
    try:yield
    finally:
        # A second termination request cannot interrupt child reaping/flush.
        for s in previous:signal.signal(s,signal.SIG_IGN)
        try:
            for process in list(_runtime_processes):stop_process(process)
        finally:
            for s,handler in previous.items():signal.signal(s,handler)


def run_runtime(command, **kwargs):
    process=(mgs_windows.spawn_runtime(command,**kwargs) if os.name=='nt'
             else subprocess.Popen(command,**kwargs))
    _runtime_processes.add(process)
    try:
        requested_signal=None
        try:
            if os.name=='nt':
                while process.poll() is None:
                    mgs_windows.check_parent()
                    try:process.wait(timeout=.1)
                    except subprocess.TimeoutExpired:pass
            else:process.wait()
        except LauncherTermination as request:
            requested_signal=request.signum
            previous={s:signal.signal(s,signal.SIG_IGN) for s in (signal.SIGTERM,signal.SIGINT)}
            try:
                if process.poll() is None:
                    if os.name=='nt':mgs_windows.request_shutdown(process)
                    else:process.send_signal(request.signum)
                try:process.wait(timeout=5)
                except subprocess.TimeoutExpired:process.kill();process.wait()
            finally:
                for s,handler in previous.items():signal.signal(s,handler)
        completed=subprocess.CompletedProcess(command,process.returncode)
        completed.termination_signal=requested_signal
        return completed
    finally:
        # Keep the profile lock until the native cancellation and join finish.
        previous={}
        import threading
        if threading.current_thread() is threading.main_thread():
            previous={s:signal.signal(s,signal.SIG_IGN) for s in (signal.SIGTERM,signal.SIGINT)}
        try:stop_process(process)
        finally:
            _runtime_processes.discard(process)
            if os.name=='nt':mgs_windows.close_runtime(process)
            for s,handler in previous.items():signal.signal(s,handler)

from mgs_assets import Disc
from build_mgs_asset_cache import build as build_cache

ROOT = Path(__file__).resolve().parents[1]
EXE_SHA = '3342f75717c5134aec832547691c2354e67a253abfcfa9e0e823e2133bb1fc32'
TRACK_SHA = (
    'af59d9b247489d4bb4be4da30b5cdd1d236d0d2052b78dd3cd14b8656578e141',
    'd213ea8be0379c823d176ca3d23d59b9a3e1defe4ddf2c55d79a6753caaaba94',
)
HEIGHTS = (720, 1080, 1440, 2160)
BUTTONS = {'up','down','left','right','cross','circle','square','triangle',
           'l1','l2','r1','r2','start','select'}
KEY_NAMES = {'up','down','left','right','return','backspace','space','tab',
             'left_shift','right_shift','left_ctrl','right_ctrl','left_alt','right_alt',
             'home','end','pageup','pagedown','insert','delete'} | set('abcdefghijklmnopqrstuvwxyz0123456789')
BINDINGS = dict(up='up', down='down', left='left', right='right', z='cross',
                x='circle', a='square', s='triangle', q='l1', w='r1',
                e='l2', r='r2', return_='start', backspace='select')
BINDINGS['return'] = BINDINGS.pop('return_')
DEFAULTS = dict(schema=1, presentation_height=2160, fullscreen=False, vsync=False,
                volume=100, mute=False, fast_loading=True, ambient_occlusion=True, bindings=BINDINGS)


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='.' + path.name, dir=path.parent)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as out:
            json.dump(value, out, indent=2, ensure_ascii=False)
            out.write('\n')
            out.flush()
            os.fsync(out.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


@dataclass(frozen=True)
class UserPaths:
    data: Path
    config: Path

    @property
    def settings(self):
        return self.config / 'settings.json'

    @property
    def installation(self):
        return self.data / 'installation.json'


def user_paths(root=ROOT, override=None, environ=None):
    env = os.environ if environ is None else environ
    if override is not None:
        data = Path(override).expanduser().resolve()
        return UserPaths(data, data / 'config')
    # AppImages mount read-only at a transient path. A portable profile must
    # live beside the actual .AppImage, never inside that mount.
    appimage=Path(env.get('APPIMAGE',''))
    if appimage.is_absolute() and (appimage.parent/'portable.txt').is_file():
        data=appimage.parent/'mgs-recomp-userdata'
        return UserPaths(data,data/'config')
    if (root / 'portable.txt').is_file():
        return UserPaths(root / 'userdata', root / 'userdata/config')
    if os.name == 'nt':
        local = Path(env.get('LOCALAPPDATA') or Path.home()/'AppData/Local')/'MGSRecomp'
        roaming = Path(env.get('APPDATA') or Path.home()/'AppData/Roaming')/'MGSRecomp'
        return UserPaths(local, roaming)
    # XDG relative values are invalid; never interpret them relative to CWD.
    def base(key, default):
        path = Path(env.get(key) or default)
        return path if path.is_absolute() else Path(default)
    return UserPaths(base('XDG_DATA_HOME', str(Path.home()/'.local/share'))/'mgs-recomp',
                     base('XDG_CONFIG_HOME', str(Path.home()/'.config'))/'mgs-recomp')


@contextmanager
def user_lock(paths):
    paths.data.mkdir(parents=True, exist_ok=True)
    if os.name == 'nt':
        with mgs_windows.profile_lock(paths.data/'session.lock') as lock:yield lock
        return
    with (paths.data / 'session.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError('MGS is already running or importing files in this profile.') from error
        yield lock


def validate_settings(value):
    # Schema 1 profiles written before SSAO keep all preferences; the new
    # presentation toggle receives its default without rewriting the file.
    if isinstance(value, dict) and set(value) == set(DEFAULTS) - {'ambient_occlusion'}:
        value = dict(value, ambient_occlusion=DEFAULTS['ambient_occlusion'])
    if (not isinstance(value, dict) or set(value) != set(DEFAULTS) or
            type(value.get('schema')) is not int or value['schema'] != 1):
        raise ValueError('Unknown settings format; preserve the file and use the documented schema.')
    if type(value['presentation_height']) is not int or value['presentation_height'] not in HEIGHTS:
        raise ValueError('Presentation height must be 720, 1080, 1440 or 2160.')
    if type(value['volume']) is not int or not 0 <= value['volume'] <= 100:
        raise ValueError('Volume must be between 0 and 100.')
    for key in ('fullscreen', 'vsync', 'mute', 'fast_loading', 'ambient_occlusion'):
        if type(value[key]) is not bool:
            raise ValueError(f'{key} must be true or false.')
    if not isinstance(value['bindings'], dict) or not value['bindings']:
        raise ValueError('At least one keyboard binding is required.')
    for key, button in value['bindings'].items():
        if (not isinstance(key, str) or key not in KEY_NAMES
                or not isinstance(button, str) or button not in BUTTONS):
            raise ValueError(f'Invalid or reserved keyboard binding: {key} = {button}')
    return value


def settings(paths):
    value = json.loads(paths.settings.read_text()) if paths.settings.exists() else json.loads(json.dumps(DEFAULTS))
    return validate_settings(value)


def track_path(source):
    source = Path(source).expanduser().resolve(strict=True)
    if source.suffix.lower() == '.bin':
        return source
    if source.suffix.lower() != '.cue':
        raise ValueError('Select a BIN image or its CUE (USA v1.0). Extract archives first.')
    text = source.read_text(encoding='utf-8-sig')
    files = re.findall(r'^\s*FILE\s+"([^"]+)"\s+BINARY\s*$', text, re.M | re.I)
    tracks = re.findall(r'^\s*TRACK\s+(\d+)\s+(\S+)\s*$', text, re.M | re.I)
    indices = re.findall(r'^\s*INDEX\s+(\d+)\s+(\S+)\s*$', text, re.M | re.I)
    if (len(files) != 1 or tracks != [('01', 'MODE2/2352')] or indices != [('01', '00:00:00')]
            or len(re.findall(r'^\s*FILE\b', text, re.M | re.I)) != 1
            or re.search(r'^\s*(PREGAP|POSTGAP)\b', text, re.M | re.I)):
        raise ValueError('This alpha accepts a single MODE2/2352 data track starting at 00:00:00.')
    return (source.parent / files[0]).resolve(strict=True)


def installed(paths, full=False):
    if not paths.installation.is_file():
        raise ValueError('Import both USA v1.0 discs first.')
    record = json.loads(paths.installation.read_text())
    if record.get('schema') != 1 or not re.fullmatch(r'usa-v1-[a-z0-9_]+', record.get('directory', '')):
        raise ValueError('Invalid installation record.')
    folder = paths.data / 'library' / record['directory']
    expected = {'disc1.bin', 'disc2.bin', 'disc1.cue', 'disc2.cue', 'SLUS_005.94', 'assets.mgsc'}
    if set(record.get('files', {})) != expected:
        raise ValueError('Incomplete installation record.')
    for name, info in record['files'].items():
        p = folder / name
        if p.is_symlink() or not p.is_file() or p.stat().st_size != info['size']:
            raise ValueError(f'Installed file missing or changed: {name}. Reimport the discs.')
        # Small boot/control files are checked every launch. Full disc/cache
        # hashing is available explicitly without adding startup loading time.
        if (full or name in {'disc1.cue','disc2.cue','SLUS_005.94'}) and digest(p) != info['sha256']:
            raise ValueError(f'Installed file integrity check failed: {name}. Reimport the discs.')
    if record['files']['SLUS_005.94']['sha256'] != EXE_SHA or any(
            record['files'][f'disc{i+1}.bin']['sha256'] != sha for i,sha in enumerate(TRACK_SHA)):
        raise ValueError('Installation does not identify the supported USA v1.0 disc pair.')
    return folder, record


def import_discs(paths, sources, progress=print):
    with user_lock(paths):
        tracks = [track_path(p) for p in sources]
        library = paths.data / 'library'
        library.mkdir(exist_ok=True)
        needed = sum(p.stat().st_size for p in tracks) + 256 * 1024 * 1024
        if shutil.disk_usage(library).free < needed:
            raise ValueError('Not enough free space for both discs and the presentation cache.')
        # Nothing in an existing installation/save profile is replaced until
        # both hashes, original executable and the complete cache are valid.
        with tempfile.TemporaryDirectory(prefix='usa-v1-', dir=library) as tmp:
            stage = Path(tmp)
            for i, source in enumerate(tracks, 1):
                progress(f'Importing and checking disc {i}/2…')
                target = stage / f'disc{i}.bin'
                sha = hashlib.sha256()
                with source.open('rb') as src, target.open('xb') as dst:
                    for block in iter(lambda: src.read(4*1024*1024), b''):
                        dst.write(block)
                        sha.update(block)
                if sha.hexdigest() != TRACK_SHA[i-1]:
                    raise ValueError(f'Disc {i} does not match USA v1.0. Check disc order, region and revision.')
                (stage/f'disc{i}.cue').write_text(f'FILE "disc{i}.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
            progress('Preparing original assets for Vulkan…')
            disc = Disc(stage/'disc1.bin')
            entry = disc.files()['/MGS/SLUS_005.94']
            exe = disc.read(entry['lba'], entry['size'])
            if hashlib.sha256(exe).hexdigest() != EXE_SHA:
                raise ValueError('Unsupported game executable.')
            (stage/'SLUS_005.94').write_bytes(exe)
            build_cache([stage/'disc1.bin', stage/'disc2.bin'], stage)
            files = {p.name: dict(size=p.stat().st_size, sha256=digest(p)) for p in stage.iterdir()
                     if p.name in {'disc1.bin','disc2.bin','disc1.cue','disc2.cue','SLUS_005.94','assets.mgsc'}}
            # Remove the temporary builder report with staging paths. Retail
            # bytes stay in the library; only hashes enter the install record.
            (stage/'manifest.json').unlink()
            final = library / ('usa-v1-' + os.urandom(8).hex())
            stage.rename(final)
            atomic_json(paths.installation, dict(schema=1, directory=final.name, files=files))
        if not paths.settings.exists():
            atomic_json(paths.settings, DEFAULTS)
        progress('Import complete. Existing memory cards, states and settings were preserved.')
        return final


def quoted(value):
    # The runtime's lightweight [paths] parser currently handles plain quoted
    # strings. Refuse unsupported path escaping rather than redirecting saves.
    value = str(value)
    if any(c in value for c in ('"','\\')) or any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise ValueError('The alpha user-data path cannot contain quotes, backslashes or control characters.')
    return '"' + value + '"'


def runtime_config(paths, media, options):
    options = validate_settings(options)
    sections = dict(
      disc=dict(exe_path=(media/'SLUS_005.94').as_posix(), cue_path=(media/'disc1.cue').as_posix(),
                additional_cues=[(media/'disc2.cue').as_posix()]),
      game=dict(profile='mgs_usa_v1_0', widescreen_16_9=True, fast_loading=options['fast_loading'],
                presentation_height=options['presentation_height'], ambient_occlusion=options['ambient_occlusion']),
      timing=dict(video_standard='ntsc'),
      video=dict(backend='vulkan',width=1280,height=720,fullscreen=options['fullscreen'],
                 vsync=options['vsync'],res_scale=1,pgxp=False,true_depth=False,true_color=False,preserve_aspect=True),
      audio=dict(volume=options['volume'],mute=options['mute']),
      input=dict(analog=False, **options['bindings']),
      paths=dict(memcard_dir=(paths.data/'memory_cards').as_posix(),savestate_dir=(paths.data/'savestates').as_posix()))
    def encode(v):
        if isinstance(v,bool):return str(v).lower()
        if isinstance(v,int):return str(v)
        if isinstance(v,list):return '[' + ', '.join(quoted(x) for x in v) + ']'
        return quoted(v)
    # Binding keys use the parser's SDL-name form (spaces are accepted by the
    # lightweight reader; TOML quoted keys are needed for the shared parser).
    lines=[]
    for section,values in sections.items():
        lines += [f'[{section}]']
        for k,v in values.items():
            if ' ' in k:
                # SDL aliases use underscores, understood by makeKeyboardMap.
                k=k.replace(' ','_')
            lines.append(f'{k} = {encode(v)}')
        lines.append('')
    return '\n'.join(lines)


def runtime_path(root=ROOT):
    suffix='.exe' if os.name=='nt' else ''
    packaged=root/('bin/mgs-runtime'+suffix)
    return packaged if packaged.exists() else root/('build-windows/ps1Runtime/ps1Runtime.exe' if os.name=='nt' else 'build/ps1Runtime/ps1Runtime')


def metadata(root=ROOT):
    p=root/'build-info.json'
    if not p.exists():p=root/'packaging/alpha.json'
    return json.loads(p.read_text())


def launch(paths, root=ROOT, reference=False, headless_frames=0, progress=print, big_picture=False):
    with user_lock(paths) as profile_lock:
        media,_ = installed(paths)
        options=settings(paths)
        if big_picture:options['fullscreen']=True
        runtime=runtime_path(root)
        if not runtime.is_file():raise ValueError('The native runtime is not built or packaged.')
        config=paths.data/'runtime.toml'
        text=runtime_config(paths,media,options)
        fd,tmp=tempfile.mkstemp(prefix='.runtime-',dir=paths.data)
        try:
            with os.fdopen(fd,'w',encoding='utf-8') as f:f.write(text)
            os.replace(tmp,config)
        finally:Path(tmp).unlink(missing_ok=True)
        env={k:v for k,v in os.environ.items() if not k.startswith(('PS1_', 'PS1RECOMP_'))}
        env['MGS_CONTROLLER_CONFIG']=str(paths.config/'controllers.toml')
        env.update(PS1_MGS_MESH_REPLAY='0' if reference else '1',
          PS1_MGS_ASSET_CACHE=str(media/'assets.mgsc'),
          PS1_PRESENT_WINDOWED='0' if options['fullscreen'] else '1',
          PS1_WINDOW_TITLE='MGS Recomp — '+metadata(root)['version'])
        if (root/'bin/shaders').is_dir():env['PS1RECOMP_VULKAN_SHADER_DIR']=str(root/'bin/shaders')
        if headless_frames:
            if not 1 <= headless_frames <= 100000:raise ValueError('Invalid headless frame limit.')
            env.update(PS1_DIAGNOSTIC_HEADLESS='1',PS1_EXIT_VBLANK=str(headless_frames),
              PS1_VBLANK_PERIOD_US='1000',SDL_AUDIODRIVER='dummy',SDL_VIDEODRIVER='dummy')
        logs=paths.data/'logs';logs.mkdir(exist_ok=True)
        fd,logpath=tempfile.mkstemp(prefix=time.strftime('session-%Y%m%d-%H%M%S-'),suffix='.log',dir=logs)
        progress('Game started. R3 / Esc pause menu · F5 save · F9 load · F11 fullscreen')
        start=time.monotonic()
        with os.fdopen(fd,'wb') as out:
            completed=run_runtime([str(runtime),'--config',str(config)],cwd=root,env=env,
                stdout=out,stderr=subprocess.STDOUT,pass_fds=(profile_lock.fileno(),))
        atomic_json(logs/'last-session.json',dict(schema=1,version=metadata(root)['version'],
          runtime_sha256=digest(runtime),returncode=completed.returncode,
          termination_signal=getattr(completed,'termination_signal',None),
          elapsed_seconds=round(time.monotonic()-start,3),headless=bool(headless_frames),
          reference_presentation=reference,log=Path(logpath).name))
        progress(f'Game closed ({completed.returncode}). Log: {logpath}')
        if getattr(completed,'termination_signal',None):
            raise LauncherTermination(completed.termination_signal)
        if completed.returncode:
            raise ValueError(f'Runtime exited with code {completed.returncode}. Export diagnostics for the report.')
        return Path(logpath)


def backup_saves(paths, output):
    with user_lock(paths):
        files=[]
        for folder,pattern in (('memory_cards','card[12].mcd'),('savestates','*.ps1s')):
            directory=paths.data/folder
            if directory.is_symlink():continue
            files.extend(p for p in directory.glob(pattern) if p.is_file() and not p.is_symlink())
            # Include history even when the current file is missing. Never
            # recurse into unrelated user files or unfinished transactions.
            for history in directory.glob(pattern+'.backups'):
                if not history.is_dir() or history.is_symlink():continue
                files.extend(p for p in history.glob('*.bak') if p.is_file() and not p.is_symlink())
        if not files:raise ValueError('No memory cards or savestates exist in this profile yet.')
        output=Path(output)
        if output.exists() or output.is_symlink():raise FileExistsError(output)
        fd,temp=tempfile.mkstemp(prefix='.mgs-backup-',dir=output.parent)
        try:
            with os.fdopen(fd,'w+b') as stream:
                with zipfile.ZipFile(stream,'w',compression=zipfile.ZIP_DEFLATED) as out:
                    for p in files:out.write(p,p.relative_to(paths.data).as_posix())
                stream.flush();os.fsync(stream.fileno())
            # Atomic publication without overwriting a concurrently created ZIP.
            if os.name=='nt':
                # Windows rename refuses an existing destination. It also
                # works on removable filesystems without hard-link support.
                os.rename(temp,output)
            else:
                os.link(temp,output)
                directory_fd=os.open(output.parent,os.O_RDONLY|os.O_DIRECTORY)
                try:os.fsync(directory_fd)
                finally:os.close(directory_fd)
        finally:Path(temp).unlink(missing_ok=True)
        return len(files)


def diagnostics(paths, output, root=ROOT):
    # Deliberately select text metadata, never recursively archive user data.
    report=dict(schema=1,build=metadata(root),system=dict(system=platform.system(),
        release=platform.release(),machine=platform.machine(),libc=platform.libc_ver()),
        settings=settings(paths),game_files_included=False,saves_included=False)
    logs=paths.data/'logs';last=logs/'last-session.json'
    log=None
    if last.is_file():
        session=json.loads(last.read_text());report['last_session']=session
        name=session.get('log','')
        if Path(name).name==name and name.startswith('session-') and name.endswith('.log'):
            candidate=logs/name
            if candidate.is_file() and not candidate.is_symlink():
                with candidate.open('rb') as stream:
                    stream.seek(max(0,candidate.stat().st_size-2*1024*1024));log=stream.read().decode('utf-8','replace')
    def redact(text):
        for path,label in sorted([(str(paths.data),'<USER_DATA>'),(str(paths.config),'<USER_CONFIG>'),
                                   (str(root),'<INSTALL>'),(str(Path.home()),'<HOME>')],key=lambda x:-len(x[0])):
            text=text.replace(path,label)
        return re.sub(r'/home/[^/\s]+','<HOME>',text)
    with zipfile.ZipFile(output,'x',compression=zipfile.ZIP_DEFLATED) as out:
        out.writestr('diagnostics.json',redact(json.dumps(report,indent=2)))
        if log is not None:out.writestr('runtime.log',redact(log))


def gui(paths):
    import queue
    import threading
    try:
        import tkinter as tk
        from tkinter import filedialog, messagebox, ttk
    except ImportError as e:
        raise RuntimeError('The graphical launcher requires Python Tk and its Tcl/Tk shared libraries '
                           '(python3-tk on Ubuntu). Terminal commands import, settings and run remain available.') from e
    try:
        window=tk.Tk()
    except tk.TclError as e:
        raise RuntimeError('Cannot open the graphical launcher. Start it in a desktop session, '
                           'or use the import/settings/run terminal commands: '+str(e)) from e
    window.title('MGS Recomp — Alpha');window.geometry('740x620')
    tab=ttk.Notebook(window);tab.pack(fill='both',expand=True,padx=14,pady=12)
    play=ttk.Frame(tab,padding=15);prefs=ttk.Frame(tab,padding=15);support=ttk.Frame(tab,padding=15)
    for panel,name in ((play,'Gioca / Importa'),(prefs,'Impostazioni'),(support,'Salvataggi / Supporto')):tab.add(panel,text=name)
    ttk.Label(play,text='Metal Gear Solid — alpha sperimentale',font=('',16,'bold')).pack(anchor='w')
    ttk.Label(play,text='Servono entrambi i CD USA v1.0. Il completamento continuo non è ancora verificato.',wraplength=640).pack(anchor='w',pady=10)
    status=tk.StringVar(value='Importa i CD oppure avvia il gioco già installato.')
    ttk.Label(window,textvariable=status,wraplength=700).pack(fill='x',padx=20,pady=(0,12))
    events=queue.Queue();busy=[False]
    def work(fn):
        if busy[0]:messagebox.showinfo('Operazione in corso','Attendi la fine dell’operazione o chiudi il gioco.');return
        busy[0]=True
        def worker():
            try:fn();events.put(('done','Operazione completata.'))
            except Exception as e:events.put(('error',str(e)))
        threading.Thread(target=worker,daemon=True).start()
    def poll():
        try:
            while True:
                kind,msg=events.get_nowait();status.set(msg)
                if kind in ('done','error'):busy[0]=False
                if kind=='error':messagebox.showerror('MGS Recomp',msg)
        except queue.Empty:pass
        window.after(100,poll)
    progress=lambda msg:events.put(('progress',msg))
    def import_clicked():
        if busy[0]:return
        files=[]
        for number in (1,2):
            p=filedialog.askopenfilename(title=f'Seleziona CD {number} USA v1.0',filetypes=[('Immagini CD','*.cue *.bin')])
            if not p:return
            files.append(p)
        work(lambda:import_discs(paths,files,progress))
    ttk.Button(play,text='Importa i due CD…',command=import_clicked).pack(fill='x',pady=6)
    ttk.Button(play,text='Avvia MGS',command=lambda:work(lambda:launch(paths,progress=progress))).pack(fill='x',pady=6)
    ttk.Label(play,text='F5 salva stato · F9 carica · F6/F7 cambia slot\nF11 schermo intero · Esc chiude il gioco\n\nI salvataggi del gioco rimangono sulle memory card.\nUsa anche il salvataggio Codec: gli stati possono cambiare compatibilità tra versioni.',wraplength=630).pack(anchor='w',pady=15)
    opts=settings(paths);height=tk.StringVar(value=str(opts['presentation_height']));volume=tk.IntVar(value=opts['volume'])
    ttk.Label(prefs,text='Risoluzione Vulkan (16:9 fisso; simulazione originale invariata)').pack(anchor='w')
    ttk.Combobox(prefs,textvariable=height,values=HEIGHTS,state='readonly').pack(fill='x',pady=5)
    toggles={k:tk.BooleanVar(value=opts[k]) for k in ('fullscreen','vsync','mute','fast_loading','ambient_occlusion')}
    for k,label in (('fullscreen','Avvia a schermo intero'),('vsync','VSync della presentazione'),('mute','Disattiva audio'),('fast_loading','Caricamenti rapidi'),('ambient_occlusion','Occlusione ambientale')):
        ttk.Checkbutton(prefs,text=label,variable=toggles[k]).pack(anchor='w')
    ttk.Label(prefs,text='Volume').pack(anchor='w');tk.Scale(prefs,from_=0,to=100,orient='horizontal',variable=volume).pack(fill='x')
    ttk.Label(prefs,text='Tastiera: un tasto = pulsante per riga (es. z = cross)').pack(anchor='w',pady=(8,0))
    binds=tk.Text(prefs,height=7);binds.pack(fill='both',expand=True)
    binds.insert('1.0','\n'.join(f'{k} = {v}' for k,v in opts['bindings'].items()))
    def save_preferences():
        try:
            pairs={}
            for line in binds.get('1.0','end').splitlines():
                if not line.strip():continue
                k,v=line.split('=',1);k=k.strip().lower();v=v.strip().lower()
                if k in pairs:raise ValueError('Tasto duplicato: '+k)
                pairs[k]=v
            value=dict(schema=1,presentation_height=int(height.get()),volume=volume.get(),bindings=pairs,
                       **{k:v.get() for k,v in toggles.items()})
            atomic_json(paths.settings,validate_settings(value));status.set('Impostazioni salvate. Si applicano al prossimo avvio.')
        except Exception as e:messagebox.showerror('Impostazioni',str(e))
    ttk.Button(prefs,text='Salva impostazioni',command=save_preferences).pack(fill='x',pady=8)
    ttk.Label(support,text='Dati utente:\n'+str(paths.data),wraplength=640).pack(anchor='w',pady=8)
    def archive(kind):
        p=filedialog.asksaveasfilename(defaultextension='.zip',initialfile=f'mgs-{kind}-{time.strftime("%Y%m%d-%H%M%S")}.zip')
        if p:work(lambda:backup_saves(paths,p) if kind=='saves' else diagnostics(paths,p))
    ttk.Button(support,text='Verifica integrità installazione',command=lambda:work(lambda:installed(paths,full=True))).pack(fill='x',pady=6)
    ttk.Button(support,text='Backup memory card e savestate…',command=lambda:archive('saves')).pack(fill='x',pady=6)
    ttk.Button(support,text='Esporta diagnostica per una segnalazione…',command=lambda:archive('diagnostics')).pack(fill='x',pady=6)
    ttk.Label(support,text='La diagnostica include impostazioni, identità della build e ultimo log con percorsi personali oscurati. Non include CD, asset, memory card o savestate. Il file viene creato localmente: puoi ispezionarlo prima di condividerlo.',wraplength=640).pack(anchor='w',pady=12)
    def close():
        if busy[0]:messagebox.showinfo('Operazione in corso','Chiudi prima il gioco o attendi il completamento dell’importazione.');return
        window.destroy()
    window.protocol('WM_DELETE_WINDOW',close);poll();window.mainloop()


def _main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--user-dir',type=Path,help='isolated profile (default XDG or portable.txt)')
    parser.add_argument('--big-picture',action='store_true',help='controller launcher and fullscreen game, without changing saved preferences')
    sub=parser.add_subparsers(dest='command')
    imp=sub.add_parser('import');imp.add_argument('disc1',type=Path);imp.add_argument('disc2',type=Path)
    sub.add_parser('verify');sub.add_parser('paths');sub.add_parser('gui')
    run=sub.add_parser('run');run.add_argument('--reference',action='store_true');run.add_argument('--headless-frames',type=int,default=0)
    cfg=sub.add_parser('settings');cfg.add_argument('--height',type=int,choices=HEIGHTS);cfg.add_argument('--volume',type=int)
    for name in ('fullscreen','vsync','mute','fast-loading','ambient-occlusion'):cfg.add_argument('--'+name,action=argparse.BooleanOptionalAction,default=None)
    for name in ('backup','diagnostics'):
        cmd=sub.add_parser(name);cmd.add_argument('output',type=Path)
    args=parser.parse_args(argv);paths=user_paths(override=args.user_dir)
    try:
        if args.command is None:
            from mgs_couch import run as couch
            couch(paths,big_picture=args.big_picture)
        elif args.command=='gui':gui(paths)  # optional legacy desktop editor
        elif args.command=='import':import_discs(paths,[args.disc1,args.disc2])
        elif args.command=='verify':installed(paths,full=True);print('Both discs, executable, cues and asset cache verified.')
        elif args.command=='run':launch(paths,reference=args.reference,headless_frames=args.headless_frames,big_picture=args.big_picture)
        elif args.command=='paths':print(json.dumps(dict(data=str(paths.data),config=str(paths.config)),indent=2))
        elif args.command=='settings':
            value=settings(paths)
            for key in ('fullscreen','vsync','mute','fast_loading','ambient_occlusion','volume'):
                if getattr(args,key) is not None:value[key]=getattr(args,key)
            if args.height is not None:value['presentation_height']=args.height
            atomic_json(paths.settings,validate_settings(value));print(json.dumps(value,indent=2))
        elif args.command=='backup':print(f'Backed up {backup_saves(paths,args.output)} save files.')
        elif args.command=='diagnostics':diagnostics(paths,args.output);print(args.output)
        return 0
    except (OSError,ValueError,KeyError,RuntimeError) as e:
        print('MGS alpha: '+str(e),file=sys.stderr);return 1


def main(argv=None):
    try:
        with termination_signals(), appimage_parent_lifetime():return _main(argv)
    except LauncherTermination as stopped:
        return 128+stopped.signum
    except OSError as error:
        print('MGS alpha: '+str(error),file=sys.stderr);return 1


if __name__=='__main__':
    sys.exit(main())
