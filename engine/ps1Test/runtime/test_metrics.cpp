#include "runtime/metrics.h"

#include "runtime/gpu/gpu.h"
#include "runtime/memory.h"
#include "runtime/pgxp.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <sstream>

namespace {

class MetricsTest : public ::testing::Test {
protected:
  void SetUp() override {
    ps1::metrics::reset();
    ps1::metrics::setEnabledForTesting(true);
    ps1::resetPGXPDiagnosticsForTesting();
  }
  void TearDown() override {
    ps1::resetPGXPDiagnosticsForTesting();
    ps1::metrics::reset();
    ps1::metrics::setEnabledForTesting(false);
  }
};

uint32_t packGpuVertex(int x, int y) {
  return (static_cast<uint32_t>(y) & 0xffffu) << 16 |
         (static_cast<uint32_t>(x) & 0xffffu);
}

ps1::PGXPPolygonSemanticInput
fullTargetComposite(int targetX = 0, int width = 512, int height = 256) {
  ps1::PGXPPolygonSemanticInput input;
  input.opcode = 0x2a;
  input.vertexCount = 4;
  input.packedVertices = {packGpuVertex(0, 0), packGpuVertex(width, 0),
                          packGpuVertex(0, height),
                          packGpuVertex(width, height)};
  input.drawOffsetX = targetX;
  input.drawOffsetY = 0;
  input.drawX0 = targetX;
  input.drawY0 = 0;
  input.drawX1 = targetX + width - 1;
  input.drawY1 = height - 1;
  input.apertureWidth = static_cast<uint32_t>(width);
  input.apertureHeight = static_cast<uint32_t>(height);
  input.programmedDisplayGeometry = true;
  return input;
}

TEST_F(MetricsTest, CountAccumulatesAcrossCalls) {
  ps1::metrics::count("gp0.op.7C");
  ps1::metrics::count("gp0.op.7C", 5);
  EXPECT_EQ(ps1::metrics::get("gp0.op.7C"), 6u);
}

TEST_F(MetricsTest, CountIsUncappedPastLegacyLogLimits) {
  // The bug this whole effort exists for: the old logs stopped at 10 or 20.
  for (int i = 0; i < 5000; ++i)
    ps1::metrics::count("fill_rect");
  EXPECT_EQ(ps1::metrics::get("fill_rect"), 5000u);
}

TEST_F(MetricsTest, UnknownCounterReadsZero) {
  EXPECT_EQ(ps1::metrics::get("never.touched"), 0u);
}

TEST_F(MetricsTest, SetStateKeepsMostRecentValue) {
  ps1::metrics::setState("display.x", 0);
  ps1::metrics::setState("display.x", 512);
  EXPECT_EQ(ps1::metrics::getState("display.x"), 512);
}

TEST_F(MetricsTest, DisabledCollectorRecordsNothing) {
  ps1::metrics::setEnabledForTesting(false);
  ps1::metrics::count("ignored");
  EXPECT_EQ(ps1::metrics::get("ignored"), 0u);
}

TEST_F(MetricsTest, DumpJsonWritesCountersAndState) {
  ps1::metrics::count("frames", 42);
  ps1::metrics::setState("display.y", -7);

  const char *path = "/tmp/ps1_metrics_dump_test.json";
  std::remove(path);
  ASSERT_TRUE(ps1::metrics::dumpJsonTo(path));

  std::ifstream in(path);
  ASSERT_TRUE(in.good());
  std::stringstream buf;
  buf << in.rdbuf();
  const std::string body = buf.str();

  EXPECT_NE(body.find("\"frames\": 42"), std::string::npos) << body;
  EXPECT_NE(body.find("\"display.y\": -7"), std::string::npos) << body;
  std::remove(path);
}

// main_host.cpp reaches GPU::publishMetrics() from two shutdown paths -- the
// normal join and the forced `_Exit` taken whenever the recompiled game thread
// is still spinning -- and count() accumulates.  Publishing the whole
// histogram on both paths doubled every reported gp0.op.XX and gp0.words.
TEST_F(MetricsTest, GpuPublishMetricsDoesNotDoubleOnSecondShutdownPath) {
  ps1::gpu::GPU gpu;
  gpu.writeGP0(0xE1000000u); // draw mode
  gpu.writeGP0(0xE1000000u);
  gpu.writeGP0(0xE6000000u); // mask bit setting

  gpu.publishMetrics();
  ASSERT_EQ(ps1::metrics::get("gp0.op.E1"), 2u);
  ASSERT_EQ(ps1::metrics::get("gp0.op.E6"), 1u);
  ASSERT_EQ(ps1::metrics::get("gp0.words"), 3u);

  gpu.publishMetrics();
  EXPECT_EQ(ps1::metrics::get("gp0.op.E1"), 2u);
  EXPECT_EQ(ps1::metrics::get("gp0.op.E6"), 1u);
  EXPECT_EQ(ps1::metrics::get("gp0.words"), 3u);
}

// ...but the second call is not a plain no-op: the game thread keeps issuing
// GP0 words during main_host's 2s join deadline, and the forced-exit report
// has to include them so gp0.* stays consistent with the live counters
// (cpu_to_vram etc.) written in that same window.
TEST_F(MetricsTest, GpuPublishMetricsReportsTrafficBetweenCalls) {
  ps1::gpu::GPU gpu;
  gpu.writeGP0(0xE1000000u);
  gpu.publishMetrics();
  ASSERT_EQ(ps1::metrics::get("gp0.op.E1"), 1u);

  gpu.writeGP0(0xE1000000u);
  gpu.publishMetrics();
  EXPECT_EQ(ps1::metrics::get("gp0.op.E1"), 2u);
  EXPECT_EQ(ps1::metrics::get("gp0.words"), 2u);
}

TEST(PgxpSemanticTest, RecognizesExactProvenanceFreeFullTargetComposites) {
  const auto reference = fullTargetComposite();
  const auto decision = ps1::classifyPGXPPolygonSemantic(reference);
  EXPECT_EQ(decision.domain, ps1::PGXPSemanticDomain::Screen2D);
  EXPECT_EQ(decision.reason, ps1::PGXPSemanticReason::DisplayAnchoredComposite);

  auto alternateWinding = reference;
  alternateWinding.packedVertices = {packGpuVertex(0, 0), packGpuVertex(0, 256),
                                     packGpuVertex(512, 0),
                                     packGpuVertex(512, 256)};
  EXPECT_EQ(ps1::classifyPGXPPolygonSemantic(alternateWinding).domain,
            ps1::PGXPSemanticDomain::Screen2D);

  auto expectProjected = [](const ps1::PGXPPolygonSemanticInput &input) {
    EXPECT_EQ(ps1::classifyPGXPPolygonSemantic(input).domain,
              ps1::PGXPSemanticDomain::Projected3D);
  };

  auto opaque = reference;
  opaque.opcode = 0x28;
  expectProjected(opaque);

  auto textured = reference;
  textured.opcode = 0x2e;
  EXPECT_EQ(ps1::classifyPGXPPolygonSemantic(textured).domain,
            ps1::PGXPSemanticDomain::Screen2D);

  auto opaqueTextured = reference;
  opaqueTextured.opcode = 0x2c;
  expectProjected(opaqueTextured);

  auto rawTextured = reference;
  rawTextured.opcode = 0x2f;
  expectProjected(rawTextured);

  auto notFullTarget = reference;
  notFullTarget.packedVertices[1] = packGpuVertex(511, 0);
  expectProjected(notFullTarget);

  auto wrongOffset = reference;
  wrongOffset.drawOffsetX = 1;
  expectProjected(wrongOffset);

  auto wrongAperture = reference;
  wrongAperture.apertureHeight = 240;
  expectProjected(wrongAperture);

  auto implicitGeometry = reference;
  implicitGeometry.programmedDisplayGeometry = false;
  expectProjected(implicitGeometry);

  auto stripOrder = reference;
  stripOrder.packedVertices = {packGpuVertex(0, 0), packGpuVertex(512, 0),
                               packGpuVertex(512, 256), packGpuVertex(0, 256)};
  expectProjected(stripOrder);

  auto projectedEvidence = reference;
  projectedEvidence.precision[2].flags = ps1::PGXPValue::VALID_XY;
  projectedEvidence.precision[2].origin = ps1::PGXPOrigin::GteProjection;
  expectProjected(projectedEvidence);
}

TEST(PgxpSemanticTest,
     RecognizesOnlyDisplayAnchoredTexturedTransitionParallelograms) {
  auto input = fullTargetComposite(512);
  input.opcode = 0x2e;

  const std::array<std::array<uint32_t, 4>, 5> transitionShapes = {{
      {packGpuVertex(256, 128), packGpuVertex(-11, 134), packGpuVertex(253, -5),
       packGpuVertex(-14, 1)},
      {packGpuVertex(256, 128), packGpuVertex(-11, 134),
       packGpuVertex(259, 261), packGpuVertex(-8, 267)},
      {packGpuVertex(256, 128), packGpuVertex(523, 122), packGpuVertex(253, -5),
       packGpuVertex(520, -11)},
      {packGpuVertex(256, 128), packGpuVertex(523, 122),
       packGpuVertex(259, 261), packGpuVertex(526, 255)},
      {packGpuVertex(256, 64), packGpuVertex(384, 128), packGpuVertex(128, 128),
       packGpuVertex(256, 192)},
  }};
  for (const auto &shape : transitionShapes) {
    input.packedVertices = shape;
    const auto decision = ps1::classifyPGXPPolygonSemantic(input);
    EXPECT_EQ(decision.domain, ps1::PGXPSemanticDomain::Screen2D);
    EXPECT_EQ(decision.reason,
              ps1::PGXPSemanticReason::DisplayAnchoredComposite);
  }

  auto expectProjected = [&](const std::array<uint32_t, 4> &shape) {
    input.packedVertices = shape;
    EXPECT_EQ(ps1::classifyPGXPPolygonSemantic(input).domain,
              ps1::PGXPSemanticDomain::Projected3D);
  };

  // A regular centred quad is a plausible CPU-generated 3D billboard. Its
  // lack of a PGXP sidecar must never be treated as positive 2D evidence.
  expectProjected({packGpuVertex(200, 80), packGpuVertex(312, 80),
                   packGpuVertex(200, 176), packGpuVertex(312, 176)});

  // Target-relative anchoring is exact and fail-closed: merely looking like a
  // centred diamond or corner panel is insufficient.
  expectProjected({packGpuVertex(256, 80), packGpuVertex(352, 128),
                   packGpuVertex(160, 128), packGpuVertex(256, 176)});
  expectProjected({packGpuVertex(256, 128), packGpuVertex(-11, 134),
                   packGpuVertex(253, -5), packGpuVertex(-13, 1)});

  input.packedVertices = transitionShapes[0];
  input.precision[1].flags = ps1::PGXPValue::VALID_XY;
  EXPECT_EQ(ps1::classifyPGXPPolygonSemantic(input).domain,
            ps1::PGXPSemanticDomain::Projected3D);

  input.precision = {};
  input.opcode = 0x2c;
  EXPECT_EQ(ps1::classifyPGXPPolygonSemantic(input).domain,
            ps1::PGXPSemanticDomain::Projected3D);
}

TEST_F(MetricsTest, GpuRoutesExactFullTargetFlatBlendAsScreen2DPacket) {
  ps1::gpu::GPU gpu;

  // Explicit 16x16 programmed aperture keeps this integration test small
  // while exercising the same state contract as Crash's 512x256 pages.
  gpu.writeGP1(0x0800000au); // 512-wide mode: five dot clocks per pixel
  gpu.writeGP1(0x06000000u | 608u | (688u << 12));
  gpu.writeGP1(0x07000000u | 37u | (53u << 10));
  gpu.writeGP1(0x05000000u);
  gpu.writeGP0(0xe3000000u);
  gpu.writeGP0(0xe4000000u | 15u | (15u << 10));
  gpu.writeGP0(0xe5000000u);

  gpu.writeGP0(0x2a101010u);
  gpu.writeGP0(packGpuVertex(0, 0));
  gpu.writeGP0(packGpuVertex(16, 0));
  gpu.writeGP0(packGpuVertex(0, 16));
  gpu.writeGP0(packGpuVertex(16, 16));

  gpu.snapshotDisplayBuffer();
  gpu.publishMetrics();
  ps1::publishPGXPStageMetrics();

  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.class.screen_2d"), 4u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.class.projected_3d"), 0u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.xyz.not_applicable"), 4u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.xyz.missing"), 0u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.fallback.full_target_composite"),
            4u);
  EXPECT_EQ(ps1::metrics::get("gpu.frame.screen_2d_triangle"), 2u);
  EXPECT_EQ(ps1::metrics::get("gpu.frame.compatibility_triangle"), 0u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex_miss"), 0u);
}

