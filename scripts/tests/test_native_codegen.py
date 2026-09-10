"""Synthetic code/data fixtures: no retail assets are required."""
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RECOMP = ROOT / 'build/ps1Recomp/ps1Recomp'


@unittest.skipUnless(RECOMP.exists(), 'Build ps1Recomp first')
class NativeCodegenTests(unittest.TestCase):
    def test_overlay_jump_table_outside_code_and_shared_ram_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            exe = bytearray(4096)
            exe[:8] = b'PS-X EXE'
            struct.pack_into('<I', exe, 0x10, 0x80010000)
            struct.pack_into('<II', exe, 0x18, 0x80010000, 2048)
            struct.pack_into('<II', exe, 2048, 0x03e00008, 0)
            (path / 'main.exe').write_bytes(exe)
            words = [0x3c028030, 0x24420060, 0x00044080, 0x00481021,
                     0x8c490000, 0, 0x01200008, 0, 0x2402000b,
                     0x03e00008, 0, 0x24020016, 0x03e00008, 0]
            overlay = bytearray(0x70)
            struct.pack_into('<14I', overlay, 0, *words)
            struct.pack_into('<3I', overlay, 0x60, 0x80300020, 0x8030002c, 0)
            # Use the same address range with different immutable identities.
            # PS1 RAM only spans 2 MiB; use 0x80100000 for the fixture.
            for offset in (0,):
                struct.pack_into('<I', overlay, offset, 0x3c028010)
            struct.pack_into('<3I', overlay, 0x60, 0x80100020, 0x8010002c, 0)
            (path / 'one.bin').write_bytes(overlay)
            overlay[0x20] = 12
            (path / 'two.bin').write_bytes(overlay)
            config = '''[binary]
path = "main.exe"
entry_point = "0x80010000"
[[functions]]
name = "main"
address = "0x80010000"
size = 8
'''
            for name in ('one', 'two'):
                config += f'''[[overlays]]
name = "{name}"
ram_base = {0x80100000}
size = 56
binary_path = "{name}.bin"
functions = [{0x80100000}]
identity_size = 36
'''
            (path / 'test.toml').write_text(config)
            run = subprocess.run([str(RECOMP), str(path / 'test.toml'), str(path / 'out.cpp')],
                                 cwd=path, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('2 targets', run.stdout)
            generated = (path / 'out.cpp').read_text()
            self.assertIn('L_80100020:', generated)
            self.assertIn('L_8010002C:', generated)
            self.assertIn('return overlay_one__80100000;', generated)
            self.assertIn('return overlay_two__80100000;', generated)
            self.assertNotIn('recomp_func_table[0x80100000]', generated)
            self.assertIn('rdram[0x100020] == 11', generated)
            self.assertIn('rdram[0x100020] == 12', generated)

    def test_additional_overlay_leaf_callback_is_dispatched(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            exe = bytearray(4096)
            exe[:8] = b'PS-X EXE'
            struct.pack_into('<I', exe, 0x10, 0x80010000)
            struct.pack_into('<II', exe, 0x18, 0x80010000, 2048)
            struct.pack_into('<II', exe, 2048, 0x03e00008, 0)
            (path / 'main.exe').write_bytes(exe)
            (path / 'callback.bin').write_bytes(struct.pack('<7I',
                0x03e00008, 0, 0, 0, 0x2402002a, 0x03e00008, 0))
            config = f'''[binary]
path = "main.exe"
entry_point = "0x80010000"
[[functions]]
name = "main"
address = "0x80010000"
size = 8
[[overlays]]
name = "callback"
ram_base = {0x80100000}
size = 28
binary_path = "callback.bin"
functions = [{0x80100000}]
additional_functions = [{0x80100010}]
identity_size = 8
'''
            (path / 'test.toml').write_text(config)
            run = subprocess.run([str(RECOMP), str(path / 'test.toml'), str(path / 'out.cpp')],
                                 cwd=path, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            generated = (path / 'out.cpp').read_text()
            self.assertIn('case 0x80100010u: return overlay_callback__80100010;', generated)
            self.assertIn('void overlay_callback__80100010(', generated)
            for invalid in (0x80100011, 0x8010001c, 0x180100010, -0x7feffff0):
                with self.subTest(invalid=invalid):
                    (path / 'test.toml').write_text(config.replace(
                        f'additional_functions = [{0x80100010}]',
                        f'additional_functions = [{invalid}]'))
                    rejected = subprocess.run(
                        [str(RECOMP), str(path / 'test.toml'), str(path / 'bad.cpp')],
                        cwd=path, capture_output=True, text=True)
                    self.assertNotEqual(rejected.returncode, 0)
