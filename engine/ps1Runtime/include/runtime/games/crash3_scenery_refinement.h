#pragma once

#include "runtime/games/crash3_profile.h"
#include <optional>
#include <span>
#include <vector>

namespace ps1::games::crash3 {
struct ScenerySurfaceRefinement {
  // Sorted source ordinals: one triangle or the two halves of one quad.
  // Higher faces are a set of obligations, not a single alias for this owner.
  std::vector<uint32_t> lowerFaces;
  std::vector<uint32_t> higherFaces;
  // Unchanged source faces present in both alternatives. These close an exact
  // 3D seam/footprint, but are NOT exclusive detail choices or low->high
  // aliases.
  std::vector<uint32_t> supportFaces;
  // One complete authored fine branch, exact 3D corner anchors/connectivity,
  // and a hole-free unfolded domain. Native partial handoffs are not a veto:
  // both exclusive branches are proven independently of their culled overlap.
  bool anchoredSourcePatch = false;
  bool operator==(const ScenerySurfaceRefinement &) const = default;
};
using SceneryPathStates = std::span<const std::vector<uint16_t>>;

// Local source-program certificates need immutable world bindings, not a
// successful camera/lighting/placement verdict for the whole resident table.
// The caller still checks every decoded path bound and the actual current SLST.
bool sceneryVariantBindingsReady(
    std::span<const ResidentPresentationResource> resources);

// Certificate-only material view. Bit 31 in the real 80041924/80041BFC remap
// descriptor denotes a direct distance selector, NOT an animation program.
// Canonicalize its nearest authored record without changing captured native
// materials, animation clocks, or the renderer's presentation records.
std::optional<ResidentPresentationMaterial> sceneryRefinementMaterial(
    const ResidentPresentationMaterial &native, uint32_t descriptor,
    const ResidentPresentationMaterial *nearest, uint32_t nearestShift);

// Many-face source alternatives: exact planar coverage, non-overlapping finer
// polygons, no loss of texel density, and an actual complete native program
// selecting each alternative. No current-view admission or guest mutation.
std::vector<ScenerySurfaceRefinement> findScenerySurfaceRefinements(
    std::span<const ResidentGpuTopologyFace> topology,
    std::span<const ResidentGpuRawVertex> vertices,
    std::span<const ResidentPresentationMaterial> materials, uint32_t world,
    uint32_t triangleCount, uint32_t quadCount,
    std::span<const SceneryPathStates> paths, bool allowSharedSupports = true);
bool scenerySurfaceRefinementContradicted(
    const ScenerySurfaceRefinement &family, uint32_t world,
    uint32_t triangleCount, std::span<const SceneryPathStates> paths);
} // namespace ps1::games::crash3
