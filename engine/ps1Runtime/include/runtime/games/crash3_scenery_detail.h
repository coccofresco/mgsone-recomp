#pragma once

#include "runtime/games/crash3_profile.h"
#include "runtime/games/crash3_scenery_refinement.h"
#include "runtime/source_selection_program.h"

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ps1::games::crash3 {

struct SceneryMaterialSelection {
  uint16_t nativeToken = 0;
  uint16_t nearestToken = 0;
  bool distanceSelected = false;
};

// The animation program runs once; only its terminal distance selector has
// two interpretations. Never freeze animation to obtain a sharper texture.
// readDescriptor returns nullopt for an unavailable captured source record.
template <typename ReadDescriptor>
std::optional<SceneryMaterialSelection>
selectSceneryMaterial(uint16_t token, uint32_t animationState,
                      uint32_t distanceShift, uint32_t nearestShift,
                      ReadDescriptor &&readDescriptor) {
  if ((token & 0x800u) == 0u)
    return SceneryMaterialSelection{token, token, false};
  auto descriptor = readDescriptor(token & 0x7ffu);
  if (!descriptor.has_value())
    return std::nullopt;
  if ((*descriptor & 0x80000000u) == 0u) {
    const uint32_t phase =
        (animationState >> ((*descriptor >> 26u) & 31u)) + (*descriptor >> 19u);
    const uint16_t index = static_cast<uint16_t>(
        (*descriptor + (phase & ((*descriptor >> 12u) & 0x7fu))) & 0x7ffu);
    if ((*descriptor & 0x800u) == 0u)
      return SceneryMaterialSelection{index, index, false};
    descriptor = readDescriptor(index);
    if (!descriptor.has_value())
      return std::nullopt;
  }
  const uint32_t base = *descriptor & 0x7ffu;
  return SceneryMaterialSelection{
      static_cast<uint16_t>(base +
                            ((*descriptor >> (distanceShift & 31u)) & 3u)),
      static_cast<uint16_t>(base +
                            ((*descriptor >> (nearestShift & 31u)) & 3u)),
      true};
}

// Separate presentation policy from the immutable native-material oracle.
// Capture + resource incarnation + face identify an override; RAM addresses,
// screen position and previous visibility never authorize one.
struct SceneryDetailOverride {
  uint32_t resourceId = 0;
  uint64_t geometryFingerprint = 0;
  uint32_t faceOrdinal = 0;
  ResidentPresentationMaterial material;
  uint32_t presentationFaceIndex = UINT32_MAX;
  bool nearPlaneClipped = false;
};

// A source-program alternative, not geometric occlusion. The initial policy
// accepts only identical native corner indices/primitive shape and normalized
// texture mapping, with a strictly dominating static texel footprint. It does
// not mistake animation, opposite sides, decals or arbitrary overlaps for LOD.
struct SceneryVariantPair {
  uint32_t lowerFace = 0;
  uint32_t higherFace = 0;
  bool exclusivePathWitness = false;
  bool coSelected =
      false; // Any complete-path contradiction permanently vetoes.
};
struct ScenerySourcePath {
  uint32_t section = 0;
  uint32_t entryBase = 0;
  uint32_t stateCount = 0;
  uint32_t resourceId = 0;
  bool operator==(const ScenerySourcePath &) const = default;
};
// Read-only extraction of the actual 80032848 property-259/639 access and
// 8001435C/80014BAC resolved-reference lookup. No resource loading, callbacks,
// reference-count mutation or guessed camera-dependent visibility.
std::optional<std::vector<ScenerySourcePath>>
readSceneryZonePaths(const Memory &memory, uint32_t zoneEntry);
struct SceneryBoundSourcePath {
  ScenerySourcePath source;
  uint32_t worldCount = 0;
  // Original zone slots -> current resource slots; -1 means an unrelated asset.
  std::array<int8_t, 8> worlds{};
  bool operator==(const SceneryBoundSourcePath &) const = default;
};
// Enumerate the actual 80014BAC hash registry, never scan arbitrary RAM or load
// guest resources. An immutable mesh can have coarse/fine witnesses in OTHER
// sections, with a different world slot. Only loaded, bound programs qualify.
std::vector<SceneryBoundSourcePath> readRegisteredSceneryPaths(
    const Memory &memory, uint32_t currentZone,
    std::span<const ResidentPresentationResource> resources);
std::vector<SceneryVariantPair> findSceneryVariantPairs(
    std::span<const ResidentGpuTopologyFace> topology,
    std::span<const ResidentPresentationMaterial> materials);
// Input is the complete decoded program of 80021674 -> 8002AAB8, including
// the independent endpoint check performed by the producer. No guest writes.
// Invalid bounds reject the entire observation without modifying evidence.
bool observeSceneryVariantMembership(
    std::vector<SceneryVariantPair> &pairs, uint32_t world,
    uint32_t triangleCount, uint32_t quadCount,
    std::span<const std::vector<uint16_t>> states);

struct SceneryDrawSelection {
  source_selection::Program program;
  // Terminal ownership obligations, NOT new geometric family certificates.
  std::vector<ScenerySurfaceRefinement> draws;
  std::vector<uint8_t> singleFaceAliases;
};
struct SceneryVariantPlan {
  uint32_t resourceId = 0;
  uint64_t geometryFingerprint = 0;
  uint64_t materialFingerprint = 0;
  // Sparse, sorted lower -> higher face replacements. Published immutably and
  // shared through resource retention and both interpolation endpoints.
  std::vector<std::array<uint32_t, 2>> replacements;
  // Complete many-face replacement obligations, never a one-face alias.
  std::vector<ScenerySurfaceRefinement> refinements;
  // Separate source evidence from executed presentation choice. Constructed
  // once in the game adapter, before Vulkan receives a scene; immutable across
  // capture retention/interpolation. Unclassified faces remain unclassified.
  std::shared_ptr<const SceneryDrawSelection> selection;
};
bool compileSceneryDrawSelection(SceneryVariantPlan &plan, uint32_t faceCount);
std::optional<uint32_t>
sceneryVariantReplacement(const SceneryVariantPlan &plan, uint32_t resourceId,
                          uint64_t geometryFingerprint, uint32_t faceOrdinal);
const ScenerySurfaceRefinement *
scenerySurfaceRefinement(const SceneryVariantPlan &plan, uint32_t resourceId,
                         uint64_t geometryFingerprint, uint32_t faceOrdinal);
struct SceneryDetailSnapshot {
  uint32_t captureId = 0;
  std::vector<SceneryDetailOverride> materials;
  std::vector<std::shared_ptr<const SceneryVariantPlan>> variants;
};
void publishSceneryDetailSnapshot(
    SceneryDetailSnapshot snapshot,
    const std::shared_ptr<const ResidentPresentationSnapshot> &owner);
std::shared_ptr<const SceneryDetailSnapshot>
sceneryDetailSnapshot(uint32_t captureId);
void clearSceneryDetailSnapshots();

using SceneryClipWeights = std::array<std::array<double, 3>, 3>;
// Consume the geometry clipper's actual barycentric construction. Inverting
// low-resolution UVs loses this information for one-texel/degenerate mappings.
ResidentPresentationMaterial
clipSceneryDetailMaterial(const ResidentPresentationMaterial &material,
                          const SceneryClipWeights &weights);
void appendSceneryDetailMaterial(SceneryDetailSnapshot &snapshot,
                                 const ResidentPresentationFace &nativeFace,
                                 uint32_t presentationFaceIndex,
                                 const ResidentPresentationMaterial &material);
// Bound to the exact emitted fan, not just the source triangle. Retention is
// allowed only when the producer republishes its owned high-detail material.
std::optional<ResidentPresentationMaterial>
applySceneryDetailMaterial(const ResidentPresentationFace &nativeFace,
                           const SceneryDetailOverride &detail,
                           uint32_t presentationFaceIndex);

} // namespace ps1::games::crash3
