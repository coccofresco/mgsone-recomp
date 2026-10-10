from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from mgs_presentation_check import audit_metrics, hardware_failures, ppm_info
from mgs_soak import vulkan_environment


class PresentationCheckTests(unittest.TestCase):
    def test_only_actual_integrated_or_discrete_device_can_pass_hardware_gate(self):
        for device_type in (None, 0, 3, 4):
            metrics = self.fixture()
            if device_type is not None:
                metrics['state']['host.renderer.vulkan_device_type'] = device_type
            self.assertTrue(hardware_failures(metrics))
            self.assertTrue(audit_metrics(metrics, require_hardware=True))
            self.assertFalse(audit_metrics(metrics)) # diagnostic remains separate
        for device_type in (1, 2):
            metrics = self.fixture()
            metrics['state']['host.renderer.vulkan_device_type'] = device_type
            self.assertTrue(audit_metrics(metrics, require_hardware=True))
            metrics['state']['host.renderer.vulkan_validation_required'] = 1
            self.assertFalse(audit_metrics(metrics, require_hardware=True))

    def fixture(self):
        return dict(counters={'mgs.source.gpu_original_format_packets':12,
                              'mgs.source.gpu_full_faces':12,
                              'mgs.source.visible_instance_captures':4,
                              'mgs.source.complete_instance_captures':4},
                    state={'mgs.asset_cache.readback_verified':1})

    def test_headless_or_prepared_only_cannot_be_certified_as_rendered(self):
        self.assertTrue(audit_metrics({}))
        m=self.fixture()
        self.assertFalse(audit_metrics(m))
        del m['counters']['mgs.source.gpu_full_faces']
        self.assertTrue(audit_metrics(m))

    def test_loaded_cache_does_not_hide_missing_models_or_replay_fallback(self):
        for key in ('mgs.source.missing_material', 'mgs.source.unmapped_instances',
                    'host.renderer.vulkan_replay_executor_downgrade',
                    'host.vulkan_memory.untracked_free'):
            m=self.fixture();m['counters'][key]=1
            self.assertIn(key,audit_metrics(m))
        m=self.fixture();m['counters']['mgs.source.complete_instance_captures']=3
        self.assertIn('incomplete visible instances',audit_metrics(m))

    def test_initial_reanchor_is_not_a_rejected_scene(self):
        m=self.fixture();m['counters']['host.renderer.vulkan_replay_route.m0_reanchor']=2
        self.assertFalse(audit_metrics(m))

    def test_enabled_ssao_requires_actual_gpu_work(self):
        m=self.fixture();m['state']['mgs.ssao.enabled']=1
        self.assertIn('enabled ambient occlusion was never resolved on GPU',audit_metrics(m))
        m['counters']['mgs.ssao.resolves']=3
        self.assertFalse(audit_metrics(m))

    def test_truncated_captures_and_missing_cache_fail_closed(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'frame.ppm';p.write_bytes(b'P6\n2 1\n255\n'+bytes(6))
            self.assertFalse(ppm_info(p)['nonblack'])
            p.write_bytes(p.read_bytes()[:-1])
            with self.assertRaises(ValueError):ppm_info(p)
            with self.assertRaises(ValueError):vulkan_environment(Path(d)/'missing')
            env=vulkan_environment(p,True)
            self.assertEqual(env['PS1_MGS_MESH_REPLAY'],'1')
            self.assertEqual(env['PS1_MGS_ASSET_VERIFY'],'1')
            self.assertNotIn('PS1_DIAGNOSTIC_HEADLESS',env)
            self.assertEqual(env['PS1_VULKAN_HEADLESS_SURFACE'],'1')


if __name__=='__main__':unittest.main()
