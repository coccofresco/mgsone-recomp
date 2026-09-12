import importlib.util
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'install_decomp_bridges.py'
SPEC = importlib.util.spec_from_file_location('decomp_bridges', SCRIPT)
bridges = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bridges)


class DecompBridgeTests(unittest.TestCase):
    def fixture(self, root):
        path = root / 'recompiled_out.part000.cpp'
        path.write_text('\n'.join(
            f'void func_{pc:08X}(uint8_t *ram, recomp_context *ctx) {{\n'
            f'    PS1_FUNCTION_TRACE_HOOK(ctx, 0x{pc:08X}u);\n'
            '    const bool restored = ctx->nativeResumePc != 0;\n}\n'
            for pc in bridges.ENTRIES))
        return path

    def test_wrong_executable_never_modifies_generated_code(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            source = self.fixture(root)
            before = source.read_bytes()
            exe = root / 'foreign.exe'
            exe.write_bytes(b'wrong regional version')
            with self.assertRaises(ValueError):
                bridges.install(root, exe)
            self.assertEqual(source.read_bytes(), before)

    def test_install_is_atomic_and_idempotent_for_local_usa(self):
        exe = bridges.ROOT / 'data/assets/usa/disc1/SLUS_005.94'
        if not exe.exists():
            self.skipTest('optional locally extracted USA executable')
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            source = self.fixture(root)
            self.assertEqual(bridges.install(root, exe), 1)
            installed = source.read_text()
            self.assertEqual(installed.count('if (mgsTryDecompiled('), 4)
            self.assertEqual(bridges.install(root, exe), 0)
            self.assertEqual(source.read_text(), installed)
            source.write_text(installed.replace('0x8001E378u);', '0x8001E370u);', 1))
            before = source.read_bytes()
            with self.assertRaises(ValueError):
                bridges.install(root, exe)
            self.assertEqual(source.read_bytes(), before)


if __name__ == '__main__':
    unittest.main()
