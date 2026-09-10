#include "runtime/cpu_context.h"
#include "runtime/games/crash3_object_presenter.h"
#include "runtime/games/crash3_profile.h"
#include "runtime/games/crash3_render_profile.h"
#include "runtime/gpu/modern_replay_visibility_audit.h"
#include "runtime/gpu/modern_replay_vulkan_abi.h"
#include "runtime/gte.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include "runtime/pgxp.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <gtest/gtest.h>

namespace {

using ps1::games::crash3::PresentationDepthStatus;
using ps1::games::crash3::PresentationSurfaceStatus;
using ps1::games::crash3::SceneryOcclusionClass;
using ps1::gpu::replay::PacketRoute;
using ps1::gpu::replay::ReplayVertexDmaTraversalExact;
using ps1::gpu::replay::ReplayVertexGp0WordSourceExact;
using ps1::gpu::replay::ReplayVertexOrderingTableAnchorExact;

class Crash3ProfileTest : public ::testing::Test {
protected:
  void TearDown() override {
    unsetenv("PS1_CRASH3_SLST_MODE");
    unsetenv("PS1_CRASH3_SLST_ADJACENT_DELTAS");
    unsetenv("PS1_CRASH3_SCENERY_OCCLUSION");
    unsetenv("PS1_CRASH3_SCENERY_GUARD_PIXELS");
    unsetenv("PS1_CRASH3_RESIDENT_ALL_FRUSTUM_COMPONENTS");
    unsetenv("PS1_CRASH3_RESIDENT_FRUSTUM_AUDIT");
    unsetenv("PS1_CRASH3_VULKAN_RESIDENT_MODE");
    unsetenv("PS1_CRASH3_VULKAN_GPU_COMPACT_OPAQUE");
    unsetenv("PS1_CRASH3_AUTOSAVE_DEMO_LEVELS");
    unsetenv("PS1_CRASH3_RESIDENT_COHORT_PRESENT");
    unsetenv("PS1_CRASH3_VULKAN_OBJECT_MODE");
    unsetenv("PS1_CRASH3_VULKAN_CULL_STRATEGY");
    unsetenv("PS1_TRACE_CRASH3_SCENERY_PROJECTION_IDENTITY");
    unsetenv("PS1_TRACE_CRASH3_OBJECT_CATALOG");
    unsetenv("PS1_TRACE_CRASH3_OBJECT_PRESENTER");
    ps1::games::crash3::configure({});
  }

  static ps1::gpu::replay::ReplayVisibilityAuditOwner knownOwner() {
    ps1::gpu::replay::ReplayVisibilityAuditOwner owner;
    owner.route = PacketRoute::Projected3D;
    owner.opaque = true;
    owner.fullyPrecise = true;
    owner.authenticDepth = true;
    owner.triangleIndex = 0;
    owner.provenance.validTopology = true;
    owner.provenance.vertexIndices = {0u, 1u, 2u};
    constexpr std::array<uint64_t, 3> tags{UINT64_C(40452086462423),
                                           UINT64_C(202274499793102),
                                           UINT64_C(164603657610929)};
    constexpr std::array<uint32_t, 3> offsets{8u, 16u, 24u};
    for (std::size_t index = 0; index < 3; ++index) {
      auto &vertex = owner.provenance.vertices[index];
      vertex.gameProfileTag = tags[index];
      vertex.dmaListSource = 0x2001u;
      vertex.dmaNodeSource = 0x1001u;
      vertex.dmaTraversalOrdinal = 7u;
      vertex.gp0WordSource = 0x1001u + offsets[index];
      vertex.evidence =
          ReplayVertexGp0WordSourceExact | ReplayVertexDmaTraversalExact;
    }
    return owner;
  }

  static std::array<ps1::gpu::replay::vulkan_abi::Vertex, 3>
  vertices(float projectiveW = 1000.0f) {
    std::array<ps1::gpu::replay::vulkan_abi::Vertex, 3> result{};
    for (auto &vertex : result)
      vertex.projectiveW = projectiveW;
    return result;
  }

  static ps1::gpu::replay::ReplayVisibilityAuditOwner
  surfaceOwner(uint32_t pc, uint16_t bucket, std::array<uint8_t, 3> indices,
               std::array<float, 3> effectiveW, bool opaque = false,
               int32_t normalizationShift = -1, uint32_t objectSource = 0u) {
    ps1::gpu::replay::ReplayVisibilityAuditOwner owner;
    owner.route = PacketRoute::Projected3D;
    owner.opaque = opaque;
    owner.fullyPrecise = true;
    owner.authenticDepth = true;
    owner.provenance.validTopology = true;
    owner.provenance.vertexIndices = indices;
    owner.effectiveProjectiveW = effectiveW;
    for (std::size_t corner = 0; corner < 3u; ++corner) {
      auto &vertex = owner.provenance.vertices[corner];
      vertex.gameProfileCallsite =
          (static_cast<uint64_t>(objectSource) << 32u) |
          (static_cast<uint64_t>(pc & 0x00ffffffu) << 8u) |
          (normalizationShift >= 0
               ? (static_cast<uint64_t>(normalizationShift + 1) << 2u)
               : 0u) |
          corner;
      vertex.dmaListSource = 0x1001u;
      vertex.dmaNodeSource = 0x4001u;
      vertex.dmaOrderingTableAnchorSource =
          0x1001u + static_cast<uint64_t>(bucket) * 4u;
      vertex.gp0WordSource = 0x4005u + corner * 4u;
      vertex.evidence = ReplayVertexGp0WordSourceExact |
                        ReplayVertexDmaTraversalExact |
                        ReplayVertexOrderingTableAnchorExact;
    }
    return owner;
  }

  static ps1::gpu::replay::ReplayVisibilityAuditOwner
  sceneryOwner(uint16_t packed, bool opaque = true, uint32_t captureId = 7u) {
    auto owner = knownOwner();
    owner.opaque = opaque;
    constexpr uint32_t gtePcOffset = 0x00417ACu;
    constexpr uint64_t sceneryMarker = UINT64_C(1) << 47u;
    for (uint32_t lane = 0; lane < 3u; ++lane) {
      auto &source = owner.provenance.vertices[lane];
      source.gameProfileCallsite =
          (static_cast<uint64_t>(gtePcOffset) << 8u) | lane;
      source.gameProfileTag =
          sceneryMarker | (static_cast<uint64_t>(packed) << 26u) | captureId;
    }
    return owner;
  }
};

TEST_F(Crash3ProfileTest,
       DemoCheckpointCollectorEnablesAuditSidecarUnlessExplicitlyDisabled) {
  setenv("PS1_CRASH3_AUTOSAVE_DEMO_LEVELS", "1", 1);
  ps1::games::crash3::configure({.enabled = true});
  EXPECT_EQ(ps1::games::crash3::residentPresentationMode(),
            ps1::games::crash3::ResidentPresentationMode::Audit);

  setenv("PS1_CRASH3_VULKAN_RESIDENT_MODE", "off", 1);
  ps1::games::crash3::configure({.enabled = true});
  EXPECT_EQ(ps1::games::crash3::residentPresentationMode(),
            ps1::games::crash3::ResidentPresentationMode::Off);
}

TEST_F(Crash3ProfileTest, DemoLevelLoadMarkerIsExactProfileOwnedAndOrdered) {
  ps1::CPUContext ctx{};
  ctx.r[ps1::A1] = 21u;
  ctx.r[ps1::A2] = static_cast<uint32_t>('j');

  ps1::games::crash3::observeDebugPrintf(ctx, 0x800100A8u);
  EXPECT_EQ(ps1::games::crash3::demoLevelLoadSignal().generation, 0u);

  ps1::games::crash3::configure({.enabled = true});
  ps1::games::crash3::observeDebugPrintf(ctx, 0x800100ACu);
  EXPECT_EQ(ps1::games::crash3::demoLevelLoadSignal().generation, 0u);

  ps1::games::crash3::observeDebugPrintf(ctx, 0x800100A8u);
  const auto level = ps1::games::crash3::demoLevelLoadSignal();
  EXPECT_EQ(level.generation, 1u);
  EXPECT_EQ(level.loadOrdinal, 21u);
  EXPECT_EQ(level.levelCode, static_cast<uint8_t>('j'));
  EXPECT_FALSE(level.title);

  ctx.r[ps1::A1] = 20u;
  ctx.r[ps1::A2] = static_cast<uint32_t>('Y');
  ps1::games::crash3::observeDebugPrintf(ctx, 0x800100A8u);
  const auto title = ps1::games::crash3::demoLevelLoadSignal();
  EXPECT_EQ(title.generation, 2u);
  EXPECT_EQ(title.loadOrdinal, 20u);
  EXPECT_EQ(title.levelCode, static_cast<uint8_t>('Y'));
  EXPECT_TRUE(title.title);
}

TEST_F(Crash3ProfileTest, IsFullyInertUnlessExplicitlyEnabled) {
  EXPECT_EQ(ps1::games::crash3::horizontalProjectionScale(), 1.0);
  EXPECT_EQ(ps1::games::crash3::screen2DPresentationX(100, 0, 511, 100), 100);
  EXPECT_EQ(ps1::games::crash3::viewportCullPresentationSxy(0x0040ffabu),
            0x0040ffabu);
  const auto points = vertices();
  EXPECT_EQ(
      ps1::games::crash3::planPresentationDepth(knownOwner(), points).status,
      PresentationDepthStatus::Disabled);
}

TEST_F(Crash3ProfileTest,
       ProjectedBillboardRepairIsConstructionOwnedAndFailsClosed) {
  ps1::CPUContext ctx{};
  ctx.reset();
  constexpr uint32_t centrePacked = (104u << 16u) | 220u;
  constexpr uint32_t vertexPacked = (96u << 16u) | 232u;
  constexpr uint32_t packetBase = 0x80018000u;
  ctx.pc = 0x800457BCu;
  ctx.r[23] = packetBase;
  ctx.cop2d[ps1::GTE_SXY2] = centrePacked;
  auto &centre = ctx.pgxpGteData[ps1::GTE_SXY2];
  centre.x = 220.25f;
  centre.y = 103.75f;
  centre.z = 48.0;
  centre.value = centrePacked;
  centre.flags = ps1::PGXPValue::VALID_XY |
                 ps1::PGXPValue::VALID_SIGNED_PROJECTION |
                 ps1::PGXPValue::VALID_TRANSFORM_LINEAGE;
  centre.origin = ps1::PGXPOrigin::GteProjection;
  centre.qx = centre.x * centre.z;
  centre.qy = centre.y * centre.z;
  centre.nearDepth = 24.0;
  centre.projectionId = 8u;
  centre.projectionContext = 9u;
  centre.projectionTransformLineage = 10u;

  ps1::PGXPValue stored;
  ps1::games::crash3::observeProjectedBillboardCentre(ctx, ps1::GTE_SXY2,
                                                      centrePacked, centre);
  EXPECT_FALSE(ps1::games::crash3::lookupProjectedBillboardPrecision(
      packetBase + 8u, vertexPacked, stored));

  ps1::games::crash3::configure({.enabled = true});
  ctx.pc = 0x800457B8u;
  ps1::games::crash3::observeProjectedBillboardCentre(ctx, ps1::GTE_SXY2,
                                                      centrePacked, centre);
  EXPECT_FALSE(ps1::games::crash3::lookupProjectedBillboardPrecision(
      packetBase + 8u, vertexPacked, stored));

  ctx.pc = 0x800457BCu;
  ps1::games::crash3::observeProjectedBillboardCentre(ctx, ps1::GTE_SXY2,
                                                      centrePacked, centre);
  ASSERT_TRUE(ps1::games::crash3::lookupProjectedBillboardPrecision(
      packetBase + 8u, vertexPacked, stored));
  ASSERT_TRUE(stored.hasSignedProjection(vertexPacked));
  EXPECT_FLOAT_EQ(stored.x, centre.x + 12.0f);
  EXPECT_FLOAT_EQ(stored.y, centre.y - 8.0f);
  EXPECT_DOUBLE_EQ(stored.z, centre.z);
  EXPECT_DOUBLE_EQ(stored.qx, centre.qx + 12.0 * centre.z);
  EXPECT_DOUBLE_EQ(stored.qy, centre.qy - 8.0 * centre.z);
  EXPECT_EQ(stored.projectionId, centre.projectionId);
  EXPECT_EQ(stored.projectionTransformLineage,
            centre.projectionTransformLineage);

  EXPECT_FALSE(ps1::games::crash3::lookupProjectedBillboardPrecision(
      packetBase + 12u, vertexPacked, stored));
  ps1::games::crash3::configure({.enabled = true});
  EXPECT_FALSE(ps1::games::crash3::lookupProjectedBillboardPrecision(
      packetBase + 8u, vertexPacked, stored));

  ps1::games::crash3::configure({.enabled = true, .widescreen16x9 = true});
  centre.x = 256.0f + (centre.x - 256.0f) * 0.75f;
  centre.qx = centre.x * centre.z;
  ps1::games::crash3::observeProjectedBillboardCentre(ctx, ps1::GTE_SXY2,
                                                   centrePacked, centre);
  ASSERT_TRUE(ps1::games::crash3::lookupProjectedBillboardPrecision(
      packetBase + 8u, vertexPacked, stored));
  EXPECT_FLOAT_EQ(stored.x, centre.x + 9.0f);
  EXPECT_DOUBLE_EQ(stored.qx, centre.qx + 9.0 * centre.z);
  EXPECT_FLOAT_EQ(stored.y, centre.y - 8.0f);
  EXPECT_DOUBLE_EQ(stored.z, centre.z);
  EXPECT_EQ(stored.value, vertexPacked);
  constexpr uint32_t leftPacked = (96u << 16u) | 208u;
  ASSERT_TRUE(ps1::games::crash3::lookupProjectedBillboardPrecision(
      packetBase + 16u, leftPacked, stored));
  EXPECT_FLOAT_EQ(stored.x, centre.x - 9.0f);
  EXPECT_DOUBLE_EQ(stored.qx, centre.qx - 9.0 * centre.z);
  EXPECT_EQ(ctx.cop2d[ps1::GTE_SXY2], centrePacked);
}

TEST_F(Crash3ProfileTest, WidescreenCullUsesExactPresentationTransform) {
  ps1::games::crash3::configure({.enabled = true, .widescreen16x9 = true});
  const auto mappedX = [](int16_t x) {
    const uint32_t packed = 0x00400000u | static_cast<uint16_t>(x);
    return static_cast<int16_t>(
        ps1::games::crash3::viewportCullPresentationSxy(packed) & 0xffffu);
  };

  EXPECT_EQ(mappedX(-86), -1);
  EXPECT_EQ(mappedX(-85), 0);
  EXPECT_EQ(mappedX(256), 256);
  EXPECT_EQ(mappedX(596), 511);
  EXPECT_EQ(mappedX(597), 512);
  EXPECT_EQ(ps1::games::crash3::viewportCullPresentationSxy(0xff80ffabu) &
                0xffff0000u,
            0xff800000u);
}

TEST_F(Crash3ProfileTest, SceneryCullGuardIsProfileScopedAndBounded) {
  ps1::games::crash3::configure({.enabled = true,
                                 .widescreen16x9 = true,
                                 .sceneryCullGuardPixels = 192u});
  EXPECT_EQ(ps1::games::crash3::sceneryCullGuardPixels(), 192u);

  ps1::games::crash3::configure({.enabled = true,
                                 .widescreen16x9 = false,
                                 .sceneryCullGuardPixels = 192u});
  EXPECT_EQ(ps1::games::crash3::sceneryCullGuardPixels(), 0u);

  ps1::games::crash3::configure({.enabled = true,
                                 .widescreen16x9 = true,
                                 .sceneryCullGuardPixels = 9000u});
  EXPECT_EQ(ps1::games::crash3::sceneryCullGuardPixels(), 2048u);
}

TEST_F(Crash3ProfileTest,
       HostResidentPresentationLeavesEveryGuestCullWitnessNative) {
  setenv("PS1_CRASH3_VULKAN_RESIDENT_MODE", "debug", 1);
  setenv("PS1_CRASH3_VULKAN_CULL_STRATEGY", "native", 1);
  ps1::games::crash3::configure(
      {.enabled = true, .widescreen16x9 = true, .sceneryCullGuardPixels = 64u});
  const auto packed = [](int16_t x, int16_t y) {
    return static_cast<uint32_t>(static_cast<uint16_t>(x)) |
           (static_cast<uint32_t>(static_cast<uint16_t>(y)) << 16u);
  };
  constexpr int32_t mainOfx = 256 << 16;
  const uint32_t architectural = packed(-64, 20);
  EXPECT_EQ(ps1::games::crash3::sceneryCullGuardPixels(), 0u);
  EXPECT_EQ(ps1::games::crash3::viewportCullPresentationSxy(architectural),
            architectural);
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x80041838u, ps1::GTE_SXY0, architectural, mainOfx),
            architectural);

  ps1::CPUContext ctx{};
  ctx.reset();
  ctx.pc = 0x8004184Cu;
  ctx.r[24] = architectural;
  ctx.r[25] = packed(512, 30);
  ctx.r[2] = packed(300, 40);
  ctx.r[31] = packed(512, 257);
  const ps1::CPUContext before = ctx;
  ps1::games::crash3::prepareSceneryCullGuardForCommand(ctx);
  EXPECT_TRUE(
      std::equal(std::begin(ctx.r), std::end(ctx.r), std::begin(before.r)));
}

TEST_F(Crash3ProfileTest,
       HostResidentPresentationCanUseWiderCullOnlyWitnesses) {
  const auto packed = [](int16_t x, int16_t y) {
    return static_cast<uint32_t>(static_cast<uint16_t>(x)) |
           (static_cast<uint32_t>(static_cast<uint16_t>(y)) << 16u);
  };
  constexpr int32_t mainOfx = 256 << 16;
  setenv("PS1_CRASH3_VULKAN_RESIDENT_MODE", "debug", 1);

  // A positive configured guard is operational by default. native/exact are
  // retained only when explicitly requested for diagnostic comparison.
  ps1::games::crash3::configure({.enabled = true,
                                 .widescreen16x9 = true,
                                 .sceneryCullGuardPixels = 256u});
  EXPECT_EQ(ps1::games::crash3::sceneryCullGuardPixels(), 256u);
  EXPECT_EQ(ps1::games::crash3::viewportCullPresentationSxy(packed(-426, 20)),
            packed(0, 20));

  setenv("PS1_CRASH3_VULKAN_CULL_STRATEGY", "exact", 1);
  ps1::games::crash3::configure({.enabled = true,
                                 .widescreen16x9 = true,
                                 .sceneryCullGuardPixels = 256u});
  EXPECT_EQ(ps1::games::crash3::viewportCullPresentationSxy(packed(-85, 20)),
            packed(0, 20));
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x80041838u, ps1::GTE_SXY0, packed(596, 20), mainOfx),
            packed(511, 20));

  setenv("PS1_CRASH3_VULKAN_CULL_STRATEGY", "guard", 1);
  ps1::games::crash3::configure({.enabled = true,
                                 .widescreen16x9 = true,
                                 .sceneryCullGuardPixels = 256u});
  // The shown left edge (-85 in native projection) is well inside the
  // culling volume, while a point 256 final pixels farther left reaches its
  // boundary. Only these temporary reads are compressed.
  EXPECT_EQ(ps1::games::crash3::viewportCullPresentationSxy(packed(-85, 20)),
            packed(128, 20));
  EXPECT_EQ(ps1::games::crash3::viewportCullPresentationSxy(packed(-426, 20)),
            packed(0, 20));
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x80041838u, ps1::GTE_SXY0, packed(256, 20), mainOfx),
            packed(256, 20));
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x80041838u, ps1::GTE_SXY0, packed(-426, 20), mainOfx),
            packed(-256, 20));
  // Packet storage in the same shared renderer gets the true 16:9 position,
  // never the wider cull-only compression.
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x800418DCu, ps1::GTE_SXY0, packed(0, 20), mainOfx),
            packed(64, 20));

  // Once packet-visible coordinates have been preserved, the branch-local
  // witnesses and bound admit 256 extra final pixels on each side.
  ps1::CPUContext shared{};
  shared.reset();
  shared.pc = 0x8004184Cu;
  shared.r[24] = packed(-256, 10);
  shared.r[25] = packed(767, 20);
  shared.r[2] = packed(300, 30);
  shared.r[31] = packed(512, 257);
  ps1::games::crash3::prepareSceneryCullGuardForCommand(shared);
  EXPECT_EQ(shared.r[24], packed(0, 10));
  EXPECT_EQ(shared.r[25], packed(1023, 20));
  EXPECT_EQ(shared.r[31], packed(1024, 257));

  ps1::CPUContext nativeWorld{};
  nativeWorld.reset();
  nativeWorld.pc = 0x8003CEE0u;
  nativeWorld.r[25] = packed(-256, 10);
  nativeWorld.r[4] = packed(767, 20);
  nativeWorld.r[5] = packed(300, 30);
  nativeWorld.r[24] = packed(512, 257);
  ps1::games::crash3::prepareSceneryCullGuardForCommand(nativeWorld);
  EXPECT_EQ(nativeWorld.r[25], packed(0, 10));
  EXPECT_EQ(nativeWorld.r[4], packed(1023, 20));
  EXPECT_EQ(nativeWorld.r[24], packed(1024, 257));

  // The procedural plane/water walkers reach AVSZ4 after subtracting their
  // cached first corner from the packed bound. Reconstructing at that point
  // keeps the cache and GTE FIFO packet-exact while all four branch witnesses
  // enter the same compressed guard domain.
  const uint32_t bounds = packed(512, 257);
  const std::array<uint32_t, 4> surfaceCorners{{
      packed(-426, 10),
      packed(596, 20),
      packed(256, 30),
      packed(700, 40),
  }};
  ps1::CPUContext surface{};
  surface.reset();
  surface.pc = 0x80043C28u;
  surface.cop2c[ps1::GTE_OFX] = static_cast<uint32_t>(mainOfx);
  surface.r[2] = bounds;
  surface.r[24] = surfaceCorners[0] - bounds;
  surface.r[25] = surfaceCorners[1];
  surface.r[4] = surfaceCorners[2];
  surface.r[5] = surfaceCorners[3];
  surface.r[6] = 0xDEADBEEFu;
  std::array<uint32_t, 4> surfacePresented{};
  for (std::size_t index = 0; index < surfaceCorners.size(); ++index) {
    surfacePresented[index] =
        ps1::games::crash3::viewportCullPresentationSxy(surfaceCorners[index]);
  }
  const ps1::CPUContext surfaceBefore = surface;
  for (const uint32_t commandPc :
       {0x800439CCu, 0x80043A80u, 0x80043C28u, 0x80043CDCu, 0x80044438u,
        0x800444F4u, 0x800446A8u, 0x80044764u}) {
    SCOPED_TRACE(commandPc);
    ps1::CPUContext candidate = surfaceBefore;
    candidate.pc = commandPc;
    ps1::games::crash3::prepareSceneryCullGuardForCommand(candidate);
    EXPECT_EQ(candidate.r[24], surfacePresented[0] - bounds);
    EXPECT_EQ(candidate.r[25], surfacePresented[1]);
    EXPECT_EQ(candidate.r[4], surfacePresented[2]);
    EXPECT_EQ(candidate.r[5], surfacePresented[3]);
    EXPECT_EQ(candidate.r[6], surfacePresented[0] & surfacePresented[1]);
    EXPECT_TRUE(std::equal(std::begin(candidate.cop2d),
                           std::end(candidate.cop2d),
                           std::begin(surfaceBefore.cop2d)));
    EXPECT_TRUE(std::equal(std::begin(candidate.cop2c),
                           std::end(candidate.cop2c),
                           std::begin(surfaceBefore.cop2c)));
  }

  ps1::CPUContext unexpectedBound = surfaceBefore;
  unexpectedBound.r[2] = packed(511, 257);
  const ps1::CPUContext unexpectedBoundBefore = unexpectedBound;
  ps1::games::crash3::prepareSceneryCullGuardForCommand(unexpectedBound);
  EXPECT_TRUE(std::equal(std::begin(unexpectedBound.r),
                         std::end(unexpectedBound.r),
                         std::begin(unexpectedBoundBefore.r)));

  ps1::CPUContext auxiliary = surfaceBefore;
  auxiliary.pc = 0x800446A8u;
  auxiliary.cop2c[ps1::GTE_OFX] = 32u << 16u;
  std::array<uint32_t, 32> auxiliaryBefore{};
  std::copy(std::begin(auxiliary.r), std::end(auxiliary.r),
            auxiliaryBefore.begin());
  ps1::games::crash3::prepareSceneryCullGuardForCommand(auxiliary);
  EXPECT_TRUE(std::equal(std::begin(auxiliary.r), std::end(auxiliary.r),
                         auxiliaryBefore.begin()));

  // The fourth quad corner is used only by the comparison at this read; its
  // later packet read still receives the unshifted final coordinate.
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x80041B14u, ps1::GTE_SXY2, packed(-426, 40), mainOfx),
            packed(0, 40));
  EXPECT_TRUE(ps1::games::crash3::rendererPresentationSxyIsCullOnlyGuard(
      0x80041B14u, ps1::GTE_SXY2, mainOfx));
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x80041CCCu, ps1::GTE_SXY2, packed(-426, 40), mainOfx),
            packed(-256, 40));
  EXPECT_FALSE(ps1::games::crash3::rendererPresentationSxyIsCullOnlyGuard(
      0x80041CCCu, ps1::GTE_SXY2, mainOfx));

  // Procedural walkers preserve every packet/cache read. Their four-corner
  // branch state is reconstructed later at AVSZ4 instead.
  EXPECT_FALSE(ps1::games::crash3::rendererPresentationSxyIsCullOnlyGuard(
      0x80043C18u, ps1::GTE_SXY0, mainOfx));
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x80043C18u, ps1::GTE_SXY1, packed(-426, 40), mainOfx),
            packed(-426, 40));
  EXPECT_FALSE(ps1::games::crash3::rendererPresentationSxyIsCullOnlyGuard(
      0x80043E80u, ps1::GTE_SXY2, mainOfx));
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x80043E80u, ps1::GTE_SXY2, packed(-426, 40), mainOfx),
            packed(-426, 40));
}

TEST_F(Crash3ProfileTest,
       EveryProceduralWalkerAdmitsWideEdgesWithoutTouchingPacketState) {
  const auto packed = [](int16_t x, int16_t y) {
    return static_cast<uint32_t>(static_cast<uint16_t>(x)) |
           (static_cast<uint32_t>(static_cast<uint16_t>(y)) << 16u);
  };
  const uint32_t bounds = packed(512, 257);
  // Reproduce the original branch, including its packed subtraction/borrow.
  const auto rejected = [bounds](const ps1::CPUContext &ctx) {
    const uint32_t outside = ctx.r[6] & ctx.r[4] & ctx.r[5];
    const uint32_t beyond = ~(ctx.r[24] | (ctx.r[25] - bounds) |
                              (ctx.r[4] - bounds) | (ctx.r[5] - bounds));
    return ((outside | beyond) & 0x80008000u) != 0u;
  };
  const std::array<std::array<uint32_t, 4>, 3> quads{{
      {packed(-80, 10), packed(-20, 10), packed(-80, 80), packed(-20, 80)},
      {packed(532, 10), packed(590, 10), packed(532, 80), packed(590, 80)},
      // Horizontally visible but completely above the frame: widening X must
      // not disable the independent vertical rejection.
      {packed(220, -80), packed(290, -80), packed(220, -20), packed(290, -20)},
  }};
  ps1::Memory memory;
  memory.write32(0x1f800384u, 0x12345678u); // terrain packet's cached first SXY
  memory.write32(0x1f800390u, 0x87654321u); // water packet's cached first SXY
  for (const uint32_t pc :
       {0x800439CCu, 0x80043A80u, 0x80043C28u, 0x80043CDCu, 0x80044438u,
        0x800444F4u, 0x800446A8u, 0x80044764u}) {
    SCOPED_TRACE(pc);
    for (std::size_t quad = 0u; quad < quads.size(); ++quad) {
      SCOPED_TRACE(quad);
      for (const bool wide : {false, true}) {
        ps1::games::crash3::configure({.enabled = true,
                                       .widescreen16x9 = wide,
                                       .sceneryCullGuardPixels = 256u});
        recomp_context ctx{};
        ctx.reset();
        ctx.mem = &memory;
        ctx.pc = pc;
        ctx.cop2c[ps1::GTE_OFX] = 256u << 16u;
        ctx.r[2] = bounds;
        ctx.r[24] = quads[quad][0] - bounds;
        ctx.r[25] = quads[quad][1];
        ctx.r[4] = quads[quad][2];
        ctx.r[5] = quads[quad][3];
        ctx.r[6] = quads[quad][0] & quads[quad][1];
        ctx.cop2d[ps1::GTE_SXY0] = quads[quad][1];
        ctx.cop2d[ps1::GTE_SXY1] = quads[quad][2];
        ctx.cop2d[ps1::GTE_SXY2] = quads[quad][3];
        const auto before = ctx;
        ASSERT_TRUE(rejected(ctx));
        ps1::games::crash3::prepareSceneryCullGuardForCommand(ctx);
        EXPECT_EQ(rejected(ctx), !wide || quad == 2u);
        for (uint8_t reg = 0u; reg < 32u; ++reg) {
          if (reg != 24u && reg != 25u && reg != 4u && reg != 5u && reg != 6u)
            EXPECT_EQ(ctx.r[reg], before.r[reg]);
          EXPECT_EQ(ctx.cop2d[reg], before.cop2d[reg]);
          EXPECT_EQ(ctx.cop2c[reg], before.cop2c[reg]);
        }
        EXPECT_EQ(memory.read32(0x1f800384u), 0x12345678u);
        EXPECT_EQ(memory.read32(0x1f800390u), 0x87654321u);
      }
    }
  }
}

