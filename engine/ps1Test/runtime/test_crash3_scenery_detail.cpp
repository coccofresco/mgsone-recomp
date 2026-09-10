#include "runtime/games/crash3_scenery_detail.h"
#include "runtime/memory.h"

#include <gtest/gtest.h>
#include <map>

using namespace ps1::games::crash3;

TEST(Crash3SceneryVariants,
     ZoneProgramReadsNativePropertiesAndResolvedHashPairs) {
  ps1::Memory memory;
  constexpr uint32_t zone = 0x80080000u, section = 0x80081000u;
  constexpr uint32_t entry = 0x80082000u, pair = 0x80083000u;
  constexpr uint32_t eid = 0x58016du, table = 0x80084000u;
  memory.write32(zone, 0x0100ffffu);
  memory.write32(zone + 8u, 7u);
  memory.write32(zone + 12u, 3u);
  memory.write32(zone + 24u, section);
  memory.write32(section + 4u, zone);
  memory.write16(section + 12u, 2u);
  memory.write16(section + 16u, 259u);
  memory.write16(section + 18u, 0x40u);
  memory.write8(section + 20u, 4u);
  memory.write8(section + 21u, 4u);
  memory.write16(section + 22u, 1u);
  memory.write16(section + 24u, 639u);
  memory.write16(section + 26u, 0x48u);
  memory.write8(section + 28u, 1u);
  memory.write8(section + 29u, 1u);
  memory.write16(section + 30u, 1u);
  memory.write16(section + 0x4cu, 1u);
  memory.write32(section + 0x50u, pair);
  memory.write16(section + 0x54u, 48u);
  memory.write32(entry, 0x0100ffffu);
  memory.write32(entry + 4u, eid);
  memory.write32(entry + 8u, 4u);
  memory.write32(entry + 12u, 49u);
  memory.write32(pair, entry);
  memory.write32(pair + 4u, eid);
  auto paths = readSceneryZonePaths(memory, zone);
  ASSERT_TRUE(paths);
  ASSERT_EQ(paths->size(), 1u);
  EXPECT_EQ(paths->front(), (ScenerySourcePath{section, entry, 48u, eid}));
  // Same unmodified game data, before the original resolver memoizes its pair.
  memory.write32(section + 0x50u, eid);
  memory.write32(0x80061198u, table);
  memory.write32(table + ((eid >> 13u) & 0x3fcu), pair - 8u);
  auto unresolved = readSceneryZonePaths(memory, zone);
  ASSERT_TRUE(unresolved);
  EXPECT_EQ(*unresolved, *paths);
  EXPECT_EQ(memory.read32(section + 0x50u), eid); // No native resolver write.
  memory.write32(entry + 12u, 48u); // Independent count disagrees.
  EXPECT_FALSE(readSceneryZonePaths(memory, zone));
  memory.write32(entry + 12u, 49u);
  memory.write32(pair, eid); // An unloaded resource must not trigger CD IO.
  EXPECT_FALSE(readSceneryZonePaths(memory, zone));
  EXPECT_EQ(memory.read32(pair), eid);
  memory.write32(pair, entry);
  memory.write32(zone + 12u, UINT32_MAX);
  EXPECT_FALSE(readSceneryZonePaths(memory, zone));
}