TEST_F(MetricsTest, GpuRoutesDisplayAnchoredTexturedPanelAsScreen2DPacket) {
  ps1::gpu::GPU gpu;

  gpu.writeGP1(0x0800000au);
  gpu.writeGP1(0x06000000u | 608u | (688u << 12));
  gpu.writeGP1(0x07000000u | 37u | (53u << 10));
  gpu.writeGP1(0x05000000u);
  gpu.writeGP0(0xe3000000u);
  gpu.writeGP0(0xe4000000u | 15u | (15u << 10));
  gpu.writeGP0(0xe5000000u);

  // A 16x16 analogue of Crash's rotated top-left transition panel. The GP0
  // 2E layout alternates XY and UV words; all UVs may be zero for routing.
  gpu.writeGP0(0x2e808080u);
  gpu.writeGP0(packGpuVertex(8, 8));
  gpu.writeGP0(0u);
  gpu.writeGP0(packGpuVertex(-1, 9));
  gpu.writeGP0(0u);
  gpu.writeGP0(packGpuVertex(7, -1));
  gpu.writeGP0(0u);
  gpu.writeGP0(packGpuVertex(-2, 0));
  gpu.writeGP0(0u);

  gpu.snapshotDisplayBuffer();
  gpu.publishMetrics();
  ps1::publishPGXPStageMetrics();

  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.class.screen_2d"), 4u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.class.projected_3d"), 0u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.xyz.not_applicable"), 4u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.xyz.missing"), 0u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.fallback.full_target_composite"),
            4u);
  EXPECT_EQ(ps1::metrics::get("gpu.frame.screen_2d_triangle"), 2u);
  EXPECT_EQ(ps1::metrics::get("gpu.frame.compatibility_triangle"), 0u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex_miss"), 0u);
}

TEST_F(MetricsTest, PgxpClassifiesEvery2DAnd3DCoordinateWithoutInventingUiZ) {
  ps1::gpu::GPU gpu;

  gpu.writeGP0(0x400000ffu); // line: two intentional Screen2D operands
  gpu.writeGP0((10u << 16) | 10u);
  gpu.writeGP0((10u << 16) | 15u);
  gpu.writeGP0(0x7800ff00u); // rectangle: one Screen2D origin operand
  gpu.writeGP0((30u << 16) | 20u);

  const uint32_t words[3] = {(40u << 16) | 40u, (40u << 16) | 50u,
                             (50u << 16) | 40u};
  ps1::PGXPValue precision[3];
  for (int i = 0; i < 3; ++i) {
    precision[i].x = static_cast<float>(words[i] & 0xffffu) + 0.25f;
    precision[i].y = static_cast<float>(words[i] >> 16) + 0.5f;
    precision[i].z = 100.0f + static_cast<float>(i);
    precision[i].value = words[i];
    precision[i].flags = ps1::PGXPValue::VALID_XY | ps1::PGXPValue::VALID_Z;
    precision[i].origin = ps1::PGXPOrigin::GteProjection;
  }
  gpu.writeGP0(0x200000ffu);
  for (int i = 0; i < 3; ++i)
    gpu.writeGP0(words[i], UINT32_MAX, &precision[i]);

  ps1::publishPGXPStageMetrics();
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.total"), 6u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.classified"), 6u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.class.unknown"), 0u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.class.screen_2d"), 3u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.class.projected_3d"), 3u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.xyz.valid"), 3u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.xyz.required"), 3u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.xyz.missing"), 0u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.xyz.not_applicable"), 3u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.fallback.intentional_2d"), 3u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.primitive.line.total"), 2u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.primitive.rectangle.total"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.primitive.flat_triangle.total"), 3u);
}

