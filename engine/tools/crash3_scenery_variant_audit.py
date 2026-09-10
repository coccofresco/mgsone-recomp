#!/usr/bin/env python3
"""Inventory exact overlapping scenery and texture variants, without guessing LOD.

Reads a local PS1S reference, validates its RAM checksum, and follows Crash 3's
resident resource records. It never loads or writes a guest state. Coordinates
and texels stay local; the report contains identities and measurements only.
Optional runtime traces prove which members were selected during a sequence.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import itertools
import json
import struct
from pathlib import Path

RAM_SIZE = 2 * 1024 * 1024
RAM_OFFSET = 16 + 128 + 12 + 64 + 256
MASK64 = (1 << 64) - 1


def fnv64(chunks):
    value = 14695981039346656037
    for chunk in chunks:
        for byte in chunk:
            value = ((value ^ byte) * 1099511628211) & MASK64
    return value


def authored_material_families(tokens, read_descriptor, extent, nearest_shift=29):
    """Check every reachable animation terminal, not a single saved phase."""
    result = []
    for token, vertex_count in sorted(tokens):
        if not token & 0x800:
            continue
        descriptor = read_descriptor(token & 0x7ff)
        if descriptor & 0x80000000:
            terminals = {descriptor}
        elif descriptor & 0x800:
            indices = {(descriptor + (phase & ((descriptor >> 12) & 127))) & 0x7ff
                       for phase in range(128)}
            terminals = {read_descriptor(index) for index in indices}
        else:
            continue  # Plain animation is not a distance family.
        for terminal in sorted(terminals):
            base = terminal & 0x7ff
            nearest = base + ((terminal >> nearest_shift) & 3)
            choices = sorted({base + ((terminal >> shift) & 3)
                              for shift in (29, 27, 25, 23, 21, 19, 17, 15)})
            areas = [extent(choice, vertex_count) for choice in choices]
            nearest_area = extent(nearest, vertex_count)
            result.append(dict(source_token=token, vertex_count=vertex_count,
                terminal_descriptor=terminal, nearest_token=nearest,
                choices=choices, texel_extents=areas, nearest_extent=nearest_area,
                nearest_has_maximum_extent=nearest_area == max(areas)))
    return result


def load_ram(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) < RAM_OFFSET + RAM_SIZE:
        raise ValueError("truncated PS1S RAM")
    magic, version, _, checksum = struct.unpack_from("<4sIII", data)
    if magic != b"PS1S" or version not in (1, 2, 3, 4):
        raise ValueError("unsupported PS1S header")
    ram = data[RAM_OFFSET:RAM_OFFSET + RAM_SIZE]
    actual = 0
    for (word,) in struct.iter_unpack("<I", ram):
        actual ^= word
    if checksum != actual:
        raise ValueError("PS1S RAM checksum mismatch")
    return ram


def nondegenerate(vertices):
    a = tuple(vertices[1][i] - vertices[0][i] for i in range(3))
    b = tuple(vertices[2][i] - vertices[0][i] for i in range(3))
    return any(a[i] * b[j] != a[j] * b[i] for i, j in ((0, 1), (1, 2), (2, 0)))


def exact_groups(faces):
    groups = collections.defaultdict(list)
    for face in faces:
        vertices = face["vertices"]
        if nondegenerate(vertices):
            groups[tuple(sorted(vertices))].append(face)
    return [members for members in groups.values() if len(members) > 1]


def winding(vertices):
    """Parity of a canonical triangle, independent of any view or rounding."""
    ordered = sorted(vertices)
    permutation = [ordered.index(vertex) for vertex in vertices]
    return sum(permutation[i] > permutation[j]
               for i in range(3) for j in range(i + 1, 3)) % 2


def triangle_refinements(faces):
    """Exact one-to-two tessellation relations; no tolerance or screen test."""
    by_triangle = collections.defaultdict(list)
    by_edge = collections.defaultdict(list)
    for face in faces:
        vertices = tuple(sorted(face["vertices"]))
        if not nondegenerate(vertices):
            continue
        by_triangle[vertices].append(face["face"])
    # Enumerate geometric triangles once, not every identical material copy.
    for vertices in by_triangle:
        for edge in itertools.combinations(vertices, 2):
            by_edge[edge].append(vertices)
    result = []
    for edge, members in by_edge.items():
        for a, b in itertools.combinations(members, 2):
            outer_a = next(v for v in a if v not in edge)
            outer_b = next(v for v in b if v not in edge)
            segment = tuple(outer_b[i] - outer_a[i] for i in range(3))
            length2 = sum(v * v for v in segment)
            for middle, opposite in (edge, edge[::-1]):
                delta = tuple(middle[i] - outer_a[i] for i in range(3))
                dot = sum(delta[i] * segment[i] for i in range(3))
                if not 0 < dot < length2:
                    continue
                # Equality in all three rational coordinates is an exact
                # segment certificate; integer arithmetic cannot round a
                # nearby but distinct surface into this relation.
                if any(delta[i] * length2 != segment[i] * dot for i in range(3)):
                    continue
                coarse = tuple(sorted((outer_a, outer_b, opposite)))
                if coarse in by_triangle:
                    result.append({"coarse_faces": by_triangle[coarse],
                                   "child_faces": [by_triangle[a], by_triangle[b]]})
    return result


def static_refinement_token(token, read_descriptor, nearest_shift):
    """Immutable nearest material only: never follow an animation terminal."""
    if not token & 0x800:
        return token if 0 < token < 0x800 else None
    descriptor = read_descriptor(token & 0x7ff)
    if not descriptor & 0x80000000:
        return None
    nearest = (descriptor & 0x7ff) + ((descriptor >> (nearest_shift & 31)) & 3)
    return nearest if 0 < nearest < 0x800 else None


def inspect_ram(ram: bytes, *, include_source_faces=False,
                include_static_distance=False) -> list[dict]:
    def read(address, fmt="<I"):
        physical = address & 0x1fffffff
        if physical + struct.calcsize(fmt) > len(ram):
            raise ValueError(f"non-RAM source address {address:#x}")
        return struct.unpack_from(fmt, ram, physical)[0]

    table = read(read(0x80065EBC) + 0x10)
    count = read(table)
    if not 1 <= count <= 8:
        raise ValueError("no coherent resident scenery table in checkpoint")
    resources = []
    for world in range(count):
        record = table + 4 + world * 0x30
        info = read(record + 0x10)
        pair = read(record)
        entry, eid = read(pair), read(pair + 4)
        if info != entry + 0x30 or not eid & 1:
            raise ValueError("unresolved resident resource identity")
        nv, nt, nq = (read(info + offset) for offset in (0x10, 0x14, 0x18))
        if not (0 < nv <= 4096 and nt <= 2048 and nq <= 2048):
            raise ValueError("invalid resident topology counts")
        vp = read(record + 0x1c) + nv * 4
        tp = read(record + 0x14) + nt * 4
        qp = read(record + 0x18)
        material_table = read(record + 0x28)
        material_remap = read(record + 0x24)
        def block(address, size):
            physical = address & 0x1fffffff
            if physical + size > len(ram):
                raise ValueError("invalid geometry fingerprint range")
            return ram[physical:physical + size]
        fingerprint = fnv64((struct.pack("<III", nt, nq, nv),
                             block(tp - nt * 4, nt * 6), block(qp, nq * 8),
                             block(vp - nv * 4, nv * 6)))
        vertices = []
        deforming_vertices = []
        for index in range(nv):
            xy = read(vp - (index + 1) * 4)
            z = read(vp + index * 2, "<H")
            vertices.append(((xy & 0xfff0) >> 2, (xy >> 18) & 0x3ffc,
                             (z & 0xfff0) >> 2))
            deforming_vertices.append(bool(xy & (1 << 18)))
        faces = []

        def append(ordinal, indices, token):
            if any(index >= nv for index in indices):
                raise ValueError("invalid source vertex index")
            face = {"face": ordinal, "source_token": token,
                    "vertices": tuple(vertices[index] for index in indices)}
            if include_source_faces:
                face["deforming"] = any(deforming_vertices[index] for index in indices)
            static_token = (static_refinement_token(
                token, lambda index: read(material_remap + 4*index),
                read(0x8005D4B4, "<B")) if include_static_distance else
                token if 0 < token < 0x800 else None)
            if static_token is not None:
                address = material_table + (static_token - 1) * 12
                if token & 0x800:
                    face["static_distance_token"] = static_token
                uv = [(read(address + off, "<H") & 255,
                       read(address + off, "<H") >> 8)
                      for off in (0, 4, 8, 10)[:3 if ordinal < nt else 4]]
                face.update(texture_page=read(address + 6, "<H") & 0x1ff,
                            private_texture_bits=read(address + 6, "<H") >> 9,
                            clut=read(address + 2, "<H"),
                            semi_transparent=(read(address + 6, "<H") & 0x60) != 0x60,
                            texture_extent=[max(p[i] for p in uv) - min(p[i] for p in uv) + 1
                                            for i in range(2)])
            faces.append(face)

        for index in range(nt):
            a, b = read(tp - (index + 1) * 4), read(tp + index * 2, "<H")
            append(index, (a >> 20, (a >> 8) & 4095, b >> 4),
                   ((a & 255) << 4) | (b & 15))
        for index in range(nq):
            a, b = read(qp + index * 8), read(qp + index * 8 + 4)
            ai, bi, ci, di = (a >> 8) & 4095, a >> 20, b >> 20, (b >> 8) & 4095
            token = ((a & 255) << 4) | (b & 15)
            append(nt + 2 * index, (bi, ai, ci), token)
            # Canonical polygon orientation, undoing the PSX strip alternation.
            append(nt + 2 * index + 1, (ci, ai, di), token)
        def material_extent(token, count):
            if token == 0:
                return 0
            address = material_table + (token - 1) * 12
            uv = [read(address + off, "<H") for off in (0, 4, 8, 10)[:count]]
            return ((max(v & 255 for v in uv) - min(v & 255 for v in uv) + 1) *
                    (max(v >> 8 for v in uv) - min(v >> 8 for v in uv) + 1))
        families = authored_material_families(
            {(f["source_token"], 3 if f["face"] < nt else 4) for f in faces},
            lambda index: read(material_remap + index * 4), material_extent)
        groups = []
        for members in exact_groups(faces):
            groups.append({"opposed_winding": len({winding(f["vertices"]) for f in members}) > 1,
                           "different_materials": len({f["source_token"] for f in members}) > 1,
                           "members": [{k: v for k, v in face.items() if k != "vertices"}
                                       for face in members]})
        resources.append({"resource_id": eid, "world": world,
                          "geometry_fingerprint": fingerprint,
                          "vertices": nv, "triangles": nt, "quads": nq,
                          "coincident_groups": groups,
                          "authored_material_families": families,
                          "exact_refinements": triangle_refinements(faces)})
        if include_source_faces:
            resources[-1]["source_faces"] = faces
    return resources


def attach_selection_traces(resources, paths):
    groups = {(r["resource_id"], tuple(sorted(m["face"] for m in g["members"]))): g
              for r in resources for g in r["coincident_groups"]}
    by_face = {(eid, face): key for key in groups for eid, members in [key] for face in members}
    counts = collections.defaultdict(collections.Counter)
    frames = collections.defaultdict(dict)
    incarnations = {r["resource_id"]: r["geometry_fingerprint"] for r in resources}
    for path in paths:
        with path.open() as stream:
            for line in stream:
                row = json.loads(line)
                if row.get("event") != "game.crash3.resident_projection_face":
                    continue
                if ((row.get("resource_geometry_fingerprint", 0) & MASK64) !=
                        incarnations.get(row["resource_id"])):
                    continue
                face = (row["resource_id"], row["resource_face_ordinal"])
                key = by_face.get(face)
                if key is not None:
                    frame = (str(path), row["frame"], row["capture_id"])
                    frames[frame].setdefault(key, {})[face[1]] = bool(row["source_selected"])
    for rows in frames.values():
        for key, observed in rows.items():
            if len(observed) == len(key[1]):
                counts[key][tuple(face for face in key[1] if observed[face])] += 1
    for key, group in groups.items():
        histogram = counts[key]
        group["native_selection_observations"] = [
            {"selected_faces": list(selected), "captures": count}
            for selected, count in sorted(histogram.items())]
        group["simultaneous_selection_observed"] = any(len(s) > 1 for s in histogram)
        group["member_switch_observed"] = sum(bool(s) and len(s) == 1 for s in histogram) > 1
        group["exclusive_switch_observed"] = (group["member_switch_observed"] and
                                                not group["simultaneous_selection_observed"])


def read_complete_paths(paths):
    """Accept only bounded, completed sparse bitmaps; absent rows then mean zero."""
    completed = {}
    prefix = "game.crash3.scenery_variant_path_"
    for filename in paths:
        active = {}
        with filename.open() as stream:
            for line in stream:
                row = json.loads(line)
                event = row.get("event", "")
                if not event.startswith(prefix):
                    continue
                kind, identity = event[len(prefix):], row["path_id"] & MASK64
                if kind == "begin":
                    if identity in active or not (0 < row["state_count"] <= 4096 and
                                                  0 < row["world_count"] <= 8):
                        raise ValueError("invalid or overlapping path begin")
                    active[identity] = {"state_count": row["state_count"],
                                        "world_count": row["world_count"],
                                        "resources": {}, "masks": {}}
                    continue
                if identity not in active:
                    raise ValueError("path row without begin")
                record = active[identity]
                if kind == "resource":
                    world = row["world"]
                    if world in record["resources"] or not (0 <= world < record["world_count"] and
                                                            0 <= row["face_count"] <= 6144):
                        raise ValueError("invalid path resource")
                    record["resources"][world] = (row["resource_id"],
                        row["geometry_fingerprint"] & MASK64, row["face_count"])
                elif kind == "mask":
                    key = row["world"], row["face"], row["word"]
                    value = row["mask"] & MASK64
                    world, face, word = key
                    resource = record["resources"].get(world)
                    if (key in record["masks"] or resource is None or value == 0 or
                        not 0 <= face < resource[2] or
                        not 0 <= word < (record["state_count"] + 63) // 64 or
                        (value << (64 * word)) >> record["state_count"]):
                        raise ValueError("invalid or duplicate path mask")
                    record["masks"][key] = value
                elif kind == "end":
                    if (row["mask_rows"] != len(record["masks"]) or
                        len(record["resources"]) != record["world_count"]):
                        raise ValueError("incomplete path bitmap")
                    if identity in completed and completed[identity] != record:
                        raise ValueError("path identity reused for different content")
                    completed[identity] = active.pop(identity)
                else:
                    raise ValueError("unknown path event")
        if active:
            raise ValueError("truncated path bitmap")
    return completed


def attach_path_certificates(resources, paths):
    complete = read_complete_paths(paths)
    for resource in resources:
        for group in resource["coincident_groups"]:
            group["complete_path_membership"] = []
        for identity, record in complete.items():
            worlds = [world for world, r in record["resources"].items()
                      if r == (resource["resource_id"], resource["geometry_fingerprint"],
                               resource["triangles"] + resource["quads"] * 2)]
            if len(worlds) != 1:
                continue
            world = worlds[0]
            masks = collections.defaultdict(int)
            for (w, face, word), value in record["masks"].items():
                if w == world:
                    masks[face] |= value << (64 * word)
            for group in resource["coincident_groups"]:
                members = [m["face"] for m in group["members"]]
                simultaneous = any(masks[a] & masks[b] for a, b in itertools.combinations(members, 2))
                group["complete_path_membership"].append({
                    "path_id": identity, "states": record["state_count"],
                    "selected_states": [masks[face].bit_count() for face in members],
                    "simultaneous_selection": simultaneous,
                    "exclusive_alternatives": all(masks[face] for face in members) and not simultaneous})
    return len(complete)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("checkpoint", type=Path)
    parser.add_argument("--trace", type=Path, action="append", default=[])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        resources = inspect_ram(load_ram(args.checkpoint))
        attach_selection_traces(resources, args.trace)
        complete_paths = attach_path_certificates(resources, args.trace)
    except (ValueError, KeyError, OSError) as error:
        parser.error(str(error))
    groups = [g for r in resources for g in r["coincident_groups"]]
    report = {"schema": "crash3-scenery-variant-audit/v2",
              "checkpoint_sha256": hashlib.sha256(args.checkpoint.read_bytes()).hexdigest(),
              "coincident_groups": len(groups),
              "different_material_groups": sum(g["different_materials"] for g in groups),
              "opposed_winding_groups": sum(g["opposed_winding"] for g in groups),
              "exclusive_switch_groups": sum(g["exclusive_switch_observed"] for g in groups),
              "exact_refinement_relations": sum(len(r["exact_refinements"]) for r in resources),
              "resources": resources,
              "complete_paths": complete_paths,
              "authored_material_terminals": sum(len(r["authored_material_families"]) for r in resources),
              "nearest_not_highest_extent": sum(not f["nearest_has_maximum_extent"]
                  for r in resources for f in r["authored_material_families"]),
              "path_exclusive_groups": sum(any(p["exclusive_alternatives"]
                  for p in g["complete_path_membership"]) for g in groups),
              "interpretation": "Exact overlap is not alone proof of LOD. Opposite sides, authored layers and detail alternatives need separate policies."}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k != "resources"}))


if __name__ == "__main__":
    main()
