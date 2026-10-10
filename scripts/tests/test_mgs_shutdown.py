import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import mgs_alpha as alpha


class LauncherShutdownTests(unittest.TestCase):
    def test_appimage_wrapper_death_requests_graceful_launcher_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            launcher=root/'launcher.py'
            launcher.write_text('''import sys,time,os
from pathlib import Path
sys.path.insert(0,sys.argv[1]);import mgs_alpha as alpha
root=Path(sys.argv[2])
try:
    with alpha.termination_signals(),alpha.appimage_parent_lifetime():
        (root/'ready').write_text(str(os.getpid()))
        while True:time.sleep(.02)
except alpha.LauncherTermination as request:
    (root/'stopped').write_text(str(request.signum))
''')
            wrapper='''import os,subprocess,sys
env=dict(os.environ,APPIMAGE=sys.executable)
child=subprocess.Popen([sys.executable,sys.argv[1],sys.argv[2],sys.argv[3]],env=env)
child.wait()
'''
            process=subprocess.Popen([sys.executable,'-c',wrapper,str(launcher),str(Path(alpha.__file__).parent),str(root)])
            child_pid=None
            try:
                deadline=time.monotonic()+5
                while not (root/'ready').exists() and time.monotonic()<deadline:time.sleep(.01)
                self.assertTrue((root/'ready').exists());child_pid=int((root/'ready').read_text())
                process.terminate();process.wait(timeout=5)
                deadline=time.monotonic()+5
                while not (root/'stopped').exists() and time.monotonic()<deadline:time.sleep(.01)
                self.assertEqual((root/'stopped').read_text(),str(signal.SIGTERM))
            finally:
                if process.poll() is None:process.kill();process.wait()
                if child_pid:
                    try:os.kill(child_pid,signal.SIGTERM)
                    except ProcessLookupError:pass

    def test_parent_signals_reach_child_and_keep_profile_locked_until_flush(self):
        for signum in (signal.SIGTERM,signal.SIGINT):
            with self.subTest(signum=signum), tempfile.TemporaryDirectory() as directory:
                root=Path(directory); paths=alpha.user_paths(override=root/'profile')
                child=root/'child.py'
                child.write_text('''import os,signal,sys,time
from pathlib import Path
root=Path(sys.argv[1])
def finish(signum,_frame):
    (root/'received').write_text(str(signum))
    while not (root/'release').exists():time.sleep(.01)
    (root/'flushed').write_text('complete')
    sys.exit(0)
signal.signal(signal.SIGTERM,finish);signal.signal(signal.SIGINT,finish)
(root/'ready').write_text(str(os.getpid()))
while True:time.sleep(.05)
''')
                parent=root/'parent.py'
                parent.write_text('''import sys,json
from pathlib import Path
sys.path.insert(0,sys.argv[1])
import mgs_alpha as alpha
root=Path(sys.argv[2])
try:
    with alpha.termination_signals(), alpha.user_lock(alpha.user_paths(override=root/'profile')) as lock:
        result=alpha.run_runtime([sys.executable,str(root/'child.py'),str(root)],pass_fds=(lock.fileno(),))
        (root/'result.json').write_text(json.dumps(dict(returncode=result.returncode,signal=result.termination_signal)))
        if result.termination_signal:raise alpha.LauncherTermination(result.termination_signal)
except alpha.LauncherTermination as request:sys.exit(128+request.signum)
''')
                process=subprocess.Popen([sys.executable,str(parent),str(Path(alpha.__file__).parent),str(root)])
                def wait_for(path):
                    deadline=time.monotonic()+5
                    while not path.exists() and process.poll() is None and time.monotonic()<deadline:time.sleep(.01)
                    self.assertTrue(path.exists(),str(path))
                try:
                    wait_for(root/'ready'); child_pid=int((root/'ready').read_text())
                    process.send_signal(signum);wait_for(root/'received')
                    self.assertEqual(int((root/'received').read_text()),signum)
                    self.assertIsNone(process.poll())
                    with self.assertRaisesRegex(ValueError,'already running'):
                        with alpha.user_lock(paths):pass
                    (root/'release').touch();self.assertEqual(process.wait(timeout=5),128+signum)
                    self.assertEqual((root/'flushed').read_text(),'complete')
                    with alpha.user_lock(paths):pass
                    with self.assertRaises(ProcessLookupError):os.kill(child_pid,0)
                    import json
                    result=json.loads((root/'result.json').read_text())
                    self.assertEqual(result,dict(returncode=0,signal=signum))
                finally:
                    (root/'release').touch()
                    if process.poll() is None:process.terminate();process.wait(timeout=6)

    def test_native_child_holds_inherited_lock_even_if_parent_is_killed(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); paths=alpha.user_paths(override=root/'profile')
            child_code='import time;time.sleep(10)'
            parent_code='''import sys,time,subprocess
from pathlib import Path
sys.path.insert(0,sys.argv[1]);import mgs_alpha as alpha
root=Path(sys.argv[2])
with alpha.user_lock(alpha.user_paths(override=root/'profile')) as lock:
    child=subprocess.Popen([sys.executable,'-c',sys.argv[3]],pass_fds=(lock.fileno(),))
    (root/'ready').write_text(str(child.pid))
    child.wait()
'''
            process=subprocess.Popen([sys.executable,'-c',parent_code,str(Path(alpha.__file__).parent),str(root),child_code])
            child_pid=None
            try:
                deadline=time.monotonic()+5
                while not (root/'ready').exists() and time.monotonic()<deadline:time.sleep(.01)
                self.assertTrue((root/'ready').exists());child_pid=int((root/'ready').read_text())
                process.kill();process.wait(timeout=5)
                with self.assertRaisesRegex(ValueError,'already running'):
                    with alpha.user_lock(paths):pass
            finally:
                if child_pid:
                    try:os.kill(child_pid,signal.SIGTERM)
                    except ProcessLookupError:pass
                if process.poll() is None:process.kill();process.wait(timeout=5)


if __name__=='__main__':unittest.main()
