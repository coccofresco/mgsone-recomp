import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mgs_presentation_check import GUARDS
from mgs_tas import sha256
from mgs_video_check import audit_video, audit_timeline, verify_capture


class VideoCheckTests(unittest.TestCase):
    def metrics(self):
        return {'counters': {'host.renderer.video.frames': 90,
                'mgs.source.gpu_original_format_packets': 200,
                'mgs.source.gpu_full_faces': 300,
                'mgs.source.visible_instance_captures': 10,
                'mgs.source.complete_instance_captures': 10},
                'state': {'mgs.asset_cache.readback_verified': 1,
                          'host.renderer.vulkan_device_type': 2,
                          'host.renderer.vulkan_validation_required': 1}}

    log = '[Shutdown] guest and ticker joined cleanly\n'

    def test_complete_video_cannot_hide_any_resource_or_renderer_failure(self):
        self.assertFalse(audit_video(self.metrics(), self.log, 90))
        for guard in GUARDS:
            with self.subTest(guard=guard):
                metrics = self.metrics()
                metrics['counters'][guard] = 1
                self.assertIn(guard, audit_video(metrics, self.log, 90))
        metrics = self.metrics()
        metrics['counters']['mgs.source.complete_instance_captures'] = 9
        self.assertIn('incomplete visible instances', audit_video(metrics, self.log, 90))

    def test_hardware_duration_and_validation_must_all_pass(self):
        metrics = self.metrics()
        metrics['state']['host.renderer.vulkan_device_type'] = 4
        self.assertTrue(audit_video(metrics, self.log, 90))
        self.assertTrue(audit_video(self.metrics(), self.log, 89))
        self.assertTrue(audit_video(self.metrics(), '', 90))
        self.assertTrue(audit_video(self.metrics(), self.log + 'Validation Error', 90))

    def test_sixty_fps_needs_runtime_intermediates_and_moving_geometry(self):
        m=self.metrics()
        self.assertTrue(audit_video(m,self.log,90,60))
        m['state']['mgs.interpolation.enabled']=1
        m['counters']['host.renderer.video.interpolated_frames']=45
        self.assertTrue(audit_video(m,self.log,90,60))
        m['counters']['mgs.interpolation.moving_faces']=100
        self.assertFalse(audit_video(m,self.log,90,60))

    def test_codec_requires_its_gpu_pass_but_not_three_dimensional_motion(self):
        m=self.metrics()
        for k in list(m['counters']):
            if k.startswith('mgs.source.'): del m['counters'][k]
        m['state']['mgs.interpolation.enabled']=1
        self.assertTrue(audit_video(m,self.log,90,60,'codec'))
        m['counters']['mgs.screen_effect.pass.codec_screen']=45
        self.assertFalse(audit_video(m,self.log,90,60,'codec'))
        m['counters']['mgs.material.missing_texture']=1
        self.assertTrue(audit_video(m,self.log,90,60,'codec'))

    def test_timeline_checks_every_captured_vblank_and_intermediate_count(self):
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory)/'timeline.csv'
            p.write_text('vblank,presentation\n'+''.join(f'{i},'+('midpoint' if i%2 else 'endpoint')+'\n' for i in range(60)))
            self.assertFalse(audit_timeline(p,0,1,60,30))
            self.assertTrue(audit_timeline(p,0,1,60,29))
            p.write_text(p.read_text().replace('5,midpoint','4,midpoint'))
            self.assertTrue(audit_timeline(p,0,1,60,30))

    def test_edit_rechecks_receipt_and_hashed_witnesses(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            video = root / 'recording.mp4'
            video.write_bytes(b'encoder output witness')
            (root / 'metrics.json').write_text(json.dumps(self.metrics()))
            (root / 'runtime.log').write_text(self.log)
            (root / 'runtime.toml').write_text('[video]\nbackend="vulkan"\n')
            report = {'format': 'mgs-vulkan-video-1', 'status': 'complete', 'seconds': 3,
                      'artifacts': {p.name: sha256(p) for p in root.iterdir()}}
            (root / 'capture.json').write_text(json.dumps(report))
            self.assertEqual(verify_capture(video), report)
            for name in report['artifacts']:
                p = root / name
                original = p.read_bytes()
                p.write_bytes(original + b'changed')
                with self.assertRaises(ValueError): verify_capture(video)
                p.write_bytes(original)
            # A stale or manually promoted "complete" receipt cannot bypass
            # the shared material gate even with matching artifact hashes.
            metrics = self.metrics()
            metrics['counters']['mgs.material.missing_texture'] = 1
            (root / 'metrics.json').write_text(json.dumps(metrics))
            report['artifacts']['metrics.json'] = sha256(root / 'metrics.json')
            (root / 'capture.json').write_text(json.dumps(report))
            with self.assertRaises(ValueError): verify_capture(video)
            (root / 'capture.json').write_text('{"encoded_frames":90}')
            with self.assertRaises(ValueError): verify_capture(video)


if __name__ == '__main__':
    unittest.main()
