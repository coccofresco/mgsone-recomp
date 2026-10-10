#!/usr/bin/env python3
"""Render an explicit frame-based promo edit from continuous video recordings."""
import argparse,json,subprocess
from pathlib import Path
from mgs_video_check import verify_capture

def run(plan_path, output):
    plan=json.loads(plan_path.read_text())
    fps=plan['fps']
    if fps not in (30,60): raise ValueError('unsupported edit frame rate')
    receipts=[]
    for clip in plan['clips']:
        receipt=verify_capture(plan_path.parent/clip['source'])
        receipts.append(receipt)
        if receipt.get('fps',30)!=fps: raise ValueError('all recordings must match the edit frame rate')
        if clip['disc'] != receipt['disc']:
            raise ValueError('Edit disc label differs from the verified recording')
        if clip['source_in_frame'] < 0 or clip['source_in_frame']+clip['frames'] > receipt['encoded_frames']:
            raise ValueError('Edit extends beyond the verified recording')
    if {r['disc'] for r in receipts}!={1,2}: raise ValueError('Promo requires footage from both discs')
    if plan.get('require_codec') and not any(r.get('content')=='codec' for r in receipts):
        raise ValueError('Promo requires a verified Codec recording')
    output.mkdir(parents=True,exist_ok=False)
    cuts=output/'edit-clips';cuts.mkdir();total=0;files=[]
    for i,clip in enumerate(plan['clips']):
        source=(plan_path.parent/clip['source']).resolve();count=clip['frames']
        if not 3*fps<=count<=15*fps:raise ValueError('Each selected clip must last 3–15 seconds')
        filters=[]
        if i==0:filters+=[f'fade=t=in:start_frame=0:nb_frames={fps//4}']
        if i==len(plan['clips'])-1:
            filters+=[f'fade=t=out:start_frame={85*fps-total}:nb_frames={5*fps-1}']
        dest=cuts/f'{i+1:02d}.mp4';files.append(dest)
        cmd=['ffmpeg','-hide_banner','-loglevel','warning','-y','-threads','4','-ss',str(clip['source_in_frame']/fps),'-i',str(source),'-an','-frames:v',str(count)]
        if filters:cmd+=['-vf',','.join(filters)]
        cmd+=['-c:v','h264_nvenc','-preset','p6','-tune','hq','-rc','constqp','-qp','18','-g','60','-bf','3','-pix_fmt','yuv420p','-colorspace','bt709','-color_primaries','bt709','-color_trc','bt709',str(dest)]
        print(f"EDIT {i+1}: {clip['name']} ({count/fps:.3f}s)",flush=True)
        subprocess.run(cmd,check=True)
        clip['timeline_start_frame']=total;total+=count
    if total!=90*fps:raise ValueError(f'Expected exactly 90 seconds, got {total}/{fps}')
    listing=output/'concat.txt';listing.write_text(''.join(f"file '{p.resolve()}'\n" for p in files))
    soundtrack=(plan_path.parent/plan['soundtrack']).resolve()
    master=output/('MGS_Recomp_Alpha_Promo_4K60.mp4' if fps==60 else 'MGS_Recomp_Alpha_Promo_4K.mp4')
    subprocess.run(['ffmpeg','-hide_banner','-loglevel','warning','-y','-f','concat','-safe','0','-i',str(listing),'-i',str(soundtrack),'-map','0:v:0','-map','1:a:0','-c:v','copy','-bsf:v',f'setts=pts=round(PTS*{fps}*TB)/({fps}*TB):dts=round(DTS*{fps}*TB)/({fps}*TB):duration=1/({fps}*TB)','-c:a','aac','-b:a','320k','-ar','48000','-t','90','-movflags','+faststart','-metadata','title=Metal Gear Solid Recomp — Alpha promo','-metadata','comment=Original-asset Vulkan video recordings. Music captured from the original title menu.',str(master)],check=True)
    print('MASTER DONE',flush=True)
    small=output/('MGS_Recomp_Alpha_Promo_1080p60.mp4' if fps==60 else 'MGS_Recomp_Alpha_Promo_1080p.mp4')
    subprocess.run(['ffmpeg','-hide_banner','-loglevel','warning','-y','-threads','4','-i',str(master),'-vf','scale=1920:1080:flags=lanczos','-c:v','h264_nvenc','-preset','p6','-tune','hq','-rc','vbr','-cq','20','-b:v','8M','-maxrate','16M','-bufsize','32M','-pix_fmt','yuv420p','-c:a','copy','-movflags','+faststart',str(small)],check=True)
    (output/'edit.json').write_text(json.dumps(plan,indent=2)+'\n')
    print('ALL EXPORTS DONE',flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('plan',type=Path);p.add_argument('output',type=Path);a=p.parse_args();run(a.plan.resolve(),a.output.resolve())
