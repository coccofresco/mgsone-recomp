// Tests for ps1Runtime -- Runtime Macros
// Validates MEM_READ/WRITE macros, unaligned loads, and GTE register access

#include <array>
#include <gtest/gtest.h>
#include <runtime/gpu/gpu.h>
#include <runtime/memory.h>
#include <runtime/ps1_runtime_macros.h>
#include <vector>

using namespace ps1;

// GTE Register Access

TEST(RuntimeMacros, GteReadWriteData) {
  CPUContext ctx;
  ctx.reset();

  gte_write_data(&ctx, 8, 0x00001000); // IR0
  EXPECT_EQ(gte_read_data(&ctx, 8), 0x00001000u);
}

TEST(RuntimeMacros, GteReadWriteControl) {
  CPUContext ctx;
  ctx.reset();

  gte_write_control(&ctx, 31, 0x7F87E000); // FLAG
  EXPECT_EQ(gte_read_control(&ctx, 31), 0xFF87E000u);
}

TEST(RuntimeMacros, GteRegisterMasking) {
  CPUContext ctx;
  ctx.reset();

  // Register index is masked to 0-31
  gte_write_data(&ctx, 0x20, 0xABCD); // 32 -> wraps to 0
  EXPECT_EQ(gte_read_data(&ctx, 0), 0xABCDu);
}

// Unaligned Loads (LWL / LWR)

TEST(RuntimeMacros, LWLAligned) {
  Memory mem;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &mem;
  ctx.bios = nullptr;
  mem.write32(0x1000, 0xAABBCCDD);

  // LWL with shift=3 (fully aligned word)
  uint32_t result = DO_LWL(&ctx, 0x00000000, 0x1003);
  EXPECT_EQ(result, 0xAABBCCDDu);
}

TEST(RuntimeMacros, LWRAligned) {
  Memory mem;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &mem;
  ctx.bios = nullptr;
  mem.write32(0x1000, 0xAABBCCDD);

  // LWR with shift=0 (fully aligned word)
  uint32_t result = DO_LWR(&ctx, 0x00000000, 0x1000);
  EXPECT_EQ(result, 0xAABBCCDDu);
}

TEST(RuntimeMacros, LWLPartialMerge) {
  Memory mem;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &mem;
  ctx.bios = nullptr;
  mem.write32(0x1000, 0xAABBCCDD);

  // LWL shift=0 -> load high byte only, keep low 3 bytes of rt
  uint32_t result = DO_LWL(&ctx, 0x11223344, 0x1000);
  EXPECT_EQ(result, (0x11223344u & 0x00FFFFFF) | (0xAABBCCDD << 24));
}

TEST(RuntimeMacros, LWRPartialMerge) {
  Memory mem;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &mem;
  ctx.bios = nullptr;
  mem.write32(0x1000, 0xAABBCCDD);

  // LWR shift=3 -> load low byte only, keep high 3 bytes of rt
  uint32_t result = DO_LWR(&ctx, 0x11223344, 0x1003);
  EXPECT_EQ(result, (0x11223344u & 0xFFFFFF00) | (0xAABBCCDD >> 24));
}

// Unaligned Stores (SWL / SWR)

TEST(RuntimeMacros, SWLFullWrite) {
  Memory mem;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &mem;
  ctx.bios = nullptr;
  mem.write32(0x2000, 0x00000000);

  DO_SWL(&ctx, 0xAABBCCDD, 0x2003);
  EXPECT_EQ(mem.read32(0x2000), 0xAABBCCDDu);
}

TEST(RuntimeMacros, SWRFullWrite) {
  Memory mem;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &mem;
  ctx.bios = nullptr;
  mem.write32(0x2000, 0x00000000);

  DO_SWR(&ctx, 0xAABBCCDD, 0x2000);
  EXPECT_EQ(mem.read32(0x2000), 0xAABBCCDDu);
}

// MEM_READ/WRITE Macros (via recomp_context)

