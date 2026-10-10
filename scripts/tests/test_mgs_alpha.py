from contextlib import ExitStack, redirect_stdout
from copy import deepcopy
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import tomllib
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import mgs_alpha as alpha
import package_alpha


class AlphaTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='mgs alpha test ')
        self.addCleanup(self.tmp.cleanup)
        self.base=Path(self.tmp.name)
        self.paths=alpha.UserPaths(self.base/'user data',self.base/'config')
        self.tracks=[self.base/'first.bin',self.base/'second.bin']
        for i,p in enumerate(self.tracks):p.write_bytes(bytes([i+1])*257)
        self.exe=b'owned-executable-fixture'

    def fake_import(self, fail=False):
        owner=self
        class Disc:
            def __init__(self,path):pass
            def files(self):return {'/MGS/SLUS_005.94':{'lba':1,'size':len(owner.exe)}}
            def read(self,lba,size):return owner.exe
        def cache(paths,output):
            (output/'assets.mgsc').write_bytes(b'cache-fixture')
            (output/'manifest.json').write_text('{}')
            if fail:raise ValueError('cache build failed')
        stack=ExitStack()
        stack.enter_context(patch.object(alpha,'TRACK_SHA',tuple(alpha.digest(p) for p in self.tracks)))
        stack.enter_context(patch.object(alpha,'EXE_SHA',__import__('hashlib').sha256(self.exe).hexdigest()))
        stack.enter_context(patch.object(alpha,'Disc',Disc))
        stack.enter_context(patch.object(alpha,'build_cache',cache))
        self.addCleanup(stack.close)
        return stack

    def test_profile_paths_ignore_cwd_and_invalid_relative_xdg(self):
        root=self.base/'app';root.mkdir()
        p=alpha.user_paths(root,environ={'XDG_DATA_HOME':'relative','XDG_CONFIG_HOME':str(self.base/'cfg')})
        self.assertEqual(p.data,Path.home()/'.local/share/mgs-recomp')
        self.assertEqual(p.config,self.base/'cfg/mgs-recomp')
        (root/'portable.txt').touch()
        self.assertEqual(alpha.user_paths(root).data,root/'userdata')
        self.assertEqual(alpha.user_paths(root,self.base/'custom').data,self.base/'custom')

    def test_single_track_cue_spaces_and_unsupported_layouts(self):
        cue=self.base/'disc.cue'
        (self.base/'my track.bin').write_bytes(b'raw')
        good='FILE "my track.bin" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n'
        cue.write_text(good);self.assertEqual(alpha.track_path(cue),self.base/'my track.bin')
        for value in (good.replace('00:00:00','00:02:00'),good+'FILE "second.bin" BINARY\n',
                      good+'TRACK 02 AUDIO\n',good.replace('2352','2048'),good+'PREGAP 00:02:00\n'):
            cue.write_text(value)
            with self.assertRaises(ValueError):alpha.track_path(cue)

    def test_settings_reject_unknown_schema_invalid_values_and_reserved_bindings(self):
        for key,value in [('schema',True),('schema',2),('volume',-1),('volume',101),
                          ('volume',True),('presentation_height',800),('fullscreen','false'),
                          ('bindings',{'f9':'circle'}),('bindings',{'w':'magic'}),
                          ('bindings',{'unknown key':'up'}),('bindings',{'z':[]})]:
            d=deepcopy(alpha.DEFAULTS);d[key]=value
            with self.subTest(key=key,value=value),self.assertRaises(ValueError):alpha.validate_settings(d)
        d=deepcopy(alpha.DEFAULTS);d['future']=1
        with self.assertRaises(ValueError):alpha.validate_settings(d)

    def test_settings_and_config_keep_native_timing_and_isolated_saves(self):
        for height in alpha.HEIGHTS:
            opts=deepcopy(alpha.DEFAULTS);opts['presentation_height']=height
            opts['bindings']['right_shift']='select'
            parsed=tomllib.loads(alpha.runtime_config(self.paths,self.base/'media files',opts))
            self.assertEqual(parsed['game']['presentation_height'],height)
            self.assertTrue(parsed['game']['ambient_occlusion'])
            self.assertEqual(parsed['video']['res_scale'],1)
            self.assertEqual(parsed['timing']['video_standard'],'ntsc')
            self.assertTrue(parsed['game']['widescreen_16_9'])
            self.assertTrue(parsed['video']['preserve_aspect'])
            self.assertEqual(parsed['paths']['memcard_dir'],str(self.paths.data/'memory_cards'))
            self.assertEqual(len(parsed['disc']['additional_cues']),1)
        bad=alpha.UserPaths(self.base/'bad"path',self.base/'cfg')
        with self.assertRaises(ValueError):alpha.runtime_config(bad,self.base,opts)
        with self.assertRaises(ValueError):alpha.quoted(self.base/'tab\tpath')

    def test_legacy_settings_gain_ssao_without_changing_existing_preferences(self):
        old=deepcopy(alpha.DEFAULTS);del old['ambient_occlusion']
        old['fullscreen']=True;old['volume']=37
        self.paths.config.mkdir(parents=True)
        self.paths.settings.write_text(json.dumps(old))
        before=self.paths.settings.read_bytes()
        loaded=alpha.settings(self.paths)
        self.assertTrue(loaded['ambient_occlusion'])
        self.assertTrue(loaded['fullscreen']);self.assertEqual(loaded['volume'],37)
        self.assertEqual(self.paths.settings.read_bytes(),before)
        loaded['ambient_occlusion']=False
        parsed=tomllib.loads(alpha.runtime_config(self.paths,self.base/'media',loaded))
        self.assertFalse(parsed['game']['ambient_occlusion'])

    def test_successful_import_and_reimport_preserve_user_data(self):
        self.fake_import()
        cards=self.paths.data/'memory_cards';cards.mkdir(parents=True)
        (cards/'card1.mcd').write_bytes(b'personal card')
        opts=deepcopy(alpha.DEFAULTS);opts['volume']=43;alpha.atomic_json(self.paths.settings,opts)
        first=alpha.import_discs(self.paths,self.tracks,lambda _:None)
        self.assertEqual(alpha.installed(self.paths,full=True)[0],first)
        second=alpha.import_discs(self.paths,self.tracks,lambda _:None)
        self.assertNotEqual(first,second)
        self.assertTrue((first/'disc1.bin').exists())
        self.assertEqual((cards/'card1.mcd').read_bytes(),b'personal card')
        self.assertEqual(alpha.settings(self.paths)['volume'],43)
        self.assertEqual(alpha.installed(self.paths,full=True)[0],second)

    def test_wrong_disc_order_never_publishes_installation(self):
        self.fake_import()
        with self.assertRaisesRegex(ValueError,'Disc 1'):
            alpha.import_discs(self.paths,list(reversed(self.tracks)),lambda _:None)
        self.assertFalse(self.paths.installation.exists())
        self.assertEqual(list((self.paths.data/'library').iterdir()),[])
        self.assertEqual(self.tracks[0].read_bytes(),bytes([1])*257)

    def test_cache_failure_retains_previous_installation_and_removes_staging(self):
        self.fake_import()
        first=alpha.import_discs(self.paths,self.tracks,lambda _:None)
        before=self.paths.installation.read_bytes()
        with patch.object(alpha,'build_cache',side_effect=ValueError('failed cache')):
            with self.assertRaisesRegex(ValueError,'failed cache'):alpha.import_discs(self.paths,self.tracks,lambda _:None)
        self.assertEqual(self.paths.installation.read_bytes(),before)
        self.assertEqual(list((self.paths.data/'library').iterdir()),[first])

    def test_verify_detects_same_size_damage_and_startup_checks_boot_files(self):
        self.fake_import();folder=alpha.import_discs(self.paths,self.tracks,lambda _:None)
        (folder/'disc1.bin').write_bytes(b'x'*257)
        alpha.installed(self.paths)  # explicit fast-launch policy, not integrity certification
        with self.assertRaisesRegex(ValueError,'disc1.bin'):alpha.installed(self.paths,full=True)
        (folder/'disc1.cue').write_text((folder/'disc1.cue').read_text().replace('disc1','other'))
        with self.assertRaisesRegex(ValueError,'disc1.cue'):alpha.installed(self.paths)

    def test_exclusive_profile_lock_prevents_save_backup_during_play(self):
        with alpha.user_lock(self.paths):
            with self.assertRaisesRegex(ValueError,'already running'):
                alpha.backup_saves(self.paths,self.base/'backup.zip')
        self.assertFalse((self.base/'backup.zip').exists())

    def test_backups_include_only_cards_and_states_and_never_overwrite(self):
        for name in ('memory_cards/card1.mcd','savestates/quick_slot_0.ps1s','library/retail.bin','savestates/debug.txt'):
            p=self.paths.data/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(name.encode())
        (self.paths.data/'savestates/link.ps1s').symlink_to(self.paths.data/'library/retail.bin')
        output=self.base/'backup.zip';self.assertEqual(alpha.backup_saves(self.paths,output),2)
        with zipfile.ZipFile(output) as z:self.assertEqual(set(z.namelist()),{'memory_cards/card1.mcd','savestates/quick_slot_0.ps1s'})
        original=output.read_bytes()
        with self.assertRaises(FileExistsError):alpha.backup_saves(self.paths,output)
        self.assertEqual(output.read_bytes(),original)

    def test_diagnostics_redact_paths_without_saves_or_assets(self):
        logs=self.paths.data/'logs';logs.mkdir(parents=True)
        (logs/'session-test.log').write_text(str(self.paths.data)+'/library/disc1.bin\n'+str(Path.home())+'/Scaricati/game.bin')
        alpha.atomic_json(logs/'last-session.json',dict(log='session-test.log',returncode=1))
        secret=self.paths.data/'memory_cards/card1.mcd';secret.parent.mkdir();secret.write_bytes(b'DO NOT UPLOAD')
        output=self.base/'diagnostics.zip';alpha.diagnostics(self.paths,output)
        with zipfile.ZipFile(output) as z:
            self.assertEqual(set(z.namelist()),{'diagnostics.json','runtime.log'})
            text=z.read('runtime.log').decode();self.assertNotIn(str(self.paths.data),text)
            self.assertNotIn(str(Path.home()),text);self.assertIn('<USER_DATA>',text)
            self.assertFalse(json.loads(z.read('diagnostics.json'))['saves_included'])

    def test_backup_includes_orphan_history_and_ignores_partial_files_and_symlinks(self):
        history=self.paths.data/'savestates/quick_slot_0.ps1s.backups'
        history.mkdir(parents=True);(history/'001.bak').write_bytes(b'previous')
        (history/'.pending-write').write_bytes(b'partial')
        secret=self.base/'private';secret.write_text('secret')
        (history/'link.bak').symlink_to(secret)
        (history.parent/'link.ps1s.backups').symlink_to(history,target_is_directory=True)
        output=self.base/'backup.zip'
        self.assertEqual(alpha.backup_saves(self.paths,output),1)
        with zipfile.ZipFile(output) as z:
            self.assertEqual(z.namelist(),['savestates/quick_slot_0.ps1s.backups/001.bak'])
            self.assertEqual(z.read(z.namelist()[0]),b'previous')

    def test_failed_backup_never_publishes_a_partial_archive(self):
        card=self.paths.data/'memory_cards/card1.mcd';card.parent.mkdir(parents=True);card.write_bytes(b'card')
        output=self.base/'backup.zip'
        for failure in ('write','sync','publish'):
            with self.subTest(failure=failure):
                target={'write':'zipfile.ZipFile.write','sync':'mgs_alpha.os.fsync','publish':'mgs_alpha.os.link'}[failure]
                with patch(target,side_effect=OSError('injected disk failure')):
                    with self.assertRaises(OSError):alpha.backup_saves(self.paths,output)
                self.assertFalse(output.exists())
                self.assertEqual(list(self.base.glob('.mgs-backup-*')),[])
                self.assertEqual(card.read_bytes(),b'card')

    def test_launch_uses_packaged_paths_and_strips_inherited_diagnostic_overrides(self):
        self.fake_import();alpha.import_discs(self.paths,self.tracks,lambda _:None)
        app=self.base/'app';(app/'bin/shaders').mkdir(parents=True)
        (app/'bin/mgs-runtime').write_bytes(b'native-runtime')
        alpha.atomic_json(app/'build-info.json',dict(version='test'))
        steam={'SteamAppId':'123456','SDL_GAMECONTROLLER_IGNORE_DEVICES':'0x054c/0x0ce6',
               'SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT':'0x28de/0x11ff',
               'SDL_GAMECONTROLLERCONFIG':'user mapping', 'SDL_JOYSTICK_HIDAPI_STEAM':'0'}
        with patch.dict(os.environ,{'PS1_TAS_REPLAY':'private/movie','PS1_MGS_ASSET_CACHE':'wrong','PS1_INTERNAL_RES_SCALE':'99',**steam}),patch.object(alpha,'run_runtime',return_value=subprocess.CompletedProcess([],0)) as run:
            alpha.launch(self.paths,app,progress=lambda _:None)
        kwargs=run.call_args.kwargs;env=kwargs['env']
        self.assertNotIn('PS1_TAS_REPLAY',env);self.assertNotIn('PS1_INTERNAL_RES_SCALE',env)
        self.assertEqual(env['PS1_MGS_MESH_REPLAY'],'1')
        for key,value in steam.items():self.assertEqual(env[key],value)
        self.assertEqual(env['MGS_CONTROLLER_CONFIG'],str(self.paths.config/'controllers.toml'))
        self.assertEqual(kwargs['cwd'],app)
        self.assertEqual(len(kwargs['pass_fds']),1) # child retains profile lock if launcher dies
        self.assertEqual(run.call_args.args[0][0],str(app/'bin/mgs-runtime'))
        parsed=tomllib.loads((self.paths.data/'runtime.toml').read_text())
        self.assertTrue(Path(parsed['disc']['exe_path']).is_file())

    def test_package_audit_rejects_unlisted_private_payload_and_modified_files(self):
        folder=self.base/'package';folder.mkdir();(folder/'readme.txt').write_text('package')
        alpha.atomic_json(folder/'package-manifest.json',dict(files={'readme.txt':alpha.digest(folder/'readme.txt')}))
        package_alpha.audit(folder)
        (folder/'disc.bin').write_bytes(b'game')
        with self.assertRaises(ValueError):package_alpha.audit(folder)
        (folder/'disc.bin').unlink();(folder/'readme.txt').write_text('changed')
        with self.assertRaises(ValueError):package_alpha.audit(folder)

    def test_package_audit_rejects_directory_symlinks(self):
        folder=self.base/'package';folder.mkdir()
        alpha.atomic_json(folder/'package-manifest.json',dict(files={}))
        (folder/'userdata').symlink_to(self.paths.data,target_is_directory=True)
        with self.assertRaisesRegex(ValueError,'symlink'):package_alpha.audit(folder)


if __name__=='__main__':unittest.main()