namespace {
TEST(Crash3SceneryVariants, RegisteredSectionsBindByAssetNotWorldSlotOrCamera) {
  ps1::Memory memory;
  constexpr uint32_t zone = 0x80080000u, section = 0x80081000u;
  constexpr uint32_t entry = 0x80082000u, pair = 0x80083000u;
  constexpr uint32_t eid = 0x58016du, table = 0x80084000u;
  constexpr uint32_t worlds = 0x80085000u, registryPair = 0x80086000u;
  memory.write32(0x80061198u, table);
  memory.write32(table + ((eid >> 13u) & 0x3fcu), registryPair);
  memory.write32(registryPair, zone);
  memory.write32(registryPair + 4, eid);
  memory.write32(zone, 0x0100ffffu);
  memory.write32(zone + 4, eid);
  memory.write32(zone + 8, 7);
  memory.write32(zone + 12, 3);
  memory.write32(zone + 16, worlds);
  memory.write32(worlds, 3);
  memory.write32(worlds + 4, 5); // Same resource, OTHER slot.
  memory.write32(worlds + 52, 7); // Unrelated resource.
  memory.write32(worlds + 100, 3);
  memory.write32(zone + 24u, section);
  memory.write32(section + 4u, zone);
  memory.write16(section + 12u, 2u);
  memory.write16(section + 16u, 259u);
  memory.write16(section + 18u, 0x40u);
  memory.write8(section + 20u, 4u);
  memory.write8(section + 21u, 4u);
  memory.write16(section + 22u, 1u);
  memory.write16(section + 24u, 639u);
  memory.write16(section + 26u, 0x48u);
  memory.write8(section + 28u, 1u);
  memory.write8(section + 29u, 1u);
  memory.write16(section + 30u, 1u);
  memory.write16(section + 0x4cu, 1u);
  memory.write32(section + 0x50u, pair);
  memory.write16(section + 0x54u, 48u);
  memory.write32(entry, 0x0100ffffu);
  memory.write32(entry + 4u, eid - 2);
  memory.write32(entry + 8u, 4u);
  memory.write32(entry + 12u, 49u);
  memory.write32(pair, entry);
  memory.write32(pair + 4u, eid - 2);
  std::array<ResidentPresentationResource, 2> resources{};
  for (uint32_t i = 0; i < 2; ++i) {
    resources[i].resourceId = 3 + 2*i;
    resources[i].geometryFingerprint = 123 + i;
    resources[i].identityValid = true;
  }
  auto paths = readRegisteredSceneryPaths(memory, 0, resources);
  ASSERT_EQ(paths.size(), 1u);
  EXPECT_EQ(paths[0].worldCount, 3u);
  EXPECT_EQ(paths[0].worlds[0], 1);
  EXPECT_EQ(paths[0].worlds[1], -1);
  EXPECT_EQ(paths[0].worlds[2], 0);
  EXPECT_EQ(paths[0].source.entryBase, entry);
  EXPECT_TRUE(readRegisteredSceneryPaths(memory, zone, resources).empty());
  memory.write32(worlds + 100, 5); // Ambiguous duplicate resource.
  EXPECT_TRUE(readRegisteredSceneryPaths(memory, 0, resources).empty());
  memory.write32(worlds + 100, 3);
  memory.write32(pair, 0x39); // Paged-out data is not loaded by the sidecar.
  EXPECT_TRUE(readRegisteredSceneryPaths(memory, 0, resources).empty());
  EXPECT_EQ(memory.read32(pair), 0x39u);
}
struct VariantFixture {
  std::vector<ResidentGpuTopologyFace> topology{4};
  std::vector<ResidentPresentationMaterial> materials{4};
  VariantFixture() {
    for (uint32_t face = 0u; face < 4u; ++face) {
      auto &t = topology[face];
      t.resourceFaceOrdinal = face;
      t.half = face % 2u + 1u;
      t.sourceVertexIndex = face % 2u ? std::array<uint32_t, 3>{0, 2, 3}
                                      : std::array<uint32_t, 3>{1, 0, 2};
      t.completeSourceVertexIndex = {0, 1, 2, 3};
      t.packed = 0x1800u | (face / 2u);
      auto &m = materials[face];
      m.decoded = m.textured = true;
      m.sourceToken = m.resolvedToken = face < 2u ? 238u : 239u;
      m.packetVertexCount = 4u;
      m.texturePage = face < 2u ? 124u : 112u;
      m.packetUv = face < 2u ? std::array<std::array<float, 2>, 4>{{{31, 16},
                                                                    {31, 31},
                                                                    {0, 16},
                                                                    {0, 31}}}
                             : std::array<std::array<float, 2>, 4>{
                                   {{191, 8}, {191, 15}, {160, 8}, {160, 15}}};
    }
  }
};
} // namespace