TEST(RuntimeMacros, MemMacros32) {
  Memory mem;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &mem;
  ctx.bios = nullptr;

  MEM_WRITE32(&ctx, 0x80010000, 0xDEADBEEF);
  EXPECT_EQ(MEM_READ32(&ctx, 0x80010000), 0xDEADBEEFu);
}

TEST(RuntimeMacros, MemMacros16) {
  Memory mem;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &mem;
  ctx.bios = nullptr;

  MEM_WRITE16(&ctx, 0x80020000, 0xBEEF);
  EXPECT_EQ(MEM_READ16(&ctx, 0x80020000), 0xBEEFu);
}

TEST(RuntimeMacros, MemMacros8) {
  Memory mem;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &mem;
  ctx.bios = nullptr;

  MEM_WRITE8(&ctx, 0x80030000, 0x42);
  EXPECT_EQ(MEM_READ8(&ctx, 0x80030000), 0x42u);
}

TEST(RuntimeMacros, PgxpPrecisionFollowsGteGprAndRamAndValidatesValue) {
  Memory mem;
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &mem;

  constexpr uint32_t packed = (170u << 16) | 260u;
  ctx.cop2d[GTE_SXY2] = packed;
  ctx.pgxpGteData[GTE_SXY2] =
      PGXPValue{260.25f, 169.75f, 1000.0f, packed,
                PGXPValue::VALID_XY | PGXPValue::VALID_Z};

  ctx.r[2] = pgxp_mfc2(&ctx, 2, GTE_SXY2);
  ASSERT_TRUE(ctx.pgxpGpr[2].hasXYZ(packed));
  pgxp_store_word(&ctx, 2, 0x80010000u, ctx.r[2]);

  ctx.pgxpGpr[3].invalidate();
  ctx.r[3] = pgxp_load_word(&ctx, 3, 0x80010000u);
  EXPECT_TRUE(ctx.pgxpGpr[3].hasXYZ(packed));
  EXPECT_FLOAT_EQ(ctx.pgxpGpr[3].x, 260.25f);

  // An ordinary overwrite invalidates the address sidecar, even if a later
  // packet happens to reuse the same address.
  mem.write32(0x80010000u, packed);
  PGXPValue stale;
  EXPECT_FALSE(mem.lookupPrecision(0x80010000u, packed, stale));
}