TEST_F(MetricsTest,
       PgxpReconcilesUnsafeScreenComponentsWithoutAcceptingUnsafeDepth) {
  constexpr uint32_t packed = (20u << 16) | 10u;
  auto projection = [] {
    ps1::PGXPValue value;
    value.x = 10.25f;
    value.y = 20.5f;
    value.z = 100.0f;
    value.value = packed;
    value.flags = ps1::PGXPValue::VALID_XY | ps1::PGXPValue::VALID_Z;
    value.origin = ps1::PGXPOrigin::GteProjection;
    return value;
  };
  auto record = [&](uint32_t source, const ps1::PGXPValue &value) {
    ps1::recordPGXPVertex(0x30, 1, source, ps1::PGXPPrimitive::GouraudTriangle,
                          ps1::PGXPSemanticDecision::projected3D(), value,
                          packed);
  };

  auto divergentX = projection();
  divergentX.x = 19.0f;
  const ps1::PGXPResolvedVertex resolvedX =
      ps1::resolvePGXPVertex(divergentX, packed);
  ASSERT_TRUE(resolvedX.hasDepth);
  EXPECT_FLOAT_EQ(resolvedX.x, 10.0f);
  EXPECT_FLOAT_EQ(resolvedX.y, 20.5f);
  EXPECT_FLOAT_EQ(resolvedX.z, 100.0f);
  EXPECT_EQ(resolvedX.xSource, ps1::PGXPCoordinateSource::NativeDelta);
  EXPECT_EQ(resolvedX.ySource, ps1::PGXPCoordinateSource::Precise);
  EXPECT_EQ(resolvedX.reconciledMask(), ps1::PGXPResolvedVertex::RECONCILED_X);
  record(0x80010004u, divergentX);

  auto nonfiniteY = projection();
  nonfiniteY.y = std::numeric_limits<float>::infinity();
  const ps1::PGXPResolvedVertex resolvedY =
      ps1::resolvePGXPVertex(nonfiniteY, packed);
  ASSERT_TRUE(resolvedY.hasDepth);
  EXPECT_FLOAT_EQ(resolvedY.x, 10.25f);
  EXPECT_FLOAT_EQ(resolvedY.y, 20.0f);
  EXPECT_EQ(resolvedY.xSource, ps1::PGXPCoordinateSource::Precise);
  EXPECT_EQ(resolvedY.ySource, ps1::PGXPCoordinateSource::NativeNonFinite);
  record(0x80010008u, nonfiniteY);

  auto divergentXY = projection();
  divergentXY.x = 19.0f;
  divergentXY.y = 29.0f;
  const ps1::PGXPResolvedVertex resolvedXY =
      ps1::resolvePGXPVertex(divergentXY, packed);
  ASSERT_TRUE(resolvedXY.hasDepth);
  EXPECT_EQ(resolvedXY.reconciledMask(),
            ps1::PGXPResolvedVertex::RECONCILED_X |
                ps1::PGXPResolvedVertex::RECONCILED_Y);
  record(0x8001000cu, divergentXY);

  auto stale = divergentXY;
  stale.value ^= 1u;
  const ps1::PGXPResolvedVertex resolvedStale =
      ps1::resolvePGXPVertex(stale, packed);
  EXPECT_FALSE(resolvedStale.hasDepth);
  EXPECT_EQ(resolvedStale.reconciledMask(), 0u);
  record(0x80010010u, stale);

  auto nonfiniteZ = divergentXY;
  nonfiniteZ.z = std::numeric_limits<float>::infinity();
  const ps1::PGXPResolvedVertex resolvedNonfiniteZ =
      ps1::resolvePGXPVertex(nonfiniteZ, packed);
  EXPECT_FALSE(resolvedNonfiniteZ.hasDepth);
  EXPECT_TRUE(resolvedNonfiniteZ.hasScreenPosition());
  EXPECT_EQ(resolvedNonfiniteZ.reconciledMask(),
            ps1::PGXPResolvedVertex::RECONCILED_X |
                ps1::PGXPResolvedVertex::RECONCILED_Y)
      << "invalid W must not discard the donor's independently usable XY";
  record(0x80010014u, nonfiniteZ);

  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.reconcile.total"), 3u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.reconcile.x"), 2u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.reconcile.y"), 2u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.reconcile.xy"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.reconcile.x.delta"), 2u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.reconcile.x.nonfinite"), 0u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.reconcile.y.delta"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.reconcile.y.nonfinite"), 1u);
}

TEST_F(MetricsTest,
       PgxpAcceptsCapturedSignedProjectionPastLegacyDeltaWithinGuardBand) {
  // Crash 3, producer VBlank 4872: opcode 2Eh, word 3 at 80076B3Ch.
  // Fractional view Z makes the authentic X projection differ from packed
  // SXY by 8.676 px, just beyond the old generic plausibility threshold.
  constexpr uint32_t packed = 0x03FFFC30u; // native (-976, 1023)
  ps1::PGXPValue live;
  live.x = -967.323974609375f;
  live.y = 1023.0f;
  live.z = 152.896484375;
  live.value = packed;
  live.flags = ps1::PGXPValue::VALID_XY | ps1::PGXPValue::VALID_Z |
               ps1::PGXPValue::VALID_SIGNED_PROJECTION;
  live.origin = ps1::PGXPOrigin::GteProjection;
  live.ramLookup = ps1::PGXPRamLookup::HitXYZ;
  live.submissionPath = ps1::PGXPSubmissionPath::HleOrderingTable;
  live.qx = static_cast<double>(live.x) * live.z;
  live.qy = static_cast<double>(live.y) * live.z;
  live.nearDepth = 144.000244140625;
  live.projectionId = 0x1234;
  live.projectionContext = 0x5678;

  const ps1::PGXPResolvedVertex resolved = ps1::resolvePGXPVertex(live, packed);
  ASSERT_TRUE(resolved.hasSignedProjection);
  ASSERT_TRUE(resolved.hasDepth);
  EXPECT_EQ(resolved.xSource, ps1::PGXPCoordinateSource::Precise);
  EXPECT_EQ(resolved.ySource, ps1::PGXPCoordinateSource::Precise);
  EXPECT_FLOAT_EQ(resolved.x, live.x);
  EXPECT_FLOAT_EQ(resolved.y, live.y);
  EXPECT_EQ(resolved.reconciledMask(), 0u);

  ps1::recordPGXPVertex(0x2e, 3, 0x80076B3Cu, ps1::PGXPPrimitive::TexturedQuad,
                        ps1::PGXPSemanticDecision::projected3D(), live, packed);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.reconcile.total"), 0u);
}

