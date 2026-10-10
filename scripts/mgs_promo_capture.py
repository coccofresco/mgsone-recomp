#!/usr/bin/env python3
"""Record continuous UHD video directly from Vulkan into NVIDIA NVENC.

No image sequence is exported. Saves and memory cards remain isolated.
"""
import argparse, hashlib, json, os, re, shutil, subprocess, time
from pathlib import Path
from mgs_gpu_check import desktop_environment
from mgs_tas import sha256
from mgs_video_check import audit_video, audit_timeline
ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT/'data/validation/presentation/ssao-distance-20261008/01-dock-final-4k.toml'
BINARY = ROOT/'build/ps1Runtime/ps1Runtime'

def prepare(out, state=None, disc=1):
    out = out.resolve(); out.mkdir(parents=True, exist_ok=False)
    for d in ('states','cards'): (out/d).mkdir()
    if state: shutil.copy2(state, out/'states/quick_slot_0.ps1s')
    for p in (ROOT/'data/memory_cards').glob('card[12].mcd'): shutil.copy2(p,out/'cards'/p.name)
    config = CONFIG.read_text()
    config = re.sub(r'memcard_dir = .*',f'memcard_dir = "{out}/cards"',config)
    config = re.sub(r'savestate_dir = .*',f'savestate_dir = "{out}/states"',config)
    if disc == 2:
        config=config.replace('/disc1.cue"','/DISC_SWAP.cue"').replace('/disc2.cue"','/disc1.cue"').replace('/DISC_SWAP.cue"','/disc2.cue"')
    (out/'runtime.toml').write_text(config)
    env=desktop_environment(os.environ)
    env.update(PS1_DIAGNOSTIC_SAVESTATE_DIR=str(out/'states'),PS1_MEMORY_CARD_DIR=str(out/'cards'),
       PS1_METRICS=str(out/'metrics.json'),PS1_METRICS_FLUSH_VBLANKS='600',
       PS1_VBLANK_PERIOD_US='1000',PS1_VULKAN_REQUIRE_HARDWARE='1',PS1_VULKAN_REQUIRE_VALIDATION='1',
       PS1_MGS_ASSET_CACHE=str(ROOT/'data/presentation/usa-source/assets.mgsc'),
       PS1_MGS_MESH_REPLAY='1',PS1_MGS_ASSET_VERIFY='1',PS1_VULKAN_HIDDEN_WINDOW='1',
       PS1_PRESENT_WINDOWED='1',SDL_AUDIODRIVER='dummy')
    if state: env['PS1_AUTOLOAD_VBLANK']='1'
    return out,env

