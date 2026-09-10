import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from exact_surface_partition import (surface, certify_partition, diagnose_coverage,
                                     connected_cover, project_footprint, certify_cover,
                                     certify_anchored_patch)


def quad(x0, y0, x1, y1, z=0):
    return surface(((x0,y0,z),(x1,y0,z),(x1,y1,z),(x0,y1,z)))


def test_exact_partition_accepts_different_tessellation_and_winding():
    parent = quad(0,0,4,4)
    children = [quad(0,0,2,4),
                surface(((2,0,0),(4,4,0),(4,0,0))),
                surface(((2,0,0),(2,4,0),(4,4,0)))]
    assert certify_partition(parent, children)


def test_equal_area_is_not_sufficient_with_overlap_and_a_hole():
    assert not certify_partition(quad(0,0,4,4), [quad(0,0,2,4),quad(0,0,2,4)])


def test_exact_plane_containment_and_full_coverage_are_required():
    parent=quad(0,0,4,4)
    assert not certify_partition(parent,[quad(0,0,2,4),quad(2,0,4,4,1)])
    assert not certify_partition(parent,[quad(0,0,2,4),quad(2,0,4,3)])
    assert not certify_partition(parent,[quad(-1,0,1,4),quad(1,0,3,4)])
    assert not certify_partition(parent,[parent])


def test_nonplanar_and_self_crossing_quads_are_not_convex_surfaces():
    assert surface(((0,0,0),(4,0,0),(4,4,1),(0,4,0))) is None
    assert surface(((0,0,0),(4,4,0),(4,0,0),(0,4,0))) is None
    assert surface(((0,0,0),(0,0,0),(1,1,1))) is None
    assert surface(((0,0,0),(4,0,0),(4,4,0),(4,4,0))) is None


def test_accepts_integer_sequences_but_does_not_round_fractional_planes():
    assert surface([[0,0,0],[4,0,0],[4,4,0],[0,4,0]]) == quad(0,0,4,4)
    assert surface(((0,0,0),(4,0,0),(4,4,0.1),(0,4,0))) is None


def test_crossing_coverage_diagnostic_is_not_a_relaxed_partition_certificate():
    result = diagnose_coverage(quad(0, 0, 4, 4),
                               [quad(-1, 0, 2, 4), quad(2, 0, 5, 4)])
    assert result["crossing_children"] == 2
    assert result["clipped_area2_sum"] == "32"
    assert result["uncovered_area2"] == "0"
    assert not result["strict_partition_certified"]


def test_planar_cover_is_exact_even_when_subdivision_crosses_parent_edges():
    parent = quad(0, 0, 4, 4)
    assert certify_cover(parent, [quad(-1, 0, 2, 4), quad(2, 0, 5, 4)])
    assert not certify_cover(parent, [quad(-1, 0, 2, 4), quad(2, 0, 5, 3)])
    assert not certify_cover(parent, [quad(-1, 0, 3, 4), quad(2, 0, 5, 4)])
    assert not certify_cover(parent, [quad(-1, 0, 2, 4), quad(2, 0, 5, 4, 1)])


def test_curved_source_patch_needs_all_anchors_connectivity_and_complete_domain():
    boundary = [(0,0,0),(4,0,0),(4,4,0),(0,4,0)]
    mesh = [(boundary[i],boundary[(i+1)%4],(2,2,1)) for i in range(4)]
    assert certify_anchored_patch(boundary,mesh)
    assert not certify_anchored_patch(boundary,mesh[:-1])
    assert not certify_anchored_patch(boundary,mesh+[mesh[0]])
    displaced = [[(x,y,z+1) for x,y,z in triangle] for triangle in mesh]
    assert not certify_anchored_patch(boundary,displaced)


def test_partial_fractional_area_and_overlap_diagnostics_do_not_round():
    triangle = surface(((0, 0, 0), (3, 0, 0), (0, 1, 0)))
    result = diagnose_coverage(quad(0, 0, 1, 1), [triangle])
    assert result["clipped_area2_sum"] == "5/3"
    assert result["uncovered_area2"] == "1/3"
    result = diagnose_coverage(quad(0, 0, 4, 4),
                               [quad(0, 0, 2, 4), quad(0, 0, 2, 4)])
    assert result["overlapping_pairs"] == 1
    assert result["uncovered_area2"] is None


def supported_mesh():
    return [
        ((0,0,0),(1,0,0),(0,4,0)), ((1,0,0),(1,4,0),(0,4,0)),
        ((3,0,0),(4,0,0),(3,4,0)), ((4,0,0),(4,4,0),(3,4,0)),
        ((1,0,0),(2,0,1),(1,4,0)), ((2,0,1),(2,4,1),(1,4,0)),
        ((2,0,1),(3,0,0),(2,4,1)), ((3,0,0),(3,4,0),(2,4,1)),
    ]


def test_connected_cover_uses_shared_source_seams_not_projected_proximity():
    mesh = supported_mesh()
    result = connected_cover(quad(0,0,4,4), mesh, [0,1,2,3], [4,5,6,7])
    assert result["support_faces"] == [4,5,6,7]
    assert result["coverage"]["uncovered_area2"] == "0"
    assert result["coverage"]["clipped_area2_sum"] == "32"
    mesh[4:] = [tuple((x,y,z+1) for x,y,z in t) for t in mesh[4:]]
    assert connected_cover(quad(0,0,4,4), mesh, [0,1,2,3], [4,5,6,7]) is None


def test_connected_cover_rejects_missing_duplicate_and_ambiguous_supports():
    mesh = supported_mesh()
    for candidates in ([4,5,6], [4,4,6,7], [0,4,5,6,7], [4,5,6,8]):
        assert connected_cover(quad(0,0,4,4), mesh, [0,1,2,3], candidates) is None
    mesh.append(((1,0,0),(2,0,2),(1,4,0)))
    assert connected_cover(quad(0,0,4,4), mesh, [0,1,2,3], [4,5,6,7,8]) is None


def test_projection_validates_original_coordinates_before_dropping_an_axis():
    assert project_footprint(((0,0,40000),(1,0,0),(0,1,0)),quad(0,0,4,4)) is None
    assert project_footprint(((0,0,0.1),(1,0,0),(0,1,0)),quad(0,0,4,4)) is None
    assert project_footprint(((0,0,0),(1,0,0),(0,1,1)),quad(0,0,4,4)) is not None


def test_connected_cover_is_independent_of_axis_winding_and_candidate_order():
    import itertools
    for axes in itertools.permutations(range(3)):
        transform = lambda p: tuple(p[i] for i in axes)
        parent = surface([transform(p) for p in ((0,0,0),(4,0,0),(4,4,0),(0,4,0))])
        mesh = [tuple(transform(p) for p in t[::-1]) for t in supported_mesh()]
        result = connected_cover(parent, mesh, [3,2,1,0], [7,6,5,4])
        assert result["support_faces"] == [4,5,6,7]
