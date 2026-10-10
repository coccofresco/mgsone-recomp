"""Windows process/lock adapters. Imported only by the Windows launcher."""
import ctypes
from ctypes import wintypes as w
from contextlib import contextmanager
import msvcrt
import os
import subprocess
import uuid

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
for name, result, args in (
    ('CreateFileW', w.HANDLE, [w.LPCWSTR, w.DWORD, w.DWORD, w.LPVOID, w.DWORD, w.DWORD, w.HANDLE]),
    ('CreateEventW', w.HANDLE, [w.LPVOID, w.BOOL, w.BOOL, w.LPCWSTR]),
    ('SetEvent', w.BOOL, [w.HANDLE]),
    ('CloseHandle', w.BOOL, [w.HANDLE]),
    ('OpenProcess', w.HANDLE, [w.DWORD, w.BOOL, w.DWORD]),
    ('WaitForSingleObject', w.DWORD, [w.HANDLE, w.DWORD]),
):
    fn = getattr(k32, name); fn.restype = result; fn.argtypes = args

_parent = None


@contextmanager
def parent_lifetime():
    global _parent
    pid = os.environ.get('MGS_LAUNCHER_PARENT_PID')
    if pid:
        _parent = k32.OpenProcess(0x00100000, False, int(pid))  # SYNCHRONIZE
        if not _parent:
            raise RuntimeError('Cannot supervise the Windows launcher process.')
    try:
        yield
    finally:
        if _parent: k32.CloseHandle(_parent)
        _parent = None


def check_parent():
    if _parent and k32.WaitForSingleObject(_parent, 0) == 0:
        import signal
        from mgs_alpha import LauncherTermination
        raise LauncherTermination(signal.SIGTERM)


@contextmanager
def profile_lock(path):
    # A zero-share file handle stays exclusive while either parent or native
    # child holds it, including if the launcher is forcibly terminated.
    handle = k32.CreateFileW(str(path), 0xC0000000, 0, None, 4, 0x80, None)
    if handle == w.HANDLE(-1).value:
        error = ctypes.get_last_error()
        if error in (32, 33):
            raise ValueError('MGS is already running or importing files in this profile.')
        raise ctypes.WinError(error)
    try:
        fd = msvcrt.open_osfhandle(handle, os.O_RDWR | os.O_BINARY)
    except BaseException:
        k32.CloseHandle(handle)
        raise
    with os.fdopen(fd, 'r+b') as lock:
        yield lock


def spawn_runtime(command, **kwargs):
    lock_fds = kwargs.pop('pass_fds', ())
    handles = [msvcrt.get_osfhandle(fd) for fd in lock_fds]
    event_name = 'Local\\MGSRecompStop-' + uuid.uuid4().hex
    event = k32.CreateEventW(None, True, False, event_name)
    if not event: raise ctypes.WinError(ctypes.get_last_error())
    env = dict(kwargs.pop('env', os.environ), PS1_SHUTDOWN_EVENT=event_name,
               PS1_LAUNCHER_PID=str(os.getpid()))
    info = subprocess.STARTUPINFO()
    info.lpAttributeList = {'handle_list': handles}
    try:
        for handle in handles: os.set_handle_inheritable(handle, True)
        process = subprocess.Popen(command, env=env, startupinfo=info,
                                   creationflags=subprocess.CREATE_NO_WINDOW,
                                   **kwargs)
        process._mgs_shutdown_event = event
        return process
    except BaseException:
        k32.CloseHandle(event)
        raise
    finally:
        for handle in handles: os.set_handle_inheritable(handle, False)


def request_shutdown(process):
    event = getattr(process, '_mgs_shutdown_event', None)
    if event:
        if not k32.SetEvent(event): raise ctypes.WinError(ctypes.get_last_error())
    else:
        process.terminate()


def close_runtime(process):
    event = getattr(process, '_mgs_shutdown_event', None)
    if event: k32.CloseHandle(event)
    process._mgs_shutdown_event = None