TEST(Crash3SceneryVariants,
     AuthoredCornerMappingRanksStaticDetailNotTokenOrder) {
  VariantFixture fixture;
  auto pairs = findSceneryVariantPairs(fixture.topology, fixture.materials);
  ASSERT_EQ(pairs.size(), 2u);
  EXPECT_EQ(pairs[0].lowerFace, 2u);
  EXPECT_EQ(pairs[0].higherFace, 0u);
  EXPECT_FALSE(pairs[0].exclusivePathWitness);
  for (auto &material : fixture.materials)
    material.sourceToken = material.resolvedToken = 900u - material.sourceToken;
  pairs = findSceneryVariantPairs(fixture.topology, fixture.materials);
  ASSERT_EQ(pairs.size(), 2u);
  EXPECT_EQ(pairs[0].higherFace, 0u);
}

TEST(Crash3SceneryVariants,
     AnimationBlendMappingAndOppositeSidesAreNotAlternatives) {
  for (uint32_t rejection = 0; rejection < 7u; ++rejection) {
    VariantFixture fixture;
    for (uint32_t face = 2; face < 4; ++face) {
      auto &m = fixture.materials[face];
      if (rejection == 0u)
        m.dynamicTexture = true;
      if (rejection == 1u)
        m.semiTransparent = true;
      if (rejection == 2u)
        std::swap(m.packetUv[0], m.packetUv[2]);
      if (rejection == 3u)
        m.packetUv = fixture.materials[0].packetUv;
      if (rejection == 4u)
        m.packetUv[0][0] = 190.5f;
      if (rejection == 5u)
        m.sourceToken |= 0x800u;
      if (rejection == 6u)
        std::swap(fixture.topology[face].sourceVertexIndex[0],
                  fixture.topology[face].sourceVertexIndex[1]);
    }
    EXPECT_TRUE(
        findSceneryVariantPairs(fixture.topology, fixture.materials).empty())
        << rejection;
  }
}

TEST(Crash3SceneryVariants,
     CompleteNativePathProvesEachHalfWithoutChangingGuestLists) {
  VariantFixture fixture;
  auto pairs = findSceneryVariantPairs(fixture.topology, fixture.materials);
  const std::vector<std::vector<uint16_t>> states{{0x1800u}, {}, {0x1801u}};
  const auto original = states;
  ASSERT_TRUE(observeSceneryVariantMembership(pairs, 0u, 0u, 2u, states));
  ASSERT_EQ(pairs.size(), 2u);
  EXPECT_TRUE(pairs[0].exclusivePathWitness);
  EXPECT_TRUE(pairs[1].exclusivePathWitness);
  EXPECT_FALSE(pairs[0].coSelected);
  EXPECT_EQ(states, original);
}

TEST(Crash3SceneryVariants,
     PartialQuadsAndBitsetWordBoundariesPreserveMembership) {
  VariantFixture fixture;
  auto pairs = findSceneryVariantPairs(fixture.topology, fixture.materials);
  std::vector<std::vector<uint16_t>> states(65u);
  states[63] = {0x0800u}; // High, first half only.
  states[64] = {0x0801u}; // Low, first half only.
  ASSERT_TRUE(observeSceneryVariantMembership(pairs, 0u, 0u, 2u, states));
  EXPECT_TRUE(pairs[0].exclusivePathWitness);
  EXPECT_FALSE(pairs[1].exclusivePathWitness);
}