TEST_F(Crash3ProfileTest,
       SceneryCullGuardWidensOnlyTemporaryPackedVisibilityWitnesses) {
  ps1::games::crash3::configure(
      {.enabled = true, .widescreen16x9 = true, .sceneryCullGuardPixels = 64u});
  ps1::CPUContext ctx{};
  ctx.reset();
  const auto packed = [](int16_t x, int16_t y) {
    return static_cast<uint32_t>(static_cast<uint16_t>(x)) |
           (static_cast<uint32_t>(static_cast<uint16_t>(y)) << 16u);
  };
  ctx.r[24] = packed(-64, 10);
  ctx.r[25] = packed(512, 20);
  ctx.r[2] = packed(300, 30);
  ctx.r[31] = packed(512, 257);
  ctx.pc = 0x8004184Cu;

  ps1::games::crash3::prepareSceneryCullGuardForCommand(ctx);
  EXPECT_EQ(ctx.r[24], packed(0, 10));
  EXPECT_EQ(ctx.r[25], packed(576, 20));
  EXPECT_EQ(ctx.r[2], packed(364, 30));
  EXPECT_EQ(ctx.r[31], packed(640, 257));

  // The fourth quad corner is shifted only at its cull-only read. Other
  // packet-writing reads retain the exact 16:9 presentation coordinate.
  constexpr int32_t mainOfx = 256 << 16;
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x80041B14u, ps1::GTE_SXY2, packed(512, 40), mainOfx),
            packed(512, 40));
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x80041CCCu, ps1::GTE_SXY2, packed(512, 40), mainOfx),
            packed(448, 40));

  const ps1::CPUContext widened = ctx;
  ctx.pc = 0x80041850u;
  ps1::games::crash3::prepareSceneryCullGuardForCommand(ctx);
  EXPECT_EQ(ctx.r[24], widened.r[24]);
  EXPECT_EQ(ctx.r[31], widened.r[31]);
}

TEST_F(Crash3ProfileTest, ScenerySelectionIsOriginalUnlessExplicitlySelected) {
  ps1::games::crash3::configure({.enabled = true});
  ps1::Memory memory;
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &memory;
  constexpr uint32_t list = 0x80030000u;
  memory.write16(list, 2u);
  memory.write16(list + 4u, 0x0007u);
  memory.write16(list + 6u, 0x7803u);

  EXPECT_EQ(ps1::games::crash3::scenerySelectionCount(&ctx, list), 2);
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&ctx, list + 4u),
            0x0007u);
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&ctx, list + 6u),
            0x7803u);
}

TEST_F(Crash3ProfileTest, TotalSlstBypassEnumeratesAllResidentScenery) {
  ps1::games::crash3::configure({.enabled = true});
  setenv("PS1_CRASH3_SLST_MODE", "bypass", 1);
  ps1::Memory memory;
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &memory;

  constexpr uint32_t owner = 0x80020000u;
  constexpr uint32_t table = 0x80021000u;
  constexpr uint32_t info0 = 0x80022000u;
  constexpr uint32_t info1 = 0x80022100u;
  constexpr uint32_t list = 0x80030000u;
  memory.write32(0x80065EBCu, owner);
  memory.write32(owner + 0x10u, table);
  memory.write32(table, 2u);
  memory.write32(table + 0x14u, info0);
  memory.write32(table + 0x14u + 0x30u, info1);
  memory.write32(info0 + 0x14u, 2u);
  memory.write32(info0 + 0x18u, 1u);
  memory.write32(info1 + 0x14u, 1u);
  memory.write32(info1 + 0x18u, 2u);
  memory.write16(list, 1u);
  memory.write16(list + 4u, 0u);

  ASSERT_EQ(ps1::games::crash3::scenerySelectionCount(&ctx, list), 6);
  std::array<uint32_t, 8> triangleCounts{};
  std::array<uint32_t, 8> quadCounts{};
  for (uint32_t index = 0; index < 6u; ++index) {
    const uint32_t packed =
        ps1::games::crash3::scenerySelectionEntry(&ctx, list + 4u + index * 2u);
    const uint32_t world = packed >> 13u;
    const uint32_t state = (packed >> 11u) & 3u;
    ASSERT_LT(world, 2u);
    if (state == 0)
      ++triangleCounts[world];
    else {
      EXPECT_EQ(state, 3u);
      ++quadCounts[world];
    }
  }
  EXPECT_EQ(triangleCounts[0], 2u);
  EXPECT_EQ(quadCounts[0], 1u);
  EXPECT_EQ(triangleCounts[1], 1u);
  EXPECT_EQ(quadCounts[1], 2u);
}

TEST_F(Crash3ProfileTest,
       AdjacentSlstPreservesOrderAddsMissingFacesAndMergesQuadHalves) {
  ps1::games::crash3::configure({.enabled = true});
  setenv("PS1_CRASH3_SLST_MODE", "adjacent", 1);
  ps1::Memory memory;
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &memory;

  constexpr uint32_t owner = 0x80020000u;
  constexpr uint32_t table = 0x80021000u;
  constexpr uint32_t info = 0x80022000u;
  constexpr uint32_t source = 0x80023000u;
  constexpr uint32_t previous = 0x80030000u;
  constexpr uint32_t current = 0x80031000u;
  memory.write32(0x80065EBCu, owner);
  memory.write32(owner + 0x10u, table);
  memory.write32(table, 1u);
  memory.write32(table + 0x14u, info);
  memory.write32(info + 0x14u, 3u);
  memory.write32(info + 0x18u, 1u);
  memory.write16(source + 2u, 1u);

  // Stored order is the reverse of func_8004103C's traversal. The neighbor
  // contributes triangle 1 and the missing half of quad 0.
  memory.write16(previous, 4u);
  memory.write16(previous + 4u, 0u);
  memory.write16(previous + 6u, 2u << 11u);
  memory.write16(previous + 8u, 1u);
  memory.write16(previous + 10u, 2u);

  ctx.r[4] = source;
  ctx.r[5] = previous;
  ctx.r[6] = current;
  ctx.r[7] = 1u;
  ctx.r[19] = 0x80024000u;
  ctx.r[31] = 0x80021A44u;
  ps1::games::crash3::observeSceneryDelta(&ctx);

  memory.write16(current, 3u);
  memory.write16(current + 4u, 0u);
  memory.write16(current + 6u, 1u << 11u);
  memory.write16(current + 8u, 2u);

  ASSERT_EQ(ps1::games::crash3::scenerySelectionCount(&ctx, current), 4);
  const std::array<uint16_t, 4> expected{0u, 3u << 11u, 1u, 2u};
  for (uint32_t index = 0; index < expected.size(); ++index) {
    EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&ctx, current + 4u +
                                                                  index * 2u),
              expected[index]);
  }
}

TEST_F(Crash3ProfileTest, AdjacentSlstCanRetainMultipleConsecutiveDeltas) {
  ps1::games::crash3::configure({.enabled = true});
  setenv("PS1_CRASH3_SLST_MODE", "adjacent", 1);
  setenv("PS1_CRASH3_SLST_ADJACENT_DELTAS", "2", 1);
  ps1::Memory memory;
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &memory;

  constexpr uint32_t owner = 0x80020000u;
  constexpr uint32_t table = 0x80021000u;
  constexpr uint32_t info = 0x80022000u;
  constexpr uint32_t source0 = 0x80023000u;
  constexpr uint32_t source1 = 0x80023100u;
  constexpr uint32_t buffer0 = 0x80030000u;
  constexpr uint32_t buffer1 = 0x80031000u;
  memory.write32(0x80065EBCu, owner);
  memory.write32(owner + 0x10u, table);
  memory.write32(table, 1u);
  memory.write32(table + 0x14u, info);
  memory.write32(info + 0x14u, 4u);
  memory.write32(info + 0x18u, 0u);
  memory.write16(source0 + 2u, 1u);
  memory.write16(source1 + 2u, 1u);

  memory.write16(buffer0, 1u);
  memory.write16(buffer0 + 4u, 0u);
  ctx.r[4] = source0;
  ctx.r[5] = buffer0;
  ctx.r[6] = buffer1;
  ctx.r[7] = 1u;
  ctx.r[19] = 0x80024000u;
  ctx.r[31] = 0x80021A44u;
  ps1::games::crash3::observeSceneryDelta(&ctx);

  // Simulate the guest's first delta result, then observe it as the previous
  // list when the second delta starts.
  memory.write16(buffer1, 2u);
  memory.write16(buffer1 + 4u, 0u);
  memory.write16(buffer1 + 6u, 1u);
  ctx.r[4] = source1;
  ctx.r[5] = buffer1;
  ctx.r[6] = buffer0;
  ctx.r[19] = 0x80024004u;
  ps1::games::crash3::observeSceneryDelta(&ctx);

  // Second result is current. The two retained adjacent states contribute
  // triangle 0 while triangle 1 is deduplicated.
  memory.write16(buffer0, 2u);
  memory.write16(buffer0 + 4u, 1u);
  memory.write16(buffer0 + 6u, 2u);
  ASSERT_EQ(ps1::games::crash3::scenerySelectionCount(&ctx, buffer0), 3);
  const std::array<uint16_t, 3> expected{0u, 1u, 2u};
  for (uint32_t index = 0; index < expected.size(); ++index) {
    EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&ctx, buffer0 + 4u +
                                                                  index * 2u),
              expected[index]);
  }
}

TEST_F(Crash3ProfileTest, AdjacentSlstKeepsCurrentOrderAcrossDeltaSwaps) {
  ps1::games::crash3::configure({.enabled = true});
  setenv("PS1_CRASH3_SLST_MODE", "adjacent", 1);
  ps1::Memory memory;
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &memory;

  constexpr uint32_t owner = 0x80020000u;
  constexpr uint32_t table = 0x80021000u;
  constexpr uint32_t info = 0x80022000u;
  constexpr uint32_t source = 0x80023000u;
  constexpr uint32_t previous = 0x80030000u;
  constexpr uint32_t current = 0x80031000u;
  memory.write32(0x80065EBCu, owner);
  memory.write32(owner + 0x10u, table);
  memory.write32(table, 1u);
  memory.write32(table + 0x14u, info);
  memory.write32(info + 0x14u, 4u);
  memory.write32(info + 0x18u, 0u);
  memory.write16(source + 2u, 1u);

  // Current decoder order is 2,1,0. The neighbor swaps the two common
  // anchors to 0,3,2; triangle 3 is accepted while 2,1,0 stay authoritative.
  memory.write16(previous, 3u);
  memory.write16(previous + 4u, 2u);
  memory.write16(previous + 6u, 3u);
  memory.write16(previous + 8u, 0u);
  ctx.r[4] = source;
  ctx.r[5] = previous;
  ctx.r[6] = current;
  ctx.r[7] = 1u;
  ctx.r[31] = 0x80021A44u;
  ps1::games::crash3::observeSceneryDelta(&ctx);

  memory.write16(current, 3u);
  memory.write16(current + 4u, 0u);
  memory.write16(current + 6u, 1u);
  memory.write16(current + 8u, 2u);
  ASSERT_EQ(ps1::games::crash3::scenerySelectionCount(&ctx, current), 4);
  const std::array<uint16_t, 4> expected{0u, 1u, 2u, 3u};
  for (uint32_t index = 0; index < expected.size(); ++index) {
    EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&ctx, current + 4u +
                                                                  index * 2u),
              expected[index]);
  }
}

TEST_F(Crash3ProfileTest,
       SymmetricSlstDecodesBothSidesOfCurrentCameraPathState) {
  ps1::games::crash3::configure({.enabled = true});
  setenv("PS1_CRASH3_SLST_MODE", "symmetric", 1);
  setenv("PS1_CRASH3_SLST_ADJACENT_DELTAS", "1", 1);
  ps1::Memory memory;
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &memory;

  constexpr uint32_t owner = 0x80020000u;
  constexpr uint32_t table = 0x80021000u;
  constexpr uint32_t info = 0x80022000u;
  constexpr uint32_t entryBase = 0x80023000u;
  constexpr uint32_t rawStart = 0x80024000u;
  constexpr uint32_t rebuildDelta = 0x80024100u;
  constexpr uint32_t stateDelta = 0x80024200u;
  constexpr uint32_t rawEnd = 0x80024300u;
  constexpr uint32_t previous = 0x80030000u;
  constexpr uint32_t current = 0x80031000u;
  constexpr uint32_t callerStack = 0x80035000u;
  constexpr uint16_t quadHalfOne = 1u << 11u;
  constexpr uint16_t quadBothHalves = 3u << 11u;

  memory.write32(0x80065EBCu, owner);
  memory.write32(owner + 0x10u, table);
  memory.write32(table, 1u);
  memory.write32(table + 0x14u, info);
  memory.write32(info + 0x14u, 3u);
  memory.write32(info + 0x18u, 1u);

  memory.write32(entryBase + 16u, rawStart);
  memory.write32(entryBase + 20u, rebuildDelta);
  memory.write32(entryBase + 24u, stateDelta);
  memory.write32(entryBase + 28u, rawEnd);

  // State 0: triangle 0, the first half of quad 0, triangle 2.
  memory.write16(rawStart, 3u);
  memory.write16(rawStart + 2u, 0u);
  memory.write16(rawStart + 4u, 0u);
  memory.write16(rawStart + 6u, quadHalfOne);
  memory.write16(rawStart + 8u, 2u);

  // State 1 removes triangle 0 and inserts triangle 1 after the quad. Each
  // rebuild stream ends in 0xffff; the final stream has no ordering swaps.
  memory.write16(rebuildDelta, 9u);
  memory.write16(rebuildDelta + 2u, 1u);
  memory.write16(rebuildDelta + 4u, 5u);
  memory.write16(rebuildDelta + 6u, 8u);
  memory.write16(rebuildDelta + 8u, 1u);
  memory.write16(rebuildDelta + 10u, 0u);
  memory.write16(rebuildDelta + 12u, 0xffffu);
  memory.write16(rebuildDelta + 14u, 1u);
  memory.write16(rebuildDelta + 16u, 1u);
  memory.write16(rebuildDelta + 18u, 0xffffu);
  memory.write16(rebuildDelta + 20u, 0xffffu);

  // State 2 uses Crash 2/3's 0x2000 swap-stream command to enable the other
  // half of quad 0 in place. The raw endpoint is an independent oracle for
  // the complete forward reconstruction.
  memory.write16(stateDelta, 6u);
  memory.write16(stateDelta + 2u, 1u);
  memory.write16(stateDelta + 4u, 3u);
  memory.write16(stateDelta + 6u, 4u);
  memory.write16(stateDelta + 8u, 0xffffu);
  memory.write16(stateDelta + 10u, 0xffffu);
  memory.write16(stateDelta + 12u, 0x3000u);
  memory.write16(stateDelta + 14u, 0xffffu);

  memory.write16(rawEnd, 3u);
  memory.write16(rawEnd + 2u, 0u);
  memory.write16(rawEnd + 4u, quadBothHalves);
  memory.write16(rawEnd + 6u, 1u);
  memory.write16(rawEnd + 8u, 2u);

  memory.write32(callerStack + 68u, entryBase);
  ctx.r[29] = callerStack;
  ctx.r[4] = rawStart;
  ctx.r[5] = previous;
  ctx.r[6] = current;
  ctx.r[7] = 1u;
  ctx.r[17] = 0u;
  ctx.r[23] = 3u;
  ctx.r[30] = 1u << 8u;
  ctx.r[31] = 0x80021968u;
  ps1::games::crash3::observeSceneryDelta(&ctx);

  // Observe the forward decode into state 1, then expose exactly the result
  // the guest decoder would have placed in its current double buffer.
  ctx.r[4] = rebuildDelta;
  ctx.r[17] = 1u;
  ctx.r[31] = 0x80021A44u;
  ps1::games::crash3::observeSceneryDelta(&ctx);
  memory.write16(current, 3u);
  memory.write16(current + 4u, quadHalfOne);
  memory.write16(current + 6u, 1u);
  memory.write16(current + 8u, 2u);

  ASSERT_EQ(ps1::games::crash3::scenerySelectionCount(&ctx, current), 4);
  const std::array<uint16_t, 4> expected{0u, quadBothHalves, 1u, 2u};
  for (uint32_t index = 0; index < expected.size(); ++index) {
    EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&ctx, current + 4u +
                                                                  index * 2u),
              expected[index]);
  }
}

TEST_F(Crash3ProfileTest,
       GeometricSlstKeepsOriginalAndOnlyAddsFacesInsideWidescreenFrustum) {
  ps1::games::crash3::configure({.enabled = true, .widescreen16x9 = true});
  setenv("PS1_CRASH3_SLST_MODE", "geometric", 1);
  ps1::Memory memory;
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &memory;

  constexpr uint32_t owner = 0x80020000u;
  constexpr uint32_t table = 0x80021000u;
  constexpr uint32_t info = 0x80022000u;
  constexpr uint32_t entryBase = 0x80023000u;
  constexpr uint32_t rawStart = 0x80024000u;
  constexpr uint32_t delta = 0x80024100u;
  constexpr uint32_t rawEnd = 0x80024200u;
  constexpr uint32_t trianglePivot = 0x80026040u;
  constexpr uint32_t quadStart = 0x80026100u;
  constexpr uint32_t vertexPivot = 0x80026240u;
  constexpr uint32_t previous = 0x80030000u;
  constexpr uint32_t destination = 0x80031000u;
  constexpr uint32_t callerStack = 0x80035000u;
  constexpr int32_t worldTranslation = -1000;

  memory.write32(0x80065EBCu, owner);
  memory.write32(owner + 0x10u, table);
  memory.write32(table, 1u);
  const uint32_t record = table + 4u;
  memory.write32(record + 0x04u, static_cast<uint32_t>(worldTranslation));
  memory.write32(record + 0x08u, static_cast<uint32_t>(worldTranslation));
  memory.write32(record + 0x0cu, 0u);
  memory.write32(record + 0x10u, info);
  memory.write32(info + 0x10u, 7u);
  memory.write32(info + 0x14u, 3u);
  memory.write32(info + 0x18u, 0u);

  const uint32_t packedTranslation =
      static_cast<uint16_t>(worldTranslation) |
      (static_cast<uint32_t>(static_cast<uint16_t>(worldTranslation)) << 16u);
  memory.write32(0x1f800280u, packedTranslation);
  memory.write32(0x1f800284u, 0u);
  memory.write32(0x1f800288u, trianglePivot);
  memory.write32(0x1f80028cu, quadStart);
  memory.write32(0x1f800290u, vertexPivot);

  const auto writeVertex = [&](uint32_t index, uint32_t x, uint32_t y,
                               uint32_t z) {
    memory.write32(vertexPivot - (index + 1u) * 4u,
                   ((y << 4u) << 16u) | (x << 4u));
    memory.write16(vertexPivot + index * 2u, static_cast<uint16_t>(z << 4u));
  };
  // After Crash's /4 unpack and -1000 world translation, vertices 0..2
  // straddle the view centre at Z=400. Vertices 4..6 sit wholly beyond the
  // widened right plane.
  writeVertex(0u, 240u, 240u, 100u);
  writeVertex(1u, 260u, 240u, 100u);
  writeVertex(2u, 250u, 260u, 100u);
  writeVertex(3u, 250u, 250u, 100u);
  writeVertex(4u, 500u, 240u, 100u);
  writeVertex(5u, 500u, 250u, 100u);
  writeVertex(6u, 500u, 260u, 100u);

  const auto writeTriangle = [&](uint32_t id, uint32_t a, uint32_t b,
                                 uint32_t c) {
    memory.write32(trianglePivot - (id + 1u) * 4u, (a << 8u) | (b << 20u));
    memory.write16(trianglePivot + id * 2u, static_cast<uint16_t>(c << 4u));
  };
  writeTriangle(0u, 0u, 1u, 2u);
  writeTriangle(1u, 0u, 1u, 2u);
  writeTriangle(2u, 4u, 5u, 6u);

  memory.write32(entryBase + 16u, rawStart);
  memory.write32(entryBase + 20u, delta);
  memory.write32(entryBase + 24u, rawEnd);
  memory.write16(rawStart, 1u);
  memory.write16(rawStart + 4u, 0u);

  // Preserve triangle 0 and add triangles 1 and 2 in one rebuild run.
  memory.write16(delta, 8u);
  memory.write16(delta + 2u, 1u);
  memory.write16(delta + 4u, 3u);
  memory.write16(delta + 6u, 7u);
  memory.write16(delta + 8u, 0xffffu);
  memory.write16(delta + 10u, 0x1001u);
  memory.write16(delta + 12u, 1u);
  memory.write16(delta + 14u, 2u);
  memory.write16(delta + 16u, 0xffffu);
  memory.write16(delta + 18u, 0xffffu);
  memory.write16(rawEnd, 3u);
  memory.write16(rawEnd + 4u, 0u);
  memory.write16(rawEnd + 6u, 1u);
  memory.write16(rawEnd + 8u, 2u);

  // Identity 4.12 camera and a 256-unit projection plane.
  ctx.cop2c[ps1::GTE_RT11RT12] = 4096u;
  ctx.cop2c[ps1::GTE_RT22RT23] = 4096u;
  ctx.cop2c[ps1::GTE_RT33] = 4096u;
  ctx.cop2c[ps1::GTE_H] = 256u;
  memory.write32(callerStack + 68u, entryBase);
  ctx.r[29] = callerStack;
  ctx.r[4] = rawStart;
  ctx.r[5] = previous;
  ctx.r[6] = destination;
  ctx.r[7] = 1u;
  ctx.r[23] = 2u;
  ctx.r[31] = 0x80021968u;
  ps1::games::crash3::observeSceneryDelta(&ctx);

  ASSERT_EQ(ps1::games::crash3::scenerySelectionCount(&ctx, rawStart), 2);
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&ctx, rawStart + 4u), 0u);
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&ctx, rawStart + 6u), 1u);
}

