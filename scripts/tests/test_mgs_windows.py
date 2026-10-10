import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import mgs_alpha as alpha
if os.name=='nt':
    import mgs_windows as windows


@unittest.skipUnless(os.name=='nt', 'requires real Win32 process and file handles')
class WindowsLauncherTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='MGS 日本 è ')
        self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name)
        self.paths=alpha.user_paths(override=self.root/'profilo è')

    def test_appdata_and_portable_paths(self):
        paths=alpha.user_paths(self.root,environ={'LOCALAPPDATA':str(self.root/'local'),'APPDATA':str(self.root/'roaming')})
        self.assertEqual(paths.data,self.root/'local/MGSRecomp')
        self.assertEqual(paths.config,self.root/'roaming/MGSRecomp')
        (self.root/'portable.txt').touch()
        self.assertEqual(alpha.user_paths(self.root).data,self.root/'userdata')

    def test_profile_lock_is_exclusive(self):
        with alpha.user_lock(self.paths):
            with self.assertRaisesRegex(ValueError,'already running'):
                with alpha.user_lock(self.paths):pass
        with alpha.user_lock(self.paths):pass

    def test_inherited_lock_survives_parent_handle_close(self):
        with alpha.user_lock(self.paths) as lock:
            process=windows.spawn_runtime([sys.executable,'-c','import time; time.sleep(60)'],pass_fds=(lock.fileno(),))
        try:
            with self.assertRaisesRegex(ValueError,'already running'):
                with alpha.user_lock(self.paths):pass
        finally:
            process.kill();process.wait();windows.close_runtime(process)
        with alpha.user_lock(self.paths):pass

    def test_named_event_requests_cooperative_shutdown(self):
        code='''import ctypes,os,time
from ctypes import wintypes as w
k=ctypes.WinDLL('kernel32',use_last_error=True)
k.OpenEventW.restype=w.HANDLE;k.OpenEventW.argtypes=[w.DWORD,w.BOOL,w.LPCWSTR]
k.WaitForSingleObject.argtypes=[w.HANDLE,w.DWORD]
h=k.OpenEventW(0x100000,False,os.environ['PS1_SHUTDOWN_EVENT'])
assert h
assert k.WaitForSingleObject(h,10000)==0
print('flushed',flush=True)
'''
        process=windows.spawn_runtime([sys.executable,'-c',code],stdout=subprocess.PIPE,text=True)
        try:
            windows.request_shutdown(process)
            output,_=process.communicate(timeout=15)
            self.assertEqual(process.returncode,0)
            self.assertEqual(output.strip(),'flushed')
        finally:
            if process.poll() is None:process.kill();process.wait()
            windows.close_runtime(process)

    def test_utf8_settings_and_backup_without_overwrite(self):
        alpha.atomic_json(self.paths.settings,alpha.settings(self.paths))
        cards=self.paths.data/'memory_cards';cards.mkdir(parents=True)
        (cards/'card1.mcd').write_bytes(b'isolated test memory card')
        archive=self.root/'backup 日本.zip'
        self.assertEqual(alpha.backup_saves(self.paths,archive),1)
        with zipfile.ZipFile(archive) as z:
            self.assertEqual(z.read('memory_cards/card1.mcd'),b'isolated test memory card')
        original=archive.read_bytes()
        with self.assertRaises(FileExistsError):alpha.backup_saves(self.paths,archive)
        self.assertEqual(archive.read_bytes(),original)


if __name__=='__main__':unittest.main()