TEST_F(MetricsTest,
       PgxpSignedProjectionGuardKeepsUnsafeAndLegacyInputsFailClosed) {
  constexpr uint32_t packed = 0x03FFFC30u; // native (-976, 1023)
  auto liveProjection = [] {
    ps1::PGXPValue value;
    value.x = -967.323974609375f;
    value.y = 1023.0f;
    value.z = 152.896484375;
    value.value = packed;
    value.flags = ps1::PGXPValue::VALID_XY | ps1::PGXPValue::VALID_Z |
                  ps1::PGXPValue::VALID_SIGNED_PROJECTION;
    value.origin = ps1::PGXPOrigin::GteProjection;
    value.ramLookup = ps1::PGXPRamLookup::HitXYZ;
    value.submissionPath = ps1::PGXPSubmissionPath::HleOrderingTable;
    value.qx = static_cast<double>(value.x) * value.z;
    value.qy = static_cast<double>(value.y) * value.z;
    value.nearDepth = 144.000244140625;
    value.projectionId = 0x1234;
    value.projectionContext = 0x5678;
    return value;
  };

  auto legacy = liveProjection();
  legacy.flags &= ~ps1::PGXPValue::VALID_SIGNED_PROJECTION;
  const ps1::PGXPResolvedVertex resolvedLegacy =
      ps1::resolvePGXPVertex(legacy, packed);
  EXPECT_TRUE(resolvedLegacy.hasDepth);
  EXPECT_FALSE(resolvedLegacy.hasSignedProjection);
  EXPECT_EQ(resolvedLegacy.xSource, ps1::PGXPCoordinateSource::NativeDelta);
  EXPECT_EQ(resolvedLegacy.ySource, ps1::PGXPCoordinateSource::Precise);

  auto nonfinite = liveProjection();
  nonfinite.x = std::numeric_limits<float>::quiet_NaN();
  const ps1::PGXPResolvedVertex resolvedNonfinite =
      ps1::resolvePGXPVertex(nonfinite, packed);
  EXPECT_TRUE(resolvedNonfinite.hasSignedProjection);
  EXPECT_EQ(resolvedNonfinite.xSource,
            ps1::PGXPCoordinateSource::NativeNonFinite);
  EXPECT_EQ(resolvedNonfinite.ySource, ps1::PGXPCoordinateSource::Precise);

  auto falseOrigin = liveProjection();
  falseOrigin.origin = ps1::PGXPOrigin::Unknown;
  const ps1::PGXPResolvedVertex resolvedFalseOrigin =
      ps1::resolvePGXPVertex(falseOrigin, packed);
  EXPECT_FALSE(falseOrigin.hasSignedProjection(packed));
  EXPECT_FALSE(resolvedFalseOrigin.hasSignedProjection);
  EXPECT_TRUE(resolvedFalseOrigin.hasDepth)
      << "valid legacy XYZ may remain usable, but homogeneous Q may not";
  EXPECT_EQ(resolvedFalseOrigin.xSource,
            ps1::PGXPCoordinateSource::NativeDelta);
  EXPECT_EQ(resolvedFalseOrigin.ySource, ps1::PGXPCoordinateSource::Precise);

  auto invalidNear = liveProjection();
  invalidNear.nearDepth = 0.0;
  const ps1::PGXPResolvedVertex resolvedInvalidNear =
      ps1::resolvePGXPVertex(invalidNear, packed);
  EXPECT_FALSE(invalidNear.hasSignedProjection(packed));
  EXPECT_FALSE(resolvedInvalidNear.hasSignedProjection);
  EXPECT_TRUE(resolvedInvalidNear.hasDepth);
  EXPECT_EQ(resolvedInvalidNear.xSource,
            ps1::PGXPCoordinateSource::NativeDelta);
  EXPECT_EQ(resolvedInvalidNear.ySource, ps1::PGXPCoordinateSource::Precise);

  // Positive W below the recorded near plane still follows the donor's
  // "every W>0" contract. The raster policy may saturate or geometrically
  // clip it later; the resolver must not discard authentic Q/W first.
  auto positiveBelowNear = liveProjection();
  positiveBelowNear.z = 100.0;
  positiveBelowNear.qx =
      static_cast<double>(positiveBelowNear.x) * positiveBelowNear.z;
  positiveBelowNear.qy =
      static_cast<double>(positiveBelowNear.y) * positiveBelowNear.z;
  const ps1::PGXPResolvedVertex resolvedPositiveBelowNear =
      ps1::resolvePGXPVertex(positiveBelowNear, packed);
  EXPECT_TRUE(resolvedPositiveBelowNear.hasSignedProjection);
  EXPECT_TRUE(resolvedPositiveBelowNear.hasDepth);
  EXPECT_EQ(resolvedPositiveBelowNear.xSource,
            ps1::PGXPCoordinateSource::PreciseGteSaturated);
  EXPECT_EQ(resolvedPositiveBelowNear.ySource,
            ps1::PGXPCoordinateSource::PreciseGteSaturated);

  // Crash 3 demo transition: W is below the authentic GTE near threshold,
  // so Q/W is intentionally far beyond the signed-11 SXY produced by the
  // saturated divider. The clamped SXY remains exact donor provenance.
  constexpr uint32_t saturatedPacked = 0x03FFFC00u; // native (-1024, 1023)
  auto saturated = liveProjection();
  saturated.value = saturatedPacked;
  saturated.x = -1024.0f;
  saturated.y = 1023.0f;
  saturated.z = 8.0;
  saturated.qx = -209628.625;
  saturated.qy = 136427.3125;
  const ps1::PGXPResolvedVertex resolvedSaturated =
      ps1::resolvePGXPVertex(saturated, saturatedPacked);
  EXPECT_TRUE(resolvedSaturated.hasSignedProjection);
  EXPECT_TRUE(resolvedSaturated.hasDepth);
  EXPECT_EQ(resolvedSaturated.xSource,
            ps1::PGXPCoordinateSource::PreciseGteSaturated);
  EXPECT_EQ(resolvedSaturated.ySource,
            ps1::PGXPCoordinateSource::PreciseGteSaturated);
  EXPECT_FLOAT_EQ(resolvedSaturated.x, saturated.x);
  EXPECT_FLOAT_EQ(resolvedSaturated.y, saturated.y);
  EXPECT_EQ(resolvedSaturated.reconciledMask(), 0u);

  auto saturatedOutsideSxy = saturated;
  saturatedOutsideSxy.x = -1024.25f;
  const ps1::PGXPResolvedVertex resolvedOutsideSxy =
      ps1::resolvePGXPVertex(saturatedOutsideSxy, saturatedPacked);
  EXPECT_EQ(resolvedOutsideSxy.xSource,
            ps1::PGXPCoordinateSource::NativeGuardBand);
  EXPECT_EQ(resolvedOutsideSxy.ySource,
            ps1::PGXPCoordinateSource::NativeGuardBand);

  auto zeroContext = liveProjection();
  zeroContext.projectionContext = 0;
  const ps1::PGXPResolvedVertex resolvedZeroContext =
      ps1::resolvePGXPVertex(zeroContext, packed);
  EXPECT_EQ(resolvedZeroContext.xSource, ps1::PGXPCoordinateSource::Precise)
      << "projectionContext is a consistency hash, not a validity flag";

  auto extreme = liveProjection();
  extreme.x = -976.0f;
  extreme.qx = (ps1::PGXP_SIGNED_PROJECTION_GUARD_BAND + 0.25) * extreme.z;
  const ps1::PGXPResolvedVertex resolvedExtreme =
      ps1::resolvePGXPVertex(extreme, packed);
  EXPECT_TRUE(resolvedExtreme.hasSignedProjection);
  EXPECT_TRUE(resolvedExtreme.hasDepth);
  EXPECT_FLOAT_EQ(resolvedExtreme.x, -976.0f);
  EXPECT_FLOAT_EQ(resolvedExtreme.y, 1023.0f);
  EXPECT_EQ(resolvedExtreme.xSource,
            ps1::PGXPCoordinateSource::NativeGuardBand);
  EXPECT_EQ(resolvedExtreme.ySource,
            ps1::PGXPCoordinateSource::NativeGuardBand);
  EXPECT_EQ(resolvedExtreme.reconciledMask(),
            ps1::PGXPResolvedVertex::RECONCILED_X |
                ps1::PGXPResolvedVertex::RECONCILED_Y);

  auto stale = liveProjection();
  stale.value ^= 1u;
  const ps1::PGXPResolvedVertex resolvedStale =
      ps1::resolvePGXPVertex(stale, packed);
  EXPECT_FALSE(resolvedStale.hasSignedProjection);
  EXPECT_FALSE(resolvedStale.hasDepth);
  EXPECT_FALSE(resolvedStale.hasScreenPosition());
  EXPECT_EQ(resolvedStale.reconciledMask(), 0u);

  ps1::metrics::setTraceFrame(4872);
  ps1::recordPGXPVertex(0x2e, 3, 0x80076B3Cu, ps1::PGXPPrimitive::TexturedQuad,
                        ps1::PGXPSemanticDecision::projected3D(), extreme,
                        packed);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.reconcile.total"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.reconcile.x.guard_band"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.reconcile.y.guard_band"), 1u);
  EXPECT_EQ(ps1::metrics::getState("pgxp.vertex.reconcile.last.frame"), 4872);
  EXPECT_EQ(ps1::metrics::getState("pgxp.vertex.reconcile.last.source"),
            static_cast<int64_t>(0x80076B3Cu));
  EXPECT_EQ(ps1::metrics::getState("pgxp.vertex.reconcile.last.packet_head"),
            static_cast<int64_t>(0x80076B30u));
  EXPECT_EQ(ps1::metrics::getState("pgxp.vertex.reconcile.last.opcode"), 0x2e);
  EXPECT_EQ(ps1::metrics::getState("pgxp.vertex.reconcile.last.component_mask"),
            ps1::PGXPResolvedVertex::RECONCILED_X |
                ps1::PGXPResolvedVertex::RECONCILED_Y);
  EXPECT_EQ(ps1::metrics::getState("pgxp.vertex.reconcile.last.projection_id"),
            0x1234);
  EXPECT_EQ(
      ps1::metrics::getState("pgxp.vertex.reconcile.last.projection_context"),
      0x5678);
  EXPECT_EQ(
      ps1::metrics::getState("pgxp.vertex.reconcile.last.signed_projection"),
      1);

  ps1::recordPGXPVertex(0x2e, 7, 0x80093720u, ps1::PGXPPrimitive::TexturedQuad,
                        ps1::PGXPSemanticDecision::projected3D(), saturated,
                        saturatedPacked);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.near_plane"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.gte_saturated_screen.precise"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.reconcile.total"), 1u)
      << "authenticated saturation must not add a reconciliation";
}

TEST_F(MetricsTest, PgxpReportsSignedProjectionSeparatelyFromXYOnly) {
  constexpr uint32_t packed = (20u << 16) | 10u;
  ps1::PGXPValue projection;
  projection.x = 19.0f;
  projection.y = 20.0f;
  projection.z = -10.0f;
  projection.value = packed;
  projection.flags =
      ps1::PGXPValue::VALID_XY | ps1::PGXPValue::VALID_SIGNED_PROJECTION;
  projection.origin = ps1::PGXPOrigin::GteProjection;
  projection.submissionPath = ps1::PGXPSubmissionPath::HleOrderingTable;
  projection.qx = -100.0f;
  projection.qy = -200.0f;
  projection.nearDepth = 500.0f + 1.0f / 4096.0f;
  projection.projectionId = 77;
  projection.projectionContext = 9;

  ps1::Memory memory;
  constexpr uint32_t address = 0x80010000u;
  memory.write32Tracked(address, packed, &projection);
  ps1::PGXPValue loaded;
  ASSERT_TRUE(memory.lookupPrecision(address, packed, loaded));
  loaded.ramLookup = ps1::consumePGXPRamLookup(address, packed);
  EXPECT_EQ(loaded.ramLookup, ps1::PGXPRamLookup::HitSignedProjection);

  ps1::recordPGXPVertex(0x30, 1, address, ps1::PGXPPrimitive::GouraudTriangle,
                        ps1::PGXPSemanticDecision::projected3D(), loaded,
                        packed);
  ps1::countPGXPStage(ps1::PGXPStage::ProjectionSigned);
  ps1::countPGXPStage(ps1::PGXPStage::Gp0WordSigned);
  ps1::publishPGXPStageMetrics();

  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.precision.signed_projection"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.precision.xy_only"), 0u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.fallback.none"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.lookup.hit_signed_projection"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.xyz.valid"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.xyz.missing"), 0u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.near_plane"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.reconcile.total"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.reconcile.x"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.reconcile.x.delta"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.projection.signed"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.gp0_word.signed"), 1u);
}