TEST_F(Crash3ProfileTest,
       GeometricResidentSlstCompletesKnownQuadAndAddsOnlySideBandQuad) {
  ps1::games::crash3::configure({.enabled = true, .widescreen16x9 = true});
  setenv("PS1_CRASH3_SLST_MODE", "geometric", 1);
  ps1::Memory memory;
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &memory;

  constexpr uint32_t owner = 0x80020000u;
  constexpr uint32_t table = 0x80021000u;
  constexpr uint32_t resourcePair = 0x80021F00u;
  constexpr uint32_t info = 0x80022000u;
  constexpr uint32_t resourceEntry = info - 0x30u;
  constexpr uint32_t resourceId = 0x12345u;
  constexpr uint32_t entryBase = 0x80023000u;
  constexpr uint32_t rawStart = 0x80024000u;
  constexpr uint32_t delta = 0x80024100u;
  constexpr uint32_t rawEnd = 0x80024200u;
  constexpr uint32_t trianglePivot = 0x80026040u;
  constexpr uint32_t quadStart = 0x80026100u;
  constexpr uint32_t vertexPivot = 0x80026240u;
  constexpr uint32_t materialTable = 0x80027000u;
  constexpr uint32_t materialRemap = 0x80027200u;
  constexpr uint32_t colorTable = 0x80028000u;
  constexpr uint32_t previous = 0x80030000u;
  constexpr uint32_t destination = 0x80031000u;
  constexpr uint32_t callerStack = 0x80035000u;
  constexpr int32_t worldTranslation = -1000;
  constexpr uint16_t quadHalfOne = 1u << 11u;
  constexpr uint16_t quadBothHalves = 3u << 11u;
  constexpr uint16_t sideBandQuadBothHalves = (3u << 11u) | 1u;

  memory.write32(0x80065EBCu, owner);
  memory.write32(owner + 0x10u, table);
  memory.write32(table, 1u);
  const uint32_t record = table + 4u;
  memory.write32(record, resourcePair);
  memory.write32(resourcePair, resourceEntry);
  memory.write32(resourcePair + 4u, resourceId);
  memory.write32(record + 0x04u, static_cast<uint32_t>(worldTranslation));
  memory.write32(record + 0x08u, static_cast<uint32_t>(worldTranslation));
  memory.write32(record + 0x0cu, 0u);
  memory.write32(record + 0x10u, info);
  memory.write32(info + 0x10u, 16u);
  memory.write32(info + 0x14u, 1u);
  memory.write32(info + 0x18u, 5u);

  const uint32_t packedTranslation =
      static_cast<uint16_t>(worldTranslation) |
      (static_cast<uint32_t>(static_cast<uint16_t>(worldTranslation)) << 16u);
  memory.write32(0x1f800280u, packedTranslation);
  memory.write32(0x1f800284u, 0u);
  memory.write32(0x1f800288u, trianglePivot);
  memory.write32(0x1f80028cu, quadStart);
  memory.write32(0x1f800290u, vertexPivot);
  memory.write32(0x1f800294u, colorTable);
  memory.write32(0x1f800298u, materialRemap);
  memory.write32(0x1f80029cu, materialTable);

  const auto writeVertex = [&](uint32_t index, uint32_t x, uint32_t y,
                               uint32_t z) {
    // Put a distinct lighting-table identity in the raw X low nibble. The
    // position decoder discards it, while func_8004103C maps it to bits 6..9
    // of the byte offset. This tests the actual embedded-index path instead
    // of relying on the all-zero table entry.
    const uint32_t lightingNibble = index & 0x0fu;
    memory.write32(vertexPivot - (index + 1u) * 4u,
                   ((y << 4u) << 16u) | (x << 4u) | lightingNibble);
    memory.write16(vertexPivot + index * 2u, static_cast<uint16_t>(z << 4u));
    const uint32_t red = 17u + index;
    const uint32_t green = 34u + index;
    const uint32_t blue = 51u + index;
    memory.write32(colorTable + (lightingNibble << 6u),
                   red | (green << 8u) | (blue << 16u));
  };
  writeVertex(0u, 240u, 240u, 100u);
  writeVertex(1u, 260u, 240u, 100u);
  writeVertex(2u, 260u, 260u, 100u);
  writeVertex(3u, 240u, 260u, 100u);
  // This second quad spans native X=512..563: outside the old 4:3 plane but
  // wholly inside the 16:9 presentation plane. Quad 2 reuses the central
  // vertices and proves that residency alone is not enough for admission.
  writeVertex(4u, 350u, 240u, 100u);
  writeVertex(5u, 370u, 240u, 100u);
  writeVertex(6u, 370u, 260u, 100u);
  writeVertex(7u, 350u, 260u, 100u);
  // Quad 3 is wholly outside the exact 16:9 plane (presentation X=544..582)
  // but intersects a 64-pixel host admission guard. It proves the guard can
  // retain neighboring resident topology without changing the final viewport.
  writeVertex(8u, 400u, 240u, 100u);
  writeVertex(9u, 420u, 240u, 100u);
  writeVertex(10u, 420u, 260u, 100u);
  writeVertex(11u, 400u, 260u, 100u);
  // A final textured quad crosses the first unsaturated GTE depth. Its first
  // half becomes two host triangles and provides a direct witness that UV and
  // Gouraud attributes follow the geometric near-plane intersections.
  writeVertex(12u, 250u, 240u, 10u);
  writeVertex(13u, 270u, 240u, 100u);
  writeVertex(14u, 270u, 260u, 100u);
  writeVertex(15u, 250u, 260u, 100u);

  // The triangle makes the complete source-path union differ from the current
  // list. The quad itself exists in every source state with only half 1 set.
  memory.write32(trianglePivot - 4u, (0u << 8u) | (1u << 20u));
  memory.write16(trianglePivot, 2u << 4u);
  memory.write32(quadStart, (0u << 8u) | (1u << 20u));
  // Low material token 1 addresses materialTable[0] directly.
  memory.write32(quadStart + 4u, (3u << 8u) | (2u << 20u) | 1u);
  memory.write32(quadStart + 8u, (4u << 8u) | (5u << 20u));
  memory.write32(quadStart + 12u, (7u << 8u) | (6u << 20u));
  // Quad 2 uses the resident renderer's 12-bit dynamic token 0x802. Its
  // first descriptor selects record 5; the terminal descriptor uses the
  // AVSZ distance-table shift to add variant 2 and reach record 7.
  memory.write32(quadStart + 16u, (0u << 8u) | (1u << 20u) | 0x80u);
  memory.write32(quadStart + 20u, (3u << 8u) | (2u << 20u) | 0x02u);
  memory.write32(quadStart + 24u, (8u << 8u) | (9u << 20u));
  memory.write32(quadStart + 28u, (11u << 8u) | (10u << 20u));
  memory.write32(quadStart + 32u, (12u << 8u) | (13u << 20u));
  memory.write32(quadStart + 36u, (15u << 8u) | (14u << 20u) | 1u);
  memory.write32(materialTable, (0x0041u << 16u) | (2u << 8u) | 1u);
  memory.write32(materialTable + 4u, (0x1160u << 16u) | (4u << 8u) | 3u);
  memory.write32(materialTable + 8u,
                 ((8u << 8u) | 7u) << 16u | (6u << 8u) | 5u);
  memory.write32(materialRemap + 2u * 4u, 0x00000805u);
  memory.write32(materialRemap + 5u * 4u, 0x00002005u);
  constexpr uint32_t dynamicMaterialOffset = (7u - 1u) * 12u;
  memory.write32(materialTable + dynamicMaterialOffset,
                 (0x0042u << 16u) | (10u << 8u) | 9u);
  memory.write32(materialTable + dynamicMaterialOffset + 4u,
                 (0x1161u << 16u) | (12u << 8u) | 11u);
  memory.write32(materialTable + dynamicMaterialOffset + 8u,
                 ((16u << 8u) | 15u) << 16u | (14u << 8u) | 13u);

  memory.write32(entryBase + 16u, rawStart);
  memory.write32(entryBase + 20u, delta);
  memory.write32(entryBase + 24u, rawEnd);
  memory.write16(rawStart, 1u);
  memory.write16(rawStart + 4u, quadHalfOne);

  // State 1 adds triangle 0 but deliberately does not change the quad state.
  memory.write16(delta, 7u);
  memory.write16(delta + 2u, 1u);
  memory.write16(delta + 4u, 3u);
  memory.write16(delta + 6u, 6u);
  memory.write16(delta + 8u, 0xffffu);
  memory.write16(delta + 10u, 1u);
  memory.write16(delta + 12u, 0u);
  memory.write16(delta + 14u, 0xffffu);
  memory.write16(delta + 16u, 0xffffu);
  memory.write16(rawEnd, 2u);
  memory.write16(rawEnd + 4u, quadHalfOne);
  memory.write16(rawEnd + 6u, 0u);

  ctx.cop2c[ps1::GTE_RT11RT12] = 4096u;
  ctx.cop2c[ps1::GTE_RT22RT23] = 4096u;
  ctx.cop2c[ps1::GTE_RT33] = 4096u;
  ctx.cop2c[ps1::GTE_H] = 256u;
  memory.write32(callerStack + 68u, entryBase);
  ctx.r[29] = callerStack;
  ctx.r[4] = rawStart;
  ctx.r[5] = previous;
  ctx.r[6] = destination;
  ctx.r[7] = 1u;
  ctx.r[23] = 2u;
  ctx.r[31] = 0x80021968u;

  // The existing geometric mode is the control: it may add the source-path
  // triangle, but it must not infer a quad half absent from every SLST state.
  ps1::games::crash3::observeSceneryDelta(&ctx);
  ASSERT_EQ(ps1::games::crash3::scenerySelectionCount(&ctx, rawStart), 2);
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&ctx, rawStart + 4u),
            quadHalfOne);

  // The resident mode deliberately has no source-path observation after this
  // reset: it must be reconstructible from a savestate's current guest SLST.
  // It completes the known quad and prepends the missing side-band quad so
  // source-authored faces win equal-bucket OT ties. The path-only triangle and
  // equally resident but central quad 2 remain absent.
  ps1::games::crash3::configure({.enabled = true, .widescreen16x9 = true});
  setenv("PS1_CRASH3_SLST_MODE", "geometric-resident", 1);
  ASSERT_EQ(ps1::games::crash3::scenerySelectionCount(&ctx, rawStart), 2);
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&ctx, rawStart + 4u),
            sideBandQuadBothHalves);
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&ctx, rawStart + 6u),
            quadBothHalves);

  ps1::games::crash3::configure(
      {.enabled = true, .widescreen16x9 = true, .sceneryCullGuardPixels = 64u});
  ASSERT_EQ(ps1::games::crash3::scenerySelectionCount(&ctx, rawStart), 3);
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&ctx, rawStart + 4u),
            static_cast<uint16_t>((3u << 11u) | 3u));
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&ctx, rawStart + 6u),
            sideBandQuadBothHalves);
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&ctx, rawStart + 8u),
            quadBothHalves);

  // The Vulkan sidecar observes the complete resident geometry without
  // changing what Crash consumes. Its exact-aperture result is retained as
  // audit metadata, but no longer acts as a second CPU cull: Vulkan owns final
  // clipping. The original one-half SLST remains byte-for-byte authoritative.
  unsetenv("PS1_CRASH3_SLST_MODE");
  setenv("PS1_CRASH3_VULKAN_RESIDENT_MODE", "debug", 1);
  // This decoder fixture mutates raw resource bytes between captures to
  // exercise deformation/material corner cases. Keep the transition journal
  // in audit-only mode so its intentional immutable-resource debounce does
  // not replace those synthetic mutations with the prior certified cohort.
  setenv("PS1_CRASH3_RESIDENT_COHORT_PRESENT", "0", 1);
  setenv("PS1_CRASH3_RESIDENT_FRUSTUM_AUDIT", "1", 1);
  ps1::games::crash3::configure(
      {.enabled = true, .widescreen16x9 = true, .sceneryCullGuardPixels = 64u});
  // The source contains only quad half 1, so Crash takes its AVSZ3 path:
  // Z=400 gives the rounded-down local depth 49. The caller's depth shift 4
  // addresses distance-table byte 3, which selects descriptor bits 12..13.
  memory.write32(destination + 0x30u, 4u);
  memory.write8(0x8005D4B4u + 3u, 12u);
  memory.write8(0x1f800000u + 208u + 3u, 12u);
  ctx.cop2c[ps1::GTE_OFX] = 256u << 16u;
  ctx.cop2c[ps1::GTE_OFY] = 128u << 16u;
  const uint64_t targetFrame = ps1::metrics::traceFrame() + 1u;
  ASSERT_EQ(ps1::games::crash3::scenerySelectionCount(&ctx, rawStart), 1);
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&ctx, rawStart + 4u),
            quadHalfOne);
  ctx.pc = 0x800417ACu;
  ps1::games::crash3::captureResidentPresentationAtProjection(ctx);
  const auto sidecar =
      ps1::games::crash3::residentPresentationSnapshot(targetFrame);
  ASSERT_TRUE(sidecar.has_value());
  EXPECT_NE(sidecar->topologyFingerprint, 0u);
  EXPECT_NE(sidecar->resourceSetFingerprint, 0u);
  EXPECT_TRUE(sidecar->resourceSetValidated);
  EXPECT_TRUE(sidecar->placementValidated);
  EXPECT_EQ(sidecar->candidateObservations, 1u);
  EXPECT_FALSE(sidecar->cohortCommitted);
  ASSERT_EQ(sidecar->resources.size(), 1u);
  EXPECT_EQ(sidecar->resources[0].resourceId, resourceId);
  EXPECT_TRUE(sidecar->resources[0].identityValid);
  ASSERT_TRUE(sidecar->gpuScene.has_value());
  EXPECT_EQ(sidecar->gpuScene->schema, 1u);
  EXPECT_EQ(sidecar->gpuScene->captureId, sidecar->captureId);
  EXPECT_EQ(sidecar->gpuScene->topologyFingerprint,
            sidecar->topologyFingerprint);
  EXPECT_EQ(sidecar->gpuScene->resourceSetFingerprint,
            sidecar->resourceSetFingerprint);
  EXPECT_TRUE(sidecar->gpuScene->completeForPresentation);
  EXPECT_EQ(sidecar->gpuScene->rotation[0][0], 4096);
  EXPECT_EQ(sidecar->gpuScene->rotation[1][1], 4096);
  EXPECT_EQ(sidecar->gpuScene->rotation[2][2], 4096);
  EXPECT_EQ(sidecar->gpuScene->projectionPlane, 256);
  EXPECT_FALSE(sidecar->gpuScene->deformation.active);
  EXPECT_EQ(sidecar->gpuScene->rawVertexCount, 16u);
  EXPECT_EQ(sidecar->gpuScene->rawFaceCount, 11u);
  ASSERT_EQ(sidecar->gpuScene->presentationFaces.size(), 11u);
  EXPECT_EQ(sidecar->gpuScene->presentationFaces[0].topologyOrdinal, 0u);
  EXPECT_EQ(sidecar->gpuScene->presentationFaces[0].resourceFaceOrdinal, 0u);
  EXPECT_EQ(sidecar->gpuScene->presentationFaces[0].resourceIndex, 0u);
  EXPECT_TRUE(sidecar->gpuScene->presentationFaces[0].material.decoded);
  EXPECT_TRUE(sidecar->gpuScene->presentationFaces[0].colorDecoded);
  EXPECT_EQ(sidecar->gpuScene->rebuiltPayloadCount, 1u);
  ASSERT_EQ(sidecar->gpuScene->resources.size(), 1u);
  const auto firstGpuPayload = sidecar->gpuScene->resources[0].payload;
  ASSERT_NE(firstGpuPayload, nullptr);
  EXPECT_EQ(firstGpuPayload->schema, 1u);
  EXPECT_EQ(firstGpuPayload->resourceId, resourceId);
  EXPECT_EQ(firstGpuPayload->geometryFingerprint,
            sidecar->resources[0].geometryFingerprint);
  ASSERT_EQ(firstGpuPayload->vertices.size(), 16u);
  EXPECT_EQ(firstGpuPayload->vertices[0].rawXy,
            memory.read32(vertexPivot - 4u));
  EXPECT_EQ(firstGpuPayload->vertices[0].rawZ, memory.read16(vertexPivot));
  EXPECT_TRUE(firstGpuPayload->vertices[0].colorDecoded);
  EXPECT_EQ(firstGpuPayload->vertices[0].color255,
            (std::array<uint8_t, 3>{{17u, 34u, 51u}}));
  ASSERT_EQ(firstGpuPayload->faces.size(), 11u);
  EXPECT_EQ(firstGpuPayload->faces[0].resourceFaceOrdinal, 0u);
  EXPECT_EQ(firstGpuPayload->faces[0].topologyOrdinal, 0u);
  EXPECT_EQ(firstGpuPayload->faces[0].sourceVertexIndex,
            (std::array<uint32_t, 3>{{1u, 0u, 2u}}));
  EXPECT_EQ(firstGpuPayload->faces[0].descriptorA,
            memory.read32(trianglePivot - 4u));
  EXPECT_EQ(firstGpuPayload->faces[0].descriptorB,
            memory.read16(trianglePivot));
  EXPECT_EQ(sidecar->gpuScene->resources[0].translationXy, packedTranslation);
  EXPECT_EQ(sidecar->gpuScene->resources[0].translationZ, 0);
  EXPECT_EQ(sidecar->sourceEntryCount, 1u);
  EXPECT_EQ(sidecar->sourceFaceCount, 1u);
  EXPECT_EQ(sidecar->residentFaceCount, 11u);
  EXPECT_EQ(sidecar->presentationFrustumFaces, 9u);
  EXPECT_EQ(sidecar->outsidePresentationFrustumFaces, 2u);
  EXPECT_EQ(sidecar->nearPlaneDeferredFaces, 0u);
  EXPECT_EQ(sidecar->nearPlaneClippedFaces, 2u);
  EXPECT_EQ(sidecar->nearPlaneGeneratedFaces, 4u);
  EXPECT_EQ(sidecar->nearPlaneRejectedFaces, 0u);
  EXPECT_EQ(sidecar->decodedMaterialFaces, 11u);
  EXPECT_EQ(sidecar->unresolvedMaterialFaces, 0u);
  EXPECT_EQ(sidecar->decodedTexturedFaces, 6u);
  EXPECT_EQ(sidecar->decodedDynamicMaterialFaces, 2u);
  EXPECT_EQ(sidecar->decodedSemiTransparentFaces, 0u);
  EXPECT_EQ(sidecar->decodedOrderingTableFaces, 11u);
  EXPECT_EQ(sidecar->unresolvedOrderingTableFaces, 0u);
  EXPECT_EQ(sidecar->decodedColorFaces, 11u);
  EXPECT_EQ(sidecar->unresolvedColorFaces, 0u);
  EXPECT_EQ(sidecar->faces.size(), 13u);
  EXPECT_TRUE(std::all_of(sidecar->faces.begin(), sidecar->faces.end(),
                          [](const auto &face) { return face.colorDecoded; }));
  const auto firstTextured = std::find_if(
      sidecar->faces.begin(), sidecar->faces.end(), [](const auto &face) {
        return face.material.textured && face.half == 1u;
      });
  ASSERT_NE(firstTextured, sidecar->faces.end());
  EXPECT_EQ(firstTextured->topologyOrdinal, 1u);
  EXPECT_TRUE(firstTextured->sourceTraversalOrdinalValid);
  EXPECT_EQ(firstTextured->sourceTraversalOrdinal, 0u);
  EXPECT_TRUE(firstTextured->orderingTableBucketValid);
  EXPECT_EQ(firstTextured->orderingTableBucket, 1855u);
  EXPECT_EQ(firstTextured->material.clut, 0x0041u);
  EXPECT_EQ(firstTextured->material.sourceToken, 1u);
  EXPECT_EQ(firstTextured->material.texturePage, 0x0160u);
  EXPECT_EQ(firstTextured->material.uv[0],
            (std::array<float, 2>{{1.0f, 2.0f}}));
  EXPECT_EQ(firstTextured->material.uv[1],
            (std::array<float, 2>{{3.0f, 4.0f}}));
  EXPECT_EQ(firstTextured->material.uv[2],
            (std::array<float, 2>{{5.0f, 6.0f}}));
  EXPECT_EQ(firstTextured->material.uvDomain,
            (std::array<float, 4>{{1.0f, 2.0f, 6.0f, 7.0f}}));
  EXPECT_EQ(firstTextured->color255[0],
            (std::array<float, 3>{{18.0f, 35.0f, 52.0f}}));

  ctx.r[3] = 0x1F800000u;
  ctx.pc = 0x8004184Cu;
  ps1::games::crash3::observeSceneryOrderingTableAverage(ctx, 49u, 3u);
  const auto orderingTableProof =
      ps1::games::crash3::residentOrderingTableProof();
  EXPECT_EQ(orderingTableProof.topologyFingerprint,
            sidecar->topologyFingerprint);
  EXPECT_EQ(orderingTableProof.matches, 1u);
  EXPECT_EQ(orderingTableProof.mismatches, 0u);
  const auto captureOrderingTableProof =
      ps1::games::crash3::residentOrderingTableProof(sidecar->captureId);
  EXPECT_EQ(captureOrderingTableProof.topologyFingerprint,
            sidecar->topologyFingerprint);
  EXPECT_EQ(captureOrderingTableProof.matches, 1u);
  EXPECT_EQ(captureOrderingTableProof.mismatches, 0u);
  EXPECT_EQ(firstTextured->color255[1],
            (std::array<float, 3>{{17.0f, 34.0f, 51.0f}}));
  EXPECT_EQ(firstTextured->color255[2],
            (std::array<float, 3>{{19.0f, 36.0f, 53.0f}}));
  const auto secondTextured = std::find_if(
      sidecar->faces.begin(), sidecar->faces.end(), [](const auto &face) {
        return face.material.textured && face.half == 2u;
      });
  ASSERT_NE(secondTextured, sidecar->faces.end());
  EXPECT_EQ(secondTextured->material.uv[0],
            (std::array<float, 2>{{3.0f, 4.0f}}));
  EXPECT_EQ(secondTextured->material.uv[2],
            (std::array<float, 2>{{7.0f, 8.0f}}));
  const auto dynamicTextured = std::find_if(
      sidecar->faces.begin(), sidecar->faces.end(),
      [](const auto &face) { return face.material.dynamicTexture; });
  ASSERT_NE(dynamicTextured, sidecar->faces.end());
  EXPECT_TRUE(dynamicTextured->material.decoded);
  EXPECT_EQ(dynamicTextured->material.sourceToken, 0x802u);
  EXPECT_EQ(dynamicTextured->material.resolvedToken, 7u);
  EXPECT_EQ(dynamicTextured->material.texturePage, 0x0161u);
  EXPECT_EQ(dynamicTextured->material.clut, 0x0042u);
  EXPECT_FALSE(dynamicTextured->material.selectorFromScratchTail);
  EXPECT_EQ(std::count_if(sidecar->faces.begin(), sidecar->faces.end(),
                          [](const auto &face) { return face.sourceSelected; }),
            1);
  EXPECT_EQ(
      std::count_if(sidecar->faces.begin(), sidecar->faces.end(),
                    [](const auto &face) { return !face.sourceSelected; }),
      12);
  EXPECT_EQ(std::count_if(
                sidecar->faces.begin(), sidecar->faces.end(),
                [](const auto &face) { return (face.packed & 0x7ffu) == 3u; }),
            2);
  const auto nearFaceCount = std::count_if(
      sidecar->faces.begin(), sidecar->faces.end(), [](const auto &face) {
        return (face.packed & 0x7ffu) == 4u && face.half == 1u &&
               face.nearPlaneClipped;
      });
  EXPECT_EQ(nearFaceCount, 2);
  const bool hasInterpolatedAttribute = std::any_of(
      sidecar->faces.begin(), sidecar->faces.end(), [](const auto &face) {
        if ((face.packed & 0x7ffu) != 4u || face.half != 1u ||
            !face.nearPlaneClipped || !face.material.textured ||
            face.material.clut != 0x0041u ||
            face.material.texturePage != 0x0160u) {
          return false;
        }
        for (const auto &uv : face.material.uv) {
          for (const float component : uv) {
            if (std::abs(component - std::round(component)) > 0.001f)
              return true;
          }
        }
        for (const auto &color : face.color255) {
          for (const float component : color) {
            if (std::abs(component - std::round(component)) > 0.001f)
              return true;
          }
        }
        return false;
      });
  EXPECT_TRUE(hasInterpolatedAttribute);

  // Crash copies its 32-byte table to scratch and indexes that destination
  // without a second bound. TRZ=4000 makes this AVSZ3 path select byte 34,
  // which is therefore adjacent renderer state rather than ROM after-table
  // data. Host completion must execute that exact bounded scratch read.
  memory.write8(0x1f800000u + 208u + 34u, 12u);
  ctx.cop2c[ps1::GTE_TRZ] = 4000u;
  setenv("PS1_CRASH3_VULKAN_GPU_COMPACT_OPAQUE", "1", 1);
  ASSERT_EQ(ps1::games::crash3::scenerySelectionCount(&ctx, rawStart), 1);
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&ctx, rawStart + 4u),
            quadHalfOne);
  ctx.pc = 0x800417ACu;
  ps1::games::crash3::captureResidentPresentationAtProjection(ctx);
  unsetenv("PS1_CRASH3_VULKAN_GPU_COMPACT_OPAQUE");
  const auto scratchTailSnapshots =
      ps1::games::crash3::residentPresentationSnapshots();
  ASSERT_FALSE(scratchTailSnapshots.empty());
  const auto &scratchTailSidecar = scratchTailSnapshots.back();
  EXPECT_TRUE(scratchTailSidecar.cohortCommitted);
  EXPECT_EQ(scratchTailSidecar.activeCohortGeneration, 1u);
  EXPECT_EQ(scratchTailSidecar.activeCohortFingerprint,
            scratchTailSidecar.resourceSetFingerprint);
  EXPECT_EQ(scratchTailSidecar.unresolvedMaterialFaces, 0u);
  EXPECT_EQ(scratchTailSidecar.decodedScratchTailDynamicMaterialFaces, 2u);
  ASSERT_TRUE(scratchTailSidecar.gpuScene.has_value());
  ASSERT_EQ(scratchTailSidecar.gpuScene->resources.size(), 1u);
  EXPECT_GT(scratchTailSidecar.gpuScene->gpuRawOnlyFaceCount, 0u);
  EXPECT_EQ(scratchTailSidecar.gpuScene->gpuRawOnlyFaceCount,
            std::count_if(scratchTailSidecar.faces.begin(),
                          scratchTailSidecar.faces.end(),
                          [](const auto &face) { return face.gpuRawOnly; }));
  EXPECT_TRUE(std::all_of(
      scratchTailSidecar.faces.begin(), scratchTailSidecar.faces.end(),
      [](const auto &face) {
        return !face.gpuRawOnly ||
               std::all_of(face.vertices.begin(), face.vertices.end(),
                           [](const auto &vertex) {
                             return vertex.x == 0.0 && vertex.y == 0.0 &&
                                    vertex.projectiveW == 0.0;
                           });
      }));
  EXPECT_EQ(scratchTailSidecar.gpuScene->reusedPayloadCount, 1u);
  EXPECT_EQ(scratchTailSidecar.gpuScene->rebuiltPayloadCount, 0u);
  // Geometry and base colours did not change: the immutable payload survives
  // the new camera/material capture instead of being rebuilt per frame.
  EXPECT_EQ(scratchTailSidecar.gpuScene->resources[0].payload, firstGpuPayload);
  const auto scratchTailDynamic = std::find_if(
      scratchTailSidecar.faces.begin(), scratchTailSidecar.faces.end(),
      [](const auto &face) { return face.material.selectorFromScratchTail; });
  ASSERT_NE(scratchTailDynamic, scratchTailSidecar.faces.end());
  EXPECT_TRUE(scratchTailDynamic->material.dynamicTexture);
  EXPECT_EQ(scratchTailDynamic->material.resolvedToken, 7u);
  // Preparing the next double-buffered cohort must not overwrite the exact
  // proof still referenced by the preceding Vulkan batch.
  const auto retainedCaptureProof =
      ps1::games::crash3::residentOrderingTableProof(sidecar->captureId);
  EXPECT_EQ(retainedCaptureProof.topologyFingerprint,
            sidecar->topologyFingerprint);
  EXPECT_EQ(retainedCaptureProof.matches, 1u);

  // The GTE exposes MAC3 through a signed 32-bit register before pushing SZ.
  // A far translation therefore wraps negative instead of saturating the raw
  // 64-bit transform to 0xffff. The resident decoder must make the same
  // distinction or its local ordering bucket diverges only at camera edges.
  ctx.cop2c[ps1::GTE_TRZ] = static_cast<uint32_t>(INT32_MAX);
  ASSERT_EQ(ps1::games::crash3::scenerySelectionCount(&ctx, rawStart), 1);
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&ctx, rawStart + 4u),
            quadHalfOne);
  ctx.pc = 0x800417ACu;
  ps1::games::crash3::captureResidentPresentationAtProjection(ctx);
  const auto wrappedSnapshots =
      ps1::games::crash3::residentPresentationSnapshots();
  ASSERT_FALSE(wrappedSnapshots.empty());
  const auto &wrappedSidecar = wrappedSnapshots.back();
  const auto wrappedFace =
      std::find_if(wrappedSidecar.faces.begin(), wrappedSidecar.faces.end(),
                   [](const auto &face) { return face.packed == (1u << 11u); });
  ASSERT_NE(wrappedFace, wrappedSidecar.faces.end());
  ASSERT_TRUE(wrappedFace->orderingTableBucketValid);
  EXPECT_EQ(wrappedFace->orderingTableBucket, 1904u);
  ctx.r[3] = 0x1F800000u;
  ctx.pc = 0x8004184Cu;
  ps1::games::crash3::observeSceneryOrderingTableAverage(ctx, 0u, 3u);
  const auto wrappedProof = ps1::games::crash3::residentOrderingTableProof();
  EXPECT_EQ(wrappedProof.matches, 2u);
  EXPECT_EQ(wrappedProof.mismatches, 0u);

  // Crash's resident renderer can intrinsically mark a vertex for its wave
  // post-processor in raw-XY bit 18.  Exercise the same code-owned handler
  // pair and table inputs used by gameplay levels: host completion must
  // deform every resident face sharing that vertex, not merely learn the
  // source-selected packet which happened to expose it.
  ctx.cop2c[ps1::GTE_TRZ] = 0u;
  constexpr uint32_t baseModelXy =
      static_cast<uint16_t>(-40) |
      (static_cast<uint32_t>(static_cast<uint16_t>(-40)) << 16u);
  const uint32_t rawVertexZero = memory.read32(vertexPivot - 4u);
  memory.write32(vertexPivot - 4u, rawVertexZero | (1u << 18u));
  memory.write32(0x1f800000u + 28u, 0u);
  memory.write32(0x1f800000u + 96u, 16u);
  memory.write32(0x1f800000u + 168u, 0x80041DC8u);
  memory.write32(0x1f800000u + 172u, 0x80041E8Cu);
  memory.write32(0x1f800000u + 204u, 0u - 400u - baseModelXy);
  memory.write32(0x1f800000u + 240u, 0u);
  memory.write16(0x8005CA80u, 8u);
  memory.write16(0x8005CA80u + 2048u, 12u);
  ASSERT_EQ(ps1::games::crash3::scenerySelectionCount(&ctx, rawStart), 1);
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&ctx, rawStart + 4u),
            quadHalfOne);
  ctx.pc = 0x800417ACu;
  ps1::games::crash3::captureResidentPresentationAtProjection(ctx);
  const auto deformedSnapshots =
      ps1::games::crash3::residentPresentationSnapshots();
  ASSERT_FALSE(deformedSnapshots.empty());
  const auto &deformed = deformedSnapshots.back();
  ASSERT_TRUE(deformed.gpuScene.has_value());
  EXPECT_TRUE(deformed.gpuScene->deformation.active);
  EXPECT_FALSE(deformed.gpuScene->deformation.distanceCued);
  EXPECT_EQ(deformed.gpuScene->deformation.waveSamples[0], 8);
  EXPECT_EQ(deformed.gpuScene->deformation.waveSamples[16], 12);
  const auto deformedFace = std::find_if(
      deformed.faces.begin(), deformed.faces.end(), [](const auto &face) {
        return std::find(face.sourceVertexIndex.begin(),
                         face.sourceVertexIndex.end(),
                         0u) != face.sourceVertexIndex.end();
      });
  ASSERT_NE(deformedFace, deformed.faces.end());
  const auto deformedCorner =
      std::find(deformedFace->sourceVertexIndex.begin(),
                deformedFace->sourceVertexIndex.end(), 0u);
  ASSERT_NE(deformedCorner, deformedFace->sourceVertexIndex.end());
  const std::size_t deformedCornerIndex = static_cast<std::size_t>(
      deformedCorner - deformedFace->sourceVertexIndex.begin());
  EXPECT_EQ(deformedFace->modelPosition[deformedCornerIndex],
            (std::array<int16_t, 3>{{-32, -40, 412}}));

  // The demo renderer selects the paired wave+distance-cue continuations.
  // They run the same intrinsic phase program even for an undisplaced
  // vertex, retain its original position, and feed the horizontal table
  // sample to GTE DPCS unless raw-XY bit 19 opts out.
  memory.write32(vertexPivot - 4u, rawVertexZero);
  memory.write32(0x1f800000u + 168u, 0x80042A34u);
  memory.write32(0x1f800000u + 172u, 0x80042B48u);
  memory.write16(0x8005CA80u, 2048u);
  ctx.cop2c[ps1::GTE_RFC] = 100u << 4u;
  ctx.cop2c[ps1::GTE_GFC] = 80u << 4u;
  ctx.cop2c[ps1::GTE_BFC] = 60u << 4u;
  ASSERT_EQ(ps1::games::crash3::scenerySelectionCount(&ctx, rawStart), 1);
  ctx.pc = 0x800417ACu;
  ps1::games::crash3::captureResidentPresentationAtProjection(ctx);
  const auto distanceCuedSnapshots =
      ps1::games::crash3::residentPresentationSnapshots();
  ASSERT_FALSE(distanceCuedSnapshots.empty());
  const auto &distanceCued = distanceCuedSnapshots.back();
  const auto distanceCuedFace =
      std::find_if(distanceCued.faces.begin(), distanceCued.faces.end(),
                   [](const auto &face) {
                     return std::find(face.sourceVertexIndex.begin(),
                                      face.sourceVertexIndex.end(),
                                      0u) != face.sourceVertexIndex.end();
                   });
  ASSERT_NE(distanceCuedFace, distanceCued.faces.end());
  const auto distanceCuedCorner =
      std::find(distanceCuedFace->sourceVertexIndex.begin(),
                distanceCuedFace->sourceVertexIndex.end(), 0u);
  ASSERT_NE(distanceCuedCorner, distanceCuedFace->sourceVertexIndex.end());
  const std::size_t distanceCuedCornerIndex = static_cast<std::size_t>(
      distanceCuedCorner - distanceCuedFace->sourceVertexIndex.begin());
  EXPECT_EQ(distanceCuedFace->modelPosition[distanceCuedCornerIndex],
            (std::array<int16_t, 3>{{-40, -40, 400}}));
  EXPECT_EQ(distanceCuedFace->color255[distanceCuedCornerIndex],
            (std::array<float, 3>{{58.0f, 57.0f, 55.0f}}));

  // Bit 18 takes the displacement branch before bit 19 is tested. The same
  // vertex must therefore move while bit 19 still suppresses its DPCS pass.
  memory.write32(vertexPivot - 4u, rawVertexZero | (1u << 18u) | (1u << 19u));
  memory.write16(0x8005CA80u, 8u);
  ASSERT_EQ(ps1::games::crash3::scenerySelectionCount(&ctx, rawStart), 1);
  ctx.pc = 0x800417ACu;
  ps1::games::crash3::captureResidentPresentationAtProjection(ctx);
  const auto flaggedSnapshots =
      ps1::games::crash3::residentPresentationSnapshots();
  ASSERT_FALSE(flaggedSnapshots.empty());
  const auto &flagged = flaggedSnapshots.back();
  const auto flaggedFace = std::find_if(
      flagged.faces.begin(), flagged.faces.end(), [](const auto &face) {
        return std::find(face.sourceVertexIndex.begin(),
                         face.sourceVertexIndex.end(),
                         0u) != face.sourceVertexIndex.end();
      });
  ASSERT_NE(flaggedFace, flagged.faces.end());
  const auto flaggedCorner =
      std::find(flaggedFace->sourceVertexIndex.begin(),
                flaggedFace->sourceVertexIndex.end(), 0u);
  ASSERT_NE(flaggedCorner, flaggedFace->sourceVertexIndex.end());
  const std::size_t flaggedCornerIndex = static_cast<std::size_t>(
      flaggedCorner - flaggedFace->sourceVertexIndex.begin());
  EXPECT_EQ(flaggedFace->modelPosition[flaggedCornerIndex],
            (std::array<int16_t, 3>{{-32, -40, 412}}));
  EXPECT_EQ(flaggedFace->color255[flaggedCornerIndex],
            (std::array<float, 3>{{17.0f, 34.0f, 51.0f}}));
}

