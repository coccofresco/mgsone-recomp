import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from build_mgs_asset_cache import build, pack_sections, SECTIONS
from test_mgs_assets import model, texture
from mgs_assets import sha, zmd, pcx


class GpuAssetCacheTests(unittest.TestCase):
    def test_pcx_retains_original_palette_and_indices_for_gpu(self):
        data = texture()
        data[16:19] = bytes([1, 0, 0])
        data.extend(bytes([0x55, 0x33, 0x0f, 0]))
        decoded = pcx(data, pixels_out=True)
        self.assertEqual(decoded['pixels'], bytes(range(8)))
        self.assertEqual(decoded['palette_words'][0], 0x8000)
        self.assertEqual(decoded['palette_rgb'][:3], bytes([1, 0, 0]))

    def test_zmd_gpu_export_preserves_geometry(self):
        data = model()
        for field in (56, 60, 68, 72, 76, 80):
            value = struct.unpack_from('<I', data, 32 + field)[0]
            struct.pack_into('<I', data, 32 + field, value - 120)
        archive = struct.pack('<4I', 0x61444d4b, 1, 124, len(data)-120) + struct.pack('<I', 0xbeef) + data
        parsed = zmd(archive, geometry=True)['models'][0]['parsed']['models'][0]
        self.assertEqual(parsed['positions_i16'][2], (10, 10, 0, -1))
        self.assertEqual(parsed['corner_normal_indices_raw'], [0, 128, 0, 128])

    def test_binary_directory_offsets_and_checksum(self):
        sections = {name: bytes(stride) for name, stride in SECTIONS}
        result = pack_sections(sections)
        magic, size, checksum = struct.unpack_from('<8sII', result)
        self.assertEqual(magic, b'MGSAC002')
        self.assertEqual(size, len(result))
        self.assertEqual(checksum, zlib.crc32(result[16:]))
        for i, (name, stride) in enumerate(SECTIONS):
            tag, offset, count, actual_stride = struct.unpack_from('<4sIII', result, 16+i*16)
            self.assertEqual((tag, count, actual_stride), (name.encode(), 1, stride))
            self.assertEqual(offset % 16, 0)
            self.assertEqual(result[offset:offset+stride], bytes(stride))
