#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace ps1::games::crash3 {

// A PS1 page-sized screen quad is a canvas operation (fade/flash), not a HUD
// element. Recognize its exact topology and all four VRAM page origins; no
// level, packet address, colour, frame number or approximate size is involved.
inline bool isFullPageScreenQuad(
    const std::array<std::array<int32_t, 2>, 4> &vertices) noexcept {
  int32_t left = vertices[0][0], right = left;
  int32_t top = vertices[0][1], bottom = top;
  for (const auto &vertex : vertices) {
    left = std::min(left, vertex[0]);
    right = std::max(right, vertex[0]);
    top = std::min(top, vertex[1]);
    bottom = std::max(bottom, vertex[1]);
  }
  if ((left != 0 && left != 512) || right != left + 512 ||
      (top != 0 && top != 256) || bottom != top + 256)
    return false;
  uint32_t corners = 0;
  for (const auto &vertex : vertices) {
    if ((vertex[0] != left && vertex[0] != right) ||
        (vertex[1] != top && vertex[1] != bottom))
      return false;
    corners |= 1u << ((vertex[0] == right ? 1u : 0u) |
                     (vertex[1] == bottom ? 2u : 0u));
  }
  // GP0 splits (0,1,2), (1,2,3). The shared 1--2 edge must be a diagonal,
  // otherwise four corner points can still describe overlapping half-quads.
  return corners == 15u && vertices[1][0] != vertices[2][0] &&
         vertices[1][1] != vertices[2][1];
}

} // namespace ps1::games::crash3
