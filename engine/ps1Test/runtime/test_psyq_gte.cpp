// Tests for the Group 1.C libgte HLEs (matrix/transform helpers).
//
// Strategy:
//   - Setter wrappers (SetRotMatrix, SetLightMatrix, ...) -- write a known
//     MATRIX into PS1 RAM, call the HLE, verify the corresponding cop2c[]
//     slots match what was loaded.
//   - Matrix builders (RotMatrix, TransMatrix, ScaleMatrix, MulMatrix) --
//     pick inputs whose 1.3.12 fixed-point output is exactly determined
//     (identity x identity, scale by 1.0, no rotation, etc.).
//   - Per-vertex transforms (RotTrans, RotTransPers) -- install identity
//     rotation + zero translation, feed a small SVECTOR, and check the
//     output VECTOR matches the input (modulo sf=1 shift handled by the
//     GTE backend).
//
// The 0.7 setters (SetGeomOffset/SetGeomScreen/SetDQA) are already covered
// in test_psyq_libapi.cpp; they're not duplicated here.

#include "runtime/cpu_context.h"
#include "runtime/gte.h"
#include "runtime/gte_control_provenance.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_libgte.h"
#include "runtime/psyq/psyq_registry.h"

#include <cstdint>
#include <gtest/gtest.h>

using namespace ps1;
using namespace ps1::psyq;

namespace {

constexpr uint32_t MAT_PTR = 0x80100000u;
constexpr uint32_t MAT2_PTR = 0x80100040u;
constexpr uint32_t VEC_PTR = 0x80100080u;
constexpr uint32_t SVEC_PTR = 0x801000A0u;
constexpr uint32_t OUT_PTR = 0x801000C0u;
constexpr uint32_t FLAG_PTR = 0x80100100u;

class PsyqGteTest : public ::testing::Test {
protected:
  Memory mem;
  recomp_context ctx;

  void SetUp() override {
    ctx.reset();
    ctx.mem = &mem;
  }

  void writeMatrixRotIdentity(uint32_t p) {
    int16_t I[3][3] = {{4096, 0, 0}, {0, 4096, 0}, {0, 0, 4096}};
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
        mem.write16(p + (i * 3 + j) * 2, static_cast<uint16_t>(I[i][j]));
  }

  // Lay out 9 distinct shorts at m[0][0]..m[2][2] so we can verify each
  // ends up in the correct cop2c slot.
  void writeMatrixDistinct(uint32_t p, int16_t base) {
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
        mem.write16(p + (i * 3 + j) * 2,
                    static_cast<uint16_t>(base + i * 3 + j));
  }

  void writeMatrixTrans(uint32_t p, int32_t tx, int32_t ty, int32_t tz) {
    mem.write32(p + 20, static_cast<uint32_t>(tx));
    mem.write32(p + 24, static_cast<uint32_t>(ty));
    mem.write32(p + 28, static_cast<uint32_t>(tz));
  }

  void writeVector(uint32_t p, int32_t x, int32_t y, int32_t z) {
    mem.write32(p + 0, static_cast<uint32_t>(x));
    mem.write32(p + 4, static_cast<uint32_t>(y));
    mem.write32(p + 8, static_cast<uint32_t>(z));
    mem.write32(p + 12, 0);
  }

  void writeSVector(uint32_t p, int16_t x, int16_t y, int16_t z) {
    mem.write16(p + 0, static_cast<uint16_t>(x));
    mem.write16(p + 2, static_cast<uint16_t>(y));
    mem.write16(p + 4, static_cast<uint16_t>(z));
    mem.write16(p + 6, 0);
  }

  int16_t readMatShort(uint32_t p, int i, int j) {
    return static_cast<int16_t>(mem.read16(p + (i * 3 + j) * 2));
  }
};

} // namespace

// InitGeom -- projection defaults from the linked PsyQ routine

TEST_F(PsyqGteTest, InitGeomProgramsPsyqProjectionDefaults) {
  for (uint32_t &reg : ctx.cop2c)
    reg = 0xDEADBEEFu;

  hle_libgte_InitGeom(&ctx);

  EXPECT_EQ(ctx.cop2c[GTE_ZSF3], 0x00000155u);
  EXPECT_EQ(ctx.cop2c[GTE_ZSF4], 0x00000100u);
  EXPECT_EQ(ctx.cop2c[GTE_H], 0x000003E8u);
  EXPECT_EQ(ctx.cop2c[GTE_DQA], 0xFFFFEF9Eu);
  EXPECT_EQ(ctx.cop2c[GTE_DQB], 0x01400000u);
  EXPECT_EQ(ctx.cop2c[GTE_OFX], 0u);
  EXPECT_EQ(ctx.cop2c[GTE_OFY], 0u);
}