TEST_F(MetricsTest, PgxpIdentifiesExactProjectionValueLostDuringPacketCopy) {
  constexpr uint32_t packed = (120u << 16) | 200u;
  ps1::PGXPValue projected;
  projected.x = 200.25f;
  projected.y = 120.5f;
  projected.z = 900.0f;
  projected.value = packed;
  projected.flags = ps1::PGXPValue::VALID_XY | ps1::PGXPValue::VALID_Z;
  projected.origin = ps1::PGXPOrigin::GteProjection;
  ps1::recordPGXPProjection(projected);

  ps1::PGXPValue lost;
  lost.ramLookup = ps1::PGXPRamLookup::Empty;
  lost.submissionPath = ps1::PGXPSubmissionPath::HleOrderingTable;
  ps1::recordPGXPVertex(0x2c, 1, 0x80010000u, ps1::PGXPPrimitive::TexturedQuad,
                        ps1::PGXPSemanticDecision::projected3D(), lost, packed);
  ps1::publishPGXPStageMetrics();

  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.class.projected_3d"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.precision.none"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.lookup.empty"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.fallback.packet_copy_loss"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.xyz.required"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.xyz.valid"), 0u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.xyz.missing"), 1u);
  EXPECT_EQ(ps1::metrics::get(
                "pgxp.vertex.source.010000.op.2C.w1.hle_otag.textured_quad."
                "none.empty.packet_copy_loss"),
            1u);
}

TEST_F(MetricsTest, PgxpPublicationUsesDeltasOnRepeatedShutdownPaths) {
  ps1::PGXPValue none;
  ps1::recordPGXPVertex(0x20, 1, UINT32_MAX, ps1::PGXPPrimitive::FlatTriangle,
                        ps1::PGXPSemanticDecision::projected3D(), none,
                        0x00100010u);
  ps1::publishPGXPStageMetrics();
  ps1::publishPGXPStageMetrics();
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.total"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.classified"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.classified.total"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.classified.unknown"), 0u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.class.projected_3d"), 1u);
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.fallback.cpu_generated"), 1u);
}

TEST(MetricsTraceTest, WritesOrderedJsonlWithFrameAndFields) {
  const char *path = "/tmp/ps1_event_trace_test.jsonl";
  std::remove(path);
  setenv("PS1_TRACE_JSONL", path, 1);
  setenv("PS1_TRACE_SOURCE", "unit-test", 1);
  ps1::metrics::resetTraceForTesting();
  ps1::metrics::setTraceFrame(17);
  ps1::metrics::trace("gpu.display_start", {{"x", 512}, {"y", 0}});

  std::ifstream in(path);
  ASSERT_TRUE(in.good());
  std::stringstream buf;
  buf << in.rdbuf();
  const std::string body = buf.str();
  EXPECT_NE(body.find("\"schema\":\"ps1trace/v1\""), std::string::npos);
  EXPECT_NE(body.find("\"source\":\"unit-test\""), std::string::npos);
  EXPECT_NE(body.find("\"frame\":17"), std::string::npos);
  EXPECT_NE(body.find("\"event\":\"gpu.display_start\""), std::string::npos);
  EXPECT_NE(body.find("\"x\":512"), std::string::npos);
  ps1::metrics::resetTraceForTesting();
  unsetenv("PS1_TRACE_JSONL");
  unsetenv("PS1_TRACE_SOURCE");
  std::remove(path);
}

TEST(MetricsTraceTest, ExplicitFrameSurvivesAsynchronousConsumption) {
  const char *path = "/tmp/ps1_event_trace_explicit_frame_test.jsonl";
  std::remove(path);
  setenv("PS1_TRACE_JSONL", path, 1);
  ps1::metrics::resetTraceForTesting();
  ps1::metrics::setTraceFrame(99);
  ps1::metrics::traceAtFrame("host.present", 17, {{"display_x", 512}});

  std::ifstream in(path);
  ASSERT_TRUE(in.good());
  std::stringstream buf;
  buf << in.rdbuf();
  const std::string body = buf.str();
  EXPECT_NE(body.find("\"frame\":17"), std::string::npos) << body;
  EXPECT_EQ(body.find("\"frame\":99"), std::string::npos) << body;
  EXPECT_NE(body.find("\"event\":\"host.present\""), std::string::npos) << body;

  ps1::metrics::resetTraceForTesting();
  unsetenv("PS1_TRACE_JSONL");
  std::remove(path);
}

TEST(MetricsTraceTest, ExactEventAllowlistDropsHighVolumeNeighbours) {
  const char *path = "/tmp/ps1_event_trace_allowlist_test.jsonl";
  std::remove(path);
  setenv("PS1_TRACE_JSONL", path, 1);
  setenv("PS1_TRACE_EVENTS", " gte.projection ; gpu.gp0_word ", 1);
  ps1::metrics::resetTraceForTesting();
  ps1::metrics::setTraceFrame(23);

  ps1::metrics::trace("gpu.polygon_route", {{"route", 1}});
  ps1::metrics::trace("gpu.gp0_word", {{"word", 0x28000000}});
  ps1::metrics::trace("gte.projection", {{"projection_id", 7}});
  ps1::metrics::trace("gpu.gp0_word.extra", {{"word", 1}});

  std::ifstream in(path);
  ASSERT_TRUE(in.good());
  std::stringstream buf;
  buf << in.rdbuf();
  const std::string body = buf.str();
  EXPECT_NE(body.find("\"event\":\"gpu.gp0_word\""), std::string::npos)
      << body;
  EXPECT_NE(body.find("\"event\":\"gte.projection\""), std::string::npos)
      << body;
  EXPECT_EQ(body.find("gpu.polygon_route"), std::string::npos) << body;
  EXPECT_EQ(body.find("gpu.gp0_word.extra"), std::string::npos) << body;

  ps1::metrics::resetTraceForTesting();
  unsetenv("PS1_TRACE_JSONL");
  unsetenv("PS1_TRACE_EVENTS");
  std::remove(path);
}

TEST(MetricsTraceTest, GteTraceHonorsInclusiveFrameWindow) {
  setenv("PS1_TRACE_JSONL", "/tmp/ps1_gte_window_test.jsonl", 1);
  setenv("PS1_TRACE_GTE", "1", 1);
  setenv("PS1_TRACE_GTE_FRAME_FROM", "10", 1);
  setenv("PS1_TRACE_GTE_FRAME_TO", "20", 1);
  ps1::metrics::resetTraceForTesting();

  ps1::metrics::setTraceFrame(9);
  EXPECT_FALSE(ps1::metrics::gteTraceEnabled());
  ps1::metrics::setTraceFrame(10);
  EXPECT_TRUE(ps1::metrics::gteTraceEnabled());
  ps1::metrics::setTraceFrame(20);
  EXPECT_TRUE(ps1::metrics::gteTraceEnabled());
  ps1::metrics::setTraceFrame(21);
  EXPECT_FALSE(ps1::metrics::gteTraceEnabled());

  unsetenv("PS1_TRACE_JSONL");
  unsetenv("PS1_TRACE_GTE");
  unsetenv("PS1_TRACE_GTE_FRAME_FROM");
  unsetenv("PS1_TRACE_GTE_FRAME_TO");
  ps1::metrics::resetTraceForTesting();
}

TEST(MetricsTraceTest, GteTraceCanBeAnchoredToFirstCheckpointFrame) {
  setenv("PS1_TRACE_JSONL", "/tmp/ps1_gte_trigger_test.jsonl", 1);
  setenv("PS1_TRACE_GTE", "1", 1);
  setenv("PS1_TRACE_GTE_TRIGGER_PC", "0x80039FA0", 1);
  setenv("PS1_TRACE_GTE_TRIGGER_FRAMES", "3", 1);
  ps1::metrics::resetTraceForTesting();

  ps1::metrics::setTraceFrame(9);
  EXPECT_FALSE(ps1::metrics::gteTraceEnabled());
  ps1::metrics::setTraceFrame(17);
  ps1::metrics::triggerGteTraceForCurrentFrame();
  EXPECT_TRUE(ps1::metrics::gteTraceEnabled());
  ps1::metrics::setTraceFrame(18);
  EXPECT_TRUE(ps1::metrics::gteTraceEnabled());
  ps1::metrics::setTraceFrame(19);
  EXPECT_TRUE(ps1::metrics::gteTraceEnabled());
  ps1::metrics::setTraceFrame(20);
  EXPECT_FALSE(ps1::metrics::gteTraceEnabled());
  ps1::metrics::triggerGteTraceForCurrentFrame();
  EXPECT_FALSE(ps1::metrics::gteTraceEnabled())
      << "the first semantic checkpoint must remain the trace anchor";

  unsetenv("PS1_TRACE_JSONL");
  unsetenv("PS1_TRACE_GTE");
  unsetenv("PS1_TRACE_GTE_TRIGGER_PC");
  unsetenv("PS1_TRACE_GTE_TRIGGER_FRAMES");
  ps1::metrics::resetTraceForTesting();
  std::remove("/tmp/ps1_gte_trigger_test.jsonl");
}