TEST(Crash3SceneryVariants,
     OneSidedVisibilityIsNotEvidenceAndCoSelectionVetoIsPermanent) {
  VariantFixture fixture;
  auto pairs = findSceneryVariantPairs(fixture.topology, fixture.materials);
  std::vector<std::vector<uint16_t>> states{{0x1800u}, {}};
  ASSERT_TRUE(observeSceneryVariantMembership(pairs, 0u, 0u, 2u, states));
  EXPECT_FALSE(pairs[0].exclusivePathWitness);
  states = {{0x1800u}, {0x1801u}};
  ASSERT_TRUE(observeSceneryVariantMembership(pairs, 0u, 0u, 2u, states));
  EXPECT_TRUE(pairs[0].exclusivePathWitness);
  states = {{0x1800u, 0x1801u}};
  ASSERT_TRUE(observeSceneryVariantMembership(pairs, 0u, 0u, 2u, states));
  EXPECT_TRUE(pairs[0].coSelected);
  states = {{0x1800u}, {0x1801u}};
  ASSERT_TRUE(observeSceneryVariantMembership(pairs, 0u, 0u, 2u, states));
  EXPECT_TRUE(pairs[0].coSelected);
}

TEST(Crash3SceneryVariants,
     InvalidPathsRejectAtomicallyAndWorldSlotsDoNotAlias) {
  VariantFixture fixture;
  auto pairs = findSceneryVariantPairs(fixture.topology, fixture.materials);
  std::vector<std::vector<uint16_t>> states{{0x1800u}, {0x1801u}, {0x1802u}};
  EXPECT_FALSE(observeSceneryVariantMembership(pairs, 0u, 0u, 2u, states));
  EXPECT_FALSE(pairs[0].exclusivePathWitness);
  states.pop_back();
  ASSERT_TRUE(observeSceneryVariantMembership(pairs, 1u, 0u, 2u, states));
  EXPECT_FALSE(pairs[0].exclusivePathWitness);
  for (auto &state : states)
    for (auto &packed : state)
      packed |= 0x2000u;
  ASSERT_TRUE(observeSceneryVariantMembership(pairs, 1u, 0u, 2u, states));
  EXPECT_TRUE(pairs[0].exclusivePathWitness);
}

TEST(Crash3SceneryVariants,
     TransparentCounterpartsUseTheSameSourceSelectionRule) {
  VariantFixture fixture;
  for (auto &material : fixture.materials)
    material.semiTransparent = true;
  EXPECT_EQ(findSceneryVariantPairs(fixture.topology, fixture.materials).size(),
            2u);
}

TEST(Crash3SceneryVariants,
     CanonicalPlanIsIncarnationBoundAndRetainedWithCapture) {
  clearSceneryDetailSnapshots();
  SceneryVariantPlan plan{7u, 23u, 45u, {{{2u, 0u}}, {{3u, 1u}}}};
  EXPECT_EQ(sceneryVariantReplacement(plan, 7u, 23u, 2u), 0u);
  EXPECT_FALSE(sceneryVariantReplacement(plan, 7u, 24u, 2u));
  EXPECT_FALSE(sceneryVariantReplacement(plan, 8u, 23u, 2u));
  EXPECT_FALSE(sceneryVariantReplacement(plan, 7u, 23u, 0u));
  auto owner = std::make_shared<ResidentPresentationSnapshot>();
  owner->captureId = 10u;
  SceneryDetailSnapshot details{
      10u, {}, {std::make_shared<const SceneryVariantPlan>(plan)}};
  publishSceneryDetailSnapshot(details, owner);
  auto retained = sceneryDetailSnapshot(10u);
  ASSERT_TRUE(retained);
  plan.replacements.clear(); // New source evidence cannot mutate old endpoints.
  EXPECT_EQ(retained->variants[0]->replacements.size(), 2u);
  clearSceneryDetailSnapshots();
  EXPECT_FALSE(sceneryDetailSnapshot(10u));
  EXPECT_EQ(retained->variants[0]->replacements.size(), 2u);
}

