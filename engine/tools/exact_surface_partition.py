"""Exact integer certificates for convex source-surface refinements.

No game, camera, screen-space tolerance, depth bias or texture policy. A
certificate means contained, interior-disjoint children cover their parent
exactly. Callers must independently prove semantic alternatives and quality.
"""
from math import gcd
from functools import reduce
from fractions import Fraction


def surface(vertices):
    if not 3 <= len(vertices) <= 4 or any(len(v) != 3 for v in vertices):
        return None
    if any(not isinstance(x, int) for v in vertices for x in v):
        return None
    vertices = tuple(tuple(v) for v in vertices)
    if len(set(vertices)) != len(vertices):
        return None
    a, b, c = vertices[:3]
    u, v = [b[i] - a[i] for i in range(3)], [c[i] - a[i] for i in range(3)]
    normal = (u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0])
    divisor = reduce(gcd, normal)
    if not divisor:
        return None
    normal = tuple(x // divisor for x in normal)
    if next(x for x in normal if x) < 0:
        normal = tuple(-x for x in normal)
    plane = (*normal, sum(normal[i] * a[i] for i in range(3)))
    if any(sum(normal[i] * p[i] for i in range(3)) != plane[3] for p in vertices):
        return None
    drop = max(range(3), key=lambda i: abs(normal[i]))
    polygon = tuple(tuple(p[i] for i in range(3) if i != drop) for p in vertices)
    area = sum(polygon[i][0] * polygon[(i+1) % len(polygon)][1] -
               polygon[i][1] * polygon[(i+1) % len(polygon)][0] for i in range(len(polygon)))
    if area < 0:
        polygon = polygon[::-1]
    if not area or any(cross(polygon[i-1], polygon[i], polygon[(i+1) % len(polygon)]) < 0
                       for i in range(len(polygon))):
        return None
    return plane, polygon, abs(area)


def cross(a, b, p):
    return (b[0]-a[0])*(p[1]-a[1]) - (b[1]-a[1])*(p[0]-a[0])


def contains(parent, child):
    return all(cross(parent[i-1], parent[i], p) >= 0
               for i in range(len(parent)) for p in child)


def interiors_overlap(a, b):
    # Separating-axis theorem. Touching shared edges/vertices are not overlap.
    return not any(all(cross(p[i-1], p[i], v) <= 0 for v in q)
                   for p, q in ((a, b), (b, a)) for i in range(len(p)))


def certify_partition(parent, children):
    if parent is None or len(children) < 2 or any(child is None for child in children):
        return False
    if any(c[0] != parent[0] or not 0 < c[2] < parent[2] or
           not contains(parent[1], c[1]) for c in children):
        return False
    if sum(c[2] for c in children) != parent[2]:
        return False
    return not any(interiors_overlap(a[1], b[1])
                   for i, a in enumerate(children) for b in children[i+1:])


def clipped_polygon(subject, clip):
    """Exact convex clipping for offline diagnosis, not a relaxed certificate."""
    output = list(subject)
    for i in range(len(clip)):
        source, output = output, []
        a, b = clip[i-1], clip[i]
        for j, end in enumerate(source):
            start = source[j-1]
            ds, de = cross(a, b, start), cross(a, b, end)
            if (ds < 0) != (de < 0):
                t = Fraction(ds, ds-de)
                output.append(tuple(start[k] + t*(end[k]-start[k]) for k in (0, 1)))
            if de >= 0:
                output.append(end)
        output = [p for j, p in enumerate(output) if p != output[j-1]]
    return tuple(output)


def polygon_area2(polygon):
    return abs(sum(polygon[i-1][0]*polygon[i][1] -
                   polygon[i-1][1]*polygon[i][0] for i in range(len(polygon))))


def diagnose_coverage(parent, children):
    """Report exact missing area only when the clipped children don't overlap.

    Crossing the parent border is allowed for this diagnostic. Noncoplanar
    children remain unsupported. Neither partial coverage nor mere area sums
    authorize a presentation replacement.
    """
    if parent is None or any(c is None or c[0] != parent[0] for c in children):
        raise ValueError("coverage diagnosis requires exact coplanar surfaces")
    clipped = [clipped_polygon(c[1], parent[1]) for c in children]
    area = sum(polygon_area2(p) for p in clipped)
    overlaps = sum(polygon_area2(clipped_polygon(a, b)) > 0
                   for i, a in enumerate(clipped) if polygon_area2(a) > 0
                   for b in clipped[i+1:] if polygon_area2(b) > 0)
    return dict(parent_area2=parent[2], child_polygons=len(children),
                crossing_children=sum(not contains(parent[1], c[1]) for c in children),
                clipped_area2_sum=str(area), overlapping_pairs=overlaps,
                uncovered_area2=str(parent[2]-area) if not overlaps else None,
                strict_partition_certified=certify_partition(parent, children))


def certify_cover(parent, children):
    """Exact planar union, allowing source subdivisions across the boundary.

    Independent rational clipping, with the same global no-overlap contract
    as the C++ integer edge sweep. This alone does not establish alternatives.
    """
    if (parent is None or not 2 <= len(children) <= 6144 or
            any(c is None or c[0] != parent[0] for c in children) or
            any(interiors_overlap(a[1], b[1]) for i, a in enumerate(children)
                for b in children[i+1:])):
        return False
    return diagnose_coverage(parent, children)["uncovered_area2"] == "0"


def project_footprint(vertices, reference):
    """Axis projection; does not assert that a shared source lies on the plane."""
    if (reference is None or not 3 <= len(vertices) <= 4 or
            any(len(v) != 3 or any(not isinstance(x, int) or not -32767 <= x <= 32767
                                  for x in v) for v in vertices)):
        return None
    drop = max(range(3), key=lambda i: abs(reference[0][i]))
    flat = [tuple(0 if k == drop else v[k] for k in range(3)) for v in vertices]
    projected = surface(flat)
    return (reference[0], projected[1], projected[2]) if projected else None


def certify_anchored_patch(boundary, mesh):
    """Source-corner anchors + one connected, unfolded fine surface.

    The detailed surface can be nonplanar; it is NOT flattened for rendering.
    Projection is only an exact domain-coverage proof, independent of view.
    """
    if not 3 <= len(boundary) <= 4 or not 2 <= len(mesh) <= 6144:
        return False
    reference = surface(boundary[:3])
    parent = project_footprint(boundary, reference)
    if not parent:
        return False
    footprints = [project_footprint(triangle, reference) for triangle in mesh]
    if any(p is None for p in footprints):
        return False
    vertices = {tuple(p) for triangle in mesh for p in triangle}
    if not set(map(tuple, boundary)) <= vertices:
        return False
    incidence = {}
    edges = []
    for i, triangle in enumerate(mesh):
        face_edges = {tuple(sorted((tuple(triangle[j-1]), tuple(triangle[j]))))
                      for j in range(3)}
        edges.append(face_edges)
        for edge in face_edges:
            incidence.setdefault(edge, set()).add(i)
    reached, pending = {0}, [0]
    while pending:
        i = pending.pop()
        neighbors = set().union(*(incidence[e] for e in edges[i])) - reached
        reached |= neighbors
        pending.extend(neighbors)
    return len(reached) == len(mesh) and certify_cover(parent, footprints)


def connected_cover(parent, mesh, seeds, candidates):
    """Independent rational-area proof for an exactly source-connected cover.

    C++ uses an integer edge sweep instead of clipping/area sums. Neither
    implementation supplies native branch, sidedness or material semantics.
    """
    if (parent is None or len(mesh) > 6144 or len(seeds) < 2 or
            len(set(seeds)) != len(seeds) or
            len(set(candidates)) != len(candidates) or set(seeds) & set(candidates) or
            any(not 0 <= i < len(mesh) for i in [*seeds, *candidates])):
        return None
    footprints = {i: project_footprint(mesh[i], parent) for i in [*seeds, *candidates]}
    footprints = {i: p for i, p in footprints.items()
                  if p and interiors_overlap(parent[1], p[1])}
    selected = set(seeds) & footprints.keys()
    if len(selected) < 2:
        return None
    def overlap(a, b):
        return interiors_overlap(footprints[a][1], footprints[b][1])
    if any(overlap(a, b) for a in selected for b in selected if a < b):
        return None
    def edges(i):
        v = [tuple(p) for p in mesh[i]]
        return {tuple(sorted((v[k-1], v[k]))) for k in range(len(v))}
    accepted_edges = set().union(*(edges(i) for i in selected))
    added = []
    while True:
        coverage = diagnose_coverage(parent, [footprints[i] for i in sorted(selected)])
        if coverage["uncovered_area2"] == "0":
            return dict(support_faces=sorted(added), coverage=coverage)
        frontier = [i for i in candidates if i not in selected and i in footprints
                    and edges(i) & accepted_edges and not any(overlap(i, j) for j in selected)]
        if not frontier or any(overlap(a, b) for a in frontier for b in frontier if a < b):
            return None
        for i in frontier:
            selected.add(i)
            added.append(i)
            accepted_edges |= edges(i)