def capture(a):
    if a.seconds <= 0 or a.start < 1:
        raise ValueError('positive capture duration and first VBlank required')
    out,env=prepare(a.output,a.state,a.disc)
    end=a.start+a.seconds*60
    pipe=out/'video.rgba.pipe';os.mkfifo(pipe)
    env.update(PS1_EXIT_VBLANK=str(end+2),PS1_VULKAN_REPLAY_READBACK='all',
       PS1_MGS_FRAME_INTERPOLATION='1' if a.fps==60 else '0',
       PS1_VULKAN_REPLAY_READBACK_INTERVAL=str(60//a.fps),PS1_VULKAN_REPLAY_READBACK_REGION='0,0,1,1',
       PS1_VULKAN_REPLAY_READBACK_FRAME_FROM=str(a.start),PS1_VULKAN_REPLAY_READBACK_FRAME_TO=str(end-1),
       PS1_VULKAN_VIDEO_PIPE=str(pipe),PS1_VULKAN_VIDEO_TIMELINE=str(out/'video-timeline.csv'),
       PS1_RAM_DUMP_PATH=str(out/'final-ram.bin'),PS1_VRAM_DUMP_PATH=str(out/'final-native-vram.ppm'))
    if a.input: env['PS1_INPUT_SCRIPT']=str(a.input.resolve())
    if a.route:
        env.update(PS1_TAS_ROUTE=str(a.route.resolve()),PS1_TAS_RECORD=str(out/'movie.mgst'),PS1_TAS_MAX_FRAMES=str(end),PS1_TAS_CHECKPOINT_INTERVAL='3600')
    if a.audio: env['PS1_SPU_CAPTURE_PCM']=str(out/'audio.s16le')
    command=['ffmpeg','-hide_banner','-y','-f','rawvideo','-pixel_format','rgba','-video_size','3840x2160','-framerate',str(a.fps),'-i',str(pipe),
       '-an','-vf','scale=in_range=full:out_range=tv:out_color_matrix=bt709,format=yuv420p',
       '-c:v','h264_nvenc','-preset','p5','-tune','hq','-rc','constqp','-qp','17',
       '-colorspace','bt709','-color_primaries','bt709','-color_trc','bt709','-movflags','+faststart',str(out/'recording.mp4')]
    started=time.monotonic()
    with (out/'runtime.log').open('w') as log,(out/'encode.log').open('w') as enc_log:
        encoder=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=enc_log,stderr=subprocess.STDOUT)
        game=subprocess.Popen([str(BINARY),'--config',str(out/'runtime.toml')],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT)
        try:
            code=game.wait(timeout=1800)
            encoder.wait(timeout=30)
            if code: raise RuntimeError(f'Runtime exit {code}')
            if encoder.returncode: raise RuntimeError('Video encoder failed')
        finally:
            if game.poll() is None: game.terminate();game.wait()
            if encoder.poll() is None: encoder.kill();encoder.wait()
            pipe.unlink()
    metrics=json.loads((out/'metrics.json').read_text())
    count=metrics.get('counters',{}).get('host.renderer.video.frames',0)
    failures=audit_video(metrics,(out/'runtime.log').read_text(errors='replace'),a.seconds*a.fps,a.fps,a.content)
    failures += audit_timeline(out/'video-timeline.csv',a.start,a.seconds,a.fps,metrics.get('counters',{}).get('host.renderer.video.interpolated_frames',0))
    probe=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-show_format','-of','json',str(out/'recording.mp4')]))
    stream=probe['streams'][0]
    if (stream['width'],stream['height'],stream['r_frame_rate'],int(stream['nb_frames']))!=(3840,2160,f'{a.fps}/1',a.seconds*a.fps):failures.append('encoded stream contract failed')
    report={'format':'mgs-vulkan-video-1','status':'failed' if failures else 'complete','failures':failures,
            'source_state':str(a.state.resolve()) if a.state else None,'source_state_sha256':sha256(a.state) if a.state else None,
            'disc':a.disc,'fps':a.fps,'content':a.content,'start_vblank':a.start,'seconds':a.seconds,'wall_seconds':round(time.monotonic()-started,3),
            'capture':f'Continuous original-asset Vulkan scanout -> encoder pipe -> H.264 NVENC, UHD {a.fps} fps; runtime source interpolation {"active" if a.fps==60 else "off"}; no image sequence or encoder interpolation',
            'interpolated_video_frames':metrics.get('counters',{}).get('host.renderer.video.interpolated_frames',0),
            'runtime_sha256':hashlib.sha256(BINARY.read_bytes()).hexdigest(),'encoded_frames':count,
            'artifacts':{name:sha256(out/name) for name in ('recording.mp4','metrics.json','runtime.log','runtime.toml','video-timeline.csv','final-ram.bin','final-native-vram.ppm')},
            'limits':'Resource completeness and actual hardware rendering; visual and temporal review still required.'}
    (out/'capture.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report),flush=True)
    if failures: raise RuntimeError(f'Presentation rejected; recording retained for diagnosis: {failures}')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',required=True,type=Path);p.add_argument('--state',type=Path)
    p.add_argument('--seconds',required=True,type=int);p.add_argument('--start',required=True,type=int)
    p.add_argument('--fps',type=int,choices=(30,60),default=60)
    p.add_argument('--content',choices=('scene','codec'),default='scene')
    p.add_argument('--disc',type=int,choices=(1,2),default=1)
    p.add_argument('--input',type=Path);p.add_argument('--route',type=Path);p.add_argument('--audio',action='store_true')
    capture(p.parse_args())