// Setters: matrix loaders

TEST_F(PsyqGteTest, SetRotMatrixCopiesNineShortsIntoCop2C0to4) {
  // Load distinct values and verify packing.  Bytes 0..1 = m[0][0],
  // 2..3 = m[0][1], etc.  Per the GTE register layout cop2c[0] holds
  // (m01<<16 | m00), cop2c[1] = (m10<<16 | m02), etc.
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      mem.write16(MAT_PTR + (i * 3 + j) * 2,
                  static_cast<uint16_t>(0x1000 + i * 3 + j));
  // Padding word at offset 18..19 -- should land in high half of
  // cop2c[GTE_RT33].
  mem.write16(MAT_PTR + 18, 0xCAFE);

  ctx.r[A0] = MAT_PTR;
  hle_libgte_SetRotMatrix(&ctx);

  EXPECT_EQ(ctx.cop2c[GTE_RT11RT12], (0x1001u << 16) | 0x1000u);
  EXPECT_EQ(ctx.cop2c[GTE_RT13RT21], (0x1003u << 16) | 0x1002u);
  EXPECT_EQ(ctx.cop2c[GTE_RT22RT23], (0x1005u << 16) | 0x1004u);
  EXPECT_EQ(ctx.cop2c[GTE_RT31RT32], (0x1007u << 16) | 0x1006u);
  EXPECT_EQ(ctx.cop2c[GTE_RT33] & 0xFFFFu, 0x1008u);
}

TEST_F(PsyqGteTest, SetTransMatrixCopiesT0T1T2IntoTrxTryTrz) {
  writeMatrixTrans(MAT_PTR, 0x11111111, 0x22222222,
                   static_cast<int32_t>(0xFFFFCCCC));
  ctx.r[A0] = MAT_PTR;
  hle_libgte_SetTransMatrix(&ctx);
  EXPECT_EQ(ctx.cop2c[GTE_TRX], 0x11111111u);
  EXPECT_EQ(ctx.cop2c[GTE_TRY], 0x22222222u);
  EXPECT_EQ(ctx.cop2c[GTE_TRZ], 0xFFFFCCCCu);
}

TEST_F(PsyqGteTest, HostMatrixLineageIgnoresStalePcButPreservesSourceIdentity) {
  writeMatrixRotIdentity(MAT_PTR);
  writeMatrixTrans(MAT_PTR, 1, 2, 3);
  ctx.pc = 0x80012340u;
  ctx.r[A0] = MAT_PTR;
  hle_libgte_SetRotMatrix(&ctx);
  hle_libgte_SetTransMatrix(&ctx);
  ASSERT_TRUE(GTE::projectionTransformLineageComplete(&ctx));
  const uint64_t first = GTE::projectionTransformLineage(&ctx);
  EXPECT_EQ(ctx.pc, 0x80012340u);
  EXPECT_EQ(ctx.gteControlWriterPc[GTE_RT11RT12],
            static_cast<uint32_t>(HostGteControlWriter::PsyqSetRotationMatrix));
  EXPECT_EQ(ctx.gteControlWriterPc[GTE_TRX],
            static_cast<uint32_t>(HostGteControlWriter::PsyqSetTranslationMatrix));

  // Numeric animation and the caller's previous instruction are not a new
  // transform construction. Source addresses and register semantics remain.
  ctx.pc = 0x80045678u;
  writeMatrixDistinct(MAT_PTR, 12);
  writeMatrixTrans(MAT_PTR, 4, 5, 6);
  hle_libgte_SetRotMatrix(&ctx);
  hle_libgte_SetTransMatrix(&ctx);
  EXPECT_EQ(GTE::projectionTransformLineage(&ctx), first);
  EXPECT_EQ(ctx.pc, 0x80045678u);
  EXPECT_EQ(ctx.cop2c[GTE_TRX], 4u);
  EXPECT_EQ(ctx.gteControlSourceToken[GTE_TRZ],
            static_cast<uint64_t>(MAT_PTR + 28u) + 1u);

  writeMatrixRotIdentity(MAT2_PTR);
  writeMatrixTrans(MAT2_PTR, 1, 2, 3);
  ctx.r[A0] = MAT2_PTR;
  hle_libgte_SetRotMatrix(&ctx);
  hle_libgte_SetTransMatrix(&ctx);
  EXPECT_NE(GTE::projectionTransformLineage(&ctx), first);
  GTE::writeControl(&ctx, GTE_TRX, 4u);
  EXPECT_EQ(ctx.gteControlWriterPc[GTE_TRX], ctx.pc);
}