TEST(MetricsTraceTest,
     SemanticTraceCanSelectExactCheckpointHitIndependentOfFrame) {
  setenv("PS1_TRACE_JSONL", "/tmp/ps1_trigger_hit_test.jsonl", 1);
  setenv("PS1_TRACE_GTE", "1", 1);
  setenv("PS1_TRACE_GTE_TRIGGER_PC", "0x800417AC", 1);
  setenv("PS1_TRACE_GTE_TRIGGER_HIT", "3", 1);
  setenv("PS1_TRACE_GPU_WORDS", "1", 1);
  setenv("PS1_TRACE_GPU_TRIGGER_PC", "0x800417AC", 1);
  setenv("PS1_TRACE_GPU_TRIGGER_HIT", "3", 1);
  ps1::metrics::resetTraceForTesting();

  for (uint64_t frame : {40u, 40u}) {
    ps1::metrics::setTraceFrame(frame);
    ps1::metrics::triggerGteTraceForCurrentFrame();
    ps1::metrics::triggerGpuWordTraceForCurrentFrame();
    EXPECT_FALSE(ps1::metrics::gteTraceEnabled());
    EXPECT_FALSE(ps1::metrics::gpuWordTraceEnabled());
  }

  ps1::metrics::setTraceFrame(99);
  ps1::metrics::triggerGteTraceForCurrentFrame();
  ps1::metrics::triggerGpuWordTraceForCurrentFrame();
  EXPECT_TRUE(ps1::metrics::gteTraceEnabled());
  EXPECT_TRUE(ps1::metrics::gpuWordTraceEnabled());

  ps1::metrics::setTraceFrame(99);
  ps1::metrics::triggerGteTraceForCurrentFrame();
  ps1::metrics::triggerGpuWordTraceForCurrentFrame();
  EXPECT_TRUE(ps1::metrics::gteTraceEnabled())
      << "later hits must not move the exact ordinal anchor";
  EXPECT_TRUE(ps1::metrics::gpuWordTraceEnabled());

  unsetenv("PS1_TRACE_JSONL");
  unsetenv("PS1_TRACE_GTE");
  unsetenv("PS1_TRACE_GTE_TRIGGER_PC");
  unsetenv("PS1_TRACE_GTE_TRIGGER_HIT");
  unsetenv("PS1_TRACE_GPU_WORDS");
  unsetenv("PS1_TRACE_GPU_TRIGGER_PC");
  unsetenv("PS1_TRACE_GPU_TRIGGER_HIT");
  ps1::metrics::resetTraceForTesting();
  std::remove("/tmp/ps1_trigger_hit_test.jsonl");
}

TEST(MetricsTraceTest, GpuWordTraceHonorsInclusiveFrameWindow) {
  setenv("PS1_TRACE_JSONL", "/tmp/ps1_gpu_word_window_test.jsonl", 1);
  setenv("PS1_TRACE_GPU_WORDS", "1", 1);
  setenv("PS1_TRACE_GPU_FRAME_FROM", "42", 1);
  setenv("PS1_TRACE_GPU_FRAME_TO", "43", 1);
  ps1::metrics::resetTraceForTesting();

  ps1::metrics::setTraceFrame(41);
  EXPECT_FALSE(ps1::metrics::gpuWordTraceEnabled());
  ps1::metrics::setTraceFrame(42);
  EXPECT_TRUE(ps1::metrics::gpuWordTraceEnabled());
  ps1::metrics::setTraceFrame(43);
  EXPECT_TRUE(ps1::metrics::gpuWordTraceEnabled());
  ps1::metrics::setTraceFrame(44);
  EXPECT_FALSE(ps1::metrics::gpuWordTraceEnabled());

  unsetenv("PS1_TRACE_JSONL");
  unsetenv("PS1_TRACE_GPU_WORDS");
  unsetenv("PS1_TRACE_GPU_FRAME_FROM");
  unsetenv("PS1_TRACE_GPU_FRAME_TO");
  ps1::metrics::resetTraceForTesting();
  std::remove("/tmp/ps1_gpu_word_window_test.jsonl");
}

TEST(MetricsTraceTest, GpuWordTraceCanFollowSemanticCheckpointForTwoFrames) {
  setenv("PS1_TRACE_JSONL", "/tmp/ps1_gpu_word_trigger_test.jsonl", 1);
  setenv("PS1_TRACE_GPU_WORDS", "1", 1);
  setenv("PS1_TRACE_GPU_TRIGGER_PC", "0x800417AC", 1);
  ps1::metrics::resetTraceForTesting();

  ps1::metrics::setTraceFrame(100);
  EXPECT_FALSE(ps1::metrics::gpuWordTraceEnabled());
  ps1::metrics::triggerGpuWordTraceForCurrentFrame();
  EXPECT_TRUE(ps1::metrics::gpuWordTraceEnabled());
  ps1::metrics::setTraceFrame(101);
  EXPECT_TRUE(ps1::metrics::gpuWordTraceEnabled());
  ps1::metrics::setTraceFrame(102);
  EXPECT_FALSE(ps1::metrics::gpuWordTraceEnabled());
  ps1::metrics::triggerGpuWordTraceForCurrentFrame();
  EXPECT_FALSE(ps1::metrics::gpuWordTraceEnabled());

  unsetenv("PS1_TRACE_JSONL");
  unsetenv("PS1_TRACE_GPU_WORDS");
  unsetenv("PS1_TRACE_GPU_TRIGGER_PC");
  ps1::metrics::resetTraceForTesting();
  std::remove("/tmp/ps1_gpu_word_trigger_test.jsonl");
}

TEST(MetricsTraceTest, SemanticCheckpointCanWaitForFreshBootFrame) {
  setenv("PS1_TRACE_JSONL", "/tmp/ps1_delayed_trigger_test.jsonl", 1);
  setenv("PS1_TRACE_GTE", "1", 1);
  setenv("PS1_TRACE_GTE_TRIGGER_PC", "0x800417AC", 1);
  setenv("PS1_TRACE_GTE_TRIGGER_FRAME_FROM", "105", 1);
  setenv("PS1_TRACE_GPU_WORDS", "1", 1);
  setenv("PS1_TRACE_GPU_TRIGGER_PC", "0x800417AC", 1);
  setenv("PS1_TRACE_GPU_TRIGGER_FRAME_FROM", "105", 1);
  ps1::metrics::resetTraceForTesting();

  ps1::metrics::setTraceFrame(104);
  ps1::metrics::triggerGteTraceForCurrentFrame();
  ps1::metrics::triggerGpuWordTraceForCurrentFrame();
  EXPECT_FALSE(ps1::metrics::gteTraceEnabled());
  EXPECT_FALSE(ps1::metrics::gpuWordTraceEnabled());

  ps1::metrics::setTraceFrame(105);
  ps1::metrics::triggerGteTraceForCurrentFrame();
  ps1::metrics::triggerGpuWordTraceForCurrentFrame();
  EXPECT_TRUE(ps1::metrics::gteTraceEnabled());
  EXPECT_TRUE(ps1::metrics::gpuWordTraceEnabled());

  unsetenv("PS1_TRACE_JSONL");
  unsetenv("PS1_TRACE_GTE");
  unsetenv("PS1_TRACE_GTE_TRIGGER_PC");
  unsetenv("PS1_TRACE_GTE_TRIGGER_FRAME_FROM");
  unsetenv("PS1_TRACE_GPU_WORDS");
  unsetenv("PS1_TRACE_GPU_TRIGGER_PC");
  unsetenv("PS1_TRACE_GPU_TRIGGER_FRAME_FROM");
  ps1::metrics::resetTraceForTesting();
  std::remove("/tmp/ps1_delayed_trigger_test.jsonl");
}

