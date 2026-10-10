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
            for pc in (*bridges.ENTRIES, 0x80024A64)) +
            '\n    PS1_PC_TRACE_HOOK(ctx, 0x800207C0u);\n')
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
            self.assertEqual(installed.count('if (mgsTryFastStageSync('), 1)
            self.assertEqual(installed.count('mgsReleaseFastPcxScratch(ctx);'), 1)
            self.assertEqual(bridges.install(root, exe), 0)
            self.assertEqual(source.read_text(), installed)
            source.write_text(installed.replace('0x8001E378u);', '0x8001E370u);', 1))
            before = source.read_bytes()
            with self.assertRaises(ValueError):
                bridges.install(root, exe)
            self.assertEqual(source.read_bytes(), before)

    def test_movie_dma_boundary_checks_retail_instructions_and_is_idempotent(self):
        exe = bridges.ROOT / 'data/assets/usa/disc1/SLUS_005.94'
        overlay = bridges.ROOT / 'data/overlays/usa/d18a.bin'
        if not exe.exists() or not overlay.exists():
            self.skipTest('optional locally extracted USA movie overlay')
        entry = bridges.MOVIE_DMA_ENTRIES['d18a']
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            source = self.fixture(root)
            source.write_text(source.read_text() + (
                f'void overlay_usa_d18a__{entry:08X}(uint8_t* rdram, recomp_context* ctx) {{\n'
                f'    PS1_PC_TRACE_HOOK(ctx, 0x{entry + 0x6c:08X}u);\n}}\n'))
            local = root / 'd18a.bin'
            data = overlay.read_bytes()
            local.write_bytes(data)
            self.assertEqual(bridges.install(root, exe, overlay_directory=root), 1)
            installed = source.read_bytes()
            self.assertEqual(bridges.install(root, exe, overlay_directory=root), 0)
            self.assertEqual(source.read_text().count('completed SDK DMA IRQs'), 1)
            for offset in (entry - 0x800c5968, entry - 0x800c5968 + 7 * 4):
                bad = bytearray(data)
                bad[offset] ^= 1
                local.write_bytes(bad)
                with self.assertRaisesRegex(ValueError, 'instruction identity'):
                    bridges.install(root, exe, overlay_directory=root)
                self.assertEqual(source.read_bytes(), installed)

    def test_disc_bindings_require_overlay_identity_and_scoped_symbols(self):
        exe = bridges.ROOT / 'data/assets/usa/disc1/SLUS_005.94'
        overlay = bridges.ROOT / 'data/overlays/usa/change.bin'
        if not exe.exists() or not overlay.exists():
            self.skipTest('optional locally extracted USA assets')
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            source = self.fixture(root)
            text = source.read_text()
            for entry in bridges.DISC_ENTRIES:
                text += (f'void overlay_usa_change__{entry:08X}(uint8_t* rdram, recomp_context* ctx) {{\n'
                         f'    PS1_FUNCTION_TRACE_HOOK(ctx, 0x{entry:08X}u);\n}}\n')
                # An overlapping PC in another overlay must stay untouched.
                text += (f'void overlay_usa_other__{entry:08X}(uint8_t* rdram, recomp_context* ctx) {{\n'
                         f'    PS1_FUNCTION_TRACE_HOOK(ctx, 0x{entry:08X}u);\n}}\n')
            source.write_text(text)
            wrong = root / 'change.bin'
            wrong.write_bytes(b'wrong release')
            with self.assertRaises(ValueError):
                bridges.install(root, exe, wrong)
            self.assertEqual(source.read_text(), text)
            self.assertEqual(bridges.install(root, exe, overlay), 1)
            self.assertEqual(source.read_text().count('if (mgsTryDiscChange('), len(bridges.DISC_ENTRIES))
            self.assertEqual(bridges.install(root, exe, overlay), 0)


if __name__ == '__main__':
    unittest.main()