TEST_F(Crash3ProfileTest,
       ResidentCohortJournalBridgesRepeatedIntrinsicSectionHandoffs) {
  setenv("PS1_CRASH3_VULKAN_RESIDENT_MODE", "debug", 1);
  ps1::games::crash3::configure({.enabled = true, .widescreen16x9 = true});
  ps1::Memory memory;
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &memory;

  constexpr uint32_t owner = 0x80010000u;
  constexpr uint32_t table = 0x80011000u;
  constexpr uint32_t rawList = 0x80012000u;
  constexpr uint32_t resourcePairBase = 0x80013000u;
  constexpr uint32_t infoBase = 0x80014030u;
  constexpr uint32_t geometryBase = 0x80018000u;
  constexpr uint32_t colorBase = 0x8001C000u;
  constexpr std::array<uint32_t, 4> resourceIds{
      {0x101u, 0x103u, 0x105u, 0x107u}};

  memory.write32(0x80065EBCu, owner);
  memory.write32(owner + 0x10u, table);
  memory.write16(rawList, 1u);
  memory.write16(rawList + 4u, 0u);
  ctx.cop2c[ps1::GTE_RT11RT12] = 4096u;
  ctx.cop2c[ps1::GTE_RT22RT23] = 4096u;
  ctx.cop2c[ps1::GTE_RT33] = 4096u;
  ctx.cop2c[ps1::GTE_H] = 256u;
  ctx.cop2c[ps1::GTE_OFX] = 256u << 16u;
  ctx.cop2c[ps1::GTE_OFY] = 128u << 16u;

  const auto installResource = [&](uint32_t world, uint32_t resource) {
    const uint32_t record = table + 4u + world * 0x30u;
    const uint32_t pair = resourcePairBase + resource * 0x10u;
    const uint32_t info = infoBase + resource * 0x100u;
    const uint32_t entry = info - 0x30u;
    const int32_t originX = static_cast<int32_t>(resource * 400u);
    const int32_t translationX = originX / 4;
    memory.write32(record, pair);
    memory.write32(pair, entry);
    memory.write32(pair + 4u, resourceIds[resource]);
    memory.write32(record + 4u, static_cast<uint32_t>(translationX));
    memory.write32(record + 8u, 0u);
    memory.write32(record + 0x0cu, 0u);
    memory.write32(record + 0x10u, info);
    memory.write32(info, static_cast<uint32_t>(originX));
    memory.write32(info + 4u, 0u);
    memory.write32(info + 8u, 0u);
    memory.write32(info + 0x0cu, 0u);
    memory.write32(info + 0x10u, 3u);
    memory.write32(info + 0x14u, 1u);
    memory.write32(info + 0x18u, 0u);

    const uint32_t trianglePivot = geometryBase + resource * 0x400u + 0x40u;
    const uint32_t quadStart = geometryBase + resource * 0x400u + 0x80u;
    const uint32_t vertexPivot = geometryBase + resource * 0x400u + 0xC0u;
    const uint32_t colors = colorBase + resource * 0x1000u;
    const uint32_t packedTranslation = static_cast<uint16_t>(translationX);
    const uint32_t scratch = 0x1f800280u + world * 0x20u;
    memory.write32(scratch, packedTranslation);
    memory.write32(scratch + 4u, 0u);
    memory.write32(scratch + 8u, trianglePivot);
    memory.write32(scratch + 0x0cu, quadStart);
    memory.write32(scratch + 0x10u, vertexPivot);
    memory.write32(scratch + 0x14u, colors);
    memory.write32(scratch + 0x18u, 0u);
    memory.write32(scratch + 0x1cu, 0u);

    memory.write32(trianglePivot - 4u, (0u << 8u) | (1u << 20u));
    memory.write16(trianglePivot, 2u << 4u);
    for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
      const uint32_t x = 16u + vertex * 8u;
      const uint32_t y = 16u + (vertex == 2u ? 8u : 0u);
      memory.write32(vertexPivot - (vertex + 1u) * 4u,
                     ((y << 4u) << 16u) | (x << 4u));
      memory.write16(vertexPivot + vertex * 2u, 100u << 4u);
    }
    memory.write32(colors, 0x004080C0u);
  };
  const auto installSet = [&](std::initializer_list<uint32_t> resources) {
    memory.write32(table, static_cast<uint32_t>(resources.size()));
    uint32_t world = 0u;
    for (const uint32_t resource : resources)
      installResource(world++, resource);
  };
  const auto capture =
      [&]() -> ps1::games::crash3::ResidentPresentationSnapshot {
    EXPECT_EQ(ps1::games::crash3::scenerySelectionCount(&ctx, rawList), 1);
    ctx.pc = 0x800417ACu;
    ps1::games::crash3::captureResidentPresentationAtProjection(ctx);
    const auto snapshots = ps1::games::crash3::residentPresentationSnapshots();
    EXPECT_FALSE(snapshots.empty());
    if (snapshots.empty())
      return {};
    return snapshots.back();
  };

  // A cold restore has no committed host cohort. One unselected native row
  // can still contain an older camera translation. A raw scene cannot pass
  // its whole-table placement certificate, so compaction must preserve the
  // precise Vulkan endpoint instead of silently falling back to guest draws.
  installSet({0u, 1u});
  memory.write32(table + 4u + 0x30u + 4u, 101u);
  memory.write32(0x1f800280u + 0x20u, 101u);
  setenv("PS1_CRASH3_VULKAN_GPU_COMPACT_OPAQUE", "1", 1);
  auto snapshot = capture();
  ASSERT_TRUE(snapshot.gpuScene.has_value());
  EXPECT_FALSE(snapshot.placementValidated);
  EXPECT_FALSE(snapshot.gpuScene->completeForPresentation);
  EXPECT_EQ(snapshot.gpuScene->gpuRawOnlyFaceCount, 0u);
  ASSERT_EQ(snapshot.faces.size(), 2u);
  for (const auto &face : snapshot.faces) {
    EXPECT_FALSE(face.gpuRawOnly);
    for (const auto &vertex : face.vertices)
      EXPECT_GT(vertex.projectiveW, 0.0);
  }
  unsetenv("PS1_CRASH3_VULKAN_GPU_COMPACT_OPAQUE");
  ps1::games::crash3::configure({.enabled = true, .widescreen16x9 = true});

  installSet({0u});
  snapshot = capture();
  EXPECT_TRUE(snapshot.cohortCandidate);
  EXPECT_EQ(snapshot.candidateObservations, 1u);
  snapshot = capture();
  EXPECT_TRUE(snapshot.cohortCommitted);
  EXPECT_EQ(snapshot.activeCohortGeneration, 1u);
  ASSERT_TRUE(snapshot.gpuScene.has_value());
  EXPECT_TRUE(snapshot.gpuScene->completeForPresentation);

  // A resource key is content-addressed only if its EID is unique in the
  // current valid rows. Two independently decodable rows which claim one EID
  // must not enter the per-resource cache under different geometry.
  installSet({0u, 1u, 2u});
  memory.write32(resourcePairBase + 2u * 0x10u + 4u, resourceIds[1]);
  snapshot = capture();
  EXPECT_FALSE(snapshot.resourceSetValidated);
  ASSERT_TRUE(snapshot.gpuScene.has_value());
  EXPECT_FALSE(snapshot.gpuScene->completeForPresentation);
  EXPECT_EQ(snapshot.individuallyValidatedResources, 1u);
  EXPECT_EQ(snapshot.sceneCacheResourceCount, 1u);
  ASSERT_EQ(snapshot.resources.size(), 3u);
  EXPECT_FALSE(snapshot.resources[1].individuallyValidated);
  EXPECT_FALSE(snapshot.resources[2].individuallyValidated);

  // A table rewrite is not one atomic memcpy: valid incoming rows can become
  // complete while a different row is still transient. Decode and debounce B
  // independently, without advancing the complete A cohort transaction.
  installSet({0u, 1u, 2u});
  memory.write32(table + 4u + 2u * 0x30u + 0x10u, 0u);
  snapshot = capture();
  EXPECT_FALSE(snapshot.resourceSetValidated);
  EXPECT_EQ(snapshot.individuallyValidatedResources, 2u);
  EXPECT_EQ(snapshot.individuallyAdmittedResources, 0u);
  EXPECT_EQ(snapshot.sceneCacheResourceCount, 1u);
  EXPECT_EQ(snapshot.faces.size(), 1u);
  // The complete cohort stays atomic, but a valid active row must still
  // refresh its evolving decoded payload while an unrelated row is invalid.
  memory.write32(colorBase, 0x00112233u);
  snapshot = capture();
  EXPECT_FALSE(snapshot.resourceSetValidated);
  EXPECT_EQ(snapshot.individuallyAdmittedResources, 1u);
  EXPECT_EQ(snapshot.sceneCacheResourceCount, 2u);
  EXPECT_EQ(snapshot.faces.size(), 2u);
  ASSERT_EQ(snapshot.resources.size(), 3u);
  EXPECT_TRUE(snapshot.resources[1].sceneCacheMember);
  const auto refreshedActive = std::find_if(
      snapshot.faces.begin(), snapshot.faces.end(),
      [&](const auto &face) { return face.resourceId == resourceIds[0]; });
  ASSERT_NE(refreshedActive, snapshot.faces.end());
  EXPECT_EQ(refreshedActive->color255[0],
            (std::array<float, 3>{{51.0f, 34.0f, 17.0f}}));

  // Admission is symmetric and bounded. Replace B with coherent C while a
  // third row remains uncertain: one absent observation preserves B, the
  // second revokes B exactly when C earns its second positive observation.
  installSet({0u, 2u, 3u});
  memory.write32(table + 4u + 2u * 0x30u + 0x10u, 0u);
  snapshot = capture();
  EXPECT_EQ(snapshot.individuallyAdmittedResources, 0u);
  EXPECT_EQ(snapshot.individuallyRevokedResources, 0u);
  EXPECT_EQ(snapshot.sceneCacheResourceCount, 2u);
  EXPECT_EQ(snapshot.faces.size(), 2u);
  snapshot = capture();
  EXPECT_EQ(snapshot.individuallyAdmittedResources, 1u);
  EXPECT_EQ(snapshot.individuallyRevokedResources, 1u);
  EXPECT_EQ(snapshot.sceneCacheResourceCount, 2u);
  EXPECT_EQ(snapshot.faces.size(), 2u);
  ASSERT_EQ(snapshot.resources.size(), 3u);
  EXPECT_TRUE(snapshot.resources[1].sceneCacheMember);

  // Return to the original stable cohort and let the speculative C row leave
  // through the same two-observation rule. No route/frame selector is needed.
  installSet({0u});
  snapshot = capture();
  EXPECT_EQ(snapshot.sceneCacheResourceCount, 2u);
  snapshot = capture();
  EXPECT_EQ(snapshot.individuallyRevokedResources, 1u);
  EXPECT_EQ(snapshot.sceneCacheResourceCount, 1u);

  // A partially rewritten second placement is not a candidate at all. The
  // complete active A cohort is reconstructed and the invalid B row cannot
  // advance debounce state.
  installSet({0u, 1u});
  memory.write32(table + 4u + 0x30u + 4u, 101u);
  memory.write32(0x1f800280u + 0x20u, 101u);
  snapshot = capture();
  EXPECT_FALSE(snapshot.resourceSetValidated);
  EXPECT_FALSE(snapshot.cohortCandidate);
  EXPECT_TRUE(snapshot.activeCohortReprojected);
  EXPECT_TRUE(snapshot.cohortPresentationValid);
  EXPECT_EQ(snapshot.reprojectedActiveResourceCount, 1u);
  EXPECT_EQ(snapshot.faces.size(), 1u);

  // Superset handoff: present B immediately while A remains the active
  // overlap. No resource needs retention because the candidate contains A.
  installSet({0u, 1u});
  snapshot = capture();
  EXPECT_TRUE(snapshot.cohortCandidate);
  EXPECT_TRUE(snapshot.candidateCohortPresented);
  EXPECT_EQ(snapshot.sharedActiveResourceCount, 1u);
  EXPECT_EQ(snapshot.retainedResourceCount, 0u);
  EXPECT_EQ(snapshot.faces.size(), 2u);
  snapshot = capture();
  EXPECT_TRUE(snapshot.cohortCommitted);
  EXPECT_EQ(snapshot.activeCohortGeneration, 2u);

  // Losing one row's shared placement is ordinary guest-side culling, not a
  // reason to abandon the certified raw scene. The remaining valid row
  // witnesses the camera offset; Vulkan can therefore consume both immutable
  // payloads while the CPU oracle atomically reprojects the same active set.
  memory.write32(table + 4u + 0x30u + 4u, 101u);
  memory.write32(0x1f800280u + 0x20u, 101u);
  snapshot = capture();
  EXPECT_FALSE(snapshot.resourceSetValidated);
  EXPECT_TRUE(snapshot.activeCohortReprojected);
  EXPECT_TRUE(snapshot.cohortPresentationValid);
  EXPECT_FALSE(snapshot.resourceIdentityConflict);
  EXPECT_EQ(snapshot.retainedResourceCount, 0u);
  EXPECT_EQ(snapshot.sceneCacheResourceCount, 2u);
  EXPECT_EQ(snapshot.resourceSetFingerprint, snapshot.activeCohortFingerprint);
  EXPECT_EQ(snapshot.faces.size(), 2u);
  ASSERT_TRUE(snapshot.gpuScene.has_value());
  EXPECT_TRUE(snapshot.gpuScene->certifiedCohortRecovery);
  EXPECT_TRUE(snapshot.gpuScene->completeForPresentation);
  ASSERT_EQ(snapshot.gpuScene->resources.size(), 2u);
  EXPECT_EQ(snapshot.gpuScene->resources[1].translationXy, 100u);
  EXPECT_EQ(snapshot.gpuScene->rawFaceCount, 2u);
  EXPECT_EQ(snapshot.gpuScene->presentationFaces.size(), 2u);
  EXPECT_EQ(
      std::count_if(snapshot.faces.begin(), snapshot.faces.end(),
                    [](const auto &face) { return face.retainedResource; }),
      1);
  EXPECT_TRUE(std::all_of(snapshot.faces.begin(), snapshot.faces.end(),
                          [](const auto &face) { return !face.gpuRawOnly; }));

  // Repacking the same resources in a different world-table order is not a
  // section transition. Stable identity is the sorted exact resource set,
  // while the fresh cache still adopts the new per-capture world ordinals.
  installSet({1u, 0u});
  snapshot = capture();
  EXPECT_FALSE(snapshot.cohortCandidate);
  EXPECT_FALSE(snapshot.cohortCommitted);
  EXPECT_EQ(snapshot.sharedActiveResourceCount, 2u);
  EXPECT_EQ(snapshot.activeCohortGeneration, 2u);
  EXPECT_EQ(snapshot.faces.size(), 2u);

  // Sliding handoff: B is intrinsic overlap, C is incoming and A is the sole
  // outgoing resource. The candidate frame and committed frame both contain
  // the complete A+B+C bridge without a section-specific identifier.
  setenv("PS1_CRASH3_VULKAN_GPU_COMPACT_OPAQUE", "1", 1);
  installSet({1u, 2u});
  snapshot = capture();
  EXPECT_TRUE(snapshot.candidateCohortPresented);
  EXPECT_FALSE(snapshot.cohortDisjoint);
  EXPECT_EQ(snapshot.sharedActiveResourceCount, 1u);
  EXPECT_EQ(snapshot.retainedResourceCount, 1u);
  EXPECT_EQ(snapshot.retainedResourceFaces, 1u);
  EXPECT_EQ(snapshot.faces.size(), 3u);
  EXPECT_TRUE(std::none_of(snapshot.faces.begin(), snapshot.faces.end(),
                           [](const auto &face) { return face.gpuRawOnly; }));
  snapshot = capture();
  EXPECT_TRUE(snapshot.cohortCommitted);
  EXPECT_EQ(snapshot.activeCohortGeneration, 3u);
  EXPECT_EQ(snapshot.retainedResourceCount, 1u);
  EXPECT_TRUE(std::none_of(snapshot.faces.begin(), snapshot.faces.end(),
                           [](const auto &face) { return face.gpuRawOnly; }));
  unsetenv("PS1_CRASH3_VULKAN_GPU_COMPACT_OPAQUE");

  // A disjoint scene never inherits the adjacent layer. The same debounce is
  // reused, but the first valid candidate is already presented on its own.
  installSet({3u});
  snapshot = capture();
  EXPECT_TRUE(snapshot.cohortCandidate);
  EXPECT_TRUE(snapshot.cohortDisjoint);
  EXPECT_TRUE(snapshot.candidateCohortPresented);
  EXPECT_EQ(snapshot.retainedResourceCount, 0u);
  EXPECT_EQ(snapshot.faces.size(), 1u);
  snapshot = capture();
  EXPECT_TRUE(snapshot.cohortCommitted);
  EXPECT_TRUE(snapshot.cohortDisjoint);
  EXPECT_EQ(snapshot.activeCohortGeneration, 4u);
  EXPECT_EQ(snapshot.retainedResourceCount, 0u);

  // A cohort made entirely of flags-1 resources has exact origin/4 placement
  // and needs no flags-0 camera-offset witness. Refresh the active cache in
  // that form so the following invalid-table recovery exercises this case.
  memory.write32(infoBase + 3u * 0x100u + 0x0cu, 1u);
  snapshot = capture();
  EXPECT_TRUE(snapshot.resourceSetValidated);
  EXPECT_FALSE(snapshot.cohortCandidate);
  EXPECT_EQ(snapshot.activeCohortGeneration, 4u);
  ASSERT_FALSE(snapshot.faces.empty());
  EXPECT_TRUE(
      std::all_of(snapshot.faces.begin(), snapshot.faces.end(),
                  [](const auto &face) { return face.resourceFlags == 1u; }));

  // Conversely, an EID which was already committed cannot silently acquire
  // different immutable geometry. Treat it as an incomplete/reused page and
  // keep presenting the certified active cache instead of committing a false
  // same-ID transition.
  constexpr uint32_t disjointVertexPivot = geometryBase + 3u * 0x400u + 0xC0u;
  memory.write32(disjointVertexPivot - 4u,
                 memory.read32(disjointVertexPivot - 4u) + 16u);
  snapshot = capture();
  EXPECT_TRUE(snapshot.resourceIdentityConflict);
  EXPECT_FALSE(snapshot.resourceSetValidated);
  EXPECT_FALSE(snapshot.cohortCandidate);
  EXPECT_FALSE(snapshot.cohortCommitted);
  EXPECT_TRUE(snapshot.activeCohortReprojected);
  EXPECT_EQ(snapshot.activeCohortGeneration, 4u);
  EXPECT_EQ(snapshot.reprojectedActiveResourceCount, 1u);
  EXPECT_EQ(snapshot.faces.size(), 1u);
}

TEST_F(Crash3ProfileTest, OversizedTotalSlstBypassFailsClosed) {
  ps1::games::crash3::configure({.enabled = true});
  setenv("PS1_CRASH3_SLST_MODE", "bypass", 1);
  ps1::Memory memory;
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &memory;
  constexpr uint32_t owner = 0x80020000u;
  constexpr uint32_t table = 0x80021000u;
  constexpr uint32_t info = 0x80022000u;
  constexpr uint32_t list = 0x80030000u;
  memory.write32(0x80065EBCu, owner);
  memory.write32(owner + 0x10u, table);
  memory.write32(table, 1u);
  memory.write32(table + 0x14u, info);
  memory.write32(info + 0x14u, 1521u);
  memory.write32(info + 0x18u, 0u);
  memory.write16(list, 1u);
  memory.write16(list + 4u, 0x123u);
  memory.write32(0x1F80009Cu, list + 4u);

  EXPECT_EQ(ps1::games::crash3::scenerySelectionCount(&ctx, list), 1);
  // Change the resident catalog after the checked failure. Entry reads must
  // use the already-resolved fail-closed result rather than rebuilding once
  // per decoder lane (which used to turn this into synthetic triangle 0).
  memory.write32(info + 0x14u, 1u);
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&ctx, list + 4u), 0x123u);
}

TEST_F(Crash3ProfileTest, InvalidResidentCountsFailClosedToOriginalSlst) {
  ps1::games::crash3::configure({.enabled = true});
  setenv("PS1_CRASH3_SLST_MODE", "bypass", 1);
  ps1::Memory memory;
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &memory;
  constexpr uint32_t owner = 0x80020000u;
  constexpr uint32_t table = 0x80021000u;
  constexpr uint32_t info = 0x80022000u;
  constexpr uint32_t list = 0x80030000u;
  memory.write32(0x80065EBCu, owner);
  memory.write32(owner + 0x10u, table);
  memory.write32(table, 1u);
  memory.write32(table + 0x14u, info);
  memory.write32(info + 0x14u, 0x801u);
  memory.write16(list, 1u);
  memory.write16(list + 4u, 0x123u);

  EXPECT_EQ(ps1::games::crash3::scenerySelectionCount(&ctx, list), 1);
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&ctx, list + 4u), 0x123u);
}

TEST_F(Crash3ProfileTest,
       WidescreenRendererReadIsScopedToSxyAndProvenCodeRange) {
  ps1::games::crash3::configure({.enabled = true, .widescreen16x9 = true});
  constexpr uint32_t packed = 0xff800200u;
  constexpr int32_t mainOfx = 256 << 16;
  constexpr int32_t offscreenOfx = 32 << 16;

  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x80041838u, ps1::GTE_SXY0, packed, mainOfx),
            0xff8001c0u);
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x80041CCCu, ps1::GTE_SXYP, packed, mainOfx),
            0xff8001c0u);
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x80041834u, ps1::GTE_SXY0, packed, mainOfx),
            packed);
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x80041CD0u, ps1::GTE_SXY0, packed, mainOfx),
            packed);
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x8003CECCu, ps1::GTE_SXY0, packed, mainOfx),
            0xff8001c0u);
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x8003CED4u, ps1::GTE_SXY2, packed, mainOfx),
            0xff8001c0u);
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x8003DF14u, ps1::GTE_SXY0, packed, mainOfx),
            0xff8001c0u);
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x8003DF1Cu, ps1::GTE_SXY2, packed, mainOfx),
            0xff8001c0u);
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x8004367Cu, ps1::GTE_SXY2, packed, mainOfx),
            0xff8001c0u);
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x8003CECCu, ps1::GTE_SXY0, packed, offscreenOfx),
            packed);
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x8003CEC8u, ps1::GTE_SXY0, packed, mainOfx),
            packed);
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x8003CED8u, ps1::GTE_SXY0, packed, mainOfx),
            packed);
  EXPECT_EQ(ps1::games::crash3::rendererPresentationSxy(
                0x80041838u, ps1::GTE_SZ3, packed, mainOfx),
            packed);
}

TEST_F(Crash3ProfileTest, UsesExactThreeQuarterProjectionAndPageCentred2D) {
  ps1::games::crash3::configure({.enabled = true, .widescreen16x9 = true});
  EXPECT_EQ(ps1::games::crash3::horizontalProjectionScale(), 0.75);
  EXPECT_EQ(ps1::games::crash3::screen2DPresentationX(0, 0, 511, 0), 64);
  EXPECT_EQ(ps1::games::crash3::screen2DPresentationX(512, 0, 511, 0), 448);
  EXPECT_EQ(ps1::games::crash3::screen2DPresentationX(512, 512, 1023, 512),
            576);
  EXPECT_EQ(ps1::games::crash3::screen2DPresentationX(1024, 512, 1023, 512),
            960);
}

TEST_F(Crash3ProfileTest, StableProjectionCallsiteIgnoresLiveRendererState) {
  ps1::games::crash3::configure({.enabled = true});
  ps1::CPUContext ctx{};
  ctx.reset();
  ctx.pc = 0x80040F94u;
  ctx.r[4] = 5u;
  ctx.r[29] = 0x80070380u;
  ps1::games::crash3::observeGteControlWrite(ctx, 5u);
  ctx.pc = 0x8003CFB8u;
  ctx.r[3] = 0x1F800000u;
  for (uint8_t reg = 16; reg <= 22; ++reg)
    ctx.r[reg] = 0x1000u * reg;

  ps1::PGXPValue first;
  ps1::games::crash3::attachProjectionTag(ctx, 2u, first);
  ASSERT_NE(first.gameProfileCallsite, 0u);
  const uint64_t firstDynamicTag = first.modelVertexSource;

  ctx.r[3] += 0x80u;
  ctx.r[18] ^= 0x1234u;
  ctx.r[31] = 0xF518F518u;
  ps1::PGXPValue second;
  ps1::games::crash3::attachProjectionTag(ctx, 2u, second);

  // Restore only the renderer scratch-base witness. Live working registers,
  // including the assembly routine's repurposed $ra, do not affect the key.
  ctx.r[3] = 0x1F800000u;
  ps1::PGXPValue third;
  ps1::games::crash3::attachProjectionTag(ctx, 2u, third);
  EXPECT_EQ(third.gameProfileCallsite, first.gameProfileCallsite);
  EXPECT_NE(second.modelVertexSource, firstDynamicTag);
  EXPECT_EQ(
      ps1::games::crash3::projectionCallsiteGtePc(first.gameProfileCallsite),
      0x8003CFB8u);
  EXPECT_EQ(ps1::games::crash3::projectionCallsiteScaleShift(
                first.gameProfileCallsite),
            5);
  EXPECT_EQ(ps1::games::crash3::projectionCallsiteVectorIndex(
                first.gameProfileCallsite),
            2u);
  EXPECT_EQ(ps1::games::crash3::projectionCallsiteObjectSource(
                first.gameProfileCallsite),
            0x80070380u);
  EXPECT_EQ(ps1::games::crash3::projectionCallsiteObjectCapture(
                first.gameProfileCallsite),
            0u);

  ctx.pc = 0x80040F94u;
  ctx.r[4] = 6u;
  ps1::games::crash3::observeGteControlWrite(ctx, 5u);
  ctx.pc = 0x8003CFB8u;
  ps1::PGXPValue differentScale;
  ps1::games::crash3::attachProjectionTag(ctx, 2u, differentScale);
  EXPECT_NE(differentScale.gameProfileCallsite, first.gameProfileCallsite);
  EXPECT_EQ(ps1::games::crash3::projectionCallsiteScaleShift(
                differentScale.gameProfileCallsite),
            6);

  ctx.pc = 0x80040F94u;
  ctx.r[4] = 31u;
  ps1::games::crash3::observeGteControlWrite(ctx, 5u);
  ctx.pc = 0x8003CFB8u;
  ps1::PGXPValue largestLegacyScale;
  ps1::games::crash3::attachProjectionTag(ctx, 0u, largestLegacyScale);
  EXPECT_EQ(ps1::games::crash3::projectionCallsiteScaleShift(
                largestLegacyScale.gameProfileCallsite),
            31);
  EXPECT_EQ(ps1::games::crash3::projectionCallsiteObjectSource(
                largestLegacyScale.gameProfileCallsite),
            0x80070380u);
  EXPECT_EQ(ps1::games::crash3::projectionCallsiteObjectCapture(
                largestLegacyScale.gameProfileCallsite),
            0u);
}

