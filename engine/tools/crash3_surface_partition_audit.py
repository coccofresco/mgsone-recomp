#!/usr/bin/env python3
"""Attribute exact coarse/fine scenery partitions to complete native programs.

Reads protected local data. No native visibility or presentation state writes.
Exports only source identities, areas and membership evidence, not game assets.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path

from crash3_scenery_variant_audit import load_ram, inspect_ram, read_complete_paths
from exact_surface_partition import (surface, contains, certify_partition,
                                     interiors_overlap, diagnose_coverage, connected_cover,
                                     certify_cover)


def audit(resource, paths, inspect_faces=()):
    triangles = resource["triangles"]
    masks = []
    for identity, path in paths.items():
        worlds = [world for world, binding in path["resources"].items()
                  if binding == (resource["resource_id"], resource["geometry_fingerprint"],
                                 triangles + 2 * resource["quads"])]
        if len(worlds) != 1:
            continue
        by_face = collections.defaultdict(int)
        for (world, face, word), bits in path["masks"].items():
            if world == worlds[0]:
                by_face[face] |= bits << (64 * word)
        masks.append((identity, path["state_count"], by_face))

    faces = resource["source_faces"]
    polygons = []
    for face in faces:
        i = face["face"]
        vertices = face["vertices"]
        members = [i]
        if i >= triangles:
            if (i - triangles) & 1:
                continue
            members.append(i+1)
            vertices = (vertices[0], vertices[1], faces[i+1]["vertices"][2], vertices[2])
        shape = surface(vertices)
        if (shape and "texture_extent" in face and not face["semi_transparent"]
                and not any(faces[j].get("deforming", False) for j in members)):
            polygons.append(dict(face=i, members=members, shape=shape, material=face))

    def selected(bits, polygon):
        # A complete polygon witness must contain BOTH native quad halves.
        value = bits[polygon["members"][0]]
        for i in polygon["members"][1:]:
            value &= bits[i]
        return value

    def any_selected(bits, polygon):
        value = 0
        for i in polygon["members"]:
            value |= bits[i]
        return value

    def witnesses_for(parent, children):
        witnesses = []
        for identity, states, bits in masks:
            high = (1 << states) - 1 if children else 0
            for child in children:
                high &= selected(bits, child)
            low = selected(bits, parent)
            if low and high:
                witnesses.append(dict(path_id=identity, low_states=low.bit_count(),
                                      complete_high_states=high.bit_count()))
        return witnesses

    def handoff_witness(parent, children):
        low_only, high_only, complete_handoffs, partial_handoffs = [], [], [], []
        for identity, states, bits in masks:
            any_low = any_selected(bits, parent)
            high = (1 << states)-1 if children else 0
            any_high = 0
            for child in children:
                high &= selected(bits, child)
                any_high |= any_selected(bits, child)
            for target, mask in (
                    (low_only, any_low & ~any_high),
                    (high_only, high & ~any_low),
                    (complete_handoffs, any_low & high),
                    (partial_handoffs, any_low & any_high & ~high)):
                if mask:
                    target.append(dict(path_id=identity, states=mask.bit_count()))
        return dict(low_only=low_only, high_only=high_only,
                    complete_handoffs=complete_handoffs,
                    partial_handoffs=partial_handoffs,
                    eligible=bool(low_only and high_only and not partial_handoffs))

    results, diagnostics, supported = [], [], []
    mesh = [f["vertices"] for f in faces]
    stable_supports = [f["face"] for f in faces if "texture_extent" in f
                       and not f["semi_transparent"] and not f.get("deforming", False)]
    by_plane = collections.defaultdict(list)
    for polygon in polygons:
        by_plane[polygon["shape"][0]].append(polygon)
    for parent in polygons:
        shape = parent["shape"]
        children = []
        crossing_candidates = []
        covering_candidates = []
        for child in by_plane[shape[0]]:
            other = child["shape"]
            if not 0 < other[2] < shape[2] or not interiors_overlap(shape[1], other[1]):
                continue
            pm, cm = parent["material"], child["material"]
            if (pm["texture_page"] & 0x1e0) != (cm["texture_page"] & 0x1e0):
                continue
            ptex = pm["texture_extent"][0] * pm["texture_extent"][1]
            ctex = cm["texture_extent"][0] * cm["texture_extent"][1]
            if ctex * shape[2] < ptex * other[2]:
                continue  # A finer geometric partition must not reduce texel density.
            if not any(selected(bits, child) & ~any_selected(bits, parent)
                       for _, _, bits in masks):
                continue
            covering_candidates.append(child)
            if not any(any_selected(bits, parent) & any_selected(bits, child)
                       for _, _, bits in masks):
                crossing_candidates.append(child)
            if contains(shape[1], other[1]):
                children.append(child)
        if any(i in inspect_faces for i in parent["members"]):
            diagnostics.append(dict(
                lower_faces=parent["members"],
                contained_candidates=len(children),
                contained_area2_sum=sum(c["shape"][2] for c in children),
                intersecting_candidates=[i for c in crossing_candidates for i in c["members"]],
                coverage=diagnose_coverage(shape, [c["shape"] for c in crossing_candidates]),
                complete_program_witnesses=witnesses_for(parent, crossing_candidates)))
        if not certify_partition(shape, [c["shape"] for c in children]):
            handoff = handoff_witness(parent, covering_candidates)
            if handoff["eligible"] and certify_cover(
                    shape, [c["shape"] for c in covering_candidates]):
                results.append(dict(lower_faces=parent["members"],
                    higher_faces=[i for c in covering_candidates for i in c["members"]],
                    parent_area2=shape[2], child_polygons=len(covering_candidates),
                    coverage_kind="cross_boundary", complete_program_witnesses=
                        witnesses_for(parent, covering_candidates),
                    handoff_witness=handoff, eligible=True))
                continue
            if len(crossing_candidates) < 2:
                continue
            seeds = [i for c in crossing_candidates for i in c["members"]]
            for identity, states, bits in masks:
                low, high = selected(bits, parent), (1 << states)-1
                for child in crossing_candidates:
                    high &= selected(bits, child)
                if not low or not high:
                    continue
                relevant = low | high
                common = [i for i in stable_supports if bits[i] & relevant == relevant]
                closure = connected_cover(shape, mesh, seeds, common)
                if closure:
                    supported.append(dict(lower_faces=parent["members"],
                        higher_faces=seeds, support_faces=closure["support_faces"],
                        coverage=closure["coverage"], complete_program_witness=dict(
                            path_id=identity, low_states=low.bit_count(),
                            complete_high_states=high.bit_count()), eligible=True))
                    break
            continue
        witnesses = witnesses_for(parent, children)
        handoff = handoff_witness(parent, children)
        results.append(dict(lower_faces=parent["members"],
                            higher_faces=[i for c in children for i in c["members"]],
                            parent_area2=shape[2], child_polygons=len(children),
                            complete_program_witnesses=witnesses,
                            handoff_witness=handoff,
                            eligible=handoff["eligible"]))
    return dict(resource_id=resource["resource_id"],
                geometry_fingerprint=resource["geometry_fingerprint"],
                complete_paths=len(masks), partitions=results,
                supported_partitions=supported,
                coverage_diagnostics=diagnostics)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("checkpoint", type=Path)
    parser.add_argument("--trace", action="append", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--inspect-face", action="append", default=[],
                        help="diagnose a remaining source face as RESOURCE_ID:ORDINAL")
    args = parser.parse_args()
    if args.output.resolve() in {p.resolve() for p in [args.checkpoint, *args.trace]}:
        parser.error("output must not overwrite an input checkpoint or trace")
    paths = read_complete_paths(args.trace)
    if not paths:
        parser.error("no complete source programs in traces")
    resources = inspect_ram(load_ram(args.checkpoint), include_source_faces=True,
                            include_static_distance=True)
    inspected = collections.defaultdict(set)
    for value in args.inspect_face:
        try:
            resource, face = (int(word, 0) for word in value.split(":"))
        except ValueError:
            parser.error("inspect-face requires RESOURCE_ID:ORDINAL integers")
        if not any(r["resource_id"] == resource and 0 <= face < r["triangles"]+2*r["quads"]
                   for r in resources):
            parser.error("inspected source face is not in this checkpoint")
        inspected[resource].add(face)
    results = [audit(resource, paths, inspected[resource["resource_id"]]) for resource in resources]
    report = dict(schema="crash3-source-surface-partitions/v1",
                  checkpoint_sha256=hashlib.sha256(args.checkpoint.read_bytes()).hexdigest(),
                  traces={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in args.trace},
                  resources=results)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+"\n")
    for result in results:
        print(result["resource_id"], "exact partitions", len(result["partitions"]),
              "with complete program witness", sum(p["eligible"] for p in result["partitions"]),
              "source-connected supported covers",len(result["supported_partitions"]))


if __name__ == "__main__":
    main()
