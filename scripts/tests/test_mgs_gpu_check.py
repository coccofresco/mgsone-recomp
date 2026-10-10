from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_gpu_check import desktop_environment, presentation_config


class HardwareMatrixTests(unittest.TestCase):
    def test_software_overrides_cannot_leak_into_hardware_run(self):
        env = desktop_environment(dict(DISPLAY=':7', WAYLAND_DISPLAY='wayland-2',
            VK_DRIVER_FILES='/cpu.json', VK_ICD_FILENAMES='/cpu.json',
            SDL_VIDEODRIVER='dummy', PS1_DIAGNOSTIC_HEADLESS='1',
            PS1_VULKAN_HEADLESS_SURFACE='1'))
        self.assertEqual(env['DISPLAY'], ':7')
        self.assertEqual(env['WAYLAND_DISPLAY'], 'wayland-2')
        for key in ('VK_DRIVER_FILES','VK_ICD_FILENAMES','SDL_VIDEODRIVER',
                    'PS1_DIAGNOSTIC_HEADLESS','PS1_VULKAN_HEADLESS_SURFACE'):
            self.assertNotIn(key, env)

    def test_4k_affects_presentation_only(self):
        config = ('[game]\nprofile="mgs_usa_v1_0"\npresentation_height=720\n'
                  '[video]\nbackend="vulkan"\nres_scale=1\nfullscreen=true\n')
        result = presentation_config(config, 2160)
        self.assertIn('presentation_height = 2160', result)
        self.assertIn('fullscreen = false', result)
        self.assertIn('res_scale=1', result)
        with self.assertRaises(ValueError):
            presentation_config(config.replace('vulkan','software'),2160)
        with self.assertRaises(ValueError):
            presentation_config(config.replace('presentation_height=720\n',''),2160)


if __name__ == '__main__':
    unittest.main()