TEST(RuntimeMacros, PgxpSurvivesCrashPackedSxyPlusThreeSequence) {
  Memory mem;
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &mem;

  constexpr uint32_t packed = (170u << 16) | 260u;
  constexpr uint32_t adjusted = (170u << 16) | 263u;
  ctx.cop2d[GTE_SXY0] = packed;
  ctx.pgxpGteData[GTE_SXY0] =
      PGXPValue{260.25f,
                169.75f,
                1000.0f,
                packed,
                PGXPValue::VALID_XY | PGXPValue::VALID_Z,
                PGXPOrigin::GteProjection};
  PGXPValue &projection = ctx.pgxpGteData[GTE_SXY0];
  projection.qx = projection.x * projection.z;
  projection.qy = projection.y * projection.z;
  projection.nearDepth = 500.0f + 1.0f / 4096.0f;
  projection.projectionId = 42;
  projection.projectionContext = 17;
  projection.gameProfileCallsite = UINT64_C(0x800123408004567A);
  projection.projectionTransformValueFingerprint = UINT64_C(0xDEADBEEFCAFEBABE);
  projection.projectionTransformLineage = 0x12345678u;
  projection.modelVertexSource = 0x80030001u;
  projection.modelValueFlowSourceFingerprint = 0x1122334455667788u;
  projection.modelValueFlowProgramFingerprint = 0x8877665544332211u;
  projection.flags |=
      PGXPValue::VALID_SIGNED_PROJECTION | PGXPValue::VALID_TRANSFORM_LINEAGE |
      PGXPValue::VALID_MODEL_VERTEX_SOURCE | PGXPValue::VALID_MODEL_VALUE_FLOW;

  ctx.r[25] = pgxp_mfc2(&ctx, 25, GTE_SXY0);
  ctx.r[24] = 0xFFFF0000u;
  ctx.pgxpGpr[24].invalidate();
  pgxp_andi(&ctx, 4, 25, 0xFFFFu);
  ASSERT_TRUE(ctx.pgxpGpr[4].hasX(ctx.r[4]));
  EXPECT_FALSE(ctx.pgxpGpr[4].hasY(ctx.r[4]));
  EXPECT_TRUE(ctx.pgxpGpr[4].hasProjectiveX(ctx.r[4]));
  pgxp_and(&ctx, 25, 25, 24);
  EXPECT_FALSE(ctx.pgxpGpr[25].hasX(ctx.r[25]));
  ASSERT_TRUE(ctx.pgxpGpr[25].hasY(ctx.r[25]));
  EXPECT_TRUE(ctx.pgxpGpr[25].hasProjectiveY(ctx.r[25]));
  pgxp_addiu(&ctx, 4, 4, 3);
  pgxp_andi(&ctx, 4, 4, 0xFFFFu);
  pgxp_or(&ctx, 25, 25, 4);

  ASSERT_EQ(ctx.r[25], adjusted);
  ASSERT_TRUE(ctx.pgxpGpr[25].hasXYZ(adjusted));
  EXPECT_FLOAT_EQ(ctx.pgxpGpr[25].x, 263.25f);
  EXPECT_FLOAT_EQ(ctx.pgxpGpr[25].y, 169.75f);
  EXPECT_FLOAT_EQ(ctx.pgxpGpr[25].z, 1000.0f);
  ASSERT_TRUE(ctx.pgxpGpr[25].hasSignedProjection(adjusted));
  EXPECT_FLOAT_EQ(ctx.pgxpGpr[25].qx, 263250.0f);
  EXPECT_FLOAT_EQ(ctx.pgxpGpr[25].qy, 169750.0f);
  EXPECT_FLOAT_EQ(ctx.pgxpGpr[25].nearDepth, 500.0f + 1.0f / 4096.0f);
  EXPECT_EQ(ctx.pgxpGpr[25].projectionId, 42u);
  EXPECT_EQ(ctx.pgxpGpr[25].projectionContext, 17u);
  EXPECT_EQ(ctx.pgxpGpr[25].gameProfileCallsite,
            UINT64_C(0x800123408004567A));
  EXPECT_EQ(ctx.pgxpGpr[25].projectionTransformValueFingerprint,
            UINT64_C(0xDEADBEEFCAFEBABE));
  EXPECT_EQ(ctx.pgxpGpr[25].projectionTransformLineage, 0x12345678u);
  EXPECT_EQ(ctx.pgxpGpr[25].modelVertexSource, 0x80030001u);
  EXPECT_EQ(ctx.pgxpGpr[25].modelValueFlowSourceFingerprint,
            0x1122334455667788u);
  EXPECT_EQ(ctx.pgxpGpr[25].modelValueFlowProgramFingerprint,
            0x8877665544332211u);
  EXPECT_EQ(ctx.pgxpGpr[25].flags & PGXPValue::VALID_INTRINSIC_PROVENANCE,
            PGXPValue::VALID_INTRINSIC_PROVENANCE);
  EXPECT_EQ(ctx.pgxpGpr[25].origin, PGXPOrigin::GteProjection);

  pgxp_mtc2(&ctx, GTE_SXY0, 25, ctx.r[25]);
  pgxp_swc2(&ctx, GTE_SXY0, 0x80012000u);
  PGXPValue stored;
  ASSERT_TRUE(mem.lookupPrecision(0x80012000u, adjusted, stored));
  EXPECT_TRUE(stored.hasXYZ(adjusted));
  EXPECT_TRUE(stored.hasSignedProjection(adjusted));
  EXPECT_FLOAT_EQ(stored.x, 263.25f);
  EXPECT_FLOAT_EQ(stored.qx, 263250.0f);
  EXPECT_EQ(stored.projectionId, 42u);
  EXPECT_EQ(stored.projectionTransformValueFingerprint,
            UINT64_C(0xDEADBEEFCAFEBABE));
  EXPECT_EQ(stored.projectionTransformLineage, 0x12345678u);
  EXPECT_EQ(stored.modelVertexSource, 0x80030001u);
  EXPECT_EQ(stored.modelValueFlowSourceFingerprint, 0x1122334455667788u);
  EXPECT_EQ(stored.modelValueFlowProgramFingerprint, 0x8877665544332211u);
}

