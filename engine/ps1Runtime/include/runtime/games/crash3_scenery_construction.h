#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace ps1::games::crash3 {

// Read-only semantic reconstruction of 8004305C/800430D4 -> 80042DBC.
// Only the adapter knows guest program addresses; arithmetic consumes owned
// values, so callers can use it for retained geometry and independent tests.
struct SceneryConstructionState {
  uint32_t triangleProgram = 0;
  uint32_t quadProgram = 0;
  bool twoPointLighting = false;
  struct Point {
    uint32_t packedXy = 0;
    int32_t z = 0;
  };
  std::array<Point, 2> points{};
  std::array<int32_t, 3> farColor{};
};

bool sceneryUsesTwoPointLighting(uint32_t triangleProgram,
                                 uint32_t quadProgram) noexcept;

struct SceneryConstructionVertex {
  uint32_t rawXy = 0;
  std::array<int16_t, 3> model{};
  std::array<float, 3> color255{};
};

struct SceneryLightingSelection {
  uint8_t pointMask = 0;
  bool flatBlack = false;
};

// The first native corner chooses which lighting callbacks run for the whole
// primitive. This is deliberately separate from per-corner shading. The span
// must contain three/four corners in native construction order, not fan order.
SceneryLightingSelection selectSceneryPrimitiveLighting(
    const SceneryConstructionState &state,
    std::span<const SceneryConstructionVertex> nativeCorners) noexcept;
std::array<float, 3> shadeSceneryConstructionVertex(
    const SceneryConstructionState &state, SceneryLightingSelection selection,
    const SceneryConstructionVertex &vertex) noexcept;

} // namespace ps1::games::crash3
