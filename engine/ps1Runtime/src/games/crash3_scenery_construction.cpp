#include "runtime/games/crash3_scenery_construction.h"
#include "runtime/gte_color_math.h"

#include <algorithm>
#include <cstdlib>

namespace ps1::games::crash3 {
namespace {
int64_t nativePointDistance(const SceneryConstructionVertex &vertex,
                            const SceneryConstructionState::Point &point) {
  const uint32_t packed =
      static_cast<uint16_t>(vertex.model[0]) |
      (uint32_t{static_cast<uint16_t>(vertex.model[1])} << 16u);
  // SUBU operates on one packed XY word, so a low-half borrow changes Y.
  // Independent float XYZ subtraction is NOT the native lighting oracle.
  const uint32_t difference = packed - point.packedXy;
  const int64_t x = static_cast<int16_t>(difference);
  const int64_t y = static_cast<int32_t>(difference) >> 16;
  const int64_t z = static_cast<int32_t>(
      static_cast<uint32_t>(static_cast<int32_t>(vertex.model[2])) -
      static_cast<uint32_t>(point.z));
  return std::abs(x) + std::abs(y) + std::abs(z);
}
} // namespace

bool sceneryUsesTwoPointLighting(uint32_t triangleProgram,
                                 uint32_t quadProgram) noexcept {
  return triangleProgram == 0x8004305cu && quadProgram == 0x800430d4u;
}

SceneryLightingSelection selectSceneryPrimitiveLighting(
    const SceneryConstructionState &state,
    std::span<const SceneryConstructionVertex> nativeCorners) noexcept {
  SceneryLightingSelection result;
  if (!state.twoPointLighting || nativeCorners.empty())
    return result;
  for (uint32_t point = 0; point < state.points.size(); ++point)
    if (nativePointDistance(nativeCorners.front(), state.points[point]) <= 2100)
      result.pointMask |= uint8_t{1} << point;
  const bool flagged = std::any_of(
      nativeCorners.begin(), nativeCorners.end(),
      [](const auto &vertex) { return (vertex.rawXy & 0x000c0000u) != 0u; });
  // 80042F74/80042FD8 emit opaque F3/F4, not the authored texture, when
  // neither light's broad phase nor an intrinsic lighting flag applies.
  result.flatBlack = result.pointMask == 0u && !flagged;
  return result;
}

std::array<float, 3> shadeSceneryConstructionVertex(
    const SceneryConstructionState &state, SceneryLightingSelection selection,
    const SceneryConstructionVertex &vertex) noexcept {
  if (!state.twoPointLighting)
    return vertex.color255;
  if (selection.flatBlack)
    return {};
  const bool bit18 = (vertex.rawXy & (1u << 18u)) != 0u;
  const bool bit19 = (vertex.rawXy & (1u << 19u)) != 0u;
  if (bit18 && !bit19)
    return vertex.color255; // 80042EA8: raw RGB2 copy, no DPCS.
  int64_t intensity = 0;
  for (uint32_t point = 0; point < state.points.size(); ++point)
    if ((selection.pointMask & (1u << point)) != 0u)
      intensity += std::max<int64_t>(
          0, 1300 - nativePointDistance(vertex, state.points[point]));
  intensity *= 4;
  if (bit19)
    intensity += 409 + 818; // Includes the unconditional branch delay slot.
  if (intensity == 0)
    return {};
  if (intensity >= 4096)
    return vertex.color255;
  return gte_math::distanceCueColor(vertex.color255, state.farColor,
                                    static_cast<int16_t>(4096 - intensity));
}

} // namespace ps1::games::crash3