TEST_F(PsyqGteTest, SetLightMatrixWritesL11ThroughL33) {
  writeMatrixDistinct(MAT_PTR, 0x2000);
  ctx.r[A0] = MAT_PTR;
  hle_libgte_SetLightMatrix(&ctx);
  EXPECT_EQ(ctx.cop2c[GTE_L11L12], (0x2001u << 16) | 0x2000u);
  EXPECT_EQ(ctx.cop2c[GTE_L13L21], (0x2003u << 16) | 0x2002u);
  EXPECT_EQ(ctx.cop2c[GTE_L22L23], (0x2005u << 16) | 0x2004u);
  EXPECT_EQ(ctx.cop2c[GTE_L31L32], (0x2007u << 16) | 0x2006u);
  EXPECT_EQ(ctx.cop2c[GTE_L33] & 0xFFFFu, 0x2008u);
}

TEST_F(PsyqGteTest, SetColorMatrixWritesLR1ThroughLB3) {
  writeMatrixDistinct(MAT_PTR, 0x3000);
  ctx.r[A0] = MAT_PTR;
  hle_libgte_SetColorMatrix(&ctx);
  EXPECT_EQ(ctx.cop2c[GTE_LR1LR2], (0x3001u << 16) | 0x3000u);
  EXPECT_EQ(ctx.cop2c[GTE_LR3LG1], (0x3003u << 16) | 0x3002u);
  EXPECT_EQ(ctx.cop2c[GTE_LG2LG3], (0x3005u << 16) | 0x3004u);
  EXPECT_EQ(ctx.cop2c[GTE_LB1LB2], (0x3007u << 16) | 0x3006u);
  EXPECT_EQ(ctx.cop2c[GTE_LB3] & 0xFFFFu, 0x3008u);
}

TEST_F(PsyqGteTest, SetBackColorWritesRBKGBKBBK) {
  ctx.r[A0] = 0x10000;
  ctx.r[A1] = 0x20000;
  ctx.r[A2] = 0x30000;
  hle_libgte_SetBackColor(&ctx);
  EXPECT_EQ(ctx.cop2c[GTE_RBK], 0x10000u);
  EXPECT_EQ(ctx.cop2c[GTE_GBK], 0x20000u);
  EXPECT_EQ(ctx.cop2c[GTE_BBK], 0x30000u);
}

TEST_F(PsyqGteTest, SetFarColorWritesRFCGFCBFC) {
  ctx.r[A0] = 0x100;
  ctx.r[A1] = 0x200;
  ctx.r[A2] = 0x300;
  hle_libgte_SetFarColor(&ctx);
  EXPECT_EQ(ctx.cop2c[GTE_RFC], 0x100u);
  EXPECT_EQ(ctx.cop2c[GTE_GFC], 0x200u);
  EXPECT_EQ(ctx.cop2c[GTE_BFC], 0x300u);
}

// Matrix builders

TEST_F(PsyqGteTest, RotMatrixZeroAnglesProducesIdentity) {
  // r = (0, 0, 0) -> cos=4096, sin=0 on every axis -> M = I.
  writeSVector(SVEC_PTR, 0, 0, 0);
  ctx.r[A0] = SVEC_PTR;
  ctx.r[A1] = MAT_PTR;
  hle_libgte_RotMatrix(&ctx);

  EXPECT_EQ(ctx.r[V0], MAT_PTR);
  EXPECT_EQ(readMatShort(MAT_PTR, 0, 0), 4096);
  EXPECT_EQ(readMatShort(MAT_PTR, 0, 1), 0);
  EXPECT_EQ(readMatShort(MAT_PTR, 0, 2), 0);
  EXPECT_EQ(readMatShort(MAT_PTR, 1, 0), 0);
  EXPECT_EQ(readMatShort(MAT_PTR, 1, 1), 4096);
  EXPECT_EQ(readMatShort(MAT_PTR, 1, 2), 0);
  EXPECT_EQ(readMatShort(MAT_PTR, 2, 0), 0);
  EXPECT_EQ(readMatShort(MAT_PTR, 2, 1), 0);
  EXPECT_EQ(readMatShort(MAT_PTR, 2, 2), 4096);
}