TEST_F(Crash3ProfileTest,
       ProceduralTerrainIdentityTracksCellNotProjectionOrderOrWalker) {
  ps1::games::crash3::configure({.enabled = true});
  ps1::Memory memory;
  ps1::games::crash3::restoreTransientState(memory);
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &memory;
  ctx.pc = 0x80043E00u;
  ctx.r[3] = 0x1f800000u;
  ctx.r[11] = 0x80090000u;
  ctx.r[8] = 100u * 256u;
  ctx.r[10] = 200u * 4u;
  ctx.r[12] = static_cast<uint32_t>(-80);
  ctx.r[13] = static_cast<uint32_t>(-42);
  ctx.cop2d[ps1::GTE_VXY0] = static_cast<uint16_t>(5 * 1024 - 80);
  ctx.cop2d[ps1::GTE_VZ0] = static_cast<uint16_t>(7 * 1024 - 42);
  memory.write32(0x1f800374u, 0x80043D48u);

  ps1::metrics::setTraceFrame(900u);
  ps1::PGXPValue first;
  ps1::games::crash3::attachProjectionTag(ctx, 0u, first);
  ps1::PGXPValue second;
  ps1::games::crash3::attachProjectionTag(ctx, 0u, second);
  ASSERT_TRUE(ps1::games::crash3::proceduralProjectionIdentity(
                  first.gameProfileCallsite, first.modelVertexSource)
                  .has_value());
  EXPECT_FALSE(
      ps1::games::crash3::proceduralProjectionOrdinal(first.modelVertexSource)
          .has_value());
  EXPECT_EQ(
      ps1::games::crash3::proceduralProjectionIdentity(first.modelVertexSource),
      ps1::games::crash3::proceduralProjectionIdentity(
          second.modelVertexSource));

  ps1::metrics::setTraceFrame(901u);
  // The origin advances one cell while the same world point is now visited
  // at local X=4 instead of 5. Neither visitation count nor frame matters.
  ctx.r[8] += 256u;
  ctx.cop2d[ps1::GTE_VXY0] = static_cast<uint16_t>(4 * 1024 - 80);
  ps1::PGXPValue nextFrame;
  ps1::games::crash3::attachProjectionTag(ctx, 0u, nextFrame);
  EXPECT_EQ(ps1::games::crash3::proceduralProjectionIdentity(
                nextFrame.modelVertexSource),
            ps1::games::crash3::proceduralProjectionIdentity(
                first.modelVertexSource));
  memory.write32(0x1f800374u, 0x80043D88u);
  ctx.r[8] = 101u * 4u;
  ctx.r[10] = 200u * 256u;
  ps1::PGXPValue transposed;
  ps1::games::crash3::attachProjectionTag(ctx, 0u, transposed);
  EXPECT_EQ(first.modelVertexSource, transposed.modelVertexSource);
  // Two different world cells can reuse the same 64x64 height-map sample.
  ctx.r[8] += 64u * 4u;
  ps1::PGXPValue repeatedSample;
  ps1::games::crash3::attachProjectionTag(ctx, 0u, repeatedSample);
  EXPECT_NE(first.modelVertexSource, repeatedSample.modelVertexSource);
  ctx.r[8] -= 64u * 4u;
  ctx.r[11] += 0x4000u;
  ps1::PGXPValue otherResource;
  ps1::games::crash3::attachProjectionTag(ctx, 0u, otherResource);
  EXPECT_NE(first.modelVertexSource, otherResource.modelVertexSource);
  EXPECT_EQ(ps1::games::crash3::proceduralProjectionIdentity(
                first.gameProfileCallsite, first.modelVertexSource),
            ps1::games::crash3::proceduralProjectionIdentity(
                first.modelVertexSource));

  // The generic tag space predates the structured schemas. A coincidental
  // payload prefix must not turn an unrelated renderer into a procedural
  // source, nor may one procedural renderer consume the other's schema.
  ps1::CPUContext unrelated = ctx;
  unrelated.pc = 0x8003CFB8u;
  ps1::PGXPValue unrelatedCallsite;
  ps1::games::crash3::attachProjectionTag(unrelated, 0u, unrelatedCallsite);
  EXPECT_FALSE(
      ps1::games::crash3::proceduralProjectionIdentity(
          unrelatedCallsite.gameProfileCallsite, first.modelVertexSource)
          .has_value());

  ps1::CPUContext water = ctx;
  water.pc = 0x80044938u;
  ps1::PGXPValue waterCallsite;
  ps1::games::crash3::attachProjectionTag(water, 0u, waterCallsite);
  EXPECT_FALSE(ps1::games::crash3::proceduralProjectionIdentity(
                   waterCallsite.gameProfileCallsite, first.modelVertexSource)
                   .has_value());
}

TEST_F(Crash3ProfileTest,
       ProceduralTerrainIdentityRejectsInvalidConstructionAndLegacyOrdinal) {
  ps1::games::crash3::configure({.enabled = true});
  ps1::Memory memory;
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &memory;
  ctx.pc = 0x80043E00u;
  ctx.r[3] = 0x1f800000u;
  ctx.r[11] = 0x80090000u;
  memory.write32(0x1f800374u, 0x80043D48u);
  const auto identity = [&] {
    ps1::PGXPValue projection;
    ps1::games::crash3::attachProjectionTag(ctx, 0u, projection);
    return ps1::games::crash3::proceduralProjectionIdentity(
        projection.gameProfileCallsite, projection.modelVertexSource);
  };
  ASSERT_TRUE(identity().has_value());
  ctx.cop2d[ps1::GTE_VXY0] = 1u;
  EXPECT_FALSE(identity().has_value());
  ctx.cop2d[ps1::GTE_VXY0] = 0u;
  memory.write32(0x1f800374u, 0u);
  EXPECT_FALSE(identity().has_value());
  const uint64_t oldTag = UINT64_C(0xC3A3000000000000) |
                          (UINT64_C(0x21) << 42u) | (UINT64_C(1) << 38u) | 1u;
  const uint64_t callsite = UINT64_C(0x43E00) << 8u;
  EXPECT_EQ(ps1::games::crash3::proceduralProjectionOrdinal(oldTag), 0u);
  EXPECT_FALSE(
      ps1::games::crash3::proceduralProjectionIdentity(callsite, oldTag)
          .has_value());
}

TEST_F(Crash3ProfileTest,
       ProceduralWaterIdentityTracksIntrinsicCellAcrossMovingGridOrigin) {
  ps1::games::crash3::configure({.enabled = true});
  ps1::CPUContext ctx{};
  ctx.reset();
  ctx.pc = 0x80044938u;
  ctx.r[3] = 0x80070000u;
  ctx.r[8] = 100u * 256u;
  ctx.r[9] = 200u;
  ctx.r[13] = static_cast<uint32_t>(-80);
  ctx.r[14] = static_cast<uint32_t>(-42);
  ctx.r[22] = ctx.r[3] + 7u * 4u;
  ctx.r[31] = 0x80044A00u;
  ctx.cop2d[ps1::GTE_VXY0] = static_cast<uint16_t>(5 * 256 - 80);
  ctx.cop2d[ps1::GTE_VZ0] = static_cast<uint16_t>(7 * 256 - 42);

  ps1::PGXPValue first;
  ps1::games::crash3::attachProjectionTag(ctx, 0u, first);
  const auto firstIdentity =
      ps1::games::crash3::proceduralProjectionIdentity(first.modelVertexSource);
  ASSERT_TRUE(firstIdentity.has_value());
  EXPECT_FALSE(
      ps1::games::crash3::proceduralProjectionOrdinal(first.modelVertexSource)
          .has_value());

  // The camera crossed one grid cell on both axes. Crash shifts the integral
  // origin and supplies the same world point through the adjacent local slot.
  ctx.r[8] = 101u * 256u;
  ctx.r[9] = 201u;
  ctx.r[13] = static_cast<uint32_t>(-122);
  ctx.r[14] = static_cast<uint32_t>(-67);
  ctx.r[22] = ctx.r[3] + 6u * 4u;
  ctx.cop2d[ps1::GTE_VXY0] = static_cast<uint16_t>(4 * 256 - 122);
  ctx.cop2d[ps1::GTE_VZ0] = static_cast<uint16_t>(6 * 256 - 67);
  ps1::PGXPValue shifted;
  ps1::games::crash3::attachProjectionTag(ctx, 0u, shifted);
  EXPECT_EQ(ps1::games::crash3::proceduralProjectionIdentity(
                shifted.modelVertexSource),
            firstIdentity);

  ctx.cop2d[ps1::GTE_VXY0] = static_cast<uint16_t>(5 * 256 - 122);
  ps1::PGXPValue adjacentCell;
  ps1::games::crash3::attachProjectionTag(ctx, 0u, adjacentCell);
  EXPECT_NE(ps1::games::crash3::proceduralProjectionIdentity(
                adjacentCell.modelVertexSource),
            firstIdentity);
}

TEST_F(Crash3ProfileTest,
       ProceduralWaterTransposedWalkerCanonicalizesToSameCell) {
  ps1::games::crash3::configure({.enabled = true});
  ps1::CPUContext regular{};
  regular.reset();
  regular.pc = 0x80044938u;
  regular.r[3] = 0x80070000u;
  regular.r[8] = 100u * 256u;
  regular.r[9] = 200u;
  regular.r[13] = static_cast<uint32_t>(-80);
  regular.r[14] = static_cast<uint32_t>(-42);
  regular.r[22] = regular.r[3] + 7u * 4u;
  regular.r[31] = 0x80044A00u;
  regular.cop2d[ps1::GTE_VXY0] = static_cast<uint16_t>(5 * 256 - 80);
  regular.cop2d[ps1::GTE_VZ0] = static_cast<uint16_t>(7 * 256 - 42);
  ps1::PGXPValue first;
  ps1::games::crash3::attachProjectionTag(regular, 0u, first);

  ps1::CPUContext transposed = regular;
  transposed.r[8] = 100u;
  transposed.r[9] = 200u * 256u;
  transposed.r[13] = static_cast<uint32_t>(-42);
  transposed.r[14] = static_cast<uint32_t>(-80);
  transposed.r[22] = transposed.r[3] + 5u * 4u;
  transposed.r[31] = 0x80044AA0u;
  transposed.cop2d[ps1::GTE_VXY0] = static_cast<uint16_t>(5 * 256 - 42);
  transposed.cop2d[ps1::GTE_VZ0] = static_cast<uint16_t>(7 * 256 - 80);
  ps1::PGXPValue second;
  ps1::games::crash3::attachProjectionTag(transposed, 0u, second);

  ASSERT_TRUE(
      ps1::games::crash3::proceduralProjectionIdentity(first.modelVertexSource)
          .has_value());
  EXPECT_EQ(ps1::games::crash3::proceduralProjectionIdentity(
                second.modelVertexSource),
            ps1::games::crash3::proceduralProjectionIdentity(
                first.modelVertexSource));
}

TEST_F(Crash3ProfileTest,
       ProceduralWaterIdentityResolvesDiagonalFromGridAlignment) {
  ps1::games::crash3::configure({.enabled = true});
  ps1::CPUContext regular{};
  regular.reset();
  regular.pc = 0x80044938u;
  regular.r[3] = 0x1F800000u;
  regular.r[8] = 3u * 256u;
  regular.r[9] = 5u;
  regular.r[13] = static_cast<uint32_t>(-6274);
  regular.r[14] = static_cast<uint32_t>(-6319);
  regular.r[22] = regular.r[3] + 32u * 4u;
  // Both model components satisfy the cache-slot equation. The fixed-point
  // integral origin identifies this as the regular walker without consulting
  // a caller PC (the live game has six such callers).
  regular.cop2d[ps1::GTE_VXY0] = static_cast<uint16_t>(32 * 256 - 6274);
  regular.cop2d[ps1::GTE_VZ0] = static_cast<uint16_t>(32 * 256 - 6319);
  regular.r[31] = 0x80044740u;
  ps1::PGXPValue regularProjection;
  ps1::games::crash3::attachProjectionTag(regular, 0u, regularProjection);
  ASSERT_TRUE(ps1::games::crash3::proceduralProjectionIdentity(
                  regularProjection.modelVertexSource)
                  .has_value());

  ps1::CPUContext transposed = regular;
  transposed.r[8] = 5u;
  transposed.r[9] = 3u * 256u;
  ps1::PGXPValue transposedProjection;
  ps1::games::crash3::attachProjectionTag(transposed, 0u, transposedProjection);
  ASSERT_TRUE(ps1::games::crash3::proceduralProjectionIdentity(
                  transposedProjection.modelVertexSource)
                  .has_value());
}

TEST_F(Crash3ProfileTest,
       ProceduralWaterIdentityRejectsGenuinelyAmbiguousDiagonal) {
  ps1::games::crash3::configure({.enabled = true});
  ps1::CPUContext ctx{};
  ctx.reset();
  ctx.pc = 0x80044938u;
  ctx.r[3] = 0x1F800000u;
  ctx.r[8] = 3u * 256u;
  ctx.r[9] = 5u * 256u;
  ctx.r[13] = static_cast<uint32_t>(-80);
  ctx.r[14] = static_cast<uint32_t>(-42);
  ctx.r[22] = ctx.r[3] + 7u * 4u;
  ctx.cop2d[ps1::GTE_VXY0] = static_cast<uint16_t>(7 * 256 - 80);
  ctx.cop2d[ps1::GTE_VZ0] = static_cast<uint16_t>(7 * 256 - 42);

  ps1::PGXPValue projection;
  ps1::games::crash3::attachProjectionTag(ctx, 0u, projection);
  EXPECT_FALSE(ps1::games::crash3::proceduralProjectionIdentity(
                   projection.modelVertexSource)
                   .has_value());
}

TEST_F(Crash3ProfileTest,
       CapturesCrash3ObjectsBeforePresentationAndBindsExactRendererNodes) {
  setenv("PS1_CRASH3_VULKAN_OBJECT_MODE", "audit", 1);
  ps1::games::crash3::configure({.enabled = true});
  ASSERT_EQ(ps1::games::crash3::objectPresentationMode(),
            ps1::games::crash3::ObjectPresentationMode::Audit);

  ps1::Memory memory;
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &memory;
  constexpr uint32_t objectAddress = 0x80030000u;
  constexpr uint32_t animationSequence = 0x80034000u;
  constexpr uint32_t rendererNode = 0x80035000u;
  constexpr uint32_t objectCommandList = 0x80036000u;
  memory.write32(objectAddress + 0x00u, 1u);
  memory.write32(objectAddress + 0x04u, 3u);
  memory.write32(objectAddress + 0x08u, 0x80041000u);
  memory.write32(objectAddress + 0x0cu, 0x80042000u);
  memory.write32(objectAddress + 0x10u, 0x80043000u);
  memory.write32(objectAddress + 0x14u, 0x80044000u);
  for (uint32_t index = 0; index < 8u; ++index)
    memory.write32(objectAddress + 0x20u + index * 4u, 0x100u + index);
  // Crash 3 PAL's exact self field is +0x40, not Crash 1's +0x60.
  memory.write32(objectAddress + 0x40u, objectAddress);
  memory.write32(objectAddress + 0x60u,
                 std::bit_cast<uint32_t>(int32_t{-12345}));
  memory.write32(objectAddress + 0x64u, 23456u);
  memory.write32(objectAddress + 0x68u, 34567u);
  memory.write32(objectAddress + 0x6cu, 0x101u);
  memory.write32(objectAddress + 0x70u, 0x202u);
  memory.write32(objectAddress + 0x74u, 0x303u);
  memory.write32(objectAddress + 0x78u, 4096u);
  memory.write32(objectAddress + 0x7cu, 8192u);
  memory.write32(objectAddress + 0x80u, 12288u);
  memory.write32(objectAddress + 0xa8u, 0x11223344u);
  memory.write32(objectAddress + 0xacu, 0x55667788u);
  memory.write32(objectAddress + 0xb0u, 0x99aabbccu);
  memory.write32(objectAddress + 0xb4u, 0x01230003u);
  memory.write32(objectAddress + 0xb8u, 0x005500aau);
  memory.write32(objectAddress + 0xe8u, animationSequence);
  memory.write32(objectAddress + 0xecu, 0x00000700u);
  memory.write32(objectAddress + 0x100u, 0x00040000u);
  memory.write32(objectAddress + 0x108u, 0x00000019u);
  memory.write32(objectAddress + 0x120u, 0x00000204u);
  memory.write8(animationSequence, 1u);

  ps1::metrics::setTraceFrame(400u);
  ctx.r[4] = objectAddress;
  ps1::games::crash3::observeObjectPrePresentation(&ctx);
  auto snapshots = ps1::games::crash3::objectPresentationSnapshots();
  ASSERT_EQ(snapshots.size(), 1u);
  ASSERT_FALSE(snapshots[0].sealed);
  ASSERT_NE(snapshots[0].captureId, 0u);
  ASSERT_EQ(snapshots[0].objects.size(), 1u);
  const uint32_t captureId = snapshots[0].captureId;
  const auto &captured = snapshots[0].objects[0];
  EXPECT_EQ(captured.address, objectAddress);
  EXPECT_EQ(captured.modelResource, 0x80041000u);
  EXPECT_EQ(captured.globalResource, 0x80042000u);
  EXPECT_EQ(captured.lightingWords[7], 0x107u);
  EXPECT_EQ(captured.translation[0], -12345);
  EXPECT_EQ(captured.translation[2], 34567);
  EXPECT_EQ(captured.rotation, (std::array<int32_t, 3>{0x101, 0x202, 0x303}));
  EXPECT_EQ(captured.scale, (std::array<int32_t, 3>{4096, 8192, 12288}));
  EXPECT_EQ(captured.animationFrame, 0x700u);
  EXPECT_EQ(captured.modelType, 1u);
  EXPECT_TRUE(captured.supportedModelType);
  EXPECT_TRUE(captured.rendererNodes.empty());

  // func_8001B858 writes the exact logical object at node+0x10. The later
  // normalization hook observes that intrinsic relationship without screen,
  // scale, visibility or frame-position matching.
  memory.write32(rendererNode + 0x10u, objectAddress);
  memory.write32(rendererNode + 0x38u, objectCommandList);
  memory.write32(objectCommandList, 0u);
  memory.write32(objectCommandList + 4u, UINT32_MAX);
  ctx.pc = 0x80040F94u;
  ctx.r[4] = 5u;
  ctx.r[29] = rendererNode;
  ctx.r[24] = 0x11112222u;
  ctx.cop2c[ps1::GTE_RT11RT12] = 0x01020304u;
  ps1::games::crash3::observeGteControlWrite(ctx, 5u);
  ctx.pc = 0x80040F98u;
  ctx.r[5] = 0x33334444u;
  ps1::games::crash3::observeGteControlWrite(ctx, 6u);
  ctx.pc = 0x80040FA0u;
  ctx.r[6] = 0x55556666u;
  ps1::games::crash3::observeGteControlWrite(ctx, 7u);
  ctx.pc = 0x8003CFB8u;
  ctx.r[3] = 0x1F800000u;
  ctx.r[18] = objectCommandList;
  ctx.r[22] = 0u;
  ps1::games::crash3::observeObjectRendererStreams(&ctx);
  ctx.r[8] = 0x81234567u;
  ps1::PGXPValue projection;
  projection.projectionId = 0x123456789u;
  ps1::games::crash3::attachProjectionTag(ctx, 2u, projection);
  EXPECT_EQ(ps1::games::crash3::projectionCallsiteObjectSource(
                projection.gameProfileCallsite),
            rendererNode);
  EXPECT_EQ(ps1::games::crash3::projectionCallsiteObjectCapture(
                projection.gameProfileCallsite),
            captureId);
  EXPECT_EQ(ps1::games::crash3::projectionCallsiteScaleShift(
                projection.gameProfileCallsite),
            5);
  EXPECT_EQ(
      ps1::games::crash3::objectProjectionCommand(projection.projectionId),
      0x81234567u);
  EXPECT_EQ(ps1::games::crash3::objectProjectionLogicalObject(
                projection.projectionId),
            objectAddress);
  EXPECT_EQ(
      ps1::games::crash3::objectProjectionCommand(projection.projectionId + 1u),
      std::nullopt);
  EXPECT_EQ(ps1::games::crash3::objectProjectionLogicalObject(
                projection.projectionId + 1u),
            std::nullopt);

  // The shared func_8004103C count hook keeps this identity only when its
  // actual list argument is the current node's exact +0x38 source.
  EXPECT_EQ(ps1::games::crash3::scenerySelectionCount(&ctx, objectCommandList),
            0);
  ps1::PGXPValue sharedObjectProjection;
  ps1::games::crash3::attachProjectionTag(ctx, 1u, sharedObjectProjection);
  EXPECT_EQ(ps1::games::crash3::projectionCallsiteObjectSource(
                sharedObjectProjection.gameProfileCallsite),
            rendererNode);
  EXPECT_EQ(ps1::games::crash3::projectionCallsiteObjectCapture(
                sharedObjectProjection.gameProfileCallsite),
            captureId);

  // The extended schema preserves all six legacy scale bits, including the
  // largest normalization exponent.
  ctx.pc = 0x80040F94u;
  ctx.r[4] = 31u;
  ctx.r[29] = rendererNode;
  ps1::games::crash3::observeGteControlWrite(ctx, 5u);
  ctx.pc = 0x8003CFB8u;
  ctx.r[3] = 0x1F800000u;
  ps1::PGXPValue largestExtendedScale;
  ps1::games::crash3::attachProjectionTag(ctx, 0u, largestExtendedScale);
  EXPECT_EQ(ps1::games::crash3::projectionCallsiteScaleShift(
                largestExtendedScale.gameProfileCallsite),
            31);
  EXPECT_EQ(ps1::games::crash3::projectionCallsiteObjectSource(
                largestExtendedScale.gameProfileCallsite),
            rendererNode);
  EXPECT_EQ(ps1::games::crash3::projectionCallsiteObjectCapture(
                largestExtendedScale.gameProfileCallsite),
            captureId);

  // Complete the second synthetic installation just as the real instruction
  // stream does after every TRX write.
  ctx.pc = 0x80040F98u;
  ctx.r[5] = 0x33334444u;
  ps1::games::crash3::observeGteControlWrite(ctx, ps1::GTE_TRY);
  ctx.pc = 0x80040FA0u;
  ctx.r[6] = 0x55556666u;
  ps1::games::crash3::observeGteControlWrite(ctx, ps1::GTE_TRZ);

  snapshots = ps1::games::crash3::objectPresentationSnapshots();
  ASSERT_EQ(snapshots.size(), 1u);
  EXPECT_EQ(snapshots[0].rendererNodeBindings, 1u);
  EXPECT_EQ(snapshots[0].rendererTransformBindings, 1u);
  EXPECT_TRUE(snapshots[0].rendererListScanAttempted);
  EXPECT_FALSE(snapshots[0].rendererListComplete);
  ASSERT_EQ(snapshots[0].objects[0].rendererNodes.size(), 1u);
  EXPECT_EQ(snapshots[0].objects[0].rendererNodes[0].address, rendererNode);
  EXPECT_EQ(snapshots[0].objects[0].rendererNodes[0].byteCount, 0x70u);
  EXPECT_FALSE(snapshots[0].objects[0].rendererNodes[0].listDiscovered);
  EXPECT_TRUE(snapshots[0].objects[0].rendererNodes[0].directBindingDiscovered);
  EXPECT_TRUE(snapshots[0].objects[0].rendererNodes[0].transformInstalled);
  EXPECT_TRUE(snapshots[0].objects[0].rendererNodes[0].type1StreamsCaptured);
  EXPECT_EQ(snapshots[0].objects[0].rendererNodes[0].type1ColorWordCount, 0u);
  EXPECT_EQ(snapshots[0].objects[0].rendererNodes[0].type1CommandWords,
            (std::vector<uint32_t>{UINT32_MAX}));
  EXPECT_EQ(snapshots[0].objects[0].rendererNodes[0].scaleShift, 31);
  EXPECT_EQ(snapshots[0].objects[0].rendererNodes[0].words[4], objectAddress);
  EXPECT_EQ(snapshots[0]
                .objects[0]
                .rendererNodes[0]
                .gteControlWords[ps1::GTE_RT11RT12],
            0x01020304u);
  EXPECT_EQ(
      snapshots[0].objects[0].rendererNodes[0].gteControlWords[ps1::GTE_TRX],
      0x11112222u);
  EXPECT_EQ(
      snapshots[0].objects[0].rendererNodes[0].gteControlWords[ps1::GTE_TRY],
      0x33334444u);
  EXPECT_EQ(
      snapshots[0].objects[0].rendererNodes[0].gteControlWords[ps1::GTE_TRZ],
      0x55556666u);

  // The next logical frame seals the complete prior catalog while starting a
  // new capture, proving that a reused node cannot inherit temporal identity.
  ps1::metrics::setTraceFrame(401u);
  ctx.r[4] = objectAddress;
  ps1::games::crash3::observeObjectPrePresentation(&ctx);
  snapshots = ps1::games::crash3::objectPresentationSnapshots();
  ASSERT_EQ(snapshots.size(), 2u);
  EXPECT_TRUE(snapshots[0].sealed);
  EXPECT_EQ(snapshots[0].captureId, captureId);
  EXPECT_FALSE(snapshots[1].sealed);
  EXPECT_NE(snapshots[1].captureId, captureId);
}

TEST_F(Crash3ProfileTest,
       CapturesCompleteRendererListBeforeCommonTransformEarlyExit) {
  setenv("PS1_CRASH3_VULKAN_OBJECT_MODE", "audit", 1);
  ps1::games::crash3::configure({.enabled = true});
  ps1::Memory memory;
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &memory;

  constexpr uint32_t rendererOwner = 0x80061BD8u;
  constexpr uint32_t rendererState = 0x80020000u;
  constexpr uint32_t type1Object = 0x80030000u;
  constexpr uint32_t type2Object = 0x80030200u;
  constexpr uint32_t type5Object = 0x80030400u;
  constexpr uint32_t type1Animation = 0x80034000u;
  constexpr uint32_t type2Animation = 0x80034100u;
  constexpr uint32_t type5Animation = 0x80034200u;
  constexpr uint32_t type1Node = 0x80035000u;
  constexpr uint32_t type2Node = 0x80035100u;
  constexpr uint32_t type5Node = 0x80035200u;

  const auto createObject = [&](uint32_t object, uint32_t animation,
                                uint8_t modelType) {
    memory.write32(object, 1u);
    memory.write32(object + 0x08u, 0x80040000u + modelType * 0x100u);
    memory.write32(object + 0x40u, object);
    memory.write32(object + 0x78u, 4096u);
    memory.write32(object + 0x7cu, 4096u);
    memory.write32(object + 0x80u, 4096u);
    memory.write32(object + 0xe8u, animation);
    memory.write8(animation, modelType);
    ctx.r[4] = object;
    ps1::games::crash3::observeObjectPrePresentation(&ctx);
  };

  ps1::metrics::setTraceFrame(450u);
  createObject(type1Object, type1Animation, 1u);
  createObject(type2Object, type2Animation, 2u);
  createObject(type5Object, type5Animation, 5u);

  // func_8001B858 prepends each normal node. The type-2 node deliberately
  // carries a negative word at +0x1c, which makes func_80040E38 take its
  // early non-common-transform path. It must remain present in the host list.
  memory.write32(type2Node + 0x00u, type5Node);
  memory.write32(type2Node + 0x04u, 0x8003D000u);
  memory.write32(type2Node + 0x08u, 0x8003E000u);
  memory.write32(type2Node + 0x0cu, 0x8003F000u);
  memory.write32(type2Node + 0x10u, type2Object);
  memory.write32(type2Node + 0x14u, 0x80040200u);
  memory.write32(type2Node + 0x1cu, 0x80000001u);
  memory.write32(type2Node + 0x20u, 0x12345670u);
  memory.write32(type2Node + 0x24u, 0x80034118u);
  memory.write32(type2Node + 0x28u, 0x80041000u);

  memory.write32(type5Node + 0x00u, type1Node);
  memory.write32(type5Node + 0x04u, 0x8003D100u);
  memory.write32(type5Node + 0x08u, 0x8003E100u);
  memory.write32(type5Node + 0x0cu, 0x8003F100u);
  memory.write32(type5Node + 0x10u, type5Object);

  memory.write32(type1Node + 0x00u, 0u);
  memory.write32(type1Node + 0x04u, 0x8003D200u);
  memory.write32(type1Node + 0x08u, 0x8003E200u);
  memory.write32(type1Node + 0x0cu, 0x8003F200u);
  memory.write32(type1Node + 0x10u, type1Object);

  memory.write32(rendererOwner + 0x0cu, rendererState);
  memory.write32(rendererState + 8336u, type2Node);

  // The first common GTE transform can belong to any later list node. The
  // scanner must start at the immutable global head, not at this current node.
  ctx.pc = 0x80040F94u;
  ctx.r[4] = 4u;
  ctx.r[29] = type1Node;
  ctx.r[24] = 0x01020304u;
  ps1::games::crash3::observeGteControlWrite(ctx, ps1::GTE_TRX);

  // func_80040E38 normalizes the rotation immediately after the translated
  // controls. The sidecar must retain these final words, not the pre-shift
  // matrix copied at TRX time.
  ctx.pc = 0x80040F98u;
  ps1::GTE::writeControl(&ctx, ps1::GTE_TRY, 0x05060708u);
  ctx.pc = 0x80040FA0u;
  ps1::GTE::writeControl(&ctx, ps1::GTE_TRZ, 0x090A0B0Cu);
  constexpr std::array<uint32_t, 5> normalizedRotation{
      0x11112222u, 0x33334444u, 0x55556666u, 0x77778888u, 0x00008001u};
  for (uint8_t reg = ps1::GTE_RT11RT12; reg <= ps1::GTE_RT33; ++reg) {
    ctx.pc = 0x80042120u + static_cast<uint32_t>(reg) * 4u;
    ps1::GTE::writeControl(&ctx, reg, normalizedRotation[reg]);
  }

  // The same leaf writer is reused later for lighting and identity matrices.
  // Once the post-TRZ sequence is complete it must not overwrite this node.
  constexpr std::array<uint32_t, 5> laterUnrelatedRotation{
      0xAAAA0000u, 0xBBBB0001u, 0xCCCC0002u, 0xDDDD0003u, 0x00000004u};
  for (uint8_t reg = ps1::GTE_RT11RT12; reg <= ps1::GTE_RT33; ++reg) {
    ctx.pc = 0x80042120u + static_cast<uint32_t>(reg) * 4u;
    ps1::GTE::writeControl(&ctx, reg, laterUnrelatedRotation[reg]);
  }

  const auto snapshots = ps1::games::crash3::objectPresentationSnapshots();
  ASSERT_EQ(snapshots.size(), 1u);
  const auto &snapshot = snapshots[0];
  EXPECT_TRUE(snapshot.rendererListScanAttempted);
  EXPECT_TRUE(snapshot.rendererListComplete);
  EXPECT_EQ(snapshot.guestRam, nullptr);
  EXPECT_EQ(snapshot.rendererListHead, type2Node);
  EXPECT_EQ(snapshot.rendererListNodes, 3u);
  EXPECT_EQ(snapshot.rendererNodeBindings, 3u);
  EXPECT_EQ(snapshot.rendererTransformBindings, 1u);
  EXPECT_EQ(snapshot.rejectedRendererNodes, 0u);

  const auto findObject = [&](uint32_t address)
      -> const ps1::games::crash3::ObjectPresentationObject & {
    const auto found = std::find_if(
        snapshot.objects.begin(), snapshot.objects.end(),
        [&](const auto &object) { return object.address == address; });
    EXPECT_NE(found, snapshot.objects.end());
    return *found;
  };
  const auto &type2 = findObject(type2Object);
  ASSERT_EQ(type2.rendererNodes.size(), 1u);
  EXPECT_TRUE(type2.rendererNodes[0].listDiscovered);
  EXPECT_FALSE(type2.rendererNodes[0].transformInstalled);
  EXPECT_EQ(type2.rendererNodes[0].byteCount, 0x2cu);
  EXPECT_EQ(type2.rendererNodes[0].words[7], 0x80000001u);
  EXPECT_EQ(type2.rendererNodes[0].words[8], 0x12345670u);
  EXPECT_EQ(type2.rendererNodes[0].words[9], 0x80034118u);

  const auto &type1 = findObject(type1Object);
  ASSERT_EQ(type1.rendererNodes.size(), 1u);
  EXPECT_TRUE(type1.rendererNodes[0].listDiscovered);
  EXPECT_TRUE(type1.rendererNodes[0].transformInstalled);
  EXPECT_EQ(type1.rendererNodes[0].gteControlWords[ps1::GTE_TRX], 0x01020304u);
  EXPECT_EQ(type1.rendererNodes[0].gteControlWords[ps1::GTE_TRY], 0x05060708u);
  EXPECT_EQ(type1.rendererNodes[0].gteControlWords[ps1::GTE_TRZ], 0x090A0B0Cu);
  EXPECT_EQ(type1.rendererNodes[0].gteControlWords[ps1::GTE_RT11RT12],
            normalizedRotation[0]);
  EXPECT_EQ(type1.rendererNodes[0].gteControlWords[ps1::GTE_RT13RT21],
            normalizedRotation[1]);
  EXPECT_EQ(type1.rendererNodes[0].gteControlWords[ps1::GTE_RT22RT23],
            normalizedRotation[2]);
  EXPECT_EQ(type1.rendererNodes[0].gteControlWords[ps1::GTE_RT31RT32],
            normalizedRotation[3]);
  EXPECT_EQ(type1.rendererNodes[0].gteControlWords[ps1::GTE_RT33], 0xffff8001u);
}