TEST(MetricsTraceTest, CheckpointTraceArmsOnlyAfterSuccessfulTimelineLoad) {
  setenv("PS1_TRACE_JSONL", "/tmp/ps1_after_load_trigger_test.jsonl", 1);
  setenv("PS1_TRACE_ARM_AFTER_LOAD", "1", 1);
  setenv("PS1_TRACE_GTE", "1", 1);
  setenv("PS1_TRACE_GTE_TRIGGER_PC", "0x800417AC", 1);
  setenv("PS1_TRACE_GPU_WORDS", "1", 1);
  setenv("PS1_TRACE_GPU_TRIGGER_PC", "0x800417AC", 1);
  ps1::metrics::resetTraceForTesting();

  EXPECT_FALSE(ps1::metrics::guestTraceArmed());
  ps1::metrics::triggerGteTraceForCurrentFrame();
  ps1::metrics::triggerGpuWordTraceForCurrentFrame();
  EXPECT_FALSE(ps1::metrics::gteTraceEnabled());
  EXPECT_FALSE(ps1::metrics::gpuWordTraceEnabled());

  ps1::metrics::armGuestTraceAfterLoad();
  EXPECT_TRUE(ps1::metrics::guestTraceArmed());
  ps1::metrics::triggerGteTraceForCurrentFrame();
  ps1::metrics::triggerGpuWordTraceForCurrentFrame();
  EXPECT_TRUE(ps1::metrics::gteTraceEnabled());
  EXPECT_TRUE(ps1::metrics::gpuWordTraceEnabled());

  unsetenv("PS1_TRACE_JSONL");
  unsetenv("PS1_TRACE_ARM_AFTER_LOAD");
  unsetenv("PS1_TRACE_GTE");
  unsetenv("PS1_TRACE_GTE_TRIGGER_PC");
  unsetenv("PS1_TRACE_GPU_WORDS");
  unsetenv("PS1_TRACE_GPU_TRIGGER_PC");
  ps1::metrics::resetTraceForTesting();
  std::remove("/tmp/ps1_after_load_trigger_test.jsonl");
}

TEST(MetricsTraceTest, ProjectionJoinStopsBothSemanticTracesAtBatchBoundary) {
  setenv("PS1_TRACE_JSONL", "/tmp/ps1_projection_join_trigger_test.jsonl", 1);
  setenv("PS1_TRACE_GTE", "1", 1);
  setenv("PS1_TRACE_GTE_TRIGGER_PC", "0x800417AC", 1);
  setenv("PS1_TRACE_GTE_TRIGGER_FRAMES", "16", 1);
  setenv("PS1_TRACE_GPU_WORDS", "1", 1);
  setenv("PS1_TRACE_GPU_TRIGGER_PC", "0x800417AC", 1);
  setenv("PS1_TRACE_GPU_TRIGGER_FRAMES", "16", 1);
  ps1::metrics::resetTraceForTesting();

  ps1::metrics::setTraceFrame(100);
  ps1::metrics::triggerGteTraceForCurrentFrame();
  ps1::metrics::triggerGpuWordTraceForCurrentFrame();
  ASSERT_TRUE(ps1::metrics::gteTraceEnabled());
  ASSERT_TRUE(ps1::metrics::gpuWordTraceEnabled());
  ASSERT_FALSE(ps1::metrics::projectionTraceComplete());
  ps1::metrics::noteGteProjectionForTrace(1000);
  ps1::metrics::noteGteProjectionForTrace(1100);

  ps1::metrics::setTraceFrame(104);
  ps1::metrics::noteGpuProjectionForTrace(999, 70);
  ps1::metrics::finishGpuTraceSubmission(70);
  EXPECT_TRUE(ps1::metrics::gteTraceEnabled());
  EXPECT_TRUE(ps1::metrics::gpuWordTraceEnabled());

  ps1::metrics::noteGpuProjectionForTrace(1050, 71);
  EXPECT_TRUE(ps1::metrics::gpuWordTraceEnabled())
      << "the matching list must remain traceable through its final word";
  ps1::metrics::finishGpuTraceSubmission(71);
  EXPECT_TRUE(ps1::metrics::projectionTraceComplete());
  EXPECT_FALSE(ps1::metrics::gteTraceEnabled());
  EXPECT_FALSE(ps1::metrics::gpuWordTraceEnabled());

  unsetenv("PS1_TRACE_JSONL");
  unsetenv("PS1_TRACE_GTE");
  unsetenv("PS1_TRACE_GTE_TRIGGER_PC");
  unsetenv("PS1_TRACE_GTE_TRIGGER_FRAMES");
  unsetenv("PS1_TRACE_GPU_WORDS");
  unsetenv("PS1_TRACE_GPU_TRIGGER_PC");
  unsetenv("PS1_TRACE_GPU_TRIGGER_FRAMES");
  ps1::metrics::resetTraceForTesting();
  std::remove("/tmp/ps1_projection_join_trigger_test.jsonl");
}

TEST(MetricsTraceTest, ModernRejectTraceHonorsOptInWindowAndReset) {
  const char *path = "/tmp/ps1_modern_reject_window_test.jsonl";
  std::remove(path);
  setenv("PS1_TRACE_JSONL", path, 1);
  setenv("PS1_TRACE_MODERN_REJECT_FRAME_FROM", "10", 1);
  setenv("PS1_TRACE_MODERN_REJECT_FRAME_TO", "20", 1);
  unsetenv("PS1_TRACE_MODERN_REJECTS");
  ps1::metrics::resetTraceForTesting();

  ps1::metrics::setTraceFrame(10);
  EXPECT_FALSE(ps1::metrics::modernRejectTraceEnabled())
      << "modern rejects must be explicitly enabled";

  setenv("PS1_TRACE_MODERN_REJECTS", "1", 1);
  EXPECT_FALSE(ps1::metrics::modernRejectTraceEnabled())
      << "the production environment is resolved once per trace run";
  ps1::metrics::resetTraceForTesting();

  ps1::metrics::setTraceFrame(9);
  EXPECT_FALSE(ps1::metrics::modernRejectTraceEnabled());
  EXPECT_TRUE(ps1::metrics::modernRejectTraceEnabledAtFrame(10));
  EXPECT_FALSE(ps1::metrics::modernRejectTraceEnabledAtFrame(21));
  EXPECT_EQ(ps1::metrics::traceFrame(), 9u)
      << "an asynchronous frame query must not mutate global trace state";
  ps1::metrics::setTraceFrame(10);
  EXPECT_TRUE(ps1::metrics::modernRejectTraceEnabled());
  ps1::metrics::setTraceFrame(20);
  EXPECT_TRUE(ps1::metrics::modernRejectTraceEnabled());
  ps1::metrics::setTraceFrame(21);
  EXPECT_FALSE(ps1::metrics::modernRejectTraceEnabled());

  unsetenv("PS1_TRACE_JSONL");
  unsetenv("PS1_TRACE_MODERN_REJECTS");
  unsetenv("PS1_TRACE_MODERN_REJECT_FRAME_FROM");
  unsetenv("PS1_TRACE_MODERN_REJECT_FRAME_TO");
  ps1::metrics::resetTraceForTesting();
  std::remove(path);
}

TEST(MetricsTraceTest,
     PgxpVertexRejectTraceWorksWithoutMetricsAndReportsXYOnly) {
  const char *path = "/tmp/ps1_pgxp_vertex_reject_xy_only_test.jsonl";
  std::remove(path);
  setenv("PS1_TRACE_JSONL", path, 1);
  setenv("PS1_TRACE_PGXP_VERTICES", "1", 1);
  setenv("PS1_TRACE_PGXP_FRAME_FROM", "17", 1);
  setenv("PS1_TRACE_PGXP_FRAME_TO", "17", 1);
  ps1::metrics::resetTraceForTesting();
  ps1::metrics::setEnabledForTesting(false);
  ps1::metrics::setTraceFrame(17);

  constexpr uint32_t packed = (5u << 16) | 0xfffdu; // native (-3, 5)
  ps1::PGXPValue precision;
  precision.x = -2.75f;
  precision.y = 5.5f;
  precision.z = 0.0f;
  precision.value = packed;
  precision.flags = ps1::PGXPValue::VALID_XY;
  precision.origin = ps1::PGXPOrigin::GteProjection;
  precision.ramLookup = ps1::PGXPRamLookup::HitXY;
  precision.submissionPath = ps1::PGXPSubmissionPath::HleOrderingTable;
  ps1::recordPGXPVertex(
      0x30, 3, 0x800775b4u, ps1::PGXPPrimitive::GouraudTriangle,
      ps1::PGXPSemanticDecision::projected3D(), precision, packed);

  std::ifstream in(path);
  ASSERT_TRUE(in.good());
  std::stringstream buf;
  buf << in.rdbuf();
  const std::string body = buf.str();
  EXPECT_NE(body.find("\"event\":\"pgxp.vertex_reject\""), std::string::npos)
      << body;
  EXPECT_NE(
      body.find("\"reason\":" + std::to_string(static_cast<int>(
                                    ps1::PGXPVertexRejectReason::XYOnly))),
      std::string::npos)
      << body;
  EXPECT_NE(body.find("\"source\":2147972532"), std::string::npos) << body;
  EXPECT_NE(body.find("\"packet_head\":2147972520"), std::string::npos) << body;
  EXPECT_NE(body.find("\"opcode\":48"), std::string::npos) << body;
  EXPECT_NE(body.find("\"word_index\":3"), std::string::npos) << body;
  EXPECT_NE(body.find("\"packed\":393213"), std::string::npos) << body;
  EXPECT_NE(body.find("\"value\":393213"), std::string::npos) << body;
  EXPECT_NE(body.find("\"flags\":3"), std::string::npos) << body;
  EXPECT_NE(body.find("\"origin\":1"), std::string::npos) << body;
  EXPECT_NE(body.find("\"ram_lookup\":5"), std::string::npos) << body;
  EXPECT_NE(body.find("\"path\":2"), std::string::npos) << body;
  EXPECT_NE(body.find("\"has_x\":1"), std::string::npos) << body;
  EXPECT_NE(body.find("\"has_y\":1"), std::string::npos) << body;
  EXPECT_NE(body.find("\"has_xy\":1"), std::string::npos) << body;
  EXPECT_NE(body.find("\"has_xyz\":0"), std::string::npos) << body;
  EXPECT_NE(body.find("\"native_x\":-3"), std::string::npos) << body;
  EXPECT_NE(body.find("\"native_y\":5"), std::string::npos) << body;
  EXPECT_NE(body.find("\"dx_q16\":16384"), std::string::npos) << body;
  EXPECT_NE(body.find("\"dy_q16\":32768"), std::string::npos) << body;
  EXPECT_NE(body.find("\"x_bits\":3224371200"), std::string::npos) << body;
  EXPECT_NE(body.find("\"y_bits\":1085276160"), std::string::npos) << body;
  EXPECT_NE(body.find("\"z_bits\":0"), std::string::npos) << body;
  EXPECT_EQ(ps1::metrics::get("pgxp.vertex.total"), 0u)
      << "the trace must not implicitly enable the metric collector";

  unsetenv("PS1_TRACE_JSONL");
  unsetenv("PS1_TRACE_PGXP_VERTICES");
  unsetenv("PS1_TRACE_PGXP_FRAME_FROM");
  unsetenv("PS1_TRACE_PGXP_FRAME_TO");
  ps1::metrics::resetTraceForTesting();
  std::remove(path);
}