TEST(Crash3SceneryDetail, StaticAndAnimatedMaterialsAreNotMistakenForLod) {
  const auto missing = [](uint32_t) -> std::optional<uint32_t> { return {}; };
  auto direct = selectSceneryMaterial(123u, 7u, 15u, 29u, missing);
  ASSERT_TRUE(direct);
  EXPECT_EQ(direct->nativeToken, 123u);
  EXPECT_EQ(direct->nearestToken, 123u);
  EXPECT_FALSE(direct->distanceSelected);
  for (uint32_t clock = 0; clock < 16; ++clock) {
    auto animated = selectSceneryMaterial(
        0x800u, clock, 15u, 29u, [](uint32_t) -> std::optional<uint32_t> {
          return (2u << 26u) | (3u << 12u) | 20u;
        });
    ASSERT_TRUE(animated);
    EXPECT_EQ(animated->nativeToken, 20u + (clock >> 2u));
    EXPECT_EQ(animated->nearestToken, animated->nativeToken);
    EXPECT_FALSE(animated->distanceSelected);
  }
}

TEST(Crash3SceneryDetail,
     DistanceDetailRetainsCurrentAnimationAndAuthoredRank) {
  // Nearest is base+1, not zero, the smallest token or the largest record.
  const std::map<uint32_t, uint32_t> program{
      {2u, 0x800u | (1u << 12u) | 4u},
      {4u, 0xa0000000u | (3u << 15u) | 40u},
      {5u, 0xa0000000u | (2u << 15u) | 50u}};
  const auto read = [&](uint32_t index) -> std::optional<uint32_t> {
    auto it = program.find(index);
    return it == program.end() ? std::nullopt : std::optional{it->second};
  };
  for (uint32_t clock = 0; clock < 8; ++clock) {
    auto result = selectSceneryMaterial(0x802u, clock, 15u, 29u, read);
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->distanceSelected);
    EXPECT_EQ(result->nativeToken, (clock & 1u) ? 52u : 43u);
    EXPECT_EQ(result->nearestToken, (clock & 1u) ? 51u : 41u);
  }
  EXPECT_FALSE(selectSceneryMaterial(0x803u, 0u, 15u, 29u, read));
  EXPECT_FALSE(selectSceneryMaterial(
      0x800u, 0u, 15u, 29u, [](uint32_t index) -> std::optional<uint32_t> {
        return index == 0u ? std::optional{0x805u} : std::nullopt;
      }));
}

TEST(Crash3SceneryDetail, CapturesFollowGpuOwnershipAndResetWithGuestTimeline) {
  clearSceneryDetailSnapshots();
  SceneryDetailSnapshot first;
  first.captureId = 1u;
  auto owner = std::make_shared<ResidentPresentationSnapshot>();
  owner->captureId = 1u;
  first.materials.push_back({7u, 23u, 4u, {.resolvedToken = 100u}});
  publishSceneryDetailSnapshot(first, owner);
  auto retained = sceneryDetailSnapshot(1u);
  ASSERT_TRUE(retained);
  first.materials[0].material.resolvedToken = 101u;
  publishSceneryDetailSnapshot(first, owner);
  EXPECT_EQ(retained->materials[0].material.resolvedToken, 100u);
  EXPECT_EQ(sceneryDetailSnapshot(1u)->materials[0].material.resolvedToken,
            101u);
  for (uint32_t id = 2u; id < 11u; ++id) {
    auto newer = std::make_shared<ResidentPresentationSnapshot>();
    newer->captureId = id;
    publishSceneryDetailSnapshot({id, {}}, newer);
  }
  EXPECT_TRUE(sceneryDetailSnapshot(1u)); // GPU still owns this older capture.
  EXPECT_FALSE(sceneryDetailSnapshot(10u)); // No source owner remains.
  owner.reset();
  EXPECT_FALSE(sceneryDetailSnapshot(1u));
  clearSceneryDetailSnapshots();
  EXPECT_FALSE(sceneryDetailSnapshot(10u));
  EXPECT_EQ(retained->materials[0].material.resolvedToken, 100u);
}