TEST_F(PsyqGteTest, RotMatrixZ90DegreesRotatesXIntoY) {
  // r = (0, 0, 1024) -- pure 90 deg Z rotation.
  // Rz(90 deg) = [[0,-1,0],[1,0,0],[0,0,1]] -> in 1.3.12 fixed point,
  // 1.0 = 4096, sin(90 deg)=4096, cos(90 deg)=0 within rounding.
  writeSVector(SVEC_PTR, 0, 0, 1024);
  ctx.r[A0] = SVEC_PTR;
  ctx.r[A1] = MAT_PTR;
  hle_libgte_RotMatrix(&ctx);

  // The matrix should be approximately:
  //   [    0  -4096   0]
  //   [ 4096      0   0]
  //   [    0      0 4096]
  // Allow +/-1 tolerance for cos(90 deg) -- std::cos may round to +/-1 instead
  // of 0.
  EXPECT_NEAR(readMatShort(MAT_PTR, 0, 0), 0, 1);
  EXPECT_NEAR(readMatShort(MAT_PTR, 0, 1), -4096, 1);
  EXPECT_NEAR(readMatShort(MAT_PTR, 1, 0), 4096, 1);
  EXPECT_NEAR(readMatShort(MAT_PTR, 1, 1), 0, 1);
  EXPECT_NEAR(readMatShort(MAT_PTR, 2, 2), 4096, 1);
}

TEST_F(PsyqGteTest, TransMatrixCopiesVectorIntoMt) {
  writeMatrixTrans(MAT_PTR, 0, 0, 0); // start zeroed
  writeVector(VEC_PTR, 0x123, 0x456, static_cast<int32_t>(0xFFFFFE00));
  ctx.r[A0] = MAT_PTR;
  ctx.r[A1] = VEC_PTR;
  hle_libgte_TransMatrix(&ctx);

  EXPECT_EQ(ctx.r[V0], MAT_PTR);
  EXPECT_EQ(static_cast<int32_t>(mem.read32(MAT_PTR + 20)), 0x123);
  EXPECT_EQ(static_cast<int32_t>(mem.read32(MAT_PTR + 24)), 0x456);
  EXPECT_EQ(mem.read32(MAT_PTR + 28), 0xFFFFFE00u);
}

TEST_F(PsyqGteTest, ScaleMatrixIdentityScaleLeavesIdentity) {
  writeMatrixRotIdentity(MAT_PTR);
  // scale = (1.0, 1.0, 1.0) in 1.3.12.
  writeVector(VEC_PTR, 4096, 4096, 4096);
  ctx.r[A0] = MAT_PTR;
  ctx.r[A1] = VEC_PTR;
  hle_libgte_ScaleMatrix(&ctx);
  EXPECT_EQ(readMatShort(MAT_PTR, 0, 0), 4096);
  EXPECT_EQ(readMatShort(MAT_PTR, 1, 1), 4096);
  EXPECT_EQ(readMatShort(MAT_PTR, 2, 2), 4096);
  EXPECT_EQ(readMatShort(MAT_PTR, 0, 1), 0);
  EXPECT_EQ(readMatShort(MAT_PTR, 1, 2), 0);
}

TEST_F(PsyqGteTest, ScaleMatrixDoublesRowsByVectorComponents) {
  writeMatrixRotIdentity(MAT_PTR);
  // scale x=2, y=2, z=0.5 in 1.3.12.
  writeVector(VEC_PTR, 8192, 8192, 2048);
  ctx.r[A0] = MAT_PTR;
  ctx.r[A1] = VEC_PTR;
  hle_libgte_ScaleMatrix(&ctx);
  EXPECT_EQ(readMatShort(MAT_PTR, 0, 0), 8192); // 4096*2
  EXPECT_EQ(readMatShort(MAT_PTR, 1, 1), 8192);
  EXPECT_EQ(readMatShort(MAT_PTR, 2, 2), 2048); // 4096*0.5
}