TEST(RuntimeMacros, PgxpSignedProjectionSurvivesMoveAddAndRamStore) {
  Memory mem;
  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &mem;

  constexpr uint32_t packed = (170u << 16) | 260u;
  constexpr uint32_t adjusted = (170u << 16) | 263u;
  PGXPValue projection;
  projection.x = 260.25f;
  projection.y = 169.75f;
  projection.z = -10.0f;
  projection.value = packed;
  projection.flags = PGXPValue::VALID_XY | PGXPValue::VALID_SIGNED_PROJECTION |
                     PGXPValue::VALID_TRANSFORM_LINEAGE |
                     PGXPValue::VALID_MODEL_VERTEX_SOURCE;
  projection.origin = PGXPOrigin::GteProjection;
  projection.qx = projection.x * projection.z;
  projection.qy = projection.y * projection.z;
  projection.nearDepth = 500.0f + 1.0f / 4096.0f;
  projection.projectionId = 91;
  projection.projectionContext = 23;
  projection.gameProfileCallsite = UINT64_C(0x8001234080045679);
  projection.projectionTransformValueFingerprint = UINT64_C(0x0123456789ABCDEF);
  projection.projectionTransformLineage = 0xABCDEFu;
  projection.modelVertexSource = 0x80044001u;
  ctx.pgxpGpr[2] = projection;
  ctx.r[2] = packed;

  pgxp_addiu(&ctx, 3, 2, 3);
  ASSERT_EQ(ctx.r[3], adjusted);
  ASSERT_TRUE(ctx.pgxpGpr[3].hasSignedProjection(adjusted));
  EXPECT_FALSE(ctx.pgxpGpr[3].hasXYZ(adjusted));
  EXPECT_FLOAT_EQ(ctx.pgxpGpr[3].x, projection.x + 3.0f);
  EXPECT_FLOAT_EQ(ctx.pgxpGpr[3].y, projection.y);
  EXPECT_FLOAT_EQ(ctx.pgxpGpr[3].qx, projection.qx - 30.0f);
  EXPECT_FLOAT_EQ(ctx.pgxpGpr[3].qy, projection.qy);

  pgxp_move_gpr(&ctx, 4, 3, adjusted);
  ASSERT_TRUE(ctx.pgxpGpr[4].hasSignedProjection(adjusted));
  pgxp_store_word(&ctx, 4, 0x80014000u, adjusted);
  PGXPValue stored;
  ASSERT_TRUE(mem.lookupPrecision(0x80014000u, adjusted, stored));
  EXPECT_TRUE(stored.hasSignedProjection(adjusted));
  EXPECT_FALSE(stored.hasXYZ(adjusted));
  EXPECT_FLOAT_EQ(stored.x, projection.x + 3.0f);
  EXPECT_FLOAT_EQ(stored.y, projection.y);
  EXPECT_FLOAT_EQ(stored.qx, projection.qx - 30.0f);
  EXPECT_FLOAT_EQ(stored.qy, projection.qy);
  EXPECT_EQ(stored.projectionId, 91u);
  EXPECT_EQ(stored.projectionContext, 23u);
  EXPECT_EQ(stored.gameProfileCallsite,
            UINT64_C(0x8001234080045679));
  EXPECT_EQ(stored.projectionTransformValueFingerprint,
            UINT64_C(0x0123456789ABCDEF));
  EXPECT_EQ(stored.projectionTransformLineage, 0xABCDEFu);
  EXPECT_EQ(stored.modelVertexSource, 0x80044001u);
}

