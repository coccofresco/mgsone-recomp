#!/usr/bin/env python3
"""Derive bubble silhouettes from the user's USA disc (no bitmap data is stored).

The source opening uses awa_1/2/3 clusters in bubble_p, bub_d_sn and splash2.
Extract connected nonblack silhouettes as continuous sphere centers/radii.
"""
from pathlib import Path
import argparse,json,hashlib
import mgs_assets
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--inventory',type=Path,default=ROOT/'data/assets/usa/disc1/inventory.json')
parser.add_argument('--stage-dir',type=Path,default=ROOT/'data/assets/usa/disc1/STAGE.DIR')
parser.add_argument('--output',type=Path,default=ROOT/'engine/ps1Runtime/shaders/games/mgs_bubble_shapes.glsl')
parser.add_argument('--report',type=Path)
parser.add_argument('--check',action='store_true')
args=parser.parse_args()
inv=json.loads(args.inventory.read_text());stage=args.stage_dir.read_bytes();all_shapes=[];meta=[]
for name in ['awa_1','awa_2','awa_3']:
 h=0
 for c in name.encode():h=(((h<<5)|(h>>11))+c)&65535
 r=next(r for r in inv['resources'] if r['stage']=='opening' and r['extension']=='p' and int(str(r['id']),16)==h)
 data=stage[r['stage_dir_offset']:r['stage_dir_offset']+r['size']];pcx=mgs_assets.pcx(data,True);w,hh=pcx['width'],pcx['height'];pixels=pcx['pixels'];pal=pcx['palette_rgb']
 unseen={i for i,v in enumerate(pixels) if max(pal[v*3:v*3+3])>8};shapes=[]
 while unseen:
  first=min(unseen);unseen.remove(first);todo=[first];component=[]
  while todo:
   i=todo.pop();component.append((i%w,i//w));x,y=i%w,i//w
   for dx,dy in [(1,0),(-1,0),(0,1),(0,-1),(1,1),(-1,-1),(1,-1),(-1,1)]:
    xx,yy=x+dx,y+dy;j=yy*w+xx
    if 0<=xx<w and 0<=yy<hh and j in unseen:unseen.remove(j);todo.append(j)
  if len(component)<2:continue
  xs,ys=zip(*component);box=[min(xs),min(ys),max(xs)+1,max(ys)+1]
  shapes.append([(box[0]+box[2])/(2*w),(box[1]+box[3])/(2*hh),(box[2]-box[0])/(2*w),(box[3]-box[1])/(2*hh)])
 meta.append({'name':name,'source_sha256':hashlib.sha256(data).hexdigest(),'count':len(shapes),'shapes':shapes});all_shapes+=shapes
 print(name,len(shapes))
lines=['// Bubble centers/radii from the connected luminous silhouettes in the', '// local USA opening PCX awa_1/2/3. No texels or palette data are embedded.', '// Runtime ownership selects the original cluster; simulation stays native.']
for n,m in enumerate(meta,1):
 lines+=['// '+m['name']+' sha256 '+m['source_sha256'],f'const vec4 mgs_bubble_cluster_{n}[{m["count"]}]=vec4[](']
 lines+=['  vec4('+','.join(f'{v:.8f}' for v in shape)+')'+(',' if i<m['count']-1 else ');') for i,shape in enumerate(m['shapes'])]
lines+=['vec2 mgs_bubble_local(vec2 q,int cluster) {','  vec2 point=q*2.0-1.0;','  float nearest=1e10;']
for n,m in enumerate(meta,1):
 lines+=[f'  if(cluster=={n}) for(int i=0;i<{m["count"]};++i) {{',f'    vec4 shape=mgs_bubble_cluster_{n}[i];','    vec2 candidate=(q-shape.xy)/shape.zw*0.88;','    float distance=dot(candidate,candidate);','    if(distance<nearest) {nearest=distance;point=candidate;}','  }']
lines+=['  return point;','}']
generated='\n'.join(lines)+'\n'
if args.check:
 if not args.output.exists() or args.output.read_text()!=generated:
  raise SystemExit('Bubble layouts are stale; regenerate from the local disc.')
else:
 args.output.write_text(generated)
if args.report:
 args.report.write_text(json.dumps(meta,indent=2)+'\n')
