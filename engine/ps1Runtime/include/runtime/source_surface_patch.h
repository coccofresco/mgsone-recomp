#pragma once

#include "runtime/source_surface_cover.h"

namespace ps1::surface_partition {
// A detailed surface need not lie on its coarse approximation's plane. It
// must retain every exact 3D source corner, form ONE edge-connected surface,
// and cover the projected domain without folds, overlaps or holes. This is
// geometry evidence only: the game adapter still owes native branch/material
// semantics. No height tolerance, screen coordinates or camera are involved.
inline bool certifyAnchoredPatch(std::span<const Point> boundary,
                                 std::span<const Triangle> triangles) {
  if (boundary.size() < 3 || boundary.size() > 4 || triangles.size() < 2 ||
      triangles.size() > 6144)
    return false;
  const auto reference = makeSurface(boundary.first(3));
  if (!reference)
    return false;
  const auto parent = projectFootprint(boundary, *reference);
  if (!parent)
    return false;
  using Edge = std::array<Point, 2>;
  const auto edge = [](Point a, Point b) -> Edge {
    return a < b ? Edge{a, b} : Edge{b, a};
  };
  std::map<Edge, std::vector<uint32_t>> incidence;
  std::set<Point> vertices;
  std::vector<Surface> pieces;
  for (uint32_t i = 0; i < triangles.size(); ++i) {
    const auto footprint = projectFootprint(triangles[i], *reference);
    if (!footprint)
      return false;
    pieces.push_back(*footprint);
    for (uint32_t j = 0; j < 3; ++j) {
      vertices.insert(triangles[i][j]);
      incidence[edge(triangles[i][j], triangles[i][(j + 1) % 3])].push_back(i);
    }
  }
  for (const auto &point : boundary)
    if (!vertices.contains(point))
      return false;
  std::vector<bool> visited(triangles.size());
  std::vector<uint32_t> pending{0};
  visited[0] = true;
  uint32_t connected = 0;
  while (!pending.empty()) {
    const uint32_t i = pending.back();
    pending.pop_back();
    ++connected;
    for (uint32_t j = 0; j < 3; ++j)
      for (uint32_t neighbor :
           incidence[edge(triangles[i][j], triangles[i][(j + 1) % 3])])
        if (!visited[neighbor]) {
          visited[neighbor] = true;
          pending.push_back(neighbor);
        }
  }
  return connected == triangles.size() && certifyCover(*parent, pieces);
}
} // namespace ps1::surface_partition