TEST(RuntimeMacros,
     PgxpProjectedScreenOffsetPreservesDepthAndIntrinsicProvenance) {
  constexpr uint32_t centrePacked = (120u << 16u) | 140u;
  constexpr uint32_t derivedPacked = (113u << 16u) | 149u;
  PGXPValue centre;
  centre.x = 140.375f;
  centre.y = 119.625f;
  centre.z = 32.5;
  centre.value = centrePacked;
  centre.flags = PGXPValue::VALID_XY | PGXPValue::VALID_SIGNED_PROJECTION |
                 PGXPValue::VALID_TRANSFORM_LINEAGE |
                 PGXPValue::VALID_MODEL_VERTEX_SOURCE |
                 PGXPValue::VALID_MODEL_VALUE_FLOW;
  centre.origin = PGXPOrigin::GteProjection;
  centre.qx = centre.x * centre.z;
  centre.qy = centre.y * centre.z;
  centre.nearDepth = 16.0;
  centre.projectionId = 73u;
  centre.projectionContext = 91u;
  centre.gameProfileCallsite = UINT64_C(0x1122334455667788);
  centre.projectionTransformValueFingerprint = UINT64_C(0x8877665544332211);
  centre.projectionTransformLineage = 0x1234u;
  centre.modelVertexSource = 0x80045679u;
  centre.modelValueFlowSourceFingerprint = 0xabcdefu;
  centre.modelValueFlowProgramFingerprint = 0x123456u;
  centre.ramLookup = PGXPRamLookup::HitSignedProjection;
  centre.submissionPath = PGXPSubmissionPath::HleOrderingTable;

  PGXPValue derived;
  ASSERT_TRUE(derivePGXPProjectedScreenOffset(
      centre, centrePacked, derivedPacked, derived));
  ASSERT_TRUE(derived.hasSignedProjection(derivedPacked));
  EXPECT_FLOAT_EQ(derived.x, centre.x + 9.0f);
  EXPECT_FLOAT_EQ(derived.y, centre.y - 7.0f);
  EXPECT_DOUBLE_EQ(derived.z, centre.z);
  EXPECT_DOUBLE_EQ(derived.qx, centre.qx + 9.0 * centre.z);
  EXPECT_DOUBLE_EQ(derived.qy, centre.qy - 7.0 * centre.z);
  EXPECT_DOUBLE_EQ(derived.nearDepth, centre.nearDepth);
  EXPECT_EQ(derived.projectionId, centre.projectionId);
  EXPECT_EQ(derived.projectionContext, centre.projectionContext);
  EXPECT_EQ(derived.gameProfileCallsite, centre.gameProfileCallsite);
  EXPECT_EQ(derived.projectionTransformValueFingerprint,
            centre.projectionTransformValueFingerprint);
  EXPECT_EQ(derived.projectionTransformLineage,
            centre.projectionTransformLineage);
  EXPECT_EQ(derived.modelVertexSource, centre.modelVertexSource);
  EXPECT_EQ(derived.modelValueFlowSourceFingerprint,
            centre.modelValueFlowSourceFingerprint);
  EXPECT_EQ(derived.modelValueFlowProgramFingerprint,
            centre.modelValueFlowProgramFingerprint);
  EXPECT_EQ(derived.flags & PGXPValue::VALID_INTRINSIC_PROVENANCE,
            PGXPValue::VALID_INTRINSIC_PROVENANCE);
  EXPECT_EQ(derived.ramLookup, PGXPRamLookup::NotApplicable);
  EXPECT_EQ(derived.submissionPath, PGXPSubmissionPath::DirectMmio);

  PGXPValue rejected = derived;
  PGXPValue xyOnly = centre;
  xyOnly.flags &= ~PGXPValue::VALID_SIGNED_PROJECTION;
  EXPECT_FALSE(derivePGXPProjectedScreenOffset(
      xyOnly, centrePacked, derivedPacked, rejected));
  EXPECT_EQ(rejected.flags, 0u);
}

