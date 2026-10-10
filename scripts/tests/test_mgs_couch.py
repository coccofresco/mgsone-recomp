import json
import os
from pathlib import Path
import sys
import tempfile
import tomllib
import unittest
from unittest.mock import patch, Mock
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import mgs_alpha as alpha
import mgs_couch as couch
import package_portable as portable


class CouchTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='mgs couch ');self.addCleanup(self.tmp.cleanup)
        self.base=Path(self.tmp.name);self.paths=alpha.user_paths(override=self.base/'profile')

    def test_native_state_protocol_round_trips_unicode_paths_and_controls(self):
        p=self.base/'state.toml'
        value=dict(status='Path /home/player/日本 "test"\nChecking',busy=False,volume=75)
        couch.write_state(p,value)
        self.assertEqual(tomllib.loads(p.read_text()),value)
        self.assertEqual(list(self.base.iterdir()),[p])

    def test_settings_change_preserves_bindings_and_validates_before_write(self):
        couch.dispatch(self.paths,dict(action='settings',presentation_height=720,volume=67),False,lambda _:None)
        before=self.paths.settings.read_bytes();opts=alpha.settings(self.paths)
        self.assertEqual(opts['presentation_height'],720)
        self.assertEqual(opts['bindings'],alpha.BINDINGS)
        with self.assertRaises(ValueError):couch.dispatch(self.paths,dict(action='settings',volume=999),False,lambda _:None)
        self.assertEqual(before,self.paths.settings.read_bytes())

    def test_couch_launch_routes_explicit_fullscreen_override(self):
        with patch.object(alpha,'launch') as launch:
            couch.dispatch(self.paths,dict(action='run'),True,lambda _:None)
        self.assertTrue(launch.call_args.kwargs['big_picture'])

    def test_game_handoff_waits_for_explicit_native_controller_release(self):
        ready=self.base/'controllers-released';process=Mock();process.poll.return_value=None
        with patch.object(couch.time,'sleep',side_effect=lambda _:ready.write_text('released')) as sleep:
            couch.wait_for_controller_release(process,ready)
        self.assertTrue(sleep.called)

    def test_failed_controller_handoff_cannot_start_another_input_owner(self):
        ready=self.base/'controllers-released';process=Mock();process.poll.return_value=None
        with self.assertRaisesRegex(RuntimeError,'cancelled'):
            couch.wait_for_controller_release(process,ready,timeout=0)
        process.poll.return_value=1
        with self.assertRaises(RuntimeError):couch.wait_for_controller_release(process,ready)

    def test_unknown_native_action_cannot_invoke_commands(self):
        with self.assertRaisesRegex(ValueError,'Unknown'):
            couch.dispatch(self.paths,dict(action='shell',command='echo unsafe'),False,lambda _:None)

    def test_appimage_portable_data_is_outside_read_only_mount(self):
        marker=self.base/'portable.txt';marker.touch()
        env={'APPIMAGE':str(self.base/'MGS.AppImage')}
        paths=alpha.user_paths(root=self.base/'read-only-mount/usr/share/mgs-recomp',environ=env)
        self.assertEqual(paths.data,self.base/'mgs-recomp-userdata')
        self.assertEqual(paths.config,paths.data/'config')
        self.assertEqual(alpha.user_paths(override=self.base/'explicit',environ=env).data,self.base/'explicit')

    def test_portable_packager_rejects_host_abi_before_building_artifact(self):
        path=self.base/'elf';path.write_bytes(b'\x7fELFpayload')
        with patch.object(portable,'abi_requirements',return_value=dict(GLIBC='GLIBC_2.43',GLIBCXX=None,CXXABI=None)):
            with self.assertRaisesRegex(ValueError,'ceiling'):portable.check_abi([path],'2.39')
        with patch.object(portable,'abi_requirements',return_value=dict(GLIBC='GLIBC_2.38',GLIBCXX=None,CXXABI=None)):
            self.assertEqual(len(portable.check_abi([path],'2.39')),1)

    def test_dependency_closure_excludes_glibc_and_rejects_missing_sdk_libraries(self):
        sdk=self.base/'sdk';sdk.mkdir();lib=sdk/'usr/lib';lib.mkdir(parents=True)
        (lib/'libexample.so.1').write_bytes(b'fixture')
        seed=self.base/'app'
        def needed(p):return ['libexample.so.1','libc.so.6'] if p==seed else ['libm.so.6']
        with patch.object(portable,'needed',side_effect=needed):
            deps=portable.dependencies([seed],sdk)
        self.assertEqual(set(deps),{'libexample.so.1'})
        with patch.object(portable,'needed',return_value=['missing.so']):
            with self.assertRaisesRegex(ValueError,'Missing'):portable.dependencies([seed],sdk)

    def test_flatpak_permissions_allow_gamepad_and_read_only_import_without_home(self):
        spec=json.loads((alpha.ROOT/'packaging/flatpak/permissions.json').read_text())
        perms=spec['finish-args']
        for p in ('--device=input','--device=dri','--filesystem=xdg-download:ro','--socket=wayland'):
            self.assertIn(p,perms)
        self.assertNotIn('--filesystem=home',perms)
        self.assertNotIn('--filesystem=host',perms)
        self.assertNotIn('--share=network',perms)


if __name__=='__main__':unittest.main()