TEST_F(Crash3ProfileTest,
       DebugRendererListRetainsImmutableGuestRamBeforePoolReuse) {
  setenv("PS1_CRASH3_VULKAN_OBJECT_MODE", "debug", 1);
  ps1::games::crash3::configure({.enabled = true});
  ps1::Memory memory;
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &memory;

  constexpr uint32_t rendererOwner = 0x80061BD8u;
  constexpr uint32_t rendererState = 0x80020000u;
  constexpr uint32_t objectAddress = 0x80030000u;
  constexpr uint32_t animationAddress = 0x80034000u;
  constexpr uint32_t rendererNode = 0x80035000u;
  constexpr uint32_t streamedSource = 0x80123450u;

  memory.write32(objectAddress, 1u);
  memory.write32(objectAddress + 0x08u, streamedSource);
  memory.write32(objectAddress + 0x40u, objectAddress);
  memory.write32(objectAddress + 0x78u, 4096u);
  memory.write32(objectAddress + 0x7cu, 4096u);
  memory.write32(objectAddress + 0x80u, 4096u);
  memory.write32(objectAddress + 0xe8u, animationAddress);
  memory.write8(animationAddress, 1u);
  memory.write32(streamedSource, 0x89abcdefu);

  ps1::metrics::setTraceFrame(500u);
  ctx.r[4] = objectAddress;
  ps1::games::crash3::observeObjectPrePresentation(&ctx);

  memory.write32(rendererNode + 0x00u, 0u);
  memory.write32(rendererNode + 0x10u, objectAddress);
  memory.write32(rendererOwner + 0x0cu, rendererState);
  memory.write32(rendererState + 8336u, rendererNode);

  ctx.pc = 0x80040F94u;
  ctx.r[4] = 4u;
  ctx.r[29] = rendererNode;
  ctx.r[24] = 0x01020304u;
  ps1::games::crash3::observeGteControlWrite(ctx, ps1::GTE_TRX);

  auto snapshots = ps1::games::crash3::objectPresentationSnapshots();
  ASSERT_EQ(snapshots.size(), 1u);
  ASSERT_NE(snapshots[0].guestRam, nullptr);
  ASSERT_EQ(snapshots[0].guestRam->size(), ps1::Memory::RAM_SIZE);
  const uint32_t sourcePhysical = ps1::Memory::toPhysical(streamedSource);
  uint32_t captured = 0u;
  std::memcpy(&captured, snapshots[0].guestRam->data() + sourcePhysical,
              sizeof(captured));
  EXPECT_EQ(captured, 0x89abcdefu);

  memory.write32(streamedSource, 0x76543210u);
  snapshots = ps1::games::crash3::objectPresentationSnapshots();
  std::memcpy(&captured, snapshots[0].guestRam->data() + sourcePhysical,
              sizeof(captured));
  EXPECT_EQ(captured, 0x89abcdefu);
}

TEST_F(Crash3ProfileTest,
       ObjectPacketBindingUsesCommandCursorAndImmutableCapture) {
  setenv("PS1_CRASH3_VULKAN_OBJECT_MODE", "audit", 1);
  ps1::games::crash3::configure({.enabled = true});
  ps1::Memory memory;
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &memory;
  constexpr uint32_t object = 0x80030000u;
  constexpr uint32_t node = 0x80035000u;
  constexpr uint32_t table = 0x80036000u;
  memory.write32(object, 1u);
  memory.write32(object + 0x40u, object);
  memory.write32(object + 0xe8u, 0x80034000u);
  memory.write8(0x80034000u, 1u);
  memory.write32(node + 0x10u, object);
  memory.write32(node + 0x38u, table);
  memory.write32(node + 0x3cu, 0x80037000u);
  memory.write32(table, 0u);
  for (uint32_t index = 1u; index <= 6u; ++index)
    memory.write32(table + index * 4u, 0x04030000u);
  memory.write32(table + 28u, UINT32_MAX);
  ps1::metrics::setTraceFrame(450u);
  ctx.r4 = object;
  ps1::games::crash3::observeObjectPrePresentation(&ctx);
  ctx.pc = 0x80040F94u;
  ctx.r4 = 0u;
  ctx.r29 = node;
  ps1::games::crash3::observeGteControlWrite(ctx, ps1::GTE_TRX);
  ctx.r3 = 0x1f800000u;
  ps1::games::crash3::observeObjectRendererStreams(&ctx);
  ctx.pc = 0x80040CB4u;
  ctx.r8 = 0x04030000u;
  ctx.r18 = table + 16u;
  ctx.r23 = 0x80040000u;
  ctx.cop2d[ps1::GTE_SXY0] = 0x123456u;
  EXPECT_EQ(ps1::GTE::readData(&ctx, ps1::GTE_SXY0), 0x123456u);
  const auto frozen = ps1::games::crash3::objectPresentationSnapshots();
  ASSERT_EQ(frozen.size(), 1u);
  const auto &bindings = frozen[0].objects[0].rendererNodes[0].packetBindings;
  ASSERT_EQ(bindings.size(), 1u);
  EXPECT_EQ(bindings[0].packetAddress, 0x80040000u);
  EXPECT_EQ(bindings[0].commandIndex, 2u);
  EXPECT_EQ(ctx.cop2d[ps1::GTE_SXY0], 0x123456u);
  ctx.r18 = table + 28u; // identical word, different source face
  ctx.r23 += 40u;
  ps1::GTE::readData(&ctx, ps1::GTE_SXY0);
  ctx.r18 = table + 32u; // terminator is not a face
  ps1::GTE::readData(&ctx, ps1::GTE_SXY0);
  ctx.r18 = table + 27u; // misalignment is not rounded into a face
  ps1::GTE::readData(&ctx, ps1::GTE_SXY0);
  const auto updated = ps1::games::crash3::objectPresentationSnapshots();
  const auto &next = updated[0].objects[0].rendererNodes[0].packetBindings;
  ASSERT_EQ(next.size(), 2u);
  EXPECT_EQ(next[1].commandIndex, 5u);
  EXPECT_EQ(bindings.size(), 1u);
  ps1::games::crash3::restoreTransientState(memory);
  ctx.r18 = table + 16u;
  ps1::GTE::readData(&ctx, ps1::GTE_SXY0);
  EXPECT_TRUE(ps1::games::crash3::objectPresentationSnapshots().empty());
}

TEST_F(Crash3ProfileTest,
       ObjectBoundingPreflightPreparesOwnedPrefixWithoutGuestWrites) {
  using namespace ps1::games::crash3;
  setenv("PS1_CRASH3_VULKAN_OBJECT_MODE", "audit", 1);
  configure({.enabled = true});
  ps1::Memory memory;
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &memory;
  constexpr uint32_t object = 0x80030000u, node = 0x80035000u;
  constexpr uint32_t table = 0x80036000u, colors = 0x80037000u;
  constexpr uint32_t scratch = 0x1f800000u;
  memory.write32(object, 1u);
  memory.write32(object + 0x40u, object);
  memory.write32(object + 0xe8u, 0x80034000u);
  memory.write8(0x80034000u, 1u);
  memory.write32(node + 0x10u, object);
  memory.write32(node + 0x0cu, 0x8003FDB4u);
  memory.write32(node + 0x2cu, 0x80040C1Cu);
  memory.write32(node + 0x38u, table);
  memory.write32(node + 0x3cu, colors);
  memory.write32(table, 2u);
  memory.write32(table + 4u, 0x04000000u);
  memory.write32(table + 8u, 0x04010000u);
  memory.write32(table + 12u, 0x04020000u);
  memory.write32(table + 16u, UINT32_MAX);
  memory.write32(colors, 0x123456u);
  memory.write32(colors + 4u, 0xabcdefu);
  memory.write32(scratch + 320u, 0xdeadbeefu); // previous object's scratch
  memory.write32(scratch + 324u, 0xcafebabeu);
  memory.write32(scratch + 244u, 1904u);
  ps1::metrics::setTraceFrame(450u);
  ctx.r4 = object;
  observeObjectPrePresentation(&ctx);
  ctx.pc = 0x80040F94u;
  ctx.r4 = 0u;
  ctx.r29 = node;
  observeGteControlWrite(ctx, ps1::GTE_TRX);
  ctx.r3 = scratch;
  ctx.pc = 0x8003E094u; // original bounding projection, before polygon dispatch
  const std::vector<uint8_t> nativeBefore(
      memory.ramPtr(), memory.ramPtr() + ps1::Memory::RAM_SIZE);
  ps1::PGXPValue point{};
  attachProjectionTag(ctx, 0u, point);
  const auto frozen = objectPresentationSnapshots();
  ASSERT_EQ(frozen.size(), 1u);
  ASSERT_EQ(frozen[0].objects[0].rendererNodes.size(), 1u);
  const auto &prepared = frozen[0].objects[0].rendererNodes[0];
  EXPECT_TRUE(hasIntrinsicObjectConstruction(prepared));
  EXPECT_EQ(prepared.type1ScratchColorWords,
            (std::vector<uint32_t>{0x123456u, 0xabcdefu}));
  EXPECT_TRUE(prepared.packetBindings.empty());
  EXPECT_EQ(
      std::memcmp(nativeBefore.data(), memory.ramPtr(), nativeBefore.size()),
      0);
  EXPECT_EQ(memory.read32(scratch + 320u), 0xdeadbeefu);
  EXPECT_EQ(memory.read32(scratch + 324u), 0xcafebabeu);
  // If the native stream is reached later, its exact consumed data wins.
  memory.write32(colors, 0x765432u);
  memory.write32(scratch + 320u, 0x987654u);
  observeObjectRendererStreams(&ctx);
  const auto consumed = objectPresentationSnapshots();
  EXPECT_EQ(consumed[0].objects[0].rendererNodes[0].type1ColorWords[0],
            0x765432u);
  EXPECT_EQ(consumed[0].objects[0].rendererNodes[0].type1ScratchColorWords[0],
            0x987654u);
  EXPECT_EQ(prepared.type1ScratchColorWords[0], 0x123456u);
  restoreTransientState(memory);
  EXPECT_TRUE(objectPresentationSnapshots().empty());
}

TEST_F(Crash3ProfileTest,
       ObjectCaptureScopesReusedRendererAddressesToLogicalOwner) {
  using namespace ps1::games::crash3;
  setenv("PS1_CRASH3_VULKAN_OBJECT_MODE", "audit", 1);
  configure({.enabled = true});
  ps1::Memory memory;
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &memory;
  constexpr std::array<uint32_t, 2> objects{0x80030000u, 0x80030800u};
  constexpr uint32_t node = 0x80035000u, scratch = 0x1f800000u;
  ps1::metrics::setTraceFrame(450u);
  for (const uint32_t object : objects) {
    memory.write32(object, 1u);
    memory.write32(object + 0x40u, object);
    memory.write32(object + 0xe8u, 0x80034000u);
    memory.write8(0x80034000u, 1u);
    ctx.r4 = object;
    observeObjectPrePresentation(&ctx);
  }
  memory.write32(node + 0x0cu, 0x8003FDB4u);
  memory.write32(node + 0x2cu, 0x80040C1Cu);
  memory.write32(node + 0x38u, 0x80036000u);
  memory.write32(node + 0x3cu, 0x80037000u);
  memory.write32(0x80036000u, 1u);
  memory.write32(0x80036004u, 0x04030000u);
  memory.write32(0x80036008u, UINT32_MAX);
  memory.write32(0x80037000u, 0x123456u);
  for (uint32_t index = 0; index < objects.size(); ++index) {
    // The pool reuses one address in the same capture. Both logical owners
    // must retain their own consumed controls; address alone is insufficient.
    memory.write32(node + 0x10u, objects[index]);
    ctx.pc = 0x80040F94u;
    ctx.r4 = 1u;
    ctx.r29 = node;
    ctx.r24 = 100u + index;
    observeGteControlWrite(ctx, ps1::GTE_TRX);
    ctx.pc = 0x80040F98u;
    observeGteControlWrite(ctx, ps1::GTE_TRY, 200u + index);
    ctx.cop2c[ps1::GTE_TRY] = 200u + index;
    ctx.pc = 0x80040FA0u;
    observeGteControlWrite(ctx, ps1::GTE_TRZ, 300u + index);
    ctx.cop2c[ps1::GTE_TRZ] = 300u + index;
    for (uint8_t reg = ps1::GTE_RT11RT12; reg <= ps1::GTE_RT33; ++reg) {
      ctx.pc = 0x80042120u + 4u * reg;
      observeGteControlWrite(ctx, reg, 400u + 10u * index + reg);
      ctx.cop2c[reg] = 400u + 10u * index + reg;
    }
    ctx.cop2c[ps1::GTE_RFC] = 500u + index;
    memory.write32(scratch + 244u, 1900u - index);
    memory.write32(scratch + 248u, 1800u - index);
    ctx.r3 = scratch;
    ctx.pc = 0x8003FE64u; // RTPT inside the 8003FDB4 vertex program
    ps1::PGXPValue point{};
    attachProjectionTag(ctx, 0u, point);
    observeObjectRendererStreams(&ctx);
    ctx.pc = 0x80040CB4u;
    ctx.r18 = 0x80036008u;
    ctx.r23 = 0x80040000u + 40u * index;
    ctx.r8 = 0x04030000u;
    observeObjectPacketBinding(ctx);
    ctx.pc = 0x80040C70u;
    ctx.cop2d[ps1::GTE_IR0] = 1000u + index;
    observeObjectDistanceCue(ctx);
  }
  const auto snapshots = objectPresentationSnapshots();
  ASSERT_EQ(snapshots.size(), 1u);
  ASSERT_EQ(snapshots[0].objects.size(), 2u);
  for (uint32_t index = 0; index < objects.size(); ++index) {
    const auto &object = snapshots[0].objects[index];
    ASSERT_EQ(object.address, objects[index]);
    ASSERT_EQ(object.rendererNodes.size(), 1u);
    const auto &captured = object.rendererNodes[0];
    EXPECT_EQ(captured.words[0x10u / 4u], objects[index]);
    EXPECT_EQ(captured.gteControlWords[ps1::GTE_TRY], 200u + index);
    EXPECT_EQ(captured.gteControlWords[ps1::GTE_TRZ], 300u + index);
    EXPECT_EQ(captured.gteControlWords[ps1::GTE_RT11RT12], 400u + 10u * index);
    EXPECT_EQ(captured.gteControlWords[ps1::GTE_RT33], 404u + 10u * index);
    EXPECT_EQ(captured.gteControlWords[ps1::GTE_RFC], 500u + index);
    EXPECT_TRUE(captured.orderingTableBasesCaptured);
    EXPECT_EQ(captured.orderingTableBasePositive, 1900u - index);
    EXPECT_EQ(captured.orderingTableBaseNonPositive, 1800u - index);
    ASSERT_EQ(captured.packetBindings.size(), 1u);
    EXPECT_EQ(captured.packetBindings[0].packetAddress,
              0x80040000u + 40u * index);
    EXPECT_TRUE(captured.distanceCueCaptured);
    EXPECT_TRUE(captured.distanceCueConsistent);
    EXPECT_EQ(captured.distanceCueIr0, 1000u + index);
  }
}

TEST_F(Crash3ProfileTest,
       ObjectPresenterDecodesCompleteUnculledType1TopologyAndMaterial) {
  ps1::Memory memory;
  constexpr uint32_t objectAddress = 0x80030000u;
  constexpr uint32_t nodeAddress = 0x80031000u;
  constexpr uint32_t commandTable = 0x80032000u;
  constexpr uint32_t colorTable = 0x80032100u;
  constexpr uint32_t textureTable = 0x80032200u;
  constexpr uint32_t frame = 0x80033000u;
  constexpr uint32_t modelResource = 0x80034000u;

  memory.write32(commandTable, 1u);
  // One independent three-vertex strip. Stack indices remain non-zero so a
  // geometric command cannot alias Crash's high-16-zero color commands.
  memory.write32(commandTable + 4u, 0x80010001u);
  memory.write32(commandTable + 8u, 0x00020000u);
  // 0x8003ce2c restores the strip HEAD for the first triangle's material.
  memory.write32(commandTable + 12u, 0x00030000u);
  memory.write32(commandTable + 16u, UINT32_MAX);
  memory.write32(colorTable, 0x00332211u);

  // Texture tokens are one-based; zero selects the untextured path.
  memory.write32(textureTable + 0u, 0x12340a05u);
  memory.write32(textureTable + 4u, 0x00600a15u);
  memory.write32(textureTable + 8u, 0x00001a05u);

  memory.write16(frame + 0u, 0u);
  memory.write16(frame + 2u, 0u);
  memory.write16(frame + 4u, 128u);
  memory.write32(frame + 0x18u, 0x1cu);
  constexpr std::array<uint8_t, 9> vertices{0u, 0u, 0u, 1u, 0u, 0u, 0u, 0u, 1u};
  for (uint32_t index = 0u; index < vertices.size(); ++index)
    memory.write8(frame + 0x1cu + index, vertices[index]);

  ps1::games::crash3::ObjectPresentationRendererNode node;
  node.address = nodeAddress;
  node.byteCount = 0x70u;
  node.listDiscovered = true;
  node.transformInstalled = true;
  node.scaleShift = 0;
  node.words[0x0cu / 4u] = 0x8003F460u;
  node.words[0x14u / 4u] = modelResource;
  // Crash copies this low half to GTE IR0 before DPCT. It extracts the three
  // overlapping 12-bit far-color lanes from node+0x20 immediately before the
  // primitive pass. Non-zero values exercise the complete fixed-point
  // interpolation rather than a host-side brightness approximation.
  node.words[0x1cu / 4u] = 1024u;
  node.words[0x20u / 4u] =
      (UINT32_C(81) << 20u) | (UINT32_C(98) << 12u) | (UINT32_C(115) << 4u);
  // The live DPCT tuple supersedes the mutable node words.  Use the same IR0
  // with a deliberately different far colour so this test proves that the
  // command-time architectural values win.
  node.distanceCueCaptured = true;
  node.distanceCueIr0 = 1024;
  node.distanceCueFarColor = {{80 << 4, 96 << 4, 112 << 4}};
  node.distanceCueProofs = 3u;
  node.words[0x30u / 4u] = frame;
  node.words[0x34u / 4u] = frame;
  node.words[0x38u / 4u] = commandTable;
  node.words[0x3cu / 4u] = colorTable;
  node.words[0x40u / 4u] = textureTable;
  node.gteControlWords[ps1::GTE_RT11RT12] = 0x00001000u;
  node.gteControlWords[ps1::GTE_RT22RT23] = 0x00001000u;
  node.gteControlWords[ps1::GTE_RT33] = 0x00001000u;
  node.gteControlWords[ps1::GTE_OFX] = 256u << 16u;
  node.gteControlWords[ps1::GTE_OFY] = 128u << 16u;
  node.gteControlWords[ps1::GTE_H] = 256u;

  ps1::games::crash3::ObjectPresentationObject object;
  object.address = objectAddress;
  object.modelResource = modelResource;
  object.animationSequence = 0x80035000u;
  object.animationFrame = 0x1234u;
  object.renderSelector = 0x05040302u;
  object.modelType = 1u;
  object.supportedModelType = true;
  object.rendererNodes.push_back(node);
  ps1::games::crash3::ObjectPresentationSnapshot snapshot;
  snapshot.captureId = 91u;
  snapshot.frameNumber = 1234u;
  snapshot.objects.push_back(std::move(object));
  auto ram = std::make_shared<std::vector<uint8_t>>(ps1::Memory::RAM_SIZE);
  std::memcpy(ram->data(), memory.ramPtr(), ram->size());
  snapshot.guestRam = std::move(ram);

  const auto scene =
      ps1::games::crash3::decodeObjectPresentationScene(snapshot);
  ASSERT_EQ(scene.meshes.size(), 1u);
  EXPECT_EQ(scene.captureId, 91u);
  EXPECT_EQ(scene.attemptedNodes, 1u);
  EXPECT_EQ(scene.completeNodes, 1u);
  EXPECT_EQ(scene.faceCount, 1u);
  const auto &mesh = scene.meshes[0];
  EXPECT_EQ(mesh.status, ps1::games::crash3::ObjectMeshDecodeStatus::Complete);
  EXPECT_EQ(mesh.logicalModelResource, modelResource);
  EXPECT_EQ(mesh.rendererModelResource, modelResource);
  EXPECT_EQ(mesh.animationSequence, 0x80035000u);
  EXPECT_EQ(mesh.animationFrame, 0x1234u);
  EXPECT_EQ(mesh.renderSelector, 0x05040302u);
  EXPECT_EQ(mesh.currentFrameResource, frame);
  EXPECT_EQ(mesh.nextFrameResource, frame);
  EXPECT_EQ(mesh.commandTableResource, commandTable);
  EXPECT_NE(mesh.rendererProgramFingerprint, 0u);
  EXPECT_NE(mesh.rendererGeometryFingerprint, 0u);
  EXPECT_EQ(mesh.materialTableAddress, textureTable);
  EXPECT_EQ(mesh.decodedVertexCount, 3u);
  EXPECT_EQ(mesh.cachedVertexCount, 0u);
  ASSERT_EQ(mesh.faces.size(), 1u);
  const auto &face = mesh.faces[0];
  EXPECT_EQ(face.commandIndex, 2u);
  ASSERT_TRUE(face.material.decoded);
  EXPECT_TRUE(face.material.textured);
  EXPECT_FALSE(face.material.semiTransparent);
  EXPECT_EQ(face.material.sourceToken, 1u);
  EXPECT_EQ(face.material.resolvedToken, 1u);
  EXPECT_EQ(face.material.texturePage, 0x60u);
  EXPECT_EQ(face.material.clut, 0x1234u);
  EXPECT_EQ(face.material.uv[1], (std::array<float, 2>{21.0f, 10.0f}));
  EXPECT_EQ(face.material.uvDomain,
            (std::array<float, 4>{5.0f, 10.0f, 20.0f, 25.0f}));
  EXPECT_EQ(face.vertices[0].model, (std::array<double, 3>{0.0, 0.0, 128.0}));
  EXPECT_EQ(face.vertices[1].model, (std::array<double, 3>{32.0, 0.0, 128.0}));
  EXPECT_EQ(face.vertices[2].model, (std::array<double, 3>{0.0, 32.0, 128.0}));
  EXPECT_DOUBLE_EQ(face.vertices[0].screen[0], 256.0);
  EXPECT_DOUBLE_EQ(face.vertices[1].screen[0], 320.0);
  EXPECT_DOUBLE_EQ(face.vertices[0].screen[1], 128.0);
  EXPECT_DOUBLE_EQ(face.vertices[2].screen[1], 192.0);
  EXPECT_DOUBLE_EQ(face.vertices[0].projectiveW, 128.0);
  for (const auto &vertex : face.vertices)
    EXPECT_EQ(vertex.color255, (std::array<float, 3>{32.0f, 49.0f, 66.0f}));

  // Entry consumes all three seeds without interpreting their high bits as
  // restarts. Real authored strips may have bit 31 on every seed. Only the
  // continuation loop (0x8003cf1c) interprets that bit as a new strip.
  for (uint32_t seedFlags = 0u; seedFlags != 8u; ++seedFlags) {
    for (uint32_t seed = 0u; seed != 3u; ++seed) {
      const uint32_t token = seed == 0u ? 1u : 0u;
      memory.write32(commandTable + 4u + seed * 4u,
                     ((seed + 1u) << 16u) | token |
                         ((seedFlags & (1u << seed)) != 0u ? 0x80000000u
                                                          : 0u));
    }
    auto seedRam = std::make_shared<std::vector<uint8_t>>(ps1::Memory::RAM_SIZE);
    std::memcpy(seedRam->data(), memory.ramPtr(), seedRam->size());
    snapshot.guestRam = std::move(seedRam);
    const auto seeded =
        ps1::games::crash3::decodeObjectPresentationScene(snapshot);
    ASSERT_EQ(seeded.completeNodes, 1u) << seedFlags;
    ASSERT_EQ(seeded.meshes[0].faces.size(), 1u) << seedFlags;
    EXPECT_EQ(seeded.meshes[0].faces[0].material.sourceToken, 1u) << seedFlags;
    EXPECT_EQ(seeded.meshes[0].faces[0].commandIndex, 2u) << seedFlags;
    for (uint32_t corner = 0u; corner != 3u; ++corner)
      EXPECT_EQ(seeded.meshes[0].faces[0].vertices[corner].model,
                face.vertices[corner].model) << seedFlags;
  }

  // The SZ-dependent primitive program has a distinct colour equation.
  // Compare all three decoded corners against the architectural DPCS lane,
  // including the hard black boundary and the no-fade interval.
  auto &fadeNode = snapshot.objects[0].rendererNodes[0];
  fadeNode.perVertexFadeCaptured = true;
  fadeNode.perVertexFadeFarColor = {};
  for (const bool preparedBeforeCulling : {false, true}) {
    fadeNode.perVertexFadeCaptured = !preparedBeforeCulling;
    fadeNode.words[0x2cu / 4u] = preparedBeforeCulling ? 0x8003FC00u : 0u;
    fadeNode.words[0x28u / 4u] = preparedBeforeCulling ? 0x8003D248u : 0u;
    fadeNode.gteControlWords[ps1::GTE_RFC] = 0u;
    fadeNode.gteControlWords[ps1::GTE_GFC] = 0u;
    fadeNode.gteControlWords[ps1::GTE_BFC] = 0u;
    for (const int32_t sz : {0, 3072, 3073, 4096, 5102, 5120, 6000, 70000}) {
      fadeNode.gteControlWords[ps1::GTE_TRZ] = static_cast<uint32_t>(sz - 128);
      const auto faded =
          ps1::games::crash3::decodeObjectPresentationScene(snapshot);
      ASSERT_EQ(faded.completeNodes, 1u);
      std::array<float, 3> expected{};
      if (sz > 3072) {
        ps1::CPUContext oracle{};
        oracle.reset();
        oracle.cop2d[ps1::GTE_RGBC] = 0x00332211u;
        oracle.cop2d[ps1::GTE_IR0] =
            static_cast<uint32_t>(std::max(0, (5120 - sz) * 2));
        ps1::GTE::DPCS(&oracle, true, false);
        const uint32_t rgb = oracle.cop2d[ps1::GTE_RGB2];
        expected = {{static_cast<float>(rgb & 255u),
                     static_cast<float>((rgb >> 8u) & 255u),
                     static_cast<float>((rgb >> 16u) & 255u)}};
      }
      for (const auto &vertex : faded.meshes[0].faces[0].vertices)
        EXPECT_EQ(vertex.color255, expected) << "SZ=" << sz;
      if (preparedBeforeCulling) {
        const auto &constructed = faded.meshes[0].faces[0];
        EXPECT_TRUE(constructed.material.semiTransparent);
        const bool emits = expected != std::array<float, 3>{};
        EXPECT_EQ(constructed.constructorEmitsFace, emits) << "SZ=" << sz;
        if (!emits) {
          const auto projection =
              ps1::games::crash3::projectObjectPresentationFace(faded.meshes[0],
                                                                constructed);
          EXPECT_EQ(projection.status,
                    ps1::games::crash3::ObjectFaceProjectionStatus::
                        ConstructorSuppressed);
          EXPECT_EQ(projection.triangleCount, 0u);
        }
      }
    }
  }
  fadeNode.perVertexFadeCaptured = true;
  // The colour computation floors SZ, without snapping modern geometry.
  fadeNode.gteControlWords[ps1::GTE_TRZ] = 4975u;
  fadeNode.gteControlWords[ps1::GTE_RT33] = 4095u;
  const auto fractional =
      ps1::games::crash3::decodeObjectPresentationScene(snapshot);
  const auto &fractionalVertex = fractional.meshes[0].faces[0].vertices[0];
  EXPECT_DOUBLE_EQ(fractionalVertex.camera[2], 5102.96875);
  EXPECT_EQ(fractionalVertex.color255, (std::array<float, 3>{16, 33, 50}));
  fadeNode.perVertexFadeConsistent = false;
  const auto conflict =
      ps1::games::crash3::decodeObjectPresentationScene(snapshot);
  EXPECT_EQ(conflict.meshes[0].status,
            ps1::games::crash3::ObjectMeshDecodeStatus::InvalidColorTable);
}

