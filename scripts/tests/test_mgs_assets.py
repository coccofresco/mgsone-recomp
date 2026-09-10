import hashlib
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_assets import FormatError, dar_members, kmd, oar, pcx, stage_members, stages, zmd


def model():
    header = struct.pack('<8i', 1, 1, 0, 0, 0, 10, 10, 10)
    row = [0, 1, 0, 0, 0, 10, 10, 10, 0, 0, 0,
           -1, -1, 4, 120, 152, 1, 156, 164, 168, 176, 0]
    vertices = b''.join(struct.pack('<4h', *v, -1) for v in [(0,0,0),(10,0,0),(10,10,0),(0,10,0)])
    body = vertices + bytes([0,1,2,3]) + struct.pack('<4h', 0,0,4096,0)
    body += bytes([0,128,0,128]) + bytes([0,0,255,0,255,255,0,255]) + struct.pack('<H', 0xabcd)
    return bytearray(header + struct.pack('<22i', *row) + body)


def texture(eight=False, width=8):
    h = bytearray(128)
    h[:4] = bytes([10,5,1,8 if eight else 1])
    struct.pack_into('<4H', h, 4, 0,0,width-1,0)
    h[65] = 1 if eight else 4
    struct.pack_into('<H', h, 66, width if eight else 1)
    struct.pack_into('<7H', h, 74, 12345, int(eight), 0,0,0,0,256 if eight else 16)
    return h


class AssetFormatTests(unittest.TestCase):
    def test_kmd_preserves_separate_corner_streams(self):
        m = kmd(model(), geometry=True)['models'][0]
        self.assertEqual(m['corner_vertex_indices'], [0,1,2,3])
        self.assertEqual(m['corner_normal_indices_raw'], [0,128,0,128])
        self.assertEqual(m['face_texture_ids'], [0xabcd])
        self.assertEqual(m['positions_i16'][2], (10,10,0,-1))

    def test_kmd_rejects_out_of_bounds_pointer(self):
        d = model();struct.pack_into('<I', d, 32+56, 0xfffffff)
        with self.assertRaises(FormatError):kmd(d)

    def test_kmd_rejects_bad_vertex_index(self):
        d=model();d[152]=4
        with self.assertRaises(FormatError):kmd(d)

    def test_kmd_rejects_bad_normal_index(self):
        d=model();d[164]=129
        with self.assertRaises(FormatError):kmd(d)

    def test_kmd_rejects_seam_without_parent(self):
        d=model();struct.pack_into('<h',d,126,0)
        with self.assertRaises(FormatError):kmd(d)

    def test_kmd_rejects_self_parent(self):
        d=model();struct.pack_into('<i',d,32+44,0)
        with self.assertRaises(FormatError):kmd(d)

    def test_kmd_rejects_truncated_body(self):
        with self.assertRaises(FormatError):kmd(model()[:-1])

    def test_zmd_shared_pool_uses_archive_relative_base(self):
        d=model()
        for offset in (56,60,68,72,76,80):
            old=struct.unpack_from('<I',d,32+offset)[0]
            struct.pack_into('<I',d,32+offset,old-120)
        packed=struct.pack('<4I',0x61444d4b,1,124,len(d)-120)+struct.pack('<I',0xbeef)+d
        parsed=zmd(packed)
        self.assertEqual(parsed['shared_data_offset'],140)
        self.assertEqual(parsed['models'][0]['parsed']['texture_ids'],['abcd'])

    def test_pcx_four_planes_decode_known_ramp(self):
        d=texture()+bytes([0x55,0x33,0x0f,0])
        p=pcx(d)
        self.assertEqual(p['indices_sha256'],hashlib.sha256(bytes(range(8))).hexdigest())

    def test_pcx_game_literal_c0(self):
        d=texture(True,1)+b'\xc0\x0c'+bytes(768)
        self.assertEqual(pcx(d)['indices_sha256'],hashlib.sha256(b'\xc0').hexdigest())

    def test_pcx_rejects_rle_overrun(self):
        with self.assertRaises(FormatError):pcx(texture(True,1)+b'\xc2\x00\x0c'+bytes(768))

    def test_pcx_palette_stp_low_bit(self):
        d=texture();d[16:19]=bytes([1,0,0]);d.extend(bytes(4))
        expected=struct.pack('<16H',0x8000,*([0]*15))
        self.assertEqual(pcx(d)['palette_ps1_sha256'],hashlib.sha256(expected).hexdigest())

    def test_dar_retains_duplicate_ids_in_order(self):
        d=struct.pack('<HHI',7,ord('k'),1)+b'a'+struct.pack('<HHI',7,ord('k'),1)+b'b'
        self.assertEqual([p for _,p in dar_members(d)],[b'a',b'b'])

    def test_dar_rejects_truncation(self):
        with self.assertRaises(FormatError):list(dar_members(struct.pack('<HHI',7,ord('k'),10)))

    def test_stage_cached_offsets_are_not_sizes(self):
        d=bytearray(4096);struct.pack_into('<HH',d,0,1,2)
        for i,t in enumerate([(1,ord('c'),ord('k'),0),(2,ord('c'),ord('p'),3),(0,ord('c'),255,5)]):
            struct.pack_into('<HBBI',d,4+8*i,*t)
        d[2048:2053]=b'abcde'
        self.assertEqual([p for _,p in stage_members(d)],[b'abc',b'de'])

    def test_stage_requires_cache_end_marker(self):
        d=bytearray(4096);struct.pack_into('<HH',d,0,1,2)
        struct.pack_into('<HBBI',d,4,1,ord('c'),ord('k'),0)
        with self.assertRaises(FormatError):list(stage_members(d))

    def test_dir_handles_full_eight_character_name(self):
        d=bytearray(4096);struct.pack_into('<I8sI',d,0,12,b'selectvr',1)
        self.assertEqual(next(stages(d))[0],'selectvr')

    def test_dir_rejects_escape_name(self):
        d=bytearray(4096);struct.pack_into('<I8sI',d,0,12,b'../evil',1)
        with self.assertRaises(FormatError):list(stages(d))

    def test_oar_table_word_offsets(self):
        d=struct.pack('<4I3H',0x6152414f,1,1,2,10,0,1)+bytes(4)
        self.assertEqual(oar(d)['archive_offset'],22)
        with self.assertRaises(FormatError):oar(d[:-3])


if __name__=='__main__':unittest.main()
