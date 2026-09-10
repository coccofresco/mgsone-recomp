#pragma once

#include "runtime/source_surface_partition.h"
#include <map>
#include <set>
#include <vector>

namespace ps1::surface_partition {

using Triangle = std::array<Point, 3>;

// Footprint along the reference plane's dropped coordinate axis. This is NOT
// a claim that the input lies on that plane. The caller owns source placement,
// surface semantics and the relation between exclusive and shared geometry.
inline std::optional<Surface> projectFootprint(std::span<const Point> vertices,
                                               const Surface &reference) {
  if (vertices.size() < 3 || vertices.size() > 4 || reference.twiceArea <= 0)
    return std::nullopt;
  uint32_t drop = 0;
  for (uint32_t i = 1; i < 3; ++i)
    if (std::abs(reference.plane[i]) > std::abs(reference.plane[drop]))
      drop = i;
  if (reference.plane[drop] == 0)
    return std::nullopt;
  std::array<Point, 4> flat{};
  for (uint32_t i = 0; i < vertices.size(); ++i) {
    for (int32_t coordinate : vertices[i])
      if (coordinate < -32767 || coordinate > 32767)
        return std::nullopt;
    flat[i] = vertices[i];
    flat[i][drop] = 0;
  }
  auto result = makeSurface(std::span{flat}.first(vertices.size()));
  if (result)
    result->plane = reference.plane;
  return result;
}

namespace cover_detail {
struct Ratio {
  uint64_t numerator = 0, denominator = 1;
};
// Exact ordering without a potentially overflowing cross multiplication or a
// compiler-specific 128-bit type. All bounds used below are in [0,1].
inline bool less(Ratio a, Ratio b) noexcept {
  bool inverted = false;
  for (;;) {
    const auto qa = a.numerator / a.denominator,
               qb = b.numerator / b.denominator;
    if (qa != qb)
      return inverted ? qa > qb : qa < qb;
    const auto ra = a.numerator % a.denominator,
               rb = b.numerator % b.denominator;
    if (ra == 0 || rb == 0)
      return ra == rb ? false : (inverted ? ra != 0 : ra == 0);
    a = {a.denominator, ra};
    b = {b.denominator, rb};
    inverted = !inverted;
  }
}
inline bool entersInterior(Point2 a, Point2 b, const Surface &parent) noexcept {
  Ratio lo{0, 1}, hi{1, 1};
  for (uint32_t i = 0; i < parent.count; ++i) {
    const auto p = parent.polygon[i],
               q = parent.polygon[(i + 1) % parent.count];
    const auto da = cross(p, q, a), db = cross(p, q, b);
    if (da <= 0 && db <= 0)
      return false;
    if (da <= 0) {
      Ratio bound{static_cast<uint64_t>(-da), static_cast<uint64_t>(db - da)};
      if (less(lo, bound))
        lo = bound;
    }
    if (db <= 0) {
      Ratio bound{static_cast<uint64_t>(da), static_cast<uint64_t>(da - db)};
      if (less(bound, hi))
        hi = bound;
    }
  }
  return less(lo, hi);
}
} // namespace cover_detail

// Certify complete coverage by an interior-disjoint projected mesh, even if
// its triangles cross the reference boundary or have T-junction subdivisions.
// Signed edge intervals cancel exactly. No remaining mesh boundary may enter
// the reference interior, and an interior witness must belong to the mesh.
// Consequently there is neither an uncovered hole nor an uncovered exterior
// component inside the connected convex reference. No raster/area tolerance.
inline bool certifyCover(const Surface &parent,
                         std::span<const Surface> pieces) {
  if (parent.count < 3 || parent.count > 4 || parent.twiceArea <= 0 ||
      pieces.size() < 2 || pieces.size() > 6144)
    return false;
  using Line = std::array<int64_t, 3>;
  struct Event {
    Point2 point{};
    int balance = 0;
  };
  std::map<Line, std::map<int64_t, Event>> lines;
  Point2 centroid{};
  for (uint32_t i = 0; i < parent.count; ++i)
    for (uint32_t axis = 0; axis < 2; ++axis)
      centroid[axis] += parent.polygon[i][axis];
  bool witness = false;
  for (uint32_t index = 0; index < pieces.size(); ++index) {
    const auto &piece = pieces[index];
    if (piece.count < 3 || piece.count > 4 || piece.twiceArea <= 0 ||
        piece.plane != parent.plane)
      return false;
    for (uint32_t other = 0; other < index; ++other)
      if (interiorsOverlap(piece, pieces[other]))
        return false;
    bool containsCentroid = true;
    for (uint32_t i = 0; i < piece.count; ++i) {
      const auto a = piece.polygon[i], b = piece.polygon[(i + 1) % piece.count];
      const int64_t dx = b[0] - a[0], dy = b[1] - a[1];
      const int64_t divisor = std::gcd(dx, dy);
      if (divisor == 0)
        return false;
      containsCentroid &=
          dx * (centroid[1] - int64_t{parent.count} * a[1]) -
              dy * (centroid[0] - int64_t{parent.count} * a[0]) >=
          0;
      const int64_t sign = dx != 0 ? (dx > 0 ? 1 : -1) : (dy > 0 ? 1 : -1);
      const auto ux = sign * dx / divisor, uy = sign * dy / divisor;
      auto &events = lines[{ux, uy, ux * a[1] - uy * a[0]}];
      const uint32_t axis = ux != 0 ? 0 : 1;
      auto &start = events[a[axis]];
      start.point = a;
      ++start.balance;
      auto &end = events[b[axis]];
      end.point = b;
      --end.balance;
    }
    witness |= containsCentroid;
  }
  if (!witness)
    return false;
  for (const auto &[line, events] : lines) {
    int balance = 0;
    std::optional<Point2> previous;
    for (const auto &[coordinate, event] : events) {
      if (std::abs(balance) > 1)
        return false;
      if (previous && balance != 0 &&
          cover_detail::entersInterior(*previous, event.point, parent))
        return false;
      balance += event.balance;
      previous = event.point;
    }
    if (balance != 0)
      return false;
  }
  return true;
}

// Source-contiguous completion by a caller-authorized shared mesh. The caller
// must independently prove that candidates are stable, compatible and present
// in the relevant native branches. Exact 3D edges, not projected proximity,
// join supports. Ambiguous overlapping frontiers fail rather than picking by
// index/visit order. Returns added triangle indices, never modifies the input.
inline std::optional<std::vector<uint32_t>>
connectedCover(const Surface &parent, std::span<const Triangle> mesh,
               std::span<const uint32_t> seeds,
               std::span<const uint32_t> candidates) {
  if (mesh.size() > 6144 || seeds.size() < 2 || seeds.size() > mesh.size() ||
      candidates.size() > mesh.size())
    return std::nullopt;
  using Edge = std::array<Point, 2>;
  const auto edge = [](Point a, Point b) {
    return a < b ? Edge{a, b} : Edge{b, a};
  };
  std::vector<std::optional<Surface>> footprints(mesh.size());
  std::vector<uint8_t> roles(mesh.size());
  const auto prepare = [&](uint32_t index, uint8_t role) {
    if (index >= mesh.size() || roles[index] != 0)
      return false;
    roles[index] = role;
    footprints[index] = projectFootprint(mesh[index], parent);
    if (footprints[index] && !interiorsOverlap(*footprints[index], parent))
      footprints[index].reset();
    return true;
  };
  for (uint32_t index : seeds)
    if (!prepare(index, 1))
      return std::nullopt;
  for (uint32_t index : candidates)
    if (!prepare(index, 2))
      return std::nullopt;
  std::vector<Surface> accepted;
  std::set<Edge> edges;
  const auto add = [&](uint32_t index) {
    accepted.push_back(*footprints[index]);
    for (uint32_t i = 0; i < 3; ++i)
      edges.insert(edge(mesh[index][i], mesh[index][(i + 1) % 3]));
  };
  for (uint32_t index : seeds)
    if (footprints[index]) {
      for (const auto &other : accepted)
        if (interiorsOverlap(*footprints[index], other))
          return std::nullopt;
      add(index);
    }
  if (accepted.size() < 2)
    return std::nullopt;
  std::vector<uint32_t> supports;
  for (;;) {
    if (certifyCover(parent, accepted))
      return supports;
    std::vector<uint32_t> frontier;
    for (uint32_t index : candidates) {
      if (roles[index] != 2 || !footprints[index])
        continue;
      bool adjacent = false;
      for (uint32_t i = 0; i < 3; ++i)
        adjacent |=
            edges.contains(edge(mesh[index][i], mesh[index][(i + 1) % 3]));
      if (!adjacent ||
          std::any_of(accepted.begin(), accepted.end(), [&](const auto &p) {
            return interiorsOverlap(*footprints[index], p);
          }))
        continue;
      for (uint32_t other : frontier)
        if (interiorsOverlap(*footprints[index], *footprints[other]))
          return std::nullopt;
      frontier.push_back(index);
    }
    if (frontier.empty())
      return std::nullopt;
    for (uint32_t index : frontier) {
      roles[index] = 1;
      add(index);
      supports.push_back(index);
    }
  }
}
} // namespace ps1::surface_partition