TEST_F(Crash3ProfileTest,
       ObjectPresenterUsesNodeLocalStreamsAndObservesAliasedScratchWords) {
  ps1::Memory memory;
  constexpr uint32_t commandTable = 0x80032000u;
  constexpr uint32_t colorTable = 0x80032100u;
  constexpr uint32_t frame = 0x80033000u;

  // The first word is the number of words copied to scratch+320.  Stack slot
  // two later overwrites words four/five through the cached VXY/Z view.
  memory.write32(commandTable, 6u);
  memory.write32(commandTable + 4u, 0x80010000u);
  memory.write32(commandTable + 8u, 0x00020000u);
  // Bit zero selects scratch for A; bits 7+ select byte offset 0x10, i.e.
  // stack slot two's packed VXY word. B remains colour-table word zero.
  memory.write32(commandTable + 12u, 0x00000801u);
  // The completing vertex reads source word eight directly even though only
  // six words are copied to scratch.
  memory.write32(commandTable + 16u, 0x00031000u);
  memory.write32(commandTable + 20u, UINT32_MAX);
  memory.write32(colorTable + 0u, 0x00332211u);
  memory.write32(colorTable + 16u, 0x00665544u);
  memory.write32(colorTable + 32u, 0x00998877u);

  memory.write16(frame + 4u, 128u);
  memory.write32(frame + 0x18u, 0x1cu);
  constexpr std::array<uint8_t, 9> vertices{0u, 0u, 0u, 1u, 0u, 0u, 0u, 0u, 1u};
  for (uint32_t index = 0u; index < vertices.size(); ++index)
    memory.write8(frame + 0x1cu + index, vertices[index]);

  ps1::games::crash3::ObjectPresentationRendererNode node;
  node.address = 0x80031000u;
  node.byteCount = 0x70u;
  node.listDiscovered = true;
  node.transformInstalled = true;
  node.scaleShift = 0;
  node.words[0x0cu / 4u] = 0x8003F460u;
  node.words[0x30u / 4u] = frame;
  node.words[0x34u / 4u] = frame;
  node.words[0x38u / 4u] = commandTable;
  node.words[0x3cu / 4u] = colorTable;
  node.type1StreamsCaptured = true;
  node.type1ColorWordCount = 6u;
  node.type1CommandWords = {0x80010000u, 0x00020000u, 0x00000801u, 0x00031000u,
                            UINT32_MAX};
  node.type1ColorWords.resize(9u);
  node.type1ColorWords[0] = 0x00332211u;
  node.type1ColorWords[4] = 0x00665544u;
  node.type1ColorWords[8] = 0x00998877u;
  node.type1ScratchColorWords.assign(node.type1ColorWords.begin(),
                                     node.type1ColorWords.begin() + 6u);
  node.gteControlWords[ps1::GTE_RT11RT12] = 0x00001000u;
  node.gteControlWords[ps1::GTE_RT22RT23] = 0x00001000u;
  node.gteControlWords[ps1::GTE_RT33] = 0x00001000u;
  node.gteControlWords[ps1::GTE_H] = 256u;

  ps1::games::crash3::ObjectPresentationObject object;
  object.address = 0x80030000u;
  object.modelType = 1u;
  object.rendererNodes.push_back(node);

  // Simulate a neighbouring object reusing both transient tables after this
  // node's exact transform boundary but before the asynchronous host decoder
  // consumes the immutable frame snapshot.  Only the node-local copies may
  // participate in this mesh.
  memory.write32(commandTable, 1u);
  memory.write32(commandTable + 4u, UINT32_MAX);
  memory.write32(colorTable, 0x00ccbbaau);
  ps1::games::crash3::ObjectPresentationSnapshot snapshot;
  snapshot.objects.push_back(std::move(object));
  auto ram = std::make_shared<std::vector<uint8_t>>(ps1::Memory::RAM_SIZE);
  std::memcpy(ram->data(), memory.ramPtr(), ram->size());
  snapshot.guestRam = std::move(ram);

  const auto scene =
      ps1::games::crash3::decodeObjectPresentationScene(snapshot);
  ASSERT_EQ(scene.meshes.size(), 1u);
  ASSERT_EQ(scene.meshes[0].status,
            ps1::games::crash3::ObjectMeshDecodeStatus::Complete);
  ASSERT_EQ(scene.meshes[0].faces.size(), 1u);
  const auto &face = scene.meshes[0].faces[0];
  // The second decoded vertex is (32, 0, 128), so its packed VXY word is 32.
  // Reading the stale initial word would incorrectly produce 0x00665544.
  EXPECT_EQ(face.vertices[0].packedSourceColor, 32u);
  EXPECT_EQ(face.vertices[0].color255,
            (std::array<float, 3>{32.0f, 0.0f, 0.0f}));
  EXPECT_EQ(face.vertices[1].packedSourceColor, 0x00332211u);
  EXPECT_EQ(face.vertices[2].packedSourceColor, 0x00998877u);
}

TEST_F(Crash3ProfileTest,
       ObjectPresenterExecutesChainedAnimatedMaterialRemapProgram) {
  ps1::Memory memory;
  constexpr uint32_t commandTable = 0x80032000u;
  constexpr uint32_t colorTable = 0x80032100u;
  constexpr uint32_t textureTable = 0x80032200u;
  constexpr uint32_t remapTable = 0x80032400u;
  constexpr uint32_t frame = 0x80033000u;

  memory.write32(commandTable, 1u);
  // The strip head's bit 8 selects the dynamic program; byte 2 is its index.
  memory.write32(commandTable + 4u, 0x80010102u);
  memory.write32(commandTable + 8u, 0x00020000u);
  memory.write32(commandTable + 12u, 0x00030000u);
  memory.write32(commandTable + 16u, UINT32_MAX);
  memory.write32(colorTable, 0x00332211u);

  // Descriptor 2 chains to descriptor 5. Descriptor 5 applies
  // animationState(2) & 3, yielding the one-based material record 7.
  memory.write32(remapTable + 2u * 4u, 0x00000805u);
  memory.write32(remapTable + 5u * 4u, 0x00003005u);
  constexpr uint32_t resolvedOffset = (7u - 1u) * 12u;
  memory.write32(textureTable + resolvedOffset + 0u, 0x43210804u);
  memory.write32(textureTable + resolvedOffset + 4u, 0x00600814u);
  memory.write32(textureTable + resolvedOffset + 8u, 0x00001804u);

  memory.write16(frame + 4u, 128u);
  memory.write32(frame + 0x18u, 0x1cu);
  constexpr std::array<uint8_t, 9> vertices{0u, 0u, 0u, 1u, 0u, 0u, 0u, 0u, 1u};
  for (uint32_t index = 0u; index < vertices.size(); ++index)
    memory.write8(frame + 0x1cu + index, vertices[index]);

  ps1::games::crash3::ObjectPresentationRendererNode node;
  node.address = 0x80031000u;
  node.byteCount = 0x70u;
  node.listDiscovered = true;
  node.transformInstalled = true;
  node.scaleShift = 0;
  node.words[0x0cu / 4u] = 0x8003F460u;
  node.words[0x18u / 4u] = 2u;
  node.words[0x30u / 4u] = frame;
  node.words[0x34u / 4u] = frame;
  node.words[0x38u / 4u] = commandTable;
  node.words[0x3cu / 4u] = colorTable;
  node.words[0x40u / 4u] = textureTable;
  node.words[0x44u / 4u] = remapTable;
  node.gteControlWords[ps1::GTE_RT11RT12] = 0x00001000u;
  node.gteControlWords[ps1::GTE_RT22RT23] = 0x00001000u;
  node.gteControlWords[ps1::GTE_RT33] = 0x00001000u;
  node.gteControlWords[ps1::GTE_H] = 256u;

  ps1::games::crash3::ObjectPresentationObject object;
  object.address = 0x80030000u;
  object.modelType = 1u;
  object.rendererNodes.push_back(node);
  ps1::games::crash3::ObjectPresentationSnapshot snapshot;
  snapshot.objects.push_back(std::move(object));
  auto ram = std::make_shared<std::vector<uint8_t>>(ps1::Memory::RAM_SIZE);
  std::memcpy(ram->data(), memory.ramPtr(), ram->size());
  snapshot.guestRam = std::move(ram);

  const auto scene =
      ps1::games::crash3::decodeObjectPresentationScene(snapshot);
  ASSERT_EQ(scene.meshes.size(), 1u);
  ASSERT_EQ(scene.meshes[0].status,
            ps1::games::crash3::ObjectMeshDecodeStatus::Complete);
  ASSERT_EQ(scene.meshes[0].faces.size(), 1u);
  const auto &material = scene.meshes[0].faces[0].material;
  ASSERT_TRUE(material.decoded);
  EXPECT_TRUE(material.dynamicTexture);
  EXPECT_TRUE(material.textured);
  EXPECT_EQ(material.sourceToken, 0x102u);
  EXPECT_EQ(material.resolvedToken, 7u);
  EXPECT_EQ(material.texturePage, 0x60u);
  EXPECT_EQ(material.clut, 0x4321u);
  EXPECT_EQ(material.uvDomain,
            (std::array<float, 4>{4.0f, 8.0f, 19.0f, 23.0f}));
}

TEST_F(Crash3ProfileTest,
       ObjectPresenterDecodesCompressedTemporalVerticesWithoutGuestCulling) {
  ps1::Memory memory;
  constexpr uint32_t commandTable = 0x80032000u;
  constexpr uint32_t colorTable = 0x80032100u;
  constexpr uint32_t frame = 0x80033000u;
  constexpr uint32_t deltaTable = 0x80034000u;

  memory.write32(commandTable, 1u);
  memory.write32(commandTable + 4u, 0x80010000u);
  memory.write32(commandTable + 8u, 0x00020000u);
  memory.write32(commandTable + 12u, 0x00030000u);
  memory.write32(commandTable + 16u, UINT32_MAX);
  memory.write32(colorTable, 0x00030201u);
  memory.write16(frame + 0u, 10u);
  memory.write16(frame + 2u, 20u);
  memory.write16(frame + 4u, 100u);
  memory.write32(frame + 0x18u, 0x1cu);
  // MSB-first component sequence: (x,z,y) for each of three vertices.
  memory.write32(frame + 0x1cu, 0x01020304u);
  memory.write32(frame + 0x20u, 0x05060708u);
  memory.write32(frame + 0x24u, 0x09000000u);
  for (uint32_t index = 0u; index < 3u; ++index)
    memory.write32(deltaTable + index * 4u, 0x1ffu);

  ps1::games::crash3::ObjectPresentationRendererNode node;
  node.address = 0x80031000u;
  node.byteCount = 0x70u;
  node.listDiscovered = true;
  node.transformInstalled = true;
  node.scaleShift = 2;
  node.words[0x0cu / 4u] = 0x8003F460u;
  node.words[0x30u / 4u] = frame;
  node.words[0x34u / 4u] = frame;
  node.words[0x38u / 4u] = commandTable;
  node.words[0x3cu / 4u] = colorTable;
  node.words[0x60u / 4u] = deltaTable;
  node.gteControlWords[ps1::GTE_RT11RT12] = 0x00001000u;
  node.gteControlWords[ps1::GTE_RT22RT23] = 0x00001000u;
  node.gteControlWords[ps1::GTE_RT33] = 0x00001000u;
  node.gteControlWords[ps1::GTE_H] = 256u;

  ps1::games::crash3::ObjectPresentationObject object;
  object.address = 0x80030000u;
  object.modelType = 1u;
  object.rendererNodes.push_back(node);
  ps1::games::crash3::ObjectPresentationSnapshot snapshot;
  snapshot.objects.push_back(std::move(object));
  auto ram = std::make_shared<std::vector<uint8_t>>(ps1::Memory::RAM_SIZE);
  std::memcpy(ram->data(), memory.ramPtr(), ram->size());
  snapshot.guestRam = std::move(ram);

  const auto scene =
      ps1::games::crash3::decodeObjectPresentationScene(snapshot);
  ASSERT_EQ(scene.meshes.size(), 1u);
  ASSERT_EQ(scene.meshes[0].status,
            ps1::games::crash3::ObjectMeshDecodeStatus::Complete);
  ASSERT_EQ(scene.meshes[0].faces.size(), 1u);
  const auto &vertices = scene.meshes[0].faces[0].vertices;
  EXPECT_EQ(vertices[0].model, (std::array<double, 3>{42.0, 84.0, 196.0}));
  EXPECT_EQ(vertices[1].model, (std::array<double, 3>{138.0, 180.0, 292.0}));
  EXPECT_EQ(vertices[2].model, (std::array<double, 3>{234.0, 276.0, 388.0}));
  // The guest normalized this object by 2^2 before projection. The host
  // reverses that categorical scale only in the common depth domain.
  EXPECT_DOUBLE_EQ(vertices[0].projectiveW, 784.0);
}

TEST_F(Crash3ProfileTest, ObjectPresenterFailsClosedWithoutImmutableRam) {
  ps1::games::crash3::ObjectPresentationRendererNode node;
  node.address = 0x80031000u;
  // An intrinsic live node/object binding is a valid semantic admission, but
  // never relaxes the immutable-resource requirement.
  node.directBindingDiscovered = true;
  ps1::games::crash3::ObjectPresentationObject object;
  object.address = 0x80030000u;
  object.modelType = 1u;
  object.rendererNodes.push_back(node);
  ps1::games::crash3::ObjectPresentationSnapshot snapshot;
  snapshot.objects.push_back(std::move(object));

  const auto scene =
      ps1::games::crash3::decodeObjectPresentationScene(snapshot);
  ASSERT_EQ(scene.meshes.size(), 1u);
  EXPECT_EQ(scene.completeNodes, 0u);
  EXPECT_EQ(scene.faceCount, 0u);
  EXPECT_EQ(scene.meshes[0].status,
            ps1::games::crash3::ObjectMeshDecodeStatus::MissingRamSnapshot);
}

TEST_F(Crash3ProfileTest,
       ObjectPresenterDecodesNearPlaneSingleStreamWithoutUsingNodeWord34) {
  ps1::Memory memory;
  constexpr uint32_t commandTable = 0x80032000u;
  constexpr uint32_t colorTable = 0x80032100u;
  constexpr uint32_t frame = 0x80033000u;

  memory.write32(commandTable, 1u);
  memory.write32(commandTable + 4u, 0x80010000u);
  memory.write32(commandTable + 8u, 0x00020000u);
  memory.write32(commandTable + 12u, 0x00030000u);
  memory.write32(commandTable + 16u, UINT32_MAX);
  memory.write32(colorTable, 0x00030201u);
  memory.write16(frame + 0u, 10u);
  memory.write16(frame + 2u, 20u);
  memory.write16(frame + 4u, 100u);
  memory.write32(frame + 0x18u, 0x1cu);
  constexpr std::array<uint8_t, 9> vertices{0u, 0u, 0u, 1u, 0u, 0u, 0u, 0u, 1u};
  for (uint32_t index = 0u; index < vertices.size(); ++index)
    memory.write8(frame + 0x1cu + index, vertices[index]);

  ps1::games::crash3::ObjectPresentationRendererNode node;
  node.address = 0x80031000u;
  node.byteCount = 0x70u;
  node.listDiscovered = true;
  node.transformInstalled = true;
  node.scaleShift = 0;
  node.words[0x0cu / 4u] = 0x8003FDB4u;
  node.words[0x30u / 4u] = frame;
  // This is func_8003D688 in a real near-plane node, not a frame pointer.
  node.words[0x34u / 4u] = 0x8003D688u;
  node.words[0x38u / 4u] = commandTable;
  node.words[0x3cu / 4u] = colorTable;
  node.gteControlWords[ps1::GTE_RT11RT12] = 0x00001000u;
  node.gteControlWords[ps1::GTE_RT22RT23] = 0x00001000u;
  node.gteControlWords[ps1::GTE_RT33] = 0x00001000u;
  node.gteControlWords[ps1::GTE_H] = 256u;

  ps1::games::crash3::ObjectPresentationObject object;
  object.address = 0x80030000u;
  object.modelType = 1u;
  object.rendererNodes.push_back(node);
  ps1::games::crash3::ObjectPresentationSnapshot snapshot;
  snapshot.objects.push_back(std::move(object));
  auto ram = std::make_shared<std::vector<uint8_t>>(ps1::Memory::RAM_SIZE);
  std::memcpy(ram->data(), memory.ramPtr(), ram->size());
  snapshot.guestRam = std::move(ram);

  const auto scene =
      ps1::games::crash3::decodeObjectPresentationScene(snapshot);
  ASSERT_EQ(scene.meshes.size(), 1u);
  EXPECT_EQ(scene.completeNodes, 1u);
  EXPECT_EQ(scene.faceCount, 1u);
  EXPECT_EQ(scene.meshes[0].status,
            ps1::games::crash3::ObjectMeshDecodeStatus::Complete);
  ASSERT_EQ(scene.meshes[0].faces.size(), 1u);
  const auto &decoded = scene.meshes[0].faces[0].vertices;
  EXPECT_EQ(decoded[0].model, (std::array<double, 3>{10.0, 20.0, 100.0}));
  EXPECT_EQ(decoded[1].model, (std::array<double, 3>{42.0, 20.0, 100.0}));
  EXPECT_EQ(decoded[2].model, (std::array<double, 3>{10.0, 52.0, 100.0}));
}

TEST_F(Crash3ProfileTest,
       ObjectPresenterDecodesVertexPreloadedIntoRendererScratch) {
  ps1::Memory memory;
  constexpr uint32_t commandTable = 0x80032000u;
  constexpr uint32_t scratchSource = 0x80032100u;
  constexpr uint32_t frame = 0x80033000u;

  memory.write32(commandTable, 2u);
  // Stack slot zero is already present in the two words copied to
  // scratch+320. Bit 26 selects that cached slot without consuming a sample.
  memory.write32(commandTable + 4u, 0x84000000u);
  memory.write32(commandTable + 8u, 0x00010000u);
  memory.write32(commandTable + 12u, 0x00020000u);
  memory.write32(commandTable + 16u, UINT32_MAX);
  memory.write32(scratchSource, 7u | (9u << 16u));
  memory.write32(scratchSource + 4u, 0u);
  memory.write16(frame + 4u, 100u);
  memory.write32(frame + 0x18u, 0x1cu);
  memory.write8(frame + 0x1cu, 1u);
  memory.write8(frame + 0x1fu + 1u, 0u);

  ps1::games::crash3::ObjectPresentationRendererNode node;
  node.address = 0x80031000u;
  node.listDiscovered = true;
  node.transformInstalled = true;
  node.scaleShift = 0;
  node.words[0x0cu / 4u] = 0x8003FDB4u;
  node.words[0x30u / 4u] = frame;
  node.words[0x34u / 4u] = 0x8003D688u;
  node.words[0x38u / 4u] = commandTable;
  node.words[0x3cu / 4u] = scratchSource;
  node.gteControlWords[ps1::GTE_RT11RT12] = 0x00001000u;
  node.gteControlWords[ps1::GTE_RT22RT23] = 0x00001000u;
  node.gteControlWords[ps1::GTE_RT33] = 0x00001000u;
  node.gteControlWords[ps1::GTE_H] = 256u;

  ps1::games::crash3::ObjectPresentationObject object;
  object.address = 0x80030000u;
  object.modelType = 1u;
  object.rendererNodes.push_back(node);
  ps1::games::crash3::ObjectPresentationSnapshot snapshot;
  snapshot.objects.push_back(std::move(object));
  auto ram = std::make_shared<std::vector<uint8_t>>(ps1::Memory::RAM_SIZE);
  std::memcpy(ram->data(), memory.ramPtr(), ram->size());
  snapshot.guestRam = std::move(ram);

  const auto scene =
      ps1::games::crash3::decodeObjectPresentationScene(snapshot);
  ASSERT_EQ(scene.meshes.size(), 1u);
  ASSERT_EQ(scene.meshes[0].status,
            ps1::games::crash3::ObjectMeshDecodeStatus::Complete);
  EXPECT_EQ(scene.meshes[0].cachedVertexCount, 1u);
  EXPECT_EQ(scene.meshes[0].decodedVertexCount, 2u);
  ASSERT_EQ(scene.meshes[0].faces.size(), 1u);
  EXPECT_EQ(scene.meshes[0].faces[0].vertices[0].model,
            (std::array<double, 3>{7.0, 9.0, 0.0}));
  EXPECT_FALSE(scene.meshes[0].faces[0].vertices[0].directProjectionValid);
  EXPECT_DOUBLE_EQ(scene.meshes[0].faces[0].vertices[0].projectiveW, 0.0);
}

TEST_F(Crash3ProfileTest,
       ObjectPresenterGeometricallyClipsNearPlaneCrossingFace) {
  ps1::games::crash3::ObjectPresentationMesh mesh;
  mesh.scaleShift = 1;
  mesh.projectionPlane = 256u;
  mesh.projectionOriginFixed = {{256 << 16, 128 << 16}};

  ps1::games::crash3::ObjectPresentationFace face;
  face.vertices[0].camera = {{-64.0, -64.0, 256.0}};
  face.vertices[1].camera = {{64.0, -64.0, 256.0}};
  face.vertices[2].camera = {{0.0, 64.0, 0.0}};
  face.material.uv = {{{{0.0f, 0.0f}}, {{64.0f, 0.0f}}, {{32.0f, 64.0f}}}};
  face.vertices[0].color255 = {{255.0f, 0.0f, 0.0f}};
  face.vertices[1].color255 = {{0.0f, 255.0f, 0.0f}};
  face.vertices[2].color255 = {{0.0f, 0.0f, 255.0f}};

  const auto projected =
      ps1::games::crash3::projectObjectPresentationFace(mesh, face);
  ASSERT_EQ(projected.status,
            ps1::games::crash3::ObjectFaceProjectionStatus::NearPlaneClipped);
  ASSERT_EQ(projected.triangleCount, 2u);
  const double firstUnsaturatedW = (128.0 + 1.0 / 4096.0) * 2.0;
  uint32_t boundaryVertices = 0u;
  for (uint8_t triangle = 0u; triangle < projected.triangleCount; ++triangle) {
    for (const auto &vertex : projected.triangles[triangle].vertices) {
      EXPECT_TRUE(std::isfinite(vertex.screen[0]));
      EXPECT_TRUE(std::isfinite(vertex.screen[1]));
      EXPECT_GE(vertex.projectiveW, firstUnsaturatedW);
      if (std::abs(vertex.projectiveW - firstUnsaturatedW) < 1.0e-9)
        ++boundaryVertices;
      for (const float component : vertex.color255) {
        EXPECT_GE(component, 0.0f);
        EXPECT_LE(component, 255.0f);
      }
    }
  }
  // The fan shares one of the two edge intersections.
  EXPECT_EQ(boundaryVertices, 3u);
}

TEST_F(Crash3ProfileTest,
       ObjectPresenterUniformCameraScaleMovesShellWithoutChangingImage) {
  ps1::games::crash3::ObjectPresentationMesh mesh;
  mesh.scaleShift = 2;
  mesh.projectionPlane = 256u;
  mesh.projectionOriginFixed = {{256 << 16, 128 << 16}};

  ps1::games::crash3::ObjectPresentationFace face;
  face.vertices[0].camera = {{-80.0, -40.0, 320.0}};
  face.vertices[1].camera = {{96.0, -24.0, 384.0}};
  face.vertices[2].camera = {{16.0, 72.0, 448.0}};

  constexpr double scale = 128.0;
  const auto original = ps1::games::crash3::projectObjectPresentationFace(
      mesh, face,
      ps1::games::crash3::ObjectPresentationProjectionMode::PositiveCamera);
  const auto expanded = ps1::games::crash3::projectObjectPresentationFace(
      mesh, face,
      ps1::games::crash3::ObjectPresentationProjectionMode::PositiveCamera,
      scale);
  ASSERT_EQ(original.status,
            ps1::games::crash3::ObjectFaceProjectionStatus::Direct);
  ASSERT_EQ(expanded.status,
            ps1::games::crash3::ObjectFaceProjectionStatus::Direct);
  ASSERT_EQ(original.triangleCount, 1u);
  ASSERT_EQ(expanded.triangleCount, 1u);
  for (std::size_t corner = 0u; corner < 3u; ++corner) {
    EXPECT_DOUBLE_EQ(expanded.triangles[0].vertices[corner].screen[0],
                     original.triangles[0].vertices[corner].screen[0]);
    EXPECT_DOUBLE_EQ(expanded.triangles[0].vertices[corner].screen[1],
                     original.triangles[0].vertices[corner].screen[1]);
    EXPECT_DOUBLE_EQ(expanded.triangles[0].vertices[corner].projectiveW,
                     original.triangles[0].vertices[corner].projectiveW *
                         scale);
  }
}

TEST_F(Crash3ProfileTest, ObjectPresenterRejectsFaceBehindIntrinsicNearPlane) {
  ps1::games::crash3::ObjectPresentationMesh mesh;
  mesh.scaleShift = 0;
  mesh.projectionPlane = 256u;
  ps1::games::crash3::ObjectPresentationFace face;
  face.vertices[0].camera = {{-1.0, -1.0, 10.0}};
  face.vertices[1].camera = {{1.0, -1.0, 20.0}};
  face.vertices[2].camera = {{0.0, 1.0, -5.0}};

  const auto projected =
      ps1::games::crash3::projectObjectPresentationFace(mesh, face);
  EXPECT_EQ(projected.status,
            ps1::games::crash3::ObjectFaceProjectionStatus::NearPlaneRejected);
  EXPECT_EQ(projected.triangleCount, 0u);
}

TEST_F(Crash3ProfileTest,
       ObjectPresenterDerivesContinuousDepthFromIntrinsicOtBases) {
  ps1::games::crash3::ObjectPresentationMesh mesh;
  mesh.scaleShift = 3;
  mesh.orderingTableBasesCaptured = true;
  mesh.orderingTableBasePositive = 2050u;
  mesh.orderingTableBaseNonPositive = 2040u;
  mesh.orderingTableZsf3 = 1365;
  mesh.orderingTableZsf4 = 1024;
  ps1::games::crash3::ObjectPresentationFace face;

  face.command = 1u;
  auto transform =
      ps1::games::crash3::objectPresentationDepthTransform(mesh, face);
  ASSERT_TRUE(transform.valid);
  EXPECT_FLOAT_EQ(transform.multiplier, 4095.0f / 4096.0f / 8.0f);
  EXPECT_FLOAT_EQ(transform.bias, 8128.0f);
  EXPECT_EQ(transform.orderingTableBase, 2050u);

  // `command << 6` is negative in the original signed branch.
  face.command = UINT32_C(0x02000000);
  transform = ps1::games::crash3::objectPresentationDepthTransform(mesh, face);
  ASSERT_TRUE(transform.valid);
  EXPECT_FLOAT_EQ(transform.multiplier, 4095.0f / 4096.0f / 8.0f);
  EXPECT_FLOAT_EQ(transform.bias, 8448.0f);
  EXPECT_EQ(transform.orderingTableBase, 2040u);

  transform =
      ps1::games::crash3::objectPresentationDepthTransform(mesh, face, 4u);
  ASSERT_TRUE(transform.valid);
  EXPECT_FLOAT_EQ(transform.multiplier, 0.125f);

  // The native OT address uses SLL 2 after testing the unshifted sign.
  // Bit 30 aliases the same address; it is not a billion-bucket depth bias.
  mesh.orderingTableBaseNonPositive = UINT32_C(0x400007f8);
  transform =
      ps1::games::crash3::objectPresentationDepthTransform(mesh, face, 4u);
  ASSERT_TRUE(transform.valid);
  EXPECT_FLOAT_EQ(transform.bias, 8448.0f);
  EXPECT_EQ(transform.orderingTableBase, UINT32_C(0x400007f8));
  mesh.orderingTableBaseNonPositive = UINT32_C(0xfffffff8);
  transform =
      ps1::games::crash3::objectPresentationDepthTransform(mesh, face, 4u);
  ASSERT_TRUE(transform.valid);
  EXPECT_FLOAT_EQ(transform.bias, 73984.0f);
}

