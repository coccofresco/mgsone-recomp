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
    def test_stack_allocation_in_branch_delay_slot_is_not_a_new_function(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            exe = bytearray(4096)
            exe[:8] = b'PS-X EXE'
            struct.pack_into('<I', exe, 0x10, 0x80010000)
            struct.pack_into('<II', exe, 0x18, 0x80010000, 2048)
            struct.pack_into('<II', exe, 2048, 0x03e00008, 0)
            (path / 'main.exe').write_bytes(exe)
            (path / 'stage.bin').write_bytes(struct.pack('<7I',
                0x24020028, 0x10800002, 0x27bdffe8, 0x24420001,
                0x24420002, 0x03e00008, 0x27bd0018))
            config = '''[binary]
path = "main.exe"
entry_point = "0x80010000"
[[functions]]
name = "main"
address = "0x80010000"
size = 8
[[overlays]]
name = "stage"
ram_base = 2148532224
size = 28
binary_path = "stage.bin"
additional_functions = [2148532224]
identity_size = 4
'''
            (path / 'test.toml').write_text(config)
            subprocess.run([str(RECOMP), str(path / 'test.toml'), str(path / 'out.cpp')],
                           cwd=path, capture_output=True, check=True)
            generated = (path / 'out.cpp').read_text()
            self.assertNotIn('void overlay_stage__80100008(', generated)
            start = generated.index('void overlay_stage__80100000(uint8_t* rdram, recomp_context* ctx) {')
            body = generated[start:generated.index('\n}\n', start) + 3]
            harness = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cassert>
#define PS1_FUNCTION_TRACE_HOOK(...)
#define PS1_PC_TRACE_HOOK(...)
namespace fmt { template<class... T> void print(T...) {} }
struct recomp_context { uint32_t r2=0,r4=0,r29=0x801ffff0,r31=0xdeadbeef,nativeResumePc=0; };
void pgxp_addiu(recomp_context* c,int d,int s,int n) {
  uint32_t value=(s==0?0:s==2?c->r2:c->r29)+n;
  if(d==2)c->r2=value;else {assert(d==29);c->r29=value;}
}
void recomp_dispatch(uint8_t*,recomp_context*,uint32_t) { abort(); }
void recomp_resume_dispatch(uint8_t*,recomp_context*,uint32_t) { abort(); }
'''
            harness += body
            harness += r'''
int main() {
  for(unsigned branch=0;branch<2;++branch) {
    recomp_context c; c.r4=branch;
    overlay_stage__80100000(nullptr,&c);
    assert(c.r2==(branch?43u:42u)); assert(c.r29==0x801ffff0);
    assert(c.r31==0xdeadbeef);
  }
}
'''
            (path / 'exercise.cpp').write_text(harness)
            subprocess.run(['c++', '-std=c++17', str(path / 'exercise.cpp'),
                            '-o', str(path / 'exercise')], capture_output=True, check=True)
            subprocess.run([str(path / 'exercise')], check=True)

    def test_growing_one_function_keeps_other_translation_shards_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            exe = bytearray(4096)
            exe[:8] = b'PS-X EXE'
            struct.pack_into('<I', exe, 0x10, 0x80010000)
            struct.pack_into('<II', exe, 0x18, 0x80010000, 2048)
            config = '[binary]\npath = "main.exe"\nentry_point = "0x80010000"\n'
            config += '[recompiler]\ntranslation_units = 3\n'
            for index in range(9):
                address = 0x80010000 + index * 0x40
                struct.pack_into('<3I', exe, 2048 + index * 0x40, 0x24020001, 0x03e00008, 0)
                config += (f'[[functions]]\nname = "body_{index}"\n'
                           f'address = "0x{address:08x}"\nsize = 12\n')
            (path / 'main.exe').write_bytes(exe)
            def generate(text):
                (path / 'test.toml').write_text(text)
                subprocess.run([str(RECOMP), str(path / 'test.toml'), str(path / 'out.cpp')],
                               cwd=path, capture_output=True, text=True, check=True)
                return {p.name: (p.read_bytes(), p.stat().st_mtime_ns)
                        for p in path.glob('out.part*.cpp')}
            original = generate(config)
            changed = generate(config + '[instruction_overrides]\n"0x80010000" = "' +
                               'ctx->r2 += 1; ' * 100 + '"\n')
            self.assertEqual(len(original), 3)
            self.assertEqual(sum(original[n] != changed[n] for n in original), 1)
            self.assertEqual(changed, generate((path / 'test.toml').read_text()))

    def test_split_routine_and_untaken_external_branch_execute_continuation(self):
        # Execute the generated C++, including its dispatch edges. The first
        # fragment ends after a conditional branch: neither implicit return nor
        # accidental fall-through into the external branch label is correct.
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            exe = bytearray(4096)
            exe[:8] = b'PS-X EXE'
            struct.pack_into('<I', exe, 0x10, 0x80010000)
            struct.pack_into('<II', exe, 0x18, 0x80010000, 2048)
            struct.pack_into('<11I', exe, 2048,
                             0x24020029, 0x10800006, 0x24420001,
                             0x24420001, 0x03e00008, 0, 0, 0,
                             0x24020063, 0x03e00008, 0)
            (path / 'main.exe').write_bytes(exe)
            config = '[binary]\npath = "main.exe"\nentry_point = "0x80010000"\n'
            for name, address, size in [('first', 0x80010000, 12),
                                        ('next', 0x8001000c, 12),
                                        ('taken', 0x80010020, 12)]:
                config += f'[[functions]]\nname = "{name}"\naddress = "0x{address:08x}"\nsize = {size}\n'
            (path / 'test.toml').write_text(config)
            subprocess.run([str(RECOMP), str(path / 'test.toml'), str(path / 'out.cpp')],
                           cwd=path, capture_output=True, text=True, check=True)
            generated = (path / 'out.cpp').read_text()
            bodies = []
            for name in ('first', 'next', 'taken'):
                start = generated.index(f'void {name}(uint8_t* rdram, recomp_context* ctx) {{')
                end = generated.index('\n}\n', start) + 3
                bodies.append(generated[start:end])
            harness = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cassert>
#define PS1_FUNCTION_TRACE_HOOK(...)
#define PS1_PC_TRACE_HOOK(...)
namespace fmt { template<class... T> void print(T...) {} }
struct recomp_context { uint32_t r2=0,r4=0,r31=0,nativeResumePc=0; };
int resumed=0;
void pgxp_addiu(recomp_context* c,int d,int s,int n) {
  assert(d==2 && (s==0 || s==2)); c->r2=(s==2?c->r2:0)+n;
}
void recomp_dispatch(uint8_t*,recomp_context*,uint32_t);
void recomp_resume_dispatch(uint8_t*,recomp_context* c,uint32_t pc) {
  assert(pc==0xdeadbeef && c->r31==pc); ++resumed;
}
'''
            harness += '\n'.join(bodies)
            harness += r'''
void recomp_dispatch(uint8_t* m,recomp_context* c,uint32_t pc) {
  if(pc==0x8001000c) next(m,c);
  else if(pc==0x80010020) taken(m,c);
  else abort();
}
int main() {
  for (unsigned restore : {0u,1u}) for (unsigned branch : {0u,1u}) {
    recomp_context c; c.r4=branch; c.r31=0xdeadbeef;
    c.nativeResumePc=restore?0x80010000:0;
    resumed=0; first(nullptr,&c);
    assert(c.r2==(branch?43u:99u)); assert(c.r31==0xdeadbeef);
    assert(resumed==int(restore));
  }
}
'''
            harness = '#include <initializer_list>\n' + harness
            (path / 'exercise.cpp').write_text(harness)
            built = subprocess.run(['c++', '-std=c++17', str(path / 'exercise.cpp'),
                                    '-o', str(path / 'exercise')], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            subprocess.run([str(path / 'exercise')], check=True)

    def test_stage_variants_with_same_prefix_use_additional_code_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            exe = bytearray(4096)
            exe[:8] = b'PS-X EXE'
            struct.pack_into('<I', exe, 0x10, 0x80010000)
            struct.pack_into('<II', exe, 0x18, 0x80010000, 2048)
            struct.pack_into('<II', exe, 2048, 0x03e00008, 0)
            (path / 'main.exe').write_bytes(exe)
            config = '''[binary]
path = "main.exe"
entry_point = "0x80010000"
[[functions]]
name = "main"
address = "0x80010000"
size = 8
'''
            for name, value in (('a', 11), ('b', 12)):
                (path / f'{name}.bin').write_bytes(struct.pack('<5I',
                    0, 0, 0x24020000 | value, 0x03e00008, 0))
                config += f'''[[overlays]]
name = "{name}"
ram_base = {0x80100000}
size = 20
binary_path = "{name}.bin"
functions = [{0x80100008}]
identity_size = 8
additional_identity_offsets = [8]
'''
            (path / 'test.toml').write_text(config)
            run = subprocess.run([str(RECOMP), str(path / 'test.toml'), str(path / 'out.cpp')],
                                 cwd=path, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            generated = (path / 'out.cpp').read_text()
            self.assertIn('rdram[0x100008] == 11', generated)
            self.assertIn('rdram[0x100008] == 12', generated)
            for offset in (-4, 1, 20, 0x100000000):
                (path / 'test.toml').write_text(config.replace(
                    'additional_identity_offsets = [8]', f'additional_identity_offsets = [{offset}]'))
                bad = subprocess.run([str(RECOMP), str(path / 'test.toml'), str(path / 'bad.cpp')],
                                     cwd=path, capture_output=True, text=True)
                self.assertNotEqual(bad.returncode, 0)

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

    def test_polling_resume_is_before_delay_slot_and_register_temporaries(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            exe = bytearray(4096)
            exe[:8] = b'PS-X EXE'
            struct.pack_into('<I', exe, 0x10, 0x80010000)
            struct.pack_into('<II', exe, 0x18, 0x80010000, 2048)
            struct.pack_into('<5I', exe, 2048,
                             0x8c820000, 0x1440fffe, 0x2442ffff,
                             0x03e00008, 0)
            (path / 'main.exe').write_bytes(exe)
            (path / 'test.toml').write_text('''[binary]
path = "main.exe"
entry_point = "0x80010000"
[[functions]]
name = "main"
address = "0x80010000"
size = 20
''')
            run = subprocess.run([str(RECOMP), str(path / 'test.toml'), str(path / 'out.cpp')],
                                 cwd=path, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            code = (path / 'out.cpp').read_text()
            boundary = code.index('runtimeBiosDrainPendingCallbacks(ctx->bios, 0x80010004u)')
            self.assertLess(boundary, code.index('uint32_t _ds_r2'))
            self.assertLess(boundary, code.index('pgxp_addiu(ctx, 2, 2, -1)'))
            self.assertIn('case 0x80010004u: goto L_80010004;', code)
            self.assertIn('recomp_resume_table[0x80010004]', code)