TEST(RuntimeMacros, PgxpBitwiseRecompositionRequiresSameProjectionId) {
  recomp_context ctx{};
  ctx.reset();
  constexpr uint32_t xWord = 260u;
  constexpr uint32_t yWord = 170u << 16;
  constexpr uint32_t packed = yWord | xWord;
  ctx.r[4] = xWord;
  ctx.r[25] = yWord;

  PGXPValue &x = ctx.pgxpGpr[4];
  x.x = 260.25f;
  x.z = -10.0f;
  x.value = xWord;
  x.flags = PGXPValue::VALID_X | PGXPValue::VALID_QX;
  x.origin = PGXPOrigin::GteProjection;
  x.qx = x.x * x.z;
  x.nearDepth = 500.0f + 1.0f / 4096.0f;
  x.projectionId = 10;
  x.projectionContext = 7;

  PGXPValue &y = ctx.pgxpGpr[25];
  y.y = 169.75f;
  y.z = -10.0f;
  y.value = yWord;
  y.flags = PGXPValue::VALID_Y | PGXPValue::VALID_QY;
  y.origin = PGXPOrigin::GteProjection;
  y.qy = y.y * y.z;
  y.nearDepth = x.nearDepth;
  y.projectionId = 11;
  y.projectionContext = x.projectionContext;

  pgxp_or(&ctx, 25, 25, 4);

  ASSERT_EQ(ctx.r[25], packed);
  EXPECT_TRUE(ctx.pgxpGpr[25].hasXY(packed));
  EXPECT_FALSE(ctx.pgxpGpr[25].hasSignedProjection(packed));
  EXPECT_EQ(ctx.pgxpGpr[25].flags & PGXPValue::VALID_SIGNED_PROJECTION, 0u);
}

TEST(RuntimeMacros, PgxpBitwiseRecompositionRequiresExactProjectiveProvenance) {
  for (int mismatch = 0; mismatch < 7; ++mismatch) {
    SCOPED_TRACE(mismatch);
    recomp_context ctx{};
    ctx.reset();
    constexpr uint32_t xWord = 260u;
    constexpr uint32_t yWord = 170u << 16;
    constexpr uint32_t packed = yWord | xWord;
    ctx.r[4] = xWord;
    ctx.r[25] = yWord;

    PGXPValue &x = ctx.pgxpGpr[4];
    x.x = 260.25f;
    x.z = -10.0;
    x.value = xWord;
    x.flags = PGXPValue::VALID_X | PGXPValue::VALID_QX |
              PGXPValue::VALID_TRANSFORM_LINEAGE |
              PGXPValue::VALID_MODEL_VERTEX_SOURCE |
              PGXPValue::VALID_MODEL_VALUE_FLOW;
    x.origin = PGXPOrigin::GteProjection;
    x.qx = x.x * x.z;
    x.nearDepth = 500.0 + 1.0 / 4096.0;
    x.projectionId = 10;
    x.projectionContext = 7;
    x.projectionTransformLineage = 0xCAFEu;
    x.modelVertexSource = 0x80050001u;
    x.modelValueFlowSourceFingerprint = 0x1122334455667788u;
    x.modelValueFlowProgramFingerprint = 0x8877665544332211u;

    PGXPValue &y = ctx.pgxpGpr[25];
    y.y = 169.75f;
    y.z = x.z;
    y.value = yWord;
    y.flags = PGXPValue::VALID_Y | PGXPValue::VALID_QY |
              PGXPValue::VALID_TRANSFORM_LINEAGE |
              PGXPValue::VALID_MODEL_VERTEX_SOURCE |
              PGXPValue::VALID_MODEL_VALUE_FLOW;
    y.origin = PGXPOrigin::GteProjection;
    y.qy = y.y * y.z;
    y.nearDepth = x.nearDepth;
    y.projectionId = x.projectionId;
    y.projectionContext = x.projectionContext;
    y.projectionTransformLineage = x.projectionTransformLineage;
    y.modelVertexSource = x.modelVertexSource;
    y.modelValueFlowSourceFingerprint = x.modelValueFlowSourceFingerprint;
    y.modelValueFlowProgramFingerprint = x.modelValueFlowProgramFingerprint;
    if (mismatch == 0)
      ++y.projectionContext;
    else if (mismatch == 1)
      y.z -= 1.0 / 4096.0;
    else if (mismatch == 2)
      y.nearDepth += 1.0 / 4096.0;
    else if (mismatch == 3)
      ++y.projectionTransformLineage;
    else if (mismatch == 4)
      y.modelVertexSource += 8u;
    else if (mismatch == 5)
      ++y.modelValueFlowSourceFingerprint;
    else
      ++y.modelValueFlowProgramFingerprint;

    pgxp_or(&ctx, 25, 25, 4);

    ASSERT_EQ(ctx.r[25], packed);
    EXPECT_TRUE(ctx.pgxpGpr[25].hasXY(packed));
    EXPECT_FALSE(ctx.pgxpGpr[25].hasSignedProjection(packed));
    EXPECT_EQ(ctx.pgxpGpr[25].flags & PGXPValue::VALID_SIGNED_PROJECTION, 0u);
  }
}