TEST_F(Crash3ProfileTest,
       ObjectNormalConstructorUsesNormalsNotRgbAndKeepsBothSides) {
  using namespace ps1::games::crash3;
  ObjectPresentationRendererNode node;
  node.words[0x2cu / 4u] = 0x8003DC00u;
  node.words[0x28u / 4u] = 0x8003E1A4u;
  node.gteControlWords[8] = 4096u;
  node.gteControlWords[12] = 4096u;
  node.gteControlWords[ps1::GTE_RFC] = 2048u;
  node.gteControlWords[ps1::GTE_GFC] = 1024u;
  node.gteControlWords[ps1::GTE_BFC] = 512u;
  std::array<uint8_t, 128> highlights{};
  ObjectPresentationFace face;
  face.vertices[0].screen = {0.0, 0.0};
  face.vertices[1].screen = {2.0, 0.0};
  face.vertices[2].screen = {0.0, 2.0};
  face.vertices[0].packedSourceColor = 64u; // normal X=2048, not red=64
  face.vertices[1].packedSourceColor = 0xff0000u;
  face.material.decoded = true;
  face.material.textured = true;
  face.material.texturePage = 0x18u;
  const auto source = face;
  ASSERT_TRUE(applyObjectNormalConstructor(node, highlights, face));
  EXPECT_FALSE(face.constructorBackFacing);
  EXPECT_TRUE(face.material.semiTransparent);
  EXPECT_EQ(face.material.texturePage, 0x38u);
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(face.vertices[i].color255,
              (std::array<float, 3>{100.0f, 50.0f, 25.0f}));
    EXPECT_EQ(face.vertices[i].screen, source.vertices[i].screen);
    EXPECT_EQ(face.vertices[i].packedSourceColor,
              source.vertices[i].packedSourceColor);
  }
  face = source;
  face.acceptsNegativeWinding = true;
  ASSERT_TRUE(applyObjectNormalConstructor(node, highlights, face));
  EXPECT_TRUE(face.constructorBackFacing);
  EXPECT_FALSE(face.material.semiTransparent);
  EXPECT_EQ(face.material.texturePage, 0x78u);
  EXPECT_EQ(face.vertices[0].color255,
            (std::array<float, 3>{50.0f, 25.0f, 12.0f}));

  ObjectPresentationMesh mesh;
  mesh.primitiveConstructor = 0x8003DC00u;
  mesh.orderingTableBasesCaptured = true;
  mesh.orderingTableBasePositive = 2254u;
  mesh.orderingTableBaseNonPositive = 0x40000770u;
  mesh.scaleShift = 0;
  mesh.orderingTableZsf3 = 1365;
  face.command =
      0u; // Normal constructor ignores the common command-class selector.
  const auto backDepth = objectPresentationDepthTransform(mesh, face, 3);
  face.constructorBackFacing = false;
  const auto frontDepth = objectPresentationDepthTransform(mesh, face, 3);
  ASSERT_TRUE(backDepth.valid);
  ASSERT_TRUE(frontDepth.valid);
  EXPECT_EQ(backDepth.orderingTableBase, 2254u);
  EXPECT_FLOAT_EQ(backDepth.bias - frontDepth.bias, 64.0f);
  EXPECT_FLOAT_EQ(backDepth.multiplier, frontDepth.multiplier);
}

TEST_F(Crash3ProfileTest,
       ObjectNormalConstructorSignedLightAndHighlightMatchGte) {
  using namespace ps1::games::crash3;
  ObjectPresentationRendererNode node;
  node.words[0x2cu / 4u] = 0x8003DC00u;
  node.words[0x28u / 4u] = 0x8003E1A4u;
  node.gteControlWords[8] = 4096u;
  node.gteControlWords[12] = 4096u;
  node.gteControlWords[ps1::GTE_RFC] = 4080u;
  node.gteControlWords[ps1::GTE_GFC] = 512u;
  node.gteControlWords[ps1::GTE_BFC] = 0u;
  std::array<uint8_t, 128> highlights{};
  for (uint32_t i = 0; i < highlights.size(); ++i)
    highlights[i] = static_cast<uint8_t>(i + 1u);
  for (const int32_t x : {-128, -64, 0, 64, 127}) {
    for (const int32_t z : {-128, -96, 0, 96, 127}) {
      for (const uint32_t cue : {0u, 1024u, 4096u}) {
        for (const bool back : {false, true}) {
          node.words[0x1cu / 4u] = cue;
          ObjectPresentationFace face;
          face.vertices[1].screen = {2.0, 0.0};
          face.vertices[2].screen = {0.0, 2.0};
          face.acceptsNegativeWinding = back;
          face.vertices[0].packedSourceColor =
              static_cast<uint8_t>(x) |
              (uint32_t{static_cast<uint8_t>(z)} << 16u);
          face.material.decoded = face.material.textured = true;
          ASSERT_TRUE(applyObjectNormalConstructor(node, highlights, face));

          ps1::CPUContext native{};
          std::copy(node.gteControlWords.begin(), node.gteControlWords.end(),
                    native.cop2c);
          native.cop2d[ps1::GTE_IR1] = x * 32;
          native.cop2d[ps1::GTE_IR3] = z * 32;
          ps1::GTE::MVMVA(&native, 1, 3, 3, true, false);
          uint32_t intensity =
              ((static_cast<int16_t>(native.cop2d[ps1::GTE_IR1]) + 4096) * 7u +
               8192u) >>
              4u;
          if (back)
            intensity >>= 1u;
          native.cop2d[ps1::GTE_IR0] =
              std::max(static_cast<int32_t>(intensity - cue), 0);
          const int32_t lookup =
              std::abs(static_cast<int16_t>(native.cop2d[ps1::GTE_IR3]) >> 3) -
              384;
          uint32_t shine = lookup < 0 ? 0u : highlights[std::min(lookup, 127)];
          if (back)
            shine >>= 3u;
          ps1::GTE::DPCS(&native, true, false);
          for (uint32_t channel = 0; channel < 3; ++channel) {
            const uint32_t rgb =
                (native.cop2d[ps1::GTE_RGB2] >> (channel * 8u)) & 255u;
            EXPECT_EQ(face.vertices[0].color255[channel],
                      static_cast<float>(std::min(rgb + shine, 255u)));
          }
        }
      }
    }
  }
}

TEST_F(Crash3ProfileTest,
       ObjectSourceConstructionRequiresExecutedKnownProgram) {
  using namespace ps1::games::crash3;
  ObjectPresentationRendererNode node;
  node.words[0x2cu / 4u] = 0x80040C1Cu;
  EXPECT_FALSE(hasIntrinsicObjectConstruction(node));
  node.listDiscovered = node.transformInstalled = node.type1StreamsCaptured =
      true;
  node.type1CommandWords = {0x80000000u, UINT32_MAX};
  node.scaleShift = 0;
  EXPECT_FALSE(hasIntrinsicObjectConstruction(node));
  node.orderingTableBasesCaptured = true;
  EXPECT_TRUE(hasIntrinsicObjectConstruction(node));
  node.listDiscovered = false;
  EXPECT_FALSE(hasIntrinsicObjectConstruction(node));
  node.directBindingDiscovered = true;
  EXPECT_TRUE(hasIntrinsicObjectConstruction(node));
  node.type1CommandWords.back() = 0u;
  EXPECT_FALSE(hasIntrinsicObjectConstruction(node));
  node.type1CommandWords.back() = UINT32_MAX;
  node.words[0x2cu / 4u] = 0x8003CC88u; // Multi-pass is not this constructor.
  EXPECT_FALSE(hasIntrinsicObjectConstruction(node));
  node.words[0x2cu / 4u] = 0x8003DC00u;
  EXPECT_FALSE(hasIntrinsicObjectConstruction(node));
  node.words[0x28u / 4u] = 0x8003E1A4u;
  EXPECT_TRUE(hasIntrinsicObjectConstruction(node));
  node.words[0x2cu / 4u] = 0x8003FC00u;
  EXPECT_FALSE(hasIntrinsicObjectConstruction(node));
  node.words[0x28u / 4u] = 0x8003D248u;
  EXPECT_TRUE(hasIntrinsicObjectConstruction(node));
  node.words[0x2cu / 4u] = 0x80040C00u;
  EXPECT_TRUE(hasIntrinsicObjectConstruction(node));

  ObjectPresentationMesh mesh;
  mesh.primitiveConstructor = 0x80040C1Cu;
  mesh.orderingTableBasesCaptured = true;
  mesh.orderingTableBasePositive = 1884u;
  mesh.orderingTableBaseNonPositive = 0x800BC000u; // unrelated scratch pointer
  mesh.orderingTableZsf3 = 1365;
  mesh.scaleShift = 0;
  ObjectPresentationFace face;
  const auto depth = objectPresentationDepthTransform(mesh, face, 3u);
  ASSERT_TRUE(depth.valid);
  EXPECT_EQ(depth.orderingTableBase, 1884u);
  EXPECT_FLOAT_EQ(depth.bias, 8192.0f + (2048.0f - 1884.0f) * 32.0f);
  mesh.primitiveConstructor = 0x8003FC00u;
  EXPECT_EQ(objectPresentationDepthTransform(mesh, face, 3u).orderingTableBase,
            1884u);
  mesh.primitiveConstructor = 0x80040C00u;
  mesh.orderingTableBaseNonPositive = 1784u;
  EXPECT_EQ(objectPresentationDepthTransform(mesh, face, 3u).orderingTableBase,
            1784u);
}

TEST_F(Crash3ProfileTest,
       ObjectNormalConstructorKeepsThinFaceAndNativeZeroAreaRule) {
  using namespace ps1::games::crash3;
  ObjectPresentationRendererNode node;
  node.words[0x2cu / 4u] = 0x8003DC00u;
  node.words[0x28u / 4u] = 0x8003E1A4u;
  node.gteControlWords[ps1::GTE_H] = 128u;
  ObjectPresentationFace face;
  face.material.decoded = face.material.textured = true;
  // Nonzero negative float area, but the native NCLIP consumes three Y=0.
  face.vertices[0].camera = {0.25, 0.25, 128.0};
  face.vertices[1].camera = {3.75, 0.45, 128.0};
  face.vertices[2].camera = {2.50, 0.26, 128.0};
  for (auto &v : face.vertices)
    v.screen = {v.camera[0], v.camera[1]};
  const auto vertices = face.vertices;
  const std::array<uint8_t, 128> highlights{};
  ASSERT_TRUE(applyObjectNormalConstructor(node, highlights, face));
  EXPECT_FALSE(face.constructorBackFacing);
  EXPECT_TRUE(face.material.semiTransparent);
  for (uint32_t i = 0; i < 3; ++i) {
    EXPECT_EQ(face.vertices[i].camera, vertices[i].camera);
    EXPECT_EQ(face.vertices[i].screen, vertices[i].screen);
  }
  node.words[0x28u / 4u] = 0u;
  EXPECT_FALSE(applyObjectNormalConstructor(node, highlights, face));
}

TEST_F(Crash3ProfileTest, ObjectPresenterDepthTransformFailsClosed) {
  ps1::games::crash3::ObjectPresentationMesh mesh;
  ps1::games::crash3::ObjectPresentationFace face;
  EXPECT_FALSE(
      ps1::games::crash3::objectPresentationDepthTransform(mesh, face).valid);
  mesh.orderingTableBasesCaptured = true;
  mesh.scaleShift = 31;
  EXPECT_FALSE(
      ps1::games::crash3::objectPresentationDepthTransform(mesh, face).valid);
}

TEST_F(Crash3ProfileTest, RejectsCrash1SelfOffsetFromCrash3ObjectCatalog) {
  setenv("PS1_CRASH3_VULKAN_OBJECT_MODE", "audit", 1);
  ps1::games::crash3::configure({.enabled = true});
  ps1::Memory memory;
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &memory;
  constexpr uint32_t impostor = 0x80036000u;
  memory.write32(impostor, 1u);
  memory.write32(impostor + 0x60u, impostor);
  ps1::metrics::setTraceFrame(500u);
  ctx.r[4] = impostor;
  ps1::games::crash3::observeObjectPrePresentation(&ctx);
  const auto snapshots = ps1::games::crash3::objectPresentationSnapshots();
  ASSERT_EQ(snapshots.size(), 1u);
  EXPECT_TRUE(snapshots[0].objects.empty());
  EXPECT_EQ(snapshots[0].rejectedObjects, 1u);
}

TEST_F(Crash3ProfileTest, StableCallsiteCoexistsWithExactModelSource) {
  ps1::games::crash3::configure({.enabled = true});
  ps1::CPUContext ctx{};
  ctx.reset();
  ctx.pc = 0x80040F94u;
  ctx.r[4] = 3u;
  ctx.r[29] = 0x80070110u;
  ps1::games::crash3::observeGteControlWrite(ctx, 5u);
  ctx.pc = 0x8003CFB8u;
  ctx.r[3] = 0x1F800000u;
  ps1::PGXPValue projection;
  projection.modelVertexSource = 0x80030001u;

  ps1::games::crash3::attachProjectionTag(ctx, 1u, projection);

  EXPECT_EQ(projection.modelVertexSource, 0x80030001u);
  EXPECT_EQ(ps1::games::crash3::projectionCallsiteVectorIndex(
                projection.gameProfileCallsite),
            1u);
  EXPECT_EQ(ps1::games::crash3::projectionCallsiteObjectSource(
                projection.gameProfileCallsite),
            0x80070110u);
}

TEST_F(Crash3ProfileTest,
       SceneryListBoundaryEndsDynamicObjectProjectionIdentity) {
  ps1::games::crash3::configure({.enabled = true});
  ps1::Memory memory;
  recomp_context recomp{};
  recomp.reset();
  recomp.mem = &memory;
  constexpr uint32_t list = 0x80030000u;
  memory.write16(list, 0u);

  ps1::CPUContext &ctx = recomp;
  ctx.pc = 0x80040F94u;
  ctx.r[4] = 3u;
  ctx.r[29] = 0x80070110u;
  ps1::games::crash3::observeGteControlWrite(ctx, ps1::GTE_TRX);
  ctx.pc = 0x8003CFB8u;
  ctx.r[3] = 0x1F800000u;
  ps1::PGXPValue objectProjection;
  ps1::games::crash3::attachProjectionTag(ctx, 0u, objectProjection);
  EXPECT_EQ(ps1::games::crash3::projectionCallsiteObjectSource(
                objectProjection.gameProfileCallsite),
            0x80070110u);

  EXPECT_EQ(ps1::games::crash3::scenerySelectionCount(&recomp, list), 0);
  ps1::PGXPValue sceneryProjection;
  ps1::games::crash3::attachProjectionTag(ctx, 0u, sceneryProjection);
  EXPECT_EQ(ps1::games::crash3::projectionCallsiteObjectSource(
                sceneryProjection.gameProfileCallsite),
            0u);
  EXPECT_EQ(ps1::games::crash3::projectionCallsiteScaleShift(
                sceneryProjection.gameProfileCallsite),
            -1);
}

TEST_F(Crash3ProfileTest,
       DiagnosticSceneryProjectionIdentityFollowsOneEntryAheadDecoder) {
  setenv("PS1_TRACE_CRASH3_SCENERY_PROJECTION_IDENTITY", "1", 1);
  ps1::games::crash3::configure({.enabled = true});
  ps1::Memory memory;
  recomp_context recomp{};
  recomp.reset();
  recomp.mem = &memory;
  constexpr uint32_t list = 0x80030000u;
  constexpr uint16_t first = 0x1123u;
  constexpr uint16_t second = 0x2ABCu;
  constexpr uint16_t third = 0x61FEu;
  memory.write16(list, 3u);
  memory.write16(list + 4u, first);
  memory.write16(list + 6u, second);
  memory.write16(list + 8u, third);
  ASSERT_EQ(ps1::games::crash3::scenerySelectionCount(&recomp, list), 3);

  ps1::CPUContext &ctx = recomp;
  ctx.pc = 0x80040F94u;
  ctx.r[4] = 3u;
  ctx.r[29] = 0x80070110u;
  ps1::games::crash3::observeGteControlWrite(ctx, 5u);
  ctx.pc = 0x80041A30u;
  ctx.r[3] = 0x1F800000u;

  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&recomp, list + 8u),
            third);
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&recomp, list + 6u),
            second);
  ps1::PGXPValue thirdProjection;
  ps1::games::crash3::attachProjectionTag(ctx, 2u, thirdProjection);
  const auto thirdPayload = ps1::games::crash3::decodeProjectionTag(
      thirdProjection.modelVertexSource);
  ASSERT_TRUE(thirdPayload.has_value());
  EXPECT_EQ((*thirdPayload >> 26u) & 0xffffu, third);
  const uint32_t captureId =
      static_cast<uint32_t>(*thirdPayload) & ((1u << 26u) - 1u);
  EXPECT_NE(captureId, 0u);

  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&recomp, list + 4u),
            first);
  ps1::PGXPValue secondProjection;
  ps1::games::crash3::attachProjectionTag(ctx, 1u, secondProjection);
  const auto secondPayload = ps1::games::crash3::decodeProjectionTag(
      secondProjection.modelVertexSource);
  ASSERT_TRUE(secondPayload.has_value());
  EXPECT_EQ((*secondPayload >> 26u) & 0xffffu, second);
  EXPECT_EQ(static_cast<uint32_t>(*secondPayload) & ((1u << 26u) - 1u),
            captureId);

  // A state-3 quad reads its prefetched selector a second time from the same
  // cursor after loading the fourth vertex.  0x800414C4 commits a complete
  // quad; the state-1 half-quad path similarly commits at 0x80041388 after it
  // installs the prefetched primitive's third GTE input.
  ps1::games::crash3::configure({.enabled = true});
  ASSERT_EQ(ps1::games::crash3::scenerySelectionCount(&recomp, list), 3);
  ctx.pc = 0x80041354u;
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&recomp, list + 8u),
            third);
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&recomp, list + 6u),
            second);
  ctx.pc = 0x80041388u;
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&recomp, list + 6u),
            second);
  ctx.pc = 0x80041A30u;
  ps1::PGXPValue halfQuadProjection;
  ps1::games::crash3::attachProjectionTag(ctx, 2u, halfQuadProjection);
  const auto halfQuadPayload = ps1::games::crash3::decodeProjectionTag(
      halfQuadProjection.modelVertexSource);
  ASSERT_TRUE(halfQuadPayload.has_value());
  EXPECT_EQ((*halfQuadPayload >> 26u) & 0xffffu, second);

  ctx.pc = 0x800414C4u;
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&recomp, list + 6u),
            second);
  ctx.pc = 0x80041A30u;
  ps1::PGXPValue committedSecondProjection;
  ps1::games::crash3::attachProjectionTag(ctx, 2u, committedSecondProjection);
  const auto committedSecondPayload = ps1::games::crash3::decodeProjectionTag(
      committedSecondProjection.modelVertexSource);
  ASSERT_TRUE(committedSecondPayload.has_value());
  EXPECT_EQ((*committedSecondPayload >> 26u) & 0xffffu, second);

  ctx.pc = 0x800415F0u;
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&recomp, list + 4u),
            first);
  EXPECT_EQ(ps1::games::crash3::scenerySelectionEntry(&recomp, list + 4u),
            first);
  ctx.pc = 0x80041A30u;
  ps1::PGXPValue committedFirstProjection;
  ps1::games::crash3::attachProjectionTag(ctx, 2u, committedFirstProjection);
  const auto committedFirstPayload = ps1::games::crash3::decodeProjectionTag(
      committedFirstProjection.modelVertexSource);
  ASSERT_TRUE(committedFirstPayload.has_value());
  EXPECT_EQ((*committedFirstPayload >> 26u) & 0xffffu, first);
}

TEST_F(Crash3ProfileTest, AppliesKnownPresentationDepthLane) {
  ps1::games::crash3::configure(
      {.enabled = true, .presentationDepthCorrections = true});
  const auto points = vertices();
  const auto decision =
      ps1::games::crash3::planPresentationDepth(knownOwner(), points);
  EXPECT_EQ(decision.status, PresentationDepthStatus::Apply);
  EXPECT_FLOAT_EQ(decision.multiplier, 31.0f);
}

TEST_F(Crash3ProfileTest,
       RestoresExactRendererScaleOnTranslucentSurfaceWithoutChangingOt) {
  ps1::games::crash3::configure(
      {.enabled = true, .presentationDepthCorrections = true});
  auto owner = knownOwner();
  owner.opaque = false;
  constexpr uint32_t objectSource = 0x80070034u;
  constexpr uint32_t gtePcOffset = 0x0041A30u;
  constexpr uint32_t scaleShift = 3u;
  for (uint32_t lane = 0; lane < 3; ++lane) {
    owner.provenance.vertices[lane].gameProfileCallsite =
        (static_cast<uint64_t>(objectSource) << 32u) |
        (static_cast<uint64_t>(gtePcOffset) << 8u) |
        (static_cast<uint64_t>(scaleShift + 1u) << 2u) | lane;
  }

  const auto decision =
      ps1::games::crash3::planPresentationDepth(owner, vertices());
  EXPECT_EQ(decision.status, PresentationDepthStatus::ApplySurfaceOnly);
  EXPECT_FLOAT_EQ(decision.multiplier, 8.0f);
  EXPECT_TRUE(decision.applies());
}

TEST_F(Crash3ProfileTest,
       ClassifiesOnlyCertifiedWarpEdgeFacesForDeferredOcclusionReplay) {
  ps1::games::crash3::configure(
      {.enabled = true, .presentationDepthCorrections = true});

  EXPECT_EQ(ps1::games::crash3::classifySceneryOcclusion(sceneryOwner(0x202du)),
            SceneryOcclusionClass::Recovered);
  EXPECT_EQ(ps1::games::crash3::classifySceneryOcclusion(sceneryOwner(0x396cu)),
            SceneryOcclusionClass::Cohort);
  EXPECT_EQ(ps1::games::crash3::classifySceneryOcclusion(sceneryOwner(0x1a0cu)),
            SceneryOcclusionClass::Cohort);
  EXPECT_EQ(ps1::games::crash3::classifySceneryOcclusion(
                sceneryOwner(0x202du, false)),
            SceneryOcclusionClass::Cohort);

  const auto primary = ps1::games::crash3::planPresentationDepth(
      sceneryOwner(0x202du), vertices());
  EXPECT_EQ(primary.status, PresentationDepthStatus::ApplySurfaceOnly);
  EXPECT_FLOAT_EQ(primary.multiplier, 4.0f);

  const auto captured = sceneryOwner(0x202du, true, 0x12345u);
  EXPECT_EQ(ps1::games::crash3::sceneryProjectionEntry(captured), 0x202du);
  EXPECT_EQ(ps1::games::crash3::sceneryProjectionCapture(captured), 0x12345u);
  auto mixedCapture = captured;
  mixedCapture.provenance.vertices[2].gameProfileTag ^= 1u;
  EXPECT_FALSE(
      ps1::games::crash3::sceneryProjectionCapture(mixedCapture).has_value());
}

TEST_F(Crash3ProfileTest, StrictSceneryComparisonModeNeedsNoRecompile) {
  setenv("PS1_CRASH3_SCENERY_OCCLUSION", "strict", 1);
  ps1::games::crash3::configure(
      {.enabled = true, .presentationDepthCorrections = true});

  EXPECT_EQ(ps1::games::crash3::classifySceneryOcclusion(sceneryOwner(0x1a0cu)),
            SceneryOcclusionClass::Recovered);
}

TEST_F(Crash3ProfileTest,
       AnchorsNativeWorldPacketMeanToExactOrderingTableBucket) {
  ps1::games::crash3::configure(
      {.enabled = true, .presentationDepthCorrections = true});
  auto owner = surfaceOwner(0x8003CE34u, 1860u, {0u, 1u, 2u},
                            {2400.0f, 2432.0f, 2464.0f}, true, 0, 0x80070110u);
  const std::array triangles{
      ps1::games::crash3::PresentationSurfaceTriangle{&owner}};

  const auto decision =
      ps1::games::crash3::planPresentedSurfaceDepth(triangles);
  ASSERT_EQ(decision.status, PresentationSurfaceStatus::Apply);
  EXPECT_FLOAT_EQ(decision.multiplier, 1.0f);
  EXPECT_FLOAT_EQ(decision.bias, 11776.0f);
  EXPECT_EQ(decision.orderingTableBucket, 1860u);
  EXPECT_EQ(decision.uniqueVertexCount, 3u);
}

TEST_F(Crash3ProfileTest,
       UsesOneQuarterScaleTransformForBothTrianglesOfTranslucentQuad) {
  ps1::games::crash3::configure(
      {.enabled = true, .presentationDepthCorrections = true});
  auto first = surfaceOwner(0x80044938u, 1125u, {0u, 1u, 2u},
                            {6108.0f, 6366.0f, 6109.0f});
  auto second = surfaceOwner(0x80044938u, 1125u, {0u, 2u, 3u},
                             {6108.0f, 6109.0f, 6112.0f});
  const std::array triangles{
      ps1::games::crash3::PresentationSurfaceTriangle{&first},
      ps1::games::crash3::PresentationSurfaceTriangle{&second}};

  const auto decision =
      ps1::games::crash3::planPresentedSurfaceDepth(triangles);
  ASSERT_EQ(decision.status, PresentationSurfaceStatus::Apply);
  EXPECT_FLOAT_EQ(decision.multiplier, 4.0f);
  EXPECT_FLOAT_EQ(decision.bias, 13033.0f);
  EXPECT_EQ(decision.orderingTableBucket, 1125u);
  EXPECT_EQ(decision.uniqueVertexCount, 4u);
  EXPECT_FALSE(first.opaque);
}

TEST_F(Crash3ProfileTest, ProjectedStrokesUseTheirIntrinsicDepthConstruction) {
  ps1::games::crash3::configure(
      {.enabled = true, .presentationDepthCorrections = true});
  auto first = surfaceOwner(0x80039EB8u, 1800u, {0u, 1u, 2u},
                            {4000.0f, 4000.0f, 4100.0f});
  auto second = surfaceOwner(0x80039EB8u, 1800u, {1u, 2u, 3u},
                             {4000.0f, 4100.0f, 4100.0f});
  const std::array triangles{
      ps1::games::crash3::PresentationSurfaceTriangle{&first},
      ps1::games::crash3::PresentationSurfaceTriangle{&second}};
  const auto line = ps1::games::crash3::planPresentedSurfaceDepth(triangles);
  ASSERT_TRUE(line.applies());
  EXPECT_FLOAT_EQ(line.multiplier, 0.5f);
  EXPECT_FLOAT_EQ(line.bias + 4000.0f * line.multiplier,
                  8192.0f + (2048.0f - 1800.0f) * 32.0f);
  first = surfaceOwner(0x80043648u, 4u, {0u, 1u, 2u}, {500, 500, 500});
  second = surfaceOwner(0x80043648u, 4u, {1u, 2u, 3u}, {500, 500, 500});
  const auto point = ps1::games::crash3::planPresentedSurfaceDepth(triangles);
  ASSERT_TRUE(point.applies());
  EXPECT_FLOAT_EQ(point.multiplier, 0.0f);
  EXPECT_EQ(std::bit_cast<uint32_t>(point.bias),
            ps1::games::crash3::CAMERA_BACKGROUND_DEPTH_BITS);
}

TEST_F(Crash3ProfileTest, PresentedSurfaceFailsClosedOnOtOrFamilyMismatch) {
  ps1::games::crash3::configure(
      {.enabled = true, .presentationDepthCorrections = true});
  auto first = surfaceOwner(0x80044938u, 1125u, {0u, 1u, 2u},
                            {6108.0f, 6366.0f, 6109.0f});
  auto second = surfaceOwner(0x80044938u, 1126u, {0u, 2u, 3u},
                             {6108.0f, 6109.0f, 6112.0f});
  std::array triangles{
      ps1::games::crash3::PresentationSurfaceTriangle{&first},
      ps1::games::crash3::PresentationSurfaceTriangle{&second}};
  EXPECT_EQ(ps1::games::crash3::planPresentedSurfaceDepth(triangles).status,
            PresentationSurfaceStatus::IncoherentOrderingTableAnchor);

  second = surfaceOwner(0x80012340u, 1125u, {0u, 2u, 3u},
                        {6108.0f, 6109.0f, 6112.0f});
  EXPECT_EQ(ps1::games::crash3::planPresentedSurfaceDepth(triangles).status,
            PresentationSurfaceStatus::UnsupportedProjectionFamily);
}

TEST_F(Crash3ProfileTest, RejectsUnknownLaneAndUnsafeDepth) {
  ps1::games::crash3::configure(
      {.enabled = true, .presentationDepthCorrections = true});
  auto owner = knownOwner();
  owner.provenance.vertices[0].gameProfileTag ^= 1u;
  auto points = vertices();
  EXPECT_EQ(ps1::games::crash3::planPresentationDepth(owner, points).status,
            PresentationDepthStatus::UnmappedOwner);

  points = vertices(200000000.0f);
  EXPECT_EQ(
      ps1::games::crash3::planPresentationDepth(knownOwner(), points).status,
      PresentationDepthStatus::ProjectiveWOutOfDomain);
}

} // namespace
