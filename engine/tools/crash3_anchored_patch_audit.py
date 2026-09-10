#!/usr/bin/env python3
"""Independently verify emitted fine patches against source RAM and programs.

The renderer proposes source ordinals; rational geometry and complete native
branch witnesses verify them independently. No game data or RAM writes.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path

from crash3_scenery_variant_audit import load_ram, inspect_ram, read_complete_paths
from exact_surface_partition import certify_anchored_patch, surface, project_footprint


def polygon(resource, face):
    faces, nt = resource['source_faces'], resource['triangles']
    if face < nt:
        return [face], faces[face]['vertices']
    first = nt + 2*((face-nt)//2)
    a,b = faces[first]['vertices'], faces[first+1]['vertices']
    return [first,first+1], (a[0],a[1],b[2],a[2])


def verify_patch(resource, lower, higher, paths):
    faces = resource['source_faces']
    if (not lower or not higher or len(set(lower)) != len(lower) or
            len(set(higher)) != len(higher) or set(lower) & set(higher) or
            any(not 0 <= i < len(faces) for i in lower+higher)):
        return dict(passed=False, reason='invalid source ordinals')
    members, boundary = polygon(resource, lower[0])
    if sorted(lower) != members:
        return dict(passed=False, reason='not one authored coarse primitive')
    geometry = certify_anchored_patch(boundary, [faces[i]['vertices'] for i in higher])
    low_witness, high_witness = [], []
    for identity, path in paths.items():
        slots = [w for w,b in path['resources'].items() if b == (
            resource['resource_id'], resource['geometry_fingerprint'], len(faces))]
        if len(slots) != 1:
            continue
        masks = collections.defaultdict(int)
        for (w,face,word),bits in path['masks'].items():
            if w == slots[0]: masks[face] |= bits << (64*word)
        low, any_high, full_high = 0, 0, (1 << path['state_count'])-1
        for i in lower: low |= masks[i]
        for i in higher:
            any_high |= masks[i]
            full_high &= masks[i]
        if low & ~any_high:
            low_witness.append(dict(path_id=identity,states=(low & ~any_high).bit_count()))
        if full_high & ~low:
            high_witness.append(dict(path_id=identity,states=(full_high & ~low).bit_count()))
    material = all('texture_extent' in faces[i] and
                   not faces[i]['semi_transparent'] and not faces[i].get('deforming',False)
                   for i in lower+higher)
    reference = surface(boundary[:3])
    footprint = project_footprint(boundary, reference)
    p = faces[lower[0]]
    for i in higher:
        unit, points = polygon(resource,i)
        c = faces[unit[0]]
        projected = project_footprint(points,reference)
        material &= (set(unit) <= set(higher) and projected is not None and
            footprint is not None and 'texture_extent' in p and 'texture_extent' in c and
            (p['texture_page'] & 0x1e0) == (c['texture_page'] & 0x1e0) and
            projected[2] < footprint[2] and
            c['texture_extent'][0]*c['texture_extent'][1]*footprint[2] >=
            p['texture_extent'][0]*p['texture_extent'][1]*projected[2])
    return dict(passed=bool(geometry and material and low_witness and high_witness),
                anchored_connected_cover=geometry, static_material_quality=material,
                coarse_only=low_witness, complete_fine_only=high_witness)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('checkpoint',type=Path)
    parser.add_argument('--trace',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if args.output.resolve() in (args.checkpoint.resolve(),args.trace.resolve()):
        parser.error('output must not overwrite an input')
    resources=inspect_ram(load_ram(args.checkpoint),include_source_faces=True,
                          include_static_distance=True)
    resources={(r['resource_id'],r['geometry_fingerprint']):r for r in resources}
    paths=read_complete_paths([args.trace])
    proposals={}
    with args.trace.open() as stream:
        for line in stream:
            r=json.loads(line)
            if r['event']!='game.crash3.scenery_surface_refinement' or not r.get('anchored_source_patch'):
                continue
            identity=(r['resource_id'],r['geometry_fingerprint'] & ((1<<64)-1))
            if identity not in resources:
                parser.error('emitted patch belongs to an unavailable source resource')
            lower,_=polygon(resources[identity],r['lower_face'])
            key=(identity,tuple(lower),r['capture_id'])
            proposal=proposals.setdefault(key,dict(lower=set(),higher=set(),expected=r['higher_faces']))
            proposal['lower'].add(r['lower_face'])
            if r.get('shared_support'):
                parser.error('anchored patch must not masquerade as a shared-support proof')
            proposal['higher'].add(r['higher_face'])
    records=[]
    for (identity,lower,capture),p in proposals.items():
        higher=sorted(p['higher'])
        proof=verify_patch(resources[identity],list(lower),higher,paths)
        complete=(set(lower)==p['lower'] and len(higher)==p['expected'])
        records.append(dict(resource_id=identity[0],geometry_fingerprint=identity[1],capture_id=capture,
            lower_faces=lower,higher_faces=higher,complete_trace=complete,
            proof=proof,passed=complete and proof['passed']))
    report=dict(schema='crash3-anchored-source-patches/v1',
        checkpoint_sha256=hashlib.sha256(args.checkpoint.read_bytes()).hexdigest(),
        trace_sha256=hashlib.sha256(args.trace.read_bytes()).hexdigest(),
        complete_programs=len(paths),patches=records,
        passed=bool(records) and all(r['passed'] for r in records))
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(passed=report['passed'],patches=len(records),
                         failures=[r['lower_faces'] for r in records if not r['passed']])))
    return 0 if report['passed'] else 1


if __name__=='__main__':
    raise SystemExit(main())