TEST(RuntimeMacros, PgxpBitwiseRecompositionDoesNotInventCpuDepth) {
  recomp_context ctx{};
  ctx.reset();
  ctx.r[4] = 0x00000103u;
  ctx.r[25] = 0x00AA0000u;

  pgxp_or(&ctx, 25, 25, 4);

  EXPECT_EQ(ctx.r[25], 0x00AA0103u);
  EXPECT_FALSE(ctx.pgxpGpr[25].hasXYZ(ctx.r[25]));
  EXPECT_EQ(ctx.pgxpGpr[25].flags, 0u);
}

TEST(RuntimeMacros, PgxpProjectivePipelinePreservesSfFalseClipAndDrawOffset) {
  Memory mem;
  recomp_context ctx{};
  gpu::GPU gpu;
  ctx.reset();
  ctx.mem = &mem;
  mem.setGPU(&gpu);

  // sf=false retains the unshifted X/Y transform. Unit X/Y matrix terms make
  // that scale directly observable, while an ordinary 4.12 Z term keeps W in
  // camera-space units. H=1000 places the first unsaturated W just above 500.
  ctx.cop2c[GTE_RT11RT12] = 1u;
  ctx.cop2c[GTE_RT13RT21] = 0u;
  ctx.cop2c[GTE_RT22RT23] = 1u;
  ctx.cop2c[GTE_RT31RT32] = 0u;
  ctx.cop2c[GTE_RT33] = 4096u;
  ctx.cop2c[GTE_TRX] = 0u;
  ctx.cop2c[GTE_TRY] = 0u;
  ctx.cop2c[GTE_TRZ] = 0u;
  ctx.cop2c[GTE_H] = 1000u;
  ctx.cop2c[GTE_OFX] = 0u;
  ctx.cop2c[GTE_OFY] = 0u;

  struct CameraVertex {
    int16_t x;
    int16_t y;
    int16_t z;
  };
  constexpr std::array<CameraVertex, 3> vertices = {
      CameraVertex{10, 10, 0},
      CameraVertex{20, 10, 1000},
      CameraVertex{10, 20, 1000},
  };
  constexpr uint32_t ramBase = 0x80018000u;
  std::array<uint32_t, vertices.size()> adjustedWords{};
  uint64_t projectionContext = 0;

  for (std::size_t i = 0; i < vertices.size(); ++i) {
    const CameraVertex vertex = vertices[i];
    ctx.cop2d[GTE_VXY0] =
        static_cast<uint32_t>(static_cast<uint16_t>(vertex.x)) |
        (static_cast<uint32_t>(static_cast<uint16_t>(vertex.y)) << 16);
    ctx.cop2d[GTE_VZ0] = static_cast<uint16_t>(vertex.z);
    GTE::RTPS(&ctx, false, false);

    const uint32_t projectedWord = ctx.cop2d[GTE_SXY2];
    const PGXPValue projected = ctx.pgxpGteData[GTE_SXY2];
    ASSERT_TRUE(projected.hasSignedProjection(projectedWord));
    EXPECT_DOUBLE_EQ(projected.z, static_cast<double>(vertex.z));
    EXPECT_DOUBLE_EQ(projected.qx, static_cast<double>(vertex.x) * 1000.0);
    EXPECT_DOUBLE_EQ(projected.qy, static_cast<double>(vertex.y) * 1000.0);
    if (i == 0)
      projectionContext = projected.projectionContext;
    else
      EXPECT_EQ(projected.projectionContext, projectionContext);

    ctx.r[2] = pgxp_mfc2(&ctx, 2, GTE_SXY2);
    pgxp_addiu(&ctx, 3, 2, 1); // Shift screen X and Qx by one pixel.
    adjustedWords[i] = ctx.r[3];
    ASSERT_TRUE(ctx.pgxpGpr[3].hasSignedProjection(adjustedWords[i]));
    EXPECT_DOUBLE_EQ(ctx.pgxpGpr[3].qx, projected.qx + projected.z);
    EXPECT_DOUBLE_EQ(ctx.pgxpGpr[3].qy, projected.qy);
    pgxp_store_word(&ctx, 3, ramBase + static_cast<uint32_t>(i * 4),
                    adjustedWords[i]);
  }

  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setProjectiveNearClip(true);
  gpu.setTrueDepth(true);
  metrics::reset();
  metrics::setEnabledForTesting(true);

  constexpr uint32_t gpuDataPort = 0x1f801810u;
  // The homogeneous companion must apply this offset as (dx*W, dy*W),
  // including at the two intersections generated from the W=0 vertex.
  mem.write32(gpuDataPort, 0xE5000000u | (4u << 11) | 3u);
  mem.write32(gpuDataPort, 0x200000FFu);
  for (std::size_t i = 0; i < adjustedWords.size(); ++i) {
    ctx.pgxpGpr[4].invalidate();
    ctx.r[4] = pgxp_load_word(&ctx, 4, ramBase + static_cast<uint32_t>(i * 4));
    EXPECT_EQ(ctx.r[4], adjustedWords[i]);
    ASSERT_TRUE(ctx.pgxpGpr[4].hasSignedProjection(ctx.r[4]));
    pgxp_store_word(&ctx, 4, gpuDataPort, ctx.r[4]);
  }

  gpu.snapshotDisplayBuffer();
  EXPECT_EQ(metrics::get("gpu.frame.precise_triangle"), 1u);
  EXPECT_EQ(metrics::get("gpu.frame.compatibility_triangle"), 0u);
  EXPECT_EQ(metrics::get("gpu.modern.near_clip.polygon"), 1u);
  EXPECT_EQ(metrics::get("gpu.modern.near_clip.output_triangle"), 2u);

  // The clipped/projective diamond reaches this sample only after the GPU draw
  // offset. The packed native triangle ends at X=24, so it remains absent from
  // authoritative VRAM and cannot make this assertion pass accidentally.
  EXPECT_EQ(gpu.getVRAM()[24 * gpu::GPU::VRAM_WIDTH + 32].raw, 0u);
  std::vector<gpu::Color32> display;
  uint32_t width = 0;
  uint32_t height = 0;
  const bool copied = gpu.copyDisplayRGBA8(display, width, height);
  EXPECT_TRUE(copied);
  ASSERT_GT(width, 32u);
  ASSERT_GT(height, 24u);
  EXPECT_EQ(display[24 * width + 32], (gpu::Color32{255, 0, 0, 255}));

  metrics::reset();
  metrics::setEnabledForTesting(false);
}