TEST(MetricsTraceTest,
     PgxpSignedProjectionUsesNearPlaneEventInsteadOfVertexReject) {
  const char *path = "/tmp/ps1_pgxp_vertex_near_plane_test.jsonl";
  std::remove(path);
  setenv("PS1_TRACE_JSONL", path, 1);
  setenv("PS1_TRACE_PGXP_VERTICES", "1", 1);
  ps1::metrics::resetTraceForTesting();
  ps1::metrics::setEnabledForTesting(false);
  ps1::metrics::setTraceFrame(19);

  constexpr uint32_t packed = (5u << 16) | 0xfffdu;
  ps1::PGXPValue precision;
  precision.x = 6.0f; // Native X is -3: force a signed-screen reconciliation.
  precision.y = 5.0f;
  precision.z = -10.0f;
  precision.value = packed;
  precision.flags =
      ps1::PGXPValue::VALID_XY | ps1::PGXPValue::VALID_SIGNED_PROJECTION;
  precision.origin = ps1::PGXPOrigin::GteProjection;
  precision.ramLookup = ps1::PGXPRamLookup::HitSignedProjection;
  precision.submissionPath = ps1::PGXPSubmissionPath::HleOrderingTable;
  precision.qx = 30.0f;
  precision.qy = -50.0f;
  precision.nearDepth = 500.0f + 1.0f / 4096.0f;
  precision.projectionId = 81;
  precision.projectionContext = 12;
  ps1::recordPGXPVertex(
      0x30, 3, 0x800775b4u, ps1::PGXPPrimitive::GouraudTriangle,
      ps1::PGXPSemanticDecision::projected3D(), precision, packed);

  std::ifstream in(path);
  ASSERT_TRUE(in.good());
  std::stringstream buf;
  buf << in.rdbuf();
  const std::string body = buf.str();
  EXPECT_NE(body.find("\"event\":\"pgxp.vertex_near_plane\""),
            std::string::npos)
      << body;
  EXPECT_EQ(body.find("\"event\":\"pgxp.vertex_reject\""), std::string::npos)
      << body;
  EXPECT_NE(body.find("\"event\":\"pgxp.vertex_reconcile\""), std::string::npos)
      << body;
  EXPECT_NE(body.find("\"component_mask\":1"), std::string::npos) << body;
  EXPECT_NE(body.find("\"projection_id\":81"), std::string::npos) << body;
  EXPECT_NE(body.find("\"projection_context\":12"), std::string::npos) << body;

  unsetenv("PS1_TRACE_JSONL");
  unsetenv("PS1_TRACE_PGXP_VERTICES");
  ps1::metrics::resetTraceForTesting();
  std::remove(path);
}

TEST(MetricsTraceTest, PgxpVertexTraceSeparatesRejectsFromReconciliation) {
  const char *path = "/tmp/ps1_pgxp_vertex_reject_reasons_test.jsonl";
  std::remove(path);
  setenv("PS1_TRACE_JSONL", path, 1);
  setenv("PS1_TRACE_PGXP_VERTICES", "1", 1);
  ps1::metrics::resetTraceForTesting();
  ps1::metrics::setEnabledForTesting(false);
  ps1::metrics::setTraceFrame(23);

  constexpr uint32_t packed = (20u << 16) | 10u;
  auto exact = [] {
    ps1::PGXPValue value;
    value.x = 10.25f;
    value.y = 20.5f;
    value.z = 100.0f;
    value.value = packed;
    value.flags = ps1::PGXPValue::VALID_XY | ps1::PGXPValue::VALID_Z;
    value.origin = ps1::PGXPOrigin::GteProjection;
    return value;
  };
  auto emit = [&](uint32_t source, const ps1::PGXPValue &value,
                  ps1::PGXPPrimitive primitive =
                      ps1::PGXPPrimitive::GouraudTriangle) {
    const auto semantic = primitive == ps1::PGXPPrimitive::Line
                              ? ps1::PGXPSemanticDecision::intrinsicScreen2D()
                              : ps1::PGXPSemanticDecision::projected3D();
    ps1::recordPGXPVertex(0x30, 1, source, primitive, semantic, value, packed);
  };

  emit(0x80010004u, ps1::PGXPValue{}); // no_xy
  auto stale = exact();
  stale.value ^= 1u;
  emit(0x80010008u, stale);
  auto nonfiniteX = exact();
  nonfiniteX.x = std::numeric_limits<float>::infinity();
  emit(0x8001000cu, nonfiniteX);
  auto nonfiniteZ = exact();
  nonfiniteZ.z = std::numeric_limits<float>::infinity();
  emit(0x80010010u, nonfiniteZ);
  auto nonpositive = exact();
  nonpositive.z = 0.0f;
  emit(0x80010014u, nonpositive);
  auto deltaX = exact();
  deltaX.x = 19.0f;
  emit(0x80010018u, deltaX);
  auto deltaY = exact();
  deltaY.y = 29.0f;
  emit(0x8001001cu, deltaY);
  emit(0x80010020u, exact()); // safe polygon: no event
  emit(0x80010024u, ps1::PGXPValue{}, ps1::PGXPPrimitive::Line);

  std::ifstream in(path);
  ASSERT_TRUE(in.good());
  std::stringstream buf;
  buf << in.rdbuf();
  const std::string body = buf.str();
  auto occurrences = [&](const std::string &needle) {
    std::size_t count = 0;
    for (std::size_t pos = 0;
         (pos = body.find(needle, pos)) != std::string::npos;
         pos += needle.size())
      ++count;
    return count;
  };
  EXPECT_EQ(occurrences("\"event\":\"pgxp.vertex_reject\""), 4u) << body;
  for (const auto reason :
       {ps1::PGXPVertexRejectReason::NoXY, ps1::PGXPVertexRejectReason::Stale,
        ps1::PGXPVertexRejectReason::NonFinite,
        ps1::PGXPVertexRejectReason::NonPositiveZ}) {
    EXPECT_EQ(
        occurrences("\"reason\":" + std::to_string(static_cast<int>(reason))),
        1u)
        << body;
  }
  EXPECT_EQ(occurrences("\"event\":\"pgxp.vertex_reconcile\""), 3u) << body;
  EXPECT_EQ(occurrences("\"component_mask\":1"), 2u) << body;
  EXPECT_EQ(occurrences("\"component_mask\":2"), 1u) << body;
  EXPECT_EQ(occurrences("\"x_source\":2"), 1u) << body;
  EXPECT_EQ(occurrences("\"x_source\":3"), 1u) << body;
  EXPECT_EQ(occurrences("\"y_source\":3"), 1u) << body;
  EXPECT_NE(body.find("\"original_x_bits\":2139095040"), std::string::npos)
      << body;
  EXPECT_NE(body.find("\"resolved_x_q16\":655360"), std::string::npos) << body;
  EXPECT_NE(body.find("\"resolved_y_q16\":1343488"), std::string::npos) << body;

  unsetenv("PS1_TRACE_JSONL");
  unsetenv("PS1_TRACE_PGXP_VERTICES");
  ps1::metrics::resetTraceForTesting();
  std::remove(path);
}

} // namespace