TEST_F(PsyqGteTest, MulMatrixIdentityTimesIdentityIsIdentity) {
  writeMatrixRotIdentity(MAT_PTR);
  writeMatrixRotIdentity(MAT2_PTR);
  ctx.r[A0] = MAT_PTR;
  ctx.r[A1] = MAT2_PTR;
  hle_libgte_MulMatrix(&ctx);
  EXPECT_EQ(ctx.r[V0], MAT_PTR);
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      EXPECT_EQ(readMatShort(MAT_PTR, i, j), (i == j) ? 4096 : 0)
          << "m[" << i << "][" << j << "] mismatch";
}

TEST_F(PsyqGteTest, MulMatrixDiagonalProductDoublesCorrectly) {
  // m0 = diag(2,2,2)  (1.3.12 -> 8192 each)
  // m1 = diag(2,2,2)
  // m0 = m0 x m1 = diag(4,4,4)  (in 1.3.12 -> 16384 each)
  int16_t two = 8192;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      mem.write16(MAT_PTR + (i * 3 + j) * 2,
                  static_cast<uint16_t>((i == j) ? two : 0));
      mem.write16(MAT2_PTR + (i * 3 + j) * 2,
                  static_cast<uint16_t>((i == j) ? two : 0));
    }
  ctx.r[A0] = MAT_PTR;
  ctx.r[A1] = MAT2_PTR;
  hle_libgte_MulMatrix(&ctx);
  EXPECT_EQ(readMatShort(MAT_PTR, 0, 0), 16384);
  EXPECT_EQ(readMatShort(MAT_PTR, 1, 1), 16384);
  EXPECT_EQ(readMatShort(MAT_PTR, 2, 2), 16384);
  EXPECT_EQ(readMatShort(MAT_PTR, 0, 1), 0);
}

// Per-vertex transforms

TEST_F(PsyqGteTest, RotTransIdentityRTZeroTRReturnsInputVector) {
  // RT = I (1.3.12, diagonal = 4096), TR = 0.
  // MVMVA computes mac_i = sum(R[i][k] * v[k]); with sf=1 the result is
  // shifted right by 12, so mac_i = v[i] for an int16 input vector.
  writeMatrixRotIdentity(MAT_PTR);
  ctx.r[A0] = MAT_PTR;
  hle_libgte_SetRotMatrix(&ctx);
  ctx.cop2c[GTE_TRX] = 0;
  ctx.cop2c[GTE_TRY] = 0;
  ctx.cop2c[GTE_TRZ] = 0;

  writeSVector(SVEC_PTR, 1, 2, 3);
  ctx.r[A0] = SVEC_PTR;
  ctx.r[A1] = OUT_PTR;
  ctx.r[A2] = FLAG_PTR;
  hle_libgte_RotTrans(&ctx);

  EXPECT_EQ(static_cast<int32_t>(mem.read32(OUT_PTR + 0)), 1);
  EXPECT_EQ(static_cast<int32_t>(mem.read32(OUT_PTR + 4)), 2);
  EXPECT_EQ(static_cast<int32_t>(mem.read32(OUT_PTR + 8)), 3);
  // FLAG should be zero (no overflow / saturation) for these tiny inputs.
  EXPECT_EQ(mem.read32(FLAG_PTR), 0u);
}

TEST_F(PsyqGteTest, RotTransAddsTranslationVector) {
  writeMatrixRotIdentity(MAT_PTR);
  ctx.r[A0] = MAT_PTR;
  hle_libgte_SetRotMatrix(&ctx);
  // TR = (10, 20, 30) -- applied as (TR << 12) + R*v, then >>12 (sf=1).
  // For v = 0 the result is exactly TR.
  ctx.cop2c[GTE_TRX] = 10;
  ctx.cop2c[GTE_TRY] = 20;
  ctx.cop2c[GTE_TRZ] = 30;

  writeSVector(SVEC_PTR, 0, 0, 0);
  ctx.r[A0] = SVEC_PTR;
  ctx.r[A1] = OUT_PTR;
  ctx.r[A2] = 0; // no flag pointer
  hle_libgte_RotTrans(&ctx);

  EXPECT_EQ(static_cast<int32_t>(mem.read32(OUT_PTR + 0)), 10);
  EXPECT_EQ(static_cast<int32_t>(mem.read32(OUT_PTR + 4)), 20);
  EXPECT_EQ(static_cast<int32_t>(mem.read32(OUT_PTR + 8)), 30);
}