TEST(Crash3SceneryDetail,
     GeometryClipWeightsPreserveDetailEvenWithDegenerateNativeUv) {
  ResidentPresentationFace face;
  face.resourceId = 7u;
  face.resourceGeometryFingerprint = 23u;
  face.resourceFaceOrdinal = 4u;
  face.half = 2u;
  face.nearPlaneClipped = true;
  face.material.decoded = true;
  face.material.textured = true;
  face.material.resolvedToken = 3u;
  face.material.packetVertexCount = 4u;
  // The inferior material can collapse to one texel. Its UVs must never be
  // inverted to recover geometry correspondence for the sharper replacement.
  face.material.packetUv = {};
  face.material.uv = {};
  auto highest = face.material;
  highest.resolvedToken = 1u;
  highest.uv = {{{132, 100}, {100, 132}, {132, 132}}};
  const SceneryClipWeights weights{{{0.5, 0.5, 0}, {0, 1, 0}, {0, 0, 1}}};
  SceneryDetailSnapshot details;
  appendSceneryDetailMaterial(details, face, 8u,
                              clipSceneryDetailMaterial(highest, weights));
  ASSERT_EQ(details.materials.size(), 1u);
  auto &detail = details.materials.front();
  auto clipped = applySceneryDetailMaterial(face, detail, 8u);
  ASSERT_TRUE(clipped);
  EXPECT_EQ(clipped->uv[0], (std::array<float, 2>{116, 116}));
  EXPECT_EQ(clipped->uv[1], (std::array<float, 2>{100, 132}));
  EXPECT_EQ(face.material.uv[0], (std::array<float, 2>{0, 0}));
  EXPECT_FALSE(applySceneryDetailMaterial(face, detail, 9u)); // Different fan.
  ++detail.geometryFingerprint;
  EXPECT_FALSE(applySceneryDetailMaterial(face, detail, 8u));
  --detail.geometryFingerprint;
  face.retainedResource = true;
  EXPECT_TRUE(applySceneryDetailMaterial(face, detail, 8u));
  face.nearPlaneClipped = false;
  EXPECT_FALSE(applySceneryDetailMaterial(face, detail, 8u));
}

TEST(Crash3SceneryDetail,
     RetainedDetailRequiresCurrentIncarnationAndExactFanBinding) {
  ResidentPresentationFace face;
  face.resourceId = 7u;
  face.resourceGeometryFingerprint = 23u;
  face.resourceFaceOrdinal = 4u;
  face.retainedResource = true;
  face.material = {.resolvedToken = 3u, .decoded = true, .textured = true};
  auto highest = face.material;
  highest.resolvedToken = 1u;
  SceneryDetailSnapshot details;
  appendSceneryDetailMaterial(details, face, 15u, highest);
  ASSERT_EQ(details.materials.size(), 1u);
  EXPECT_EQ(applySceneryDetailMaterial(face, details.materials[0], 15u)
                ->resolvedToken,
            1u);
  EXPECT_EQ(face.material.resolvedToken, 3u);
  face.resourceGeometryFingerprint = 24u;
  EXPECT_FALSE(applySceneryDetailMaterial(face, details.materials[0], 15u));
  face.material = highest;
  appendSceneryDetailMaterial(details, face, 16u, highest);
  EXPECT_EQ(details.materials.size(), 1u); // Already highest, no second choice.
}

TEST(Crash3SceneryDetail,
     HighestAuthoredMaterialAlsoOwnsTexturingAndBlendClass) {
  ResidentPresentationFace face;
  face.resourceId = 7u;
  face.resourceGeometryFingerprint = 23u;
  face.resourceFaceOrdinal = 4u;
  face.material = {.resolvedToken = 0u, .decoded = true};
  auto highest = face.material;
  highest.resolvedToken = 1u;
  highest.textured = true;
  highest.semiTransparent = true;
  SceneryDetailSnapshot details;
  appendSceneryDetailMaterial(details, face, 0u, highest);
  ASSERT_EQ(details.materials.size(), 1u);
  const auto applied =
      applySceneryDetailMaterial(face, details.materials[0], 0u);
  ASSERT_TRUE(applied);
  EXPECT_TRUE(applied->textured);
  EXPECT_TRUE(applied->semiTransparent);
  EXPECT_FALSE(face.material.textured);
  EXPECT_FALSE(face.material.semiTransparent);
}
