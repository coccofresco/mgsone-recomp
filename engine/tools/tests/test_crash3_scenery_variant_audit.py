import json
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import crash3_scenery_variant_audit as audit


def test_exact_overlap_does_not_weld_nearby_or_degenerate_geometry():
    a, b, c = (0, 0, 0), (4, 0, 0), (0, 4, 0)
    faces = [{"vertices": v} for v in ((a, b, c), (c, b, a),
              (a, b, (0, 4, 1)), (a, a, b), (b, a, a))]
    assert len(audit.exact_groups(faces)) == 1
    assert audit.winding((a, b, c)) != audit.winding((c, b, a))


def test_state_integrity_and_truncation(tmp_path):
    state = tmp_path / "state.ps1s"
    state.write_bytes(struct.pack("<4sIII", b"PS1S", 4, 0, 0) +
                      bytes(audit.RAM_OFFSET - 16 + audit.RAM_SIZE))
    assert len(audit.load_ram(state)) == audit.RAM_SIZE
    damaged = bytearray(state.read_bytes())
    damaged[audit.RAM_OFFSET] = 1
    state.write_bytes(damaged)
    with pytest.raises(ValueError, match="checksum"):
        audit.load_ram(state)
    state.write_bytes(b"PS1S")
    with pytest.raises(ValueError, match="truncated"):
        audit.load_ram(state)


def test_refinement_requires_exact_complete_triangle_partition():
    a, b, c, m = (0, 0, 0), (4, 0, 0), (0, 4, 0), (2, 0, 0)
    faces = [{"face": i, "vertices": vertices} for i, vertices in enumerate(
        ((a, b, c), (a, m, c), (m, b, c)))]
    assert audit.triangle_refinements(faces) == [
        {"coarse_faces": [0], "child_faces": [[1], [2]]}]
    assert not audit.triangle_refinements(faces[:2])
    faces[2]["vertices"] = ((2, 0, 1), b, c)
    assert not audit.triangle_refinements(faces)


def test_selection_distinguishes_switches_layers_and_partial_catalogs(tmp_path):
    group = {"members": [{"face": 1}, {"face": 2}]}
    resources = [{"resource_id": 9, "geometry_fingerprint": 17,
                  "coincident_groups": [group]}]
    path = tmp_path / "trace.jsonl"
    rows = []
    for frame, selected in enumerate(([1], [2], [1, 2])):
        for face in (1, 2):
            rows.append(dict(event="game.crash3.resident_projection_face",
                             resource_id=9, resource_face_ordinal=face,
                             resource_geometry_fingerprint=17,
                             frame=frame, capture_id=frame, source_selected=face in selected))
    # One missing face cannot establish an exclusive selection.
    rows.append(dict(rows[0], frame=3, capture_id=3))
    rows.extend(dict(row, resource_geometry_fingerprint=18, frame=4, capture_id=4)
                for row in rows[:2])
    path.write_text("\n".join(json.dumps(row) for row in rows))
    audit.attach_selection_traces(resources, [path])
    assert not group["exclusive_switch_observed"]
    assert group["member_switch_observed"]
    assert group["simultaneous_selection_observed"]
    assert sum(r["captures"] for r in group["native_selection_observations"]) == 3


def path_rows(second_mask=2):
    prefix = "game.crash3.scenery_variant_path_"
    return [dict(event=prefix + kind, path_id=31, **fields) for kind, fields in (
        ("begin", dict(state_count=2, world_count=1)),
        ("resource", dict(world=0, resource_id=9, geometry_fingerprint=17, face_count=3)),
        ("mask", dict(world=0, face=1, word=0, mask=1)),
        ("mask", dict(world=0, face=2, word=0, mask=second_mask)),
        ("end", dict(mask_rows=2)))]


def test_complete_path_proves_exclusivity_only_for_the_matching_incarnation(tmp_path):
    path = tmp_path / "path.jsonl"
    group = {"members": [{"face": 1}, {"face": 2}]}
    resources = [dict(resource_id=9, geometry_fingerprint=17, triangles=3, quads=0,
                      coincident_groups=[group])]
    path.write_text("\n".join(map(json.dumps, path_rows())))
    assert audit.attach_path_certificates(resources, [path]) == 1
    assert group["complete_path_membership"][0]["exclusive_alternatives"]
    path.write_text("\n".join(map(json.dumps, path_rows(second_mask=3))))
    audit.attach_path_certificates(resources, [path])
    assert not group["complete_path_membership"][0]["exclusive_alternatives"]
    assert group["complete_path_membership"][0]["simultaneous_selection"]
    resources[0]["geometry_fingerprint"] = 18
    audit.attach_path_certificates(resources, [path])
    assert not group["complete_path_membership"]


def test_partial_and_out_of_range_path_data_cannot_certify_absence(tmp_path):
    path = tmp_path / "path.jsonl"
    rows = path_rows()
    path.write_text("\n".join(map(json.dumps, rows[:-1])))
    with pytest.raises(ValueError, match="truncated"):
        audit.read_complete_paths([path])
    rows[2]["word"] = 1
    path.write_text("\n".join(map(json.dumps, rows)))
    with pytest.raises(ValueError, match="mask"):
        audit.read_complete_paths([path])
    rows = path_rows()
    rows[-1]["mask_rows"] = 3
    path.write_text("\n".join(map(json.dumps, rows)))
    with pytest.raises(ValueError, match="incomplete"):
        audit.read_complete_paths([path])


def test_fingerprint_is_unsigned_fnv1a_over_exact_little_endian_chunks():
    assert audit.fnv64([b"fo", b"obar"]) == 0x85944171f73967e8


def test_authored_rank_audit_covers_all_animation_terminals_and_detects_exceptions():
    descriptors = {2: 0x800 | (1 << 12) | 4,
                   4: 0xa0000000 | (3 << 15) | 40,
                   5: 0xa0000000 | (2 << 15) | 50}
    result = audit.authored_material_families(
        {(123, 3), (0x802, 4)}, descriptors.__getitem__,
        lambda token, count: {40: 0, 41: 32, 43: 8, 50: 0, 51: 16, 52: 8}[token])
    assert len(result) == 2
    assert {r["nearest_token"] for r in result} == {41, 51}
    assert all(r["nearest_has_maximum_extent"] for r in result)
    result = audit.authored_material_families({(0x802, 4)}, descriptors.__getitem__,
        lambda token, count: token)
    assert not any(r["nearest_has_maximum_extent"] for r in result)
def test_static_distance_refinement_is_not_an_animation_snapshot():
    records = {3: 0x8000000d, 4: 0x0000080d, 5: 0x800007ff}
    assert audit.static_refinement_token(13, records.__getitem__, 29) == 13
    assert audit.static_refinement_token(0x803, records.__getitem__, 29) == 13
    assert audit.static_refinement_token(0x804, records.__getitem__, 29) is None
    assert audit.static_refinement_token(0, records.__getitem__, 29) is None
    assert audit.static_refinement_token(0x805, records.__getitem__, 0) is None
