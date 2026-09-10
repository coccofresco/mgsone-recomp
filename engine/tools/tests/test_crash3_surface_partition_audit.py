"""Synthetic source data only: no dependency on a game checkpoint."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from crash3_surface_partition_audit import audit
from test_exact_surface_partition import supported_mesh


def fixture():
    faces = []
    for x0, x1 in ((0, 4), (0, 2), (2, 4)):
        a, b, c, d = ((x0, 0, 0), (x1, 0, 0),
                      (x0, 4, 0), (x1, 4, 0))
        for vertices in ((a, b, c), (b, c, d)):
            faces.append(dict(face=len(faces), vertices=vertices,
                              texture_extent=[32, 32], texture_page=0x60,
                              semi_transparent=False))
    resource = dict(resource_id=42, geometry_fingerprint=123,
                    triangles=0, quads=3, source_faces=faces)
    path = dict(state_count=2, resources={0: (42, 123, 6)},
                masks={(0, face, 0): 1 if face < 2 else 2
                       for face in range(6)})
    return resource, {999: path}


def test_complete_program_and_exact_many_face_cover():
    resource, paths = fixture()
    result = audit(resource, paths)
    assert result["complete_paths"] == 1
    assert result["partitions"] == [dict(
        lower_faces=[0, 1], higher_faces=[2, 3, 4, 5], parent_area2=32,
        child_polygons=2, complete_program_witnesses=[dict(
            path_id=999, low_states=1, complete_high_states=1)],
        handoff_witness=dict(low_only=[dict(path_id=999, states=1)],
            high_only=[dict(path_id=999, states=1)], complete_handoffs=[],
            partial_handoffs=[], eligible=True), eligible=True)]


def test_geometry_alone_cannot_certify_native_alternatives():
    resource, paths = fixture()
    paths[999]["masks"][(0, 3, 0)] = 0  # one quad half never exists
    assert not audit(resource, paths)["partitions"]
    resource, paths = fixture()
    paths[999]["masks"][(0, 3, 0)] |= 1  # even one co-selected half vetoes
    partitions = audit(resource, paths)["partitions"]
    assert partitions and not any(p["eligible"] for p in partitions)


def test_complete_handoff_is_distinct_from_partial_overlap_or_permanent_layer():
    resource, paths = fixture()
    paths[999]["state_count"] = 3
    for face in range(6):
        paths[999]["masks"][(0, face, 0)] |= 4
    result = audit(resource, paths)["partitions"][0]
    assert result["eligible"]
    assert result["handoff_witness"]["complete_handoffs"] == [dict(path_id=999, states=1)]
    paths[999]["masks"][(0, 5, 0)] &= ~4
    result = audit(resource, paths)["partitions"][0]
    assert not result["eligible"]
    assert result["handoff_witness"]["partial_handoffs"] == [dict(path_id=999, states=1)]


def test_bound_different_sections_can_witness_the_two_exclusive_branches():
    resource, paths = fixture()
    original = paths[999]
    paths[1000] = dict(state_count=1, resources=original["resources"],
                      masks={(0, face, 0): 1 for face in range(2, 6)})
    original["state_count"] = 1
    original["masks"] = {(0, face, 0): 1 for face in range(2)}
    result = audit(resource, paths)["partitions"][0]
    assert result["eligible"]
    assert not result["complete_program_witnesses"]  # Not one artificial stitched path.
    assert result["handoff_witness"]["low_only"] == [dict(path_id=999, states=1)]
    assert result["handoff_witness"]["high_only"] == [dict(path_id=1000, states=1)]


def test_native_culling_of_coarse_half_still_requires_complete_fine_surface():
    resource, paths = fixture()
    paths[999]["masks"][(0, 1, 0)] = 0
    result = audit(resource, paths)["partitions"][0]
    assert result["eligible"]
    assert result["lower_faces"] == [0, 1]
    paths[999]["masks"][(0, 3, 0)] = 0
    assert not audit(resource, paths)["partitions"]


def test_must_observe_all_fine_polygons_in_one_actual_state():
    resource, paths = fixture()
    paths[999]["state_count"] = 3
    paths[999]["masks"].update({(0, 4, 0): 4, (0, 5, 0): 4})
    result = audit(resource, paths)
    assert len(result["partitions"]) == 1
    assert not result["partitions"][0]["eligible"]
    assert not result["partitions"][0]["complete_program_witnesses"]


def test_geometry_fingerprint_and_material_quality_are_required():
    resource, paths = fixture()
    paths[999]["resources"][0] = (42, 124, 6)
    assert not audit(resource, paths)["partitions"]
    resource, paths = fixture()
    resource["source_faces"][2]["texture_extent"] = [1, 1]
    assert not audit(resource, paths)["partitions"]


def test_membership_after_first_sixty_four_states_is_not_lost():
    resource, paths = fixture()
    paths[999]["state_count"] = 66
    paths[999]["masks"] = {(0, face, 1): 1 if face < 2 else 2
                            for face in range(6)}
    assert audit(resource, paths)["partitions"][0]["eligible"]


def test_deforming_rest_planes_are_not_source_coverage_certificates():
    resource, paths = fixture()
    resource["source_faces"][3]["deforming"] = True
    assert not audit(resource, paths)["partitions"]


def test_requested_coverage_diagnostic_preserves_failed_geometry():
    resource, paths = fixture()
    resource["source_faces"][4]["texture_extent"] = [1, 1]
    result = audit(resource, paths, inspect_faces={0})
    assert not result["partitions"]
    diagnostic, = result["coverage_diagnostics"]
    assert diagnostic["coverage"]["uncovered_area2"] == "16"
    assert not diagnostic["coverage"]["strict_partition_certified"]


def supported_fixture():
    mesh = supported_mesh() + [((0,0,0),(4,0,0),(0,4,0)),
                               ((4,0,0),(0,4,0),(4,4,0))]
    faces = [dict(face=i, vertices=v, texture_extent=[32,32], texture_page=0x60,
                  semi_transparent=False) for i,v in enumerate(mesh)]
    resource = dict(resource_id=42, geometry_fingerprint=123, triangles=8,
                    quads=1, source_faces=faces)
    path = dict(state_count=2, resources={0: (42,123,10)}, masks={
        (0,i,0): 2 if i < 4 else (3 if i < 8 else 1) for i in range(10)})
    return resource, {999: path}


def test_supported_partition_keeps_shared_structure_separate_from_alternatives():
    resource, paths = supported_fixture()
    result = audit(resource, paths)
    assert not result["partitions"]
    family, = result["supported_partitions"]
    assert family["lower_faces"] == [8,9]
    assert family["higher_faces"] == [0,1,2,3]
    assert family["support_faces"] == [4,5,6,7]
    assert family["coverage"]["uncovered_area2"] == "0"
    assert family["complete_program_witness"] == dict(
        path_id=999, low_states=1, complete_high_states=1)


def test_supported_partition_requires_stability_in_all_low_and_all_high_states():
    for missing in (1,2):
        resource, paths = supported_fixture()
        paths[999]["masks"][(0,7,0)] = missing
        assert not audit(resource, paths)["supported_partitions"]
    resource, paths = supported_fixture()
    paths[999]["state_count"] = 3
    for i in range(7):
        paths[999]["masks"][(0,i,0)] |= 4
    assert not audit(resource, paths)["supported_partitions"]


def test_supported_partition_does_not_borrow_a_transparent_or_deforming_layer():
    for flag in ("semi_transparent", "deforming"):
        resource, paths = supported_fixture()
        resource["source_faces"][7][flag] = True
        assert not audit(resource, paths)["supported_partitions"]