TEST_F(PsyqGteTest, RotTransPersWritesOutputsAndDoesNotCrash) {
  // Build a sane projection: identity rotation, zero translation,
  // H = 256, OFX = OFY = 0, DQA = DQB = 0.  The point goes through
  // perspective division but all we verify here is that the wrapper:
  //   1. populates *sxy / *p / *flag without faulting,
  //   2. returns a value (in $v0) -- we don't pin its exact magnitude.
  writeMatrixRotIdentity(MAT_PTR);
  ctx.r[A0] = MAT_PTR;
  hle_libgte_SetRotMatrix(&ctx);
  ctx.cop2c[GTE_TRX] = 0;
  ctx.cop2c[GTE_TRY] = 0;
  ctx.cop2c[GTE_TRZ] = 0;
  ctx.cop2c[GTE_OFX] = 0;
  ctx.cop2c[GTE_OFY] = 0;
  ctx.cop2c[GTE_H] = 256;
  ctx.cop2c[GTE_DQA] = 0;
  ctx.cop2c[GTE_DQB] = 0;

  writeSVector(SVEC_PTR, 100, 200, 1024);
  // Pre-poison the output slots so we can prove they were written.
  mem.write32(OUT_PTR + 0, 0xDEADBEEF);
  mem.write32(OUT_PTR + 4, 0xDEADBEEF);
  mem.write32(FLAG_PTR + 0, 0xDEADBEEF);

  ctx.r[A0] = SVEC_PTR;
  ctx.r[A1] = OUT_PTR;     // *sxy
  ctx.r[A2] = OUT_PTR + 4; // *p
  ctx.r[A3] = FLAG_PTR;    // *flag
  hle_libgte_RotTransPers(&ctx);

  EXPECT_NE(mem.read32(OUT_PTR + 0), 0xDEADBEEFu);
  EXPECT_NE(mem.read32(FLAG_PTR), 0xDEADBEEFu);

  const uint32_t packedSxy = mem.read32(OUT_PTR);
  PGXPValue projected;
  ASSERT_TRUE(mem.lookupPrecision(OUT_PTR, packedSxy, projected));
  EXPECT_TRUE(projected.hasXYZ(packedSxy));
  EXPECT_EQ(projected.origin, PGXPOrigin::GteProjection);
  EXPECT_FLOAT_EQ(projected.x, ctx.pgxpGteData[GTE_SXY2].x);
  EXPECT_FLOAT_EQ(projected.y, ctx.pgxpGteData[GTE_SXY2].y);
  EXPECT_FLOAT_EQ(projected.z, ctx.pgxpGteData[GTE_SXY2].z);
}

// Registry coverage

TEST_F(PsyqGteTest, RegisterLibgteCoversAllNewNames) {
  psyq_register_libgte();

  const char *names[] = {
      // 0.7
      "libgte_SetGeomOffset",
      "libgte_SetGeomScreen",
      "libgte_SetDQA",
      // 1.C -- control-register loaders
      "libgte_InitGeom",
      "libgte_SetRotMatrix",
      "libgte_SetTransMatrix",
      "libgte_SetLightMatrix",
      "libgte_SetColorMatrix",
      "libgte_SetBackColor",
      "libgte_SetFarColor",
      // 1.C -- matrix builders
      "libgte_RotMatrix",
      "libgte_TransMatrix",
      "libgte_ScaleMatrix",
      "libgte_MulMatrix",
      // 1.C -- per-vertex transforms
      "libgte_RotTrans",
      "libgte_RotTransPers",
  };
  // Set up RAM so dispatch through any entry doesn't read from address 0.
  writeMatrixRotIdentity(MAT_PTR);
  writeMatrixRotIdentity(MAT2_PTR);
  writeMatrixTrans(MAT_PTR, 0, 0, 0);
  writeVector(VEC_PTR, 4096, 4096, 4096);
  writeSVector(SVEC_PTR, 0, 0, 0);

  for (const char *n : names) {
    ctx.reset();
    ctx.mem = &mem;
    ctx.r[A0] = MAT_PTR;
    ctx.r[A1] = MAT2_PTR;
    ctx.r[A2] = OUT_PTR;
    ctx.r[A3] = FLAG_PTR;
    EXPECT_NO_FATAL_FAILURE(psyq_dispatch(n, &ctx))
        << "psyq_dispatch failed for: " << n;
  }
}
