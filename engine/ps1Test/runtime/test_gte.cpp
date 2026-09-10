// Tests for ps1Runtime -- GTE (Geometry Transform Engine)
// Validates the 22 GTE commands with PS1-accurate fixed-point math

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <runtime/games/crash3_profile.h>
#include <runtime/gte.h>
#include <runtime/metrics.h>
#include <sstream>

using namespace ps1;

// Test Fixture

class GTETest : public ::testing::Test {
protected:
  CPUContext ctx;
  void SetUp() override { ctx.reset(); }
  void TearDown() override { games::crash3::configure({}); }

  // Helper to set rotation matrix (identity in 4.12 format)
  void setIdentityRotation() {
    // RT11=4096 RT12=0
    ctx.cop2c[GTE_RT11RT12] = static_cast<uint32_t>(4096) | (0u << 16);
    // RT13=0 RT21=0
    ctx.cop2c[GTE_RT13RT21] = 0;
    // RT22=4096 RT23=0
    ctx.cop2c[GTE_RT22RT23] = static_cast<uint32_t>(4096) | (0u << 16);
    // RT31=0 RT32=0
    ctx.cop2c[GTE_RT31RT32] = 0;
    // RT33=4096
    ctx.cop2c[GTE_RT33] = 4096;
  }

  void setTranslation(int32_t x, int32_t y, int32_t z) {
    ctx.cop2c[GTE_TRX] = static_cast<uint32_t>(x);
    ctx.cop2c[GTE_TRY] = static_cast<uint32_t>(y);
    ctx.cop2c[GTE_TRZ] = static_cast<uint32_t>(z);
  }

  void setV0(int16_t x, int16_t y, int16_t z) {
    ctx.cop2d[GTE_VXY0] =
        static_cast<uint32_t>(static_cast<uint16_t>(x)) |
        (static_cast<uint32_t>(static_cast<uint16_t>(y)) << 16);
    ctx.cop2d[GTE_VZ0] = static_cast<uint32_t>(static_cast<uint16_t>(z));
  }

  void setProjection(uint16_t h, int32_t ofx, int32_t ofy, int16_t dqa,
                     int32_t dqb) {
    ctx.cop2c[GTE_H] = h;
    ctx.cop2c[GTE_OFX] = static_cast<uint32_t>(ofx);
    ctx.cop2c[GTE_OFY] = static_cast<uint32_t>(ofy);
    ctx.cop2c[GTE_DQA] = static_cast<uint32_t>(static_cast<uint16_t>(dqa));
    ctx.cop2c[GTE_DQB] = static_cast<uint32_t>(dqb);
  }
};

// NCLIP

TEST_F(GTETest, NCLIP_CounterClockwise) {
  // Triangle: (0,0) (100,0) (0,100) -> positive area
  ctx.cop2d[GTE_SXY0] = 0;                                // (0,0)
  ctx.cop2d[GTE_SXY1] = 100;                              // (100,0)
  ctx.cop2d[GTE_SXY2] = static_cast<uint32_t>(100) << 16; // (0,100)
  GTE::NCLIP(&ctx, true, true);
  int32_t mac0 = static_cast<int32_t>(ctx.cop2d[GTE_MAC0]);
  EXPECT_GT(mac0, 0); // Counter-clockwise -> positive
}

TEST_F(GTETest, NCLIP_Clockwise) {
  // Reversed triangle: (0,0) (0,100) (100,0) -> negative
  ctx.cop2d[GTE_SXY0] = 0;
  ctx.cop2d[GTE_SXY1] = static_cast<uint32_t>(100) << 16;
  ctx.cop2d[GTE_SXY2] = 100;
  GTE::NCLIP(&ctx, true, true);
  int32_t mac0 = static_cast<int32_t>(ctx.cop2d[GTE_MAC0]);
  EXPECT_LT(mac0, 0); // Clockwise -> negative
}

// SQR

TEST_F(GTETest, SQR_Squares) {
  ctx.cop2d[GTE_IR1] = static_cast<uint32_t>(static_cast<int16_t>(100));
  ctx.cop2d[GTE_IR2] = static_cast<uint32_t>(static_cast<int16_t>(-50));
  ctx.cop2d[GTE_IR3] = static_cast<uint32_t>(static_cast<int16_t>(200));
  GTE::SQR(&ctx, false, false);
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC1]), 10000);
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC2]), 2500);
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC3]), 40000);
}

// AVSZ3 / AVSZ4

TEST_F(GTETest, AVSZ3_AverageZ) {
  ctx.cop2d[GTE_SZ1] = 100;
  ctx.cop2d[GTE_SZ2] = 200;
  ctx.cop2d[GTE_SZ3] = 300;
  // ZSF3 = 4096/3 ~= 1365 in fixed 1.3.12
  ctx.cop2c[GTE_ZSF3] = static_cast<uint32_t>(static_cast<int16_t>(1365));
  GTE::AVSZ3(&ctx, true, true);
  // MAC0 = 1365 * (100+200+300) = 819000
  // OTZ = MAC0 >> 12 = 819000 >> 12 = 199
  EXPECT_EQ(ctx.cop2d[GTE_OTZ], 199u);
}

TEST_F(GTETest, AVSZ4_AverageZ) {
  ctx.cop2d[GTE_SZ0] = 100;
  ctx.cop2d[GTE_SZ1] = 200;
  ctx.cop2d[GTE_SZ2] = 300;
  ctx.cop2d[GTE_SZ3] = 400;
  // ZSF4 = 4096/4 = 1024
  ctx.cop2c[GTE_ZSF4] = static_cast<uint32_t>(static_cast<int16_t>(1024));
  GTE::AVSZ4(&ctx, true, true);
  // MAC0 = 1024 * 1000 = 1024000
  // OTZ = 1024000 >> 12 = 250
  EXPECT_EQ(ctx.cop2d[GTE_OTZ], 250u);
}

// OP (Outer Product)

TEST_F(GTETest, OP_CrossProduct) {
  // Rotation diagonal: R11=100, R22=200, R33=300
  ctx.cop2c[GTE_RT11RT12] = static_cast<uint32_t>(static_cast<uint16_t>(100));
  ctx.cop2c[GTE_RT22RT23] = static_cast<uint32_t>(static_cast<uint16_t>(200));
  ctx.cop2c[GTE_RT33] = static_cast<uint32_t>(static_cast<uint16_t>(300));
  // IR = (10, 20, 30)
  ctx.cop2d[GTE_IR1] = 10;
  ctx.cop2d[GTE_IR2] = 20;
  ctx.cop2d[GTE_IR3] = 30;
  GTE::OP(&ctx, false, false);
  // MAC1 = R22*IR3 - R33*IR2 = 200*30 - 300*20 = 6000-6000 = 0
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC1]), 0);
  // MAC2 = R33*IR1 - R11*IR3 = 300*10 - 100*30 = 3000-3000 = 0
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC2]), 0);
}

// GPF (General Purpose Interpolation)

TEST_F(GTETest, GPF_Multiply) {
  ctx.cop2d[GTE_IR0] =
      static_cast<uint32_t>(static_cast<int16_t>(4096)); // 1.0 in 4.12
  ctx.cop2d[GTE_IR1] = static_cast<uint32_t>(static_cast<int16_t>(100));
  ctx.cop2d[GTE_IR2] = static_cast<uint32_t>(static_cast<int16_t>(200));
  ctx.cop2d[GTE_IR3] = static_cast<uint32_t>(static_cast<int16_t>(300));
  GTE::GPF(&ctx, true, false); // sf=1 -> shift 12
  // MAC1 = 4096 * 100 = 409600 >> 12 = 100
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC1]), 100);
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC2]), 200);
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC3]), 300);
}

// MVMVA

TEST_F(GTETest, MVMVA_Identity) {
  setIdentityRotation();
  setTranslation(0, 0, 0);
  setV0(100, 200, 300);
  GTE::MVMVA(&ctx, 0, 0, 0, true, false);
  // Identity rotation x (100,200,300) + (0,0,0) = (100,200,300)
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC1]), 100);
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC2]), 200);
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC3]), 300);
}

TEST_F(GTETest, MVMVA_WithTranslation) {
  setIdentityRotation();
  setTranslation(1000, 2000, 3000);
  setV0(100, 200, 300);
  GTE::MVMVA(&ctx, 0, 0, 0, true, false);
  // Identity x V0 + TR = (100+1000, 200+2000, 300+3000)
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC1]), 1100);
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC2]), 2200);
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC3]), 3300);
}

// RTPS / perspective division

TEST_F(GTETest, RTPS_UsesRefinedUnrReciprocal) {
  setIdentityRotation();
  setTranslation(0, 0, 0);
  setV0(100, 50, 1000);
  setProjection(/*h=*/1000, /*ofx=*/160 << 16, /*ofy=*/120 << 16,
                /*dqa=*/0, /*dqb=*/0);

  GTE::RTPS(&ctx, true, false);

  // H == SZ3 gives a 1.0 perspective factor (0x10000).  The projected
  // vertex is therefore OFX/OFY plus the unscaled X/Y coordinates.
  EXPECT_EQ(ctx.cop2d[GTE_SZ3], 1000u);
  EXPECT_EQ(static_cast<int16_t>(ctx.cop2d[GTE_SXY2] & 0xFFFF), 260);
  EXPECT_EQ(static_cast<int16_t>(ctx.cop2d[GTE_SXY2] >> 16), 170);
  EXPECT_EQ(ctx.cop2c[GTE_FLAG] & gte_flag::DIV_OVERFLOW, 0u);
  ASSERT_TRUE(ctx.pgxpGteData[GTE_SXY2].hasXYZ(ctx.cop2d[GTE_SXY2]));
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].x, 260.0f, 0.001f);
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].y, 170.0f, 0.001f);
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].z, 1000.0f, 0.001f);
}

TEST_F(GTETest, Crash3WidescreenPreservesGuestGteAndWidensPgxpPresentation) {
  games::crash3::configure({.enabled = true, .widescreen16x9 = true});
  setIdentityRotation();
  setTranslation(0, 0, 0);
  setV0(100, 50, 1000);
  setProjection(/*h=*/1000, /*ofx=*/160 << 16, /*ofy=*/120 << 16,
                /*dqa=*/0, /*dqb=*/0);

  GTE::RTPS(&ctx, true, false);

  // Guest-visible SXY remains the native PS1 result. Only the parallel PGXP
  // presentation coordinate gets the 3/4 horizontal displacement.
  EXPECT_EQ(static_cast<int16_t>(ctx.cop2d[GTE_SXY2] & 0xFFFF), 260);
  EXPECT_EQ(static_cast<int16_t>(ctx.cop2d[GTE_SXY2] >> 16), 170);
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].x, 235.0f, 0.001f);
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].y, 170.0f, 0.001f);
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].z, 1000.0f, 0.001f);
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].qx, 235000.0, 0.001);
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].qy, 170000.0, 0.001);
}

TEST_F(GTETest, Crash3RendererReadsWideSxyWithoutChangingNclipFifo) {
  games::crash3::configure({.enabled = true, .widescreen16x9 = true});
  ctx.pc = 0x80041838u;
  ctx.cop2c[GTE_OFX] = 256u << 16;
  ctx.cop2d[GTE_SXY0] = uint32_t{100} | (uint32_t{50} << 16);
  ctx.cop2d[GTE_SXY1] = uint32_t{300} | (uint32_t{50} << 16);
  ctx.cop2d[GTE_SXY2] = uint32_t{100} | (uint32_t{200} << 16);

  const uint32_t presented = GTE::readData(&ctx, GTE_SXY1);
  EXPECT_EQ(static_cast<int16_t>(presented), 289);
  EXPECT_EQ(ctx.cop2d[GTE_SXY1], uint32_t{300} | (uint32_t{50} << 16));

  GTE::NCLIP(&ctx, false, false);
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC0]), 30000);
}

TEST_F(GTETest, Crash3WorldViewportRejectReadsWideSxyWithoutChangingFifo) {
  games::crash3::configure({.enabled = true, .widescreen16x9 = true});
  ctx.pc = 0x8003CECCu;
  ctx.cop2c[GTE_OFX] = 256u << 16;
  ctx.cop2d[GTE_SXY0] = uint32_t{100} | (uint32_t{50} << 16);
  ctx.cop2d[GTE_SXY1] = uint32_t{300} | (uint32_t{50} << 16);
  ctx.cop2d[GTE_SXY2] = uint32_t{100} | (uint32_t{200} << 16);

  const uint32_t presented = GTE::readData(&ctx, GTE_SXY0);
  EXPECT_EQ(static_cast<int16_t>(presented), 139);
  EXPECT_EQ(ctx.cop2d[GTE_SXY0], uint32_t{100} | (uint32_t{50} << 16));

  GTE::NCLIP(&ctx, false, false);
  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC0]), 30000);
}

TEST_F(GTETest, Crash3OffscreenWorldPassKeepsNativeSxy) {
  games::crash3::configure({.enabled = true, .widescreen16x9 = true});
  ctx.pc = 0x8003CECCu;
  ctx.cop2c[GTE_OFX] = 32u << 16;
  ctx.cop2d[GTE_SXY0] = uint32_t{100} | (uint32_t{50} << 16);

  EXPECT_EQ(GTE::readData(&ctx, GTE_SXY0),
            uint32_t{100} | (uint32_t{50} << 16));
  EXPECT_EQ(ctx.cop2d[GTE_SXY0], uint32_t{100} | (uint32_t{50} << 16));
}

TEST_F(GTETest, Crash3PointSpriteCullReadsWideSxy) {
  games::crash3::configure({.enabled = true, .widescreen16x9 = true});
  ctx.pc = 0x8004367Cu;
  ctx.cop2c[GTE_OFX] = 256u << 16;
  ctx.cop2d[GTE_SXY2] = uint32_t{100} | (uint32_t{50} << 16);

  EXPECT_EQ(GTE::readData(&ctx, GTE_SXY2),
            uint32_t{139} | (uint32_t{50} << 16));
  EXPECT_EQ(ctx.cop2d[GTE_SXY2], uint32_t{100} | (uint32_t{50} << 16));
}

TEST_F(GTETest, RTPS_PgxpPreservesPreShiftTransformPrecision) {
  setIdentityRotation();
  // A fractional 4.12 rotation component is discarded by architectural MAC1
  // when sf=1, but must survive in the parallel PGXP projection.
  ctx.cop2c[GTE_RT11RT12] = 4097u;
  setTranslation(0, 0, 0);
  setV0(1, 0, 1000);
  setProjection(/*h=*/1000, /*ofx=*/160 << 16, /*ofy=*/120 << 16,
                /*dqa=*/0, /*dqb=*/0);

  GTE::RTPS(&ctx, true, false);

  EXPECT_EQ(static_cast<int32_t>(ctx.cop2d[GTE_MAC1]), 1);
  EXPECT_EQ(static_cast<int16_t>(ctx.cop2d[GTE_SXY2] & 0xFFFF), 161);
  ASSERT_TRUE(ctx.pgxpGteData[GTE_SXY2].hasXYZ(ctx.cop2d[GTE_SXY2]));
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].x, 161.000244140625f, 0.00001f);
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].y, 120.0f, 0.00001f);
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].z, 1000.0f, 0.00001f);
}

TEST_F(GTETest, RTPS_PgxpKeepsSfFalseXYAndAlwaysScalesDepthFromFourTwelve) {
  setIdentityRotation();
  setTranslation(0, 0, 0);
  setV0(1, 0, 1000);
  setProjection(/*h=*/1, /*ofx=*/0, /*ofy=*/0, /*dqa=*/0, /*dqb=*/0);

  GTE::RTPS(&ctx, false, false);

  ASSERT_TRUE(ctx.pgxpGteData[GTE_SXY2].hasXYZ(ctx.cop2d[GTE_SXY2]));
  // With sf=0 X/Y retain their unshifted 4.12 magnitude, while SZ always
  // receives transform Z >> 12.  This asymmetric scaling is GTE semantics.
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].x, 4.096f, 0.00001f);
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].y, 0.0f, 0.00001f);
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].z, 1000.0f, 0.00001f);
}

TEST_F(GTETest, RTPS_PgxpPreservesFractionalDepthBeforeSzQuantization) {
  setIdentityRotation();
  ctx.cop2c[GTE_RT33] = 4097u;
  setTranslation(0, 0, 0);
  setV0(100, 0, 1000);
  setProjection(/*h=*/1000, /*ofx=*/160 << 16, /*ofy=*/120 << 16,
                /*dqa=*/0, /*dqb=*/0);

  GTE::RTPS(&ctx, true, false);

  EXPECT_EQ(ctx.cop2d[GTE_SZ3], 1000u);
  ASSERT_TRUE(ctx.pgxpGteData[GTE_SXY2].hasXYZ(ctx.cop2d[GTE_SXY2]));
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].z, 1000.244140625f, 0.00001f);
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].x, 160.0f + 100000.0f / 1000.244140625f,
              0.00002f);
}

TEST_F(GTETest, RTPT_PgxpPrecisionFollowsTheThreeVertexFifo) {
  setIdentityRotation();
  ctx.cop2c[GTE_RT11RT12] = 4097u;
  setTranslation(0, 0, 0);
  setV0(1, 0, 1000);
  ctx.cop2d[GTE_VXY1] = 2u;
  ctx.cop2d[GTE_VZ1] = 1000u;
  ctx.cop2d[GTE_VXY2] = 3u;
  ctx.cop2d[GTE_VZ2] = 1000u;
  setProjection(/*h=*/1000, /*ofx=*/160 << 16, /*ofy=*/120 << 16,
                /*dqa=*/0, /*dqb=*/0);

  GTE::RTPT(&ctx, true, false);

  for (uint8_t reg : {GTE_SXY0, GTE_SXY1, GTE_SXY2})
    ASSERT_TRUE(ctx.pgxpGteData[reg].hasXYZ(ctx.cop2d[reg]));
  EXPECT_NE(ctx.pgxpGteData[GTE_SXY0].projectionId,
            ctx.pgxpGteData[GTE_SXY1].projectionId);
  EXPECT_NE(ctx.pgxpGteData[GTE_SXY1].projectionId,
            ctx.pgxpGteData[GTE_SXY2].projectionId);
  EXPECT_EQ(ctx.pgxpGteData[GTE_SXY0].projectionContext,
            ctx.pgxpGteData[GTE_SXY1].projectionContext);
  EXPECT_EQ(ctx.pgxpGteData[GTE_SXY1].projectionContext,
            ctx.pgxpGteData[GTE_SXY2].projectionContext);
  EXPECT_NE(ctx.pgxpGteData[GTE_SXY0].projectionTransformValueFingerprint, 0u);
  EXPECT_EQ(ctx.pgxpGteData[GTE_SXY0].projectionTransformValueFingerprint,
            ctx.pgxpGteData[GTE_SXY1].projectionTransformValueFingerprint);
  EXPECT_EQ(ctx.pgxpGteData[GTE_SXY1].projectionTransformValueFingerprint,
            ctx.pgxpGteData[GTE_SXY2].projectionTransformValueFingerprint);
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY0].x, 161.000244140625f, 0.00001f);
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY1].x, 162.00048828125f, 0.00001f);
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].x, 163.000732421875f, 0.00001f);
}

TEST_F(GTETest, RTPS_PgxpKeepsSubpixelRatioBelowDivisionLimit) {
  setIdentityRotation();
  // Preserve a fractional camera-space X which the architectural MAC loses.
  ctx.cop2c[GTE_RT11RT12] = 4097u;
  setTranslation(0, 0, 0);
  setV0(7, 0, 501);
  setProjection(/*h=*/1000, /*ofx=*/0, /*ofy=*/0, /*dqa=*/0, /*dqb=*/0);

  GTE::RTPS(&ctx, true, false);

  EXPECT_EQ(ctx.cop2c[GTE_FLAG] & gte_flag::DIV_OVERFLOW, 0u);
  ASSERT_TRUE(ctx.pgxpGteData[GTE_SXY2].hasXYZ(ctx.cop2d[GTE_SXY2]));
  const float expectedX =
      static_cast<float>((7.0 * 4097.0 / 4096.0) * (1000.0 / 501.0));
  const float packedX =
      static_cast<float>(static_cast<int16_t>(ctx.cop2d[GTE_SXY2]));
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].x, expectedX, 0.00001f);
  EXPECT_LT(std::abs(ctx.pgxpGteData[GTE_SXY2].x - packedX), 1.0f);
  const PGXPValue &projection = ctx.pgxpGteData[GTE_SXY2];
  ASSERT_TRUE(projection.hasSignedProjection(ctx.cop2d[GTE_SXY2]));
  EXPECT_NEAR(projection.qx, 7.0f * 4097.0f / 4096.0f * 1000.0f, 0.001f);
  EXPECT_FLOAT_EQ(projection.qy, 0.0f);
  EXPECT_FLOAT_EQ(projection.nearDepth, 500.0f + 1.0f / 4096.0f);
  EXPECT_GT(projection.z, projection.nearDepth);
}

TEST_F(GTETest, RTPS_SaturatesDivisionAtExactlyTwo) {
  setIdentityRotation();
  setTranslation(0, 0, 0);
  setV0(10, 10, 500);
  setProjection(/*h=*/1000, /*ofx=*/0, /*ofy=*/0, /*dqa=*/0, /*dqb=*/0);

  GTE::RTPS(&ctx, true, false);

  EXPECT_NE(ctx.cop2c[GTE_FLAG] & gte_flag::DIV_OVERFLOW, 0u);
  ASSERT_TRUE(ctx.pgxpGteData[GTE_SXY2].hasXYZ(ctx.cop2d[GTE_SXY2]));
  constexpr float maximumProjectionRatio =
      static_cast<float>(0x1FFFFu) / 65536.0f;
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].x, 10.0f * maximumProjectionRatio,
              0.000001f);
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].y, 10.0f * maximumProjectionRatio,
              0.000001f);
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].z, 500.0f, 0.000001f);
  const float packedX =
      static_cast<float>(static_cast<int16_t>(ctx.cop2d[GTE_SXY2]));
  EXPECT_LT(std::abs(ctx.pgxpGteData[GTE_SXY2].x - packedX), 1.0f);
  const PGXPValue &projection = ctx.pgxpGteData[GTE_SXY2];
  ASSERT_TRUE(projection.hasSignedProjection(ctx.cop2d[GTE_SXY2]));
  EXPECT_FLOAT_EQ(projection.qx, 10000.0f);
  EXPECT_FLOAT_EQ(projection.qy, 10000.0f);
  EXPECT_FLOAT_EQ(projection.nearDepth, 500.0f + 1.0f / 4096.0f);
  EXPECT_LT(projection.z, projection.nearDepth);
}

TEST_F(GTETest, RTPS_PgxpMarksFirstUnsaturatedFourTwelveDepthAsNearPlane) {
  setIdentityRotation();
  // TRZ + RT33*VZ yields 500 + 1/4096, the first 4.12 depth strictly
  // beyond H/2. Architectural SZ3 still quantizes this to 500.
  ctx.cop2c[GTE_RT33] = 1u;
  setTranslation(0, 0, 500);
  setV0(10, 10, 1);
  setProjection(/*h=*/1000, /*ofx=*/0, /*ofy=*/0, /*dqa=*/0, /*dqb=*/0);

  GTE::RTPS(&ctx, true, false);

  const PGXPValue &projection = ctx.pgxpGteData[GTE_SXY2];
  ASSERT_TRUE(projection.hasSignedProjection(ctx.cop2d[GTE_SXY2]));
  EXPECT_FLOAT_EQ(projection.z, 500.0f + 1.0f / 4096.0f);
  EXPECT_FLOAT_EQ(projection.z, projection.nearDepth);
  EXPECT_FLOAT_EQ(projection.qx, 10000.0f);
  EXPECT_FLOAT_EQ(projection.qy, 10000.0f);
}

TEST_F(GTETest, RTPS_PgxpKeepsMaximumHNearPlaneTickDistinct) {
  setIdentityRotation();
  ctx.cop2c[GTE_RT33] = 1u;
  setTranslation(0, 0, 32767);
  setProjection(/*h=*/65535, /*ofx=*/0, /*ofy=*/0, /*dqa=*/0, /*dqb=*/0);

  // At maximum H, float32 cannot distinguish these adjacent 4.12 depths.
  // The homogeneous sidecar must: the exact H/2 boundary is saturated and
  // clipped, while its first representable 4.12 successor is retained.
  setV0(10, 10, 2048);
  GTE::RTPS(&ctx, true, false);
  const PGXPValue boundary = ctx.pgxpGteData[GTE_SXY2];
  ASSERT_TRUE(boundary.hasSignedProjection(ctx.cop2d[GTE_SXY2]));

  setV0(10, 10, 2049);
  GTE::RTPS(&ctx, true, false);
  const PGXPValue firstUnsaturated = ctx.pgxpGteData[GTE_SXY2];
  ASSERT_TRUE(firstUnsaturated.hasSignedProjection(ctx.cop2d[GTE_SXY2]));

  constexpr double boundaryDepth = 32767.5;
  constexpr double firstDepth = boundaryDepth + 1.0 / 4096.0;
  EXPECT_DOUBLE_EQ(boundary.z, boundaryDepth);
  EXPECT_DOUBLE_EQ(boundary.nearDepth, firstDepth);
  EXPECT_LT(boundary.z, boundary.nearDepth);
  EXPECT_DOUBLE_EQ(firstUnsaturated.z, firstDepth);
  EXPECT_DOUBLE_EQ(firstUnsaturated.nearDepth, firstDepth);
  EXPECT_EQ(firstUnsaturated.z, firstUnsaturated.nearDepth);
  EXPECT_NE(boundary.z, firstUnsaturated.z);
}

TEST_F(GTETest, RTPS_PgxpSaturatesAboveDivisionLimitAndClampsSxy) {
  setIdentityRotation();
  // Keep Z fractional to prove saturation does not fall back to quantized SZ3.
  ctx.cop2c[GTE_RT33] = 4097u;
  setTranslation(0, 0, 0);
  setV0(600, 10, 499);
  setProjection(/*h=*/1000, /*ofx=*/0, /*ofy=*/0, /*dqa=*/0, /*dqb=*/0);

  GTE::RTPS(&ctx, true, false);

  EXPECT_NE(ctx.cop2c[GTE_FLAG] & gte_flag::DIV_OVERFLOW, 0u);
  EXPECT_NE(ctx.cop2c[GTE_FLAG] & gte_flag::SX2_SATURATED, 0u);
  ASSERT_TRUE(ctx.pgxpGteData[GTE_SXY2].hasXYZ(ctx.cop2d[GTE_SXY2]));
  constexpr float maximumProjectionRatio =
      static_cast<float>(0x1FFFFu) / 65536.0f;
  EXPECT_EQ(ctx.pgxpGteData[GTE_SXY2].x, 1023.0f);
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].y, 10.0f * maximumProjectionRatio,
              0.000001f);
  EXPECT_NEAR(ctx.pgxpGteData[GTE_SXY2].z, 499.0f * 4097.0f / 4096.0f,
              0.00001f);
  const float packedY =
      static_cast<float>(static_cast<int16_t>(ctx.cop2d[GTE_SXY2] >> 16));
  EXPECT_LT(std::abs(ctx.pgxpGteData[GTE_SXY2].y - packedY), 1.0f);
  const PGXPValue &projection = ctx.pgxpGteData[GTE_SXY2];
  ASSERT_TRUE(projection.hasSignedProjection(ctx.cop2d[GTE_SXY2]));
  EXPECT_FLOAT_EQ(projection.qx, 600000.0f);
  EXPECT_FLOAT_EQ(projection.qy, 10000.0f);
  EXPECT_FLOAT_EQ(projection.nearDepth, 500.0f + 1.0f / 4096.0f);
  EXPECT_LT(projection.z, projection.nearDepth);
}

TEST_F(GTETest, RTPS_PgxpKeepsHomogeneousProjectionAtZeroDepth) {
  setIdentityRotation();
  setTranslation(0, 0, 0);
  setV0(10, 20, 0);
  setProjection(/*h=*/1000, /*ofx=*/160 << 16, /*ofy=*/120 << 16,
                /*dqa=*/0, /*dqb=*/0);

  GTE::RTPS(&ctx, true, false);

  const uint32_t packed = ctx.cop2d[GTE_SXY2];
  const PGXPValue &projection = ctx.pgxpGteData[GTE_SXY2];
  EXPECT_EQ(packed, 0x009F00B3u);
  EXPECT_FALSE(projection.hasXYZ(packed));
  ASSERT_TRUE(projection.hasSignedProjection(packed));
  EXPECT_EQ(projection.flags,
            PGXPValue::VALID_XY | PGXPValue::VALID_SIGNED_PROJECTION);
  constexpr float maximumProjectionRatio =
      static_cast<float>(0x1FFFFu) / 65536.0f;
  EXPECT_NEAR(projection.x, 160.0f + 10.0f * maximumProjectionRatio, 0.00001f);
  EXPECT_NEAR(projection.y, 120.0f + 20.0f * maximumProjectionRatio, 0.00001f);
  EXPECT_FLOAT_EQ(projection.z, 0.0f);
  EXPECT_FLOAT_EQ(projection.qx, 10000.0f);
  EXPECT_FLOAT_EQ(projection.qy, 20000.0f);
  EXPECT_FLOAT_EQ(projection.nearDepth, 500.0f + 1.0f / 4096.0f);
  EXPECT_NE(projection.projectionId, 0u);

  const PGXPResolvedVertex resolved = resolvePGXPVertex(projection, packed);
  EXPECT_TRUE(resolved.hasSignedProjection);
  EXPECT_FALSE(resolved.hasDepth);
  EXPECT_EQ(resolved.projectionId, projection.projectionId);
  EXPECT_EQ(resolved.projectionContext, projection.projectionContext);
  EXPECT_FLOAT_EQ(resolved.qx, projection.qx);
  EXPECT_FLOAT_EQ(resolved.qy, projection.qy);
  EXPECT_EQ(resolved.xSource, PGXPCoordinateSource::Precise);
  EXPECT_EQ(resolved.ySource, PGXPCoordinateSource::Precise);
  EXPECT_FLOAT_EQ(resolved.x, projection.x);
  EXPECT_FLOAT_EQ(resolved.y, projection.y);
}

TEST_F(GTETest, RTPS_PgxpSaturatesScreenProjectionWhenHAndDepthAreZero) {
  setIdentityRotation();
  setTranslation(0, 0, 0);
  setV0(10, 20, 0);
  setProjection(/*h=*/0, /*ofx=*/160 << 16, /*ofy=*/120 << 16,
                /*dqa=*/0, /*dqb=*/0);

  GTE::RTPS(&ctx, true, false);

  const uint32_t packed = ctx.cop2d[GTE_SXY2];
  const PGXPValue &projection = ctx.pgxpGteData[GTE_SXY2];
  constexpr float maximumProjectionRatio =
      static_cast<float>(0x1FFFFu) / 65536.0f;
  EXPECT_EQ(packed, 0x009F00B3u);
  EXPECT_EQ(ctx.cop2c[GTE_FLAG], 0x80020000u);
  ASSERT_TRUE(projection.hasSignedProjection(packed));
  EXPECT_FALSE(projection.hasXYZ(packed));
  EXPECT_EQ(projection.flags,
            PGXPValue::VALID_XY | PGXPValue::VALID_SIGNED_PROJECTION);
  EXPECT_NEAR(projection.x, 160.0f + 10.0f * maximumProjectionRatio, 0.00001f);
  EXPECT_NEAR(projection.y, 120.0f + 20.0f * maximumProjectionRatio, 0.00001f);
  EXPECT_DOUBLE_EQ(projection.z, 0.0);
  EXPECT_DOUBLE_EQ(projection.qx, 0.0);
  EXPECT_DOUBLE_EQ(projection.qy, 0.0);
  EXPECT_DOUBLE_EQ(projection.nearDepth, 1.0 / 4096.0);
}

TEST_F(GTETest, RTPS_PgxpKeepsHomogeneousProjectionBehindCamera) {
  setIdentityRotation();
  setTranslation(0, 0, 0);
  setV0(10, 20, -100);
  setProjection(/*h=*/1000, /*ofx=*/160 << 16, /*ofy=*/120 << 16,
                /*dqa=*/0, /*dqb=*/0);

  GTE::RTPS(&ctx, true, false);

  const uint32_t packed = ctx.cop2d[GTE_SXY2];
  const PGXPValue &projection = ctx.pgxpGteData[GTE_SXY2];
  EXPECT_EQ(packed, 0x009F00B3u);
  EXPECT_EQ(ctx.cop2c[GTE_FLAG], 0x80060000u);
  EXPECT_FALSE(projection.hasXYZ(packed));
  ASSERT_TRUE(projection.hasSignedProjection(packed));
  EXPECT_EQ(projection.flags,
            PGXPValue::VALID_XY | PGXPValue::VALID_SIGNED_PROJECTION);
  EXPECT_FLOAT_EQ(projection.z, -100.0f);
  EXPECT_FLOAT_EQ(projection.qx, -6000.0f);
  EXPECT_FLOAT_EQ(projection.qy, 8000.0f);
  EXPECT_FLOAT_EQ(projection.nearDepth, 500.0f + 1.0f / 4096.0f);
  constexpr float maximumProjectionRatio =
      static_cast<float>(0x1FFFFu) / 65536.0f;
  EXPECT_NEAR(projection.x, 160.0f + 10.0f * maximumProjectionRatio, 0.00001f);
  EXPECT_NEAR(projection.y, 120.0f + 20.0f * maximumProjectionRatio, 0.00001f);

  const PGXPResolvedVertex resolved = resolvePGXPVertex(projection, packed);
  EXPECT_TRUE(resolved.hasSignedProjection);
  EXPECT_FALSE(resolved.hasDepth);
  EXPECT_FLOAT_EQ(resolved.z, -100.0f);
  EXPECT_EQ(resolved.xSource, PGXPCoordinateSource::Precise);
  EXPECT_EQ(resolved.ySource, PGXPCoordinateSource::Precise);
  EXPECT_FLOAT_EQ(resolved.x, projection.x);
  EXPECT_FLOAT_EQ(resolved.y, projection.y);
}

TEST_F(GTETest,
       Crash3WideAdmissionKeepsExtremeBehindVertexForGeometricClipping) {
  games::crash3::configure({.enabled = true, .widescreen16x9 = true});
  setIdentityRotation();
  setTranslation(0, 0, 0);
  setV0(600, 20, -100);
  setProjection(/*h=*/1000, /*ofx=*/256 << 16, /*ofy=*/128 << 16,
                /*dqa=*/0, /*dqb=*/0);

  // Authenticate this as Crash's native-world projection, then reproduce its
  // exact 16:9 packet read. The architectural GTE value remains untouched.
  ctx.pc = 0x8003CE34u;
  GTE::RTPS(&ctx, true, false);
  const uint32_t architectural = ctx.cop2d[GTE_SXY2];
  ASSERT_NE(ctx.pgxpGteData[GTE_SXY2].gameProfileCallsite, 0u);
  ctx.pc = 0x8003CED4u;
  const uint32_t presented = GTE::readData(&ctx, GTE_SXY2);
  EXPECT_EQ(ctx.cop2d[GTE_SXY2], architectural);

  const PGXPValue &projection = ctx.pgxpGteData[GTE_SXY2];
  ASSERT_TRUE(projection.hasSignedProjection(presented));
  ASSERT_LT(projection.z, 0.0);
  const PGXPResolvedVertex resolved = resolvePGXPVertex(projection, presented);
  EXPECT_TRUE(resolved.hasSignedProjection);
  EXPECT_FALSE(resolved.hasDepth);
  EXPECT_EQ(resolved.xSource, PGXPCoordinateSource::PreciseGteSaturated);
  EXPECT_EQ(resolved.ySource, PGXPCoordinateSource::PreciseGteSaturated);
  EXPECT_FLOAT_EQ(resolved.x, projection.x);
  EXPECT_FLOAT_EQ(resolved.y, projection.y);
  EXPECT_DOUBLE_EQ(resolved.qx, projection.qx);
  EXPECT_DOUBLE_EQ(resolved.qy, projection.qy);

  PGXPValue unowned = projection;
  unowned.gameProfileCallsite = 0u;
  const PGXPResolvedVertex generic = resolvePGXPVertex(unowned, presented);
  EXPECT_EQ(generic.xSource, PGXPCoordinateSource::NativeDelta);
  EXPECT_TRUE((generic.reconciledMask() & PGXPResolvedVertex::RECONCILED_X) !=
              0u);
}

// FLAG register

TEST_F(GTETest, FlagClearOnCommand) {
  ctx.cop2c[GTE_FLAG] = 0xFFFFFFFF;
  ctx.cop2d[GTE_IR1] = 1;
  ctx.cop2d[GTE_IR2] = 1;
  ctx.cop2d[GTE_IR3] = 1;
  GTE::SQR(&ctx, false, false);
  // FLAG should have been reset at start (no overflow from 1*1)
  EXPECT_EQ(ctx.cop2c[GTE_FLAG] & 0x7F87E000, 0u);
}

// Register access

TEST_F(GTETest, LZCR_PositiveValue) {
  GTE::writeData(&ctx, GTE_LZCS, 0x00010000); // bit 16 set
  uint32_t lzcr = GTE::readData(&ctx, GTE_LZCR);
  EXPECT_EQ(lzcr, 15u); // 15 leading zeros before bit 16
}

TEST_F(GTETest, IRGB_ReadWrite) {
  GTE::writeData(&ctx, GTE_IRGB, 0x001F);    // R=31
  EXPECT_EQ(ctx.cop2d[GTE_IR1], 31u * 0x80); // 31 * 128 = 3968
}

TEST_F(GTETest, DataRegisterWritesExtendLikeHardware) {
  GTE::writeData(&ctx, GTE_IR1, 0x0000FFFF);
  EXPECT_EQ(ctx.cop2d[GTE_IR1], 0xFFFFFFFFu);

  GTE::writeData(&ctx, GTE_SZ3, 0xABCDFFFF);
  EXPECT_EQ(ctx.cop2d[GTE_SZ3], 0x0000FFFFu);
}

TEST_F(GTETest, SXYPReadsNewestAndWritePushesFIFO) {
  ctx.cop2d[GTE_SXY0] = 1;
  ctx.cop2d[GTE_SXY1] = 2;
  ctx.cop2d[GTE_SXY2] = 3;
  ctx.cop2d[GTE_SXYP] = 0xDEADBEEF;

  EXPECT_EQ(GTE::readData(&ctx, GTE_SXYP), 3u);
  GTE::writeData(&ctx, GTE_SXYP, 4);
  EXPECT_EQ(ctx.cop2d[GTE_SXY0], 2u);
  EXPECT_EQ(ctx.cop2d[GTE_SXY1], 3u);
  EXPECT_EQ(ctx.cop2d[GTE_SXY2], 4u);
}

TEST_F(GTETest, IRGBReadClampsInsteadOfWrapping) {
  ctx.cop2d[GTE_IR1] = static_cast<uint32_t>(-128);
  ctx.cop2d[GTE_IR2] = 0x1000;
  ctx.cop2d[GTE_IR3] = 0x0800;
  EXPECT_EQ(GTE::readData(&ctx, GTE_ORGB), 0x43E0u);
}

TEST_F(GTETest, ReadOnlyDataRegistersIgnoreWrites) {
  ctx.cop2d[GTE_ORGB] = 0x1234;
  GTE::writeData(&ctx, GTE_ORGB, 0xDEADBEEF);
  EXPECT_EQ(ctx.cop2d[GTE_ORGB], 0x1234u);

  GTE::writeData(&ctx, GTE_LZCS, 0x00010000);
  EXPECT_EQ(ctx.cop2d[GTE_LZCR], 15u);
  GTE::writeData(&ctx, GTE_LZCR, 7);
  EXPECT_EQ(ctx.cop2d[GTE_LZCR], 15u);
}

TEST_F(GTETest, ControlRegisterWritesExtendAndMaskLikeHardware) {
  GTE::writeControl(&ctx, GTE_DQA, 0x0000FFFF);
  EXPECT_EQ(ctx.cop2c[GTE_DQA], 0xFFFFFFFFu);

  GTE::writeControl(&ctx, GTE_FLAG, 0xFFFFFFFF);
  EXPECT_EQ(ctx.cop2c[GTE_FLAG], 0xFFFFF000u);

  GTE::writeControl(&ctx, GTE_FLAG, gte_flag::IR0_SATURATED);
  EXPECT_EQ(ctx.cop2c[GTE_FLAG], gte_flag::IR0_SATURATED);
}

TEST_F(GTETest, TracksExactControlAndModelSourceProvenance) {
  ctx.pc = 0x80012340u;
  GTE::writeControl(&ctx, GTE_RT11RT12, 0x00001000u, 0x80020000u);
  GTE::writeControl(&ctx, GTE_TRZ, 0x00000100u, 0x8002001Cu);
  EXPECT_EQ(ctx.gteControlWriterPc[GTE_RT11RT12], 0x80012340u);
  EXPECT_EQ(ctx.gteControlSourceToken[GTE_RT11RT12], 0x80020001u);
  EXPECT_EQ(ctx.gteControlSourceToken[GTE_TRZ], 0x8002001Du);
  EXPECT_LT(ctx.gteControlWriteGeneration[GTE_RT11RT12],
            ctx.gteControlWriteGeneration[GTE_TRZ]);

  GTE::writeData(&ctx, GTE_VXY0, 0x00020001u);
  EXPECT_EQ(ctx.gteDataSourceToken[GTE_VXY0], 0u);
  GTE::noteDataSource(&ctx, GTE_VXY0, 0x80030000u);
  EXPECT_EQ(ctx.gteDataSourceToken[GTE_VXY0], 0x80030001u);
  GTE::writeData(&ctx, GTE_VXY0, 0x00040003u);
  EXPECT_EQ(ctx.gteDataSourceToken[GTE_VXY0], 0u);
}

TEST_F(GTETest, ProjectionTraceCarriesIntrinsicTransformAndModelSources) {
  const char *path = "/tmp/ps1_gte_projection_provenance_test.jsonl";
  std::remove(path);
  setenv("PS1_TRACE_JSONL", path, 1);
  setenv("PS1_TRACE_GTE", "1", 1);
  ps1::metrics::resetTraceForTesting();
  ps1::metrics::setTraceFrame(12);

  ctx.pc = 0x800417ACu;
  const std::array<uint32_t, 5> rotationWords{4096u, 0u, 4096u, 0u, 4096u};
  for (uint8_t reg = GTE_RT11RT12; reg <= GTE_RT33; ++reg) {
    const uint32_t offset = static_cast<uint32_t>(reg) * 4u;
    GTE::writeControl(&ctx, reg, rotationWords[reg], 0x80020000u + offset);
  }
  GTE::writeControl(&ctx, GTE_TRX, 10u, 0x80020014u);
  GTE::writeControl(&ctx, GTE_TRY, 20u, 0x80020018u);
  GTE::writeControl(&ctx, GTE_TRZ, 1000u, 0x8002001Cu);
  GTE::writeControl(&ctx, GTE_H, 256u);
  GTE::writeControl(&ctx, GTE_OFX, 0u);
  GTE::writeControl(&ctx, GTE_OFY, 0u);
  GTE::writeData(&ctx, GTE_VXY0, 2u | (3u << 16));
  GTE::noteDataSource(&ctx, GTE_VXY0, 0x80030000u);
  GTE::writeData(&ctx, GTE_VZ0, 4u);
  GTE::noteDataSource(&ctx, GTE_VZ0, 0x80030004u);

  GTE::RTPS(&ctx, true, false);

  const PGXPValue &projection = ctx.pgxpGteData[GTE_SXY2];
  ASSERT_TRUE(projection.hasSignedProjection(ctx.cop2d[GTE_SXY2]));
  EXPECT_EQ(projection.flags & PGXPValue::VALID_INTRINSIC_PROVENANCE,
            PGXPValue::VALID_TRANSFORM_LINEAGE |
                PGXPValue::VALID_MODEL_VERTEX_SOURCE);
  EXPECT_EQ(projection.flags & PGXPValue::VALID_MODEL_VALUE_FLOW, 0u)
      << "ordinary runs must not invent diagnostic value-flow evidence";
  EXPECT_EQ(projection.modelValueFlowSourceFingerprint, 0u);
  EXPECT_EQ(projection.modelValueFlowProgramFingerprint, 0u);
  EXPECT_NE(projection.projectionTransformLineage, 0u);
  EXPECT_NE(projection.projectionTransformValueFingerprint, 0u);
  EXPECT_EQ(projection.modelVertexSource, 0x80030001u);

  std::ifstream in(path);
  ASSERT_TRUE(in.good());
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string body = buffer.str();
  EXPECT_NE(body.find("\"event\":\"gte.projection\""), std::string::npos)
      << body;
  EXPECT_NE(body.find("\"transform_source\":2147614720"), std::string::npos)
      << body;
  EXPECT_NE(body.find("\"model_source\":2147680256"), std::string::npos)
      << body;
  EXPECT_NE(body.find("\"lineage_complete\":1"), std::string::npos) << body;
  EXPECT_NE(body.find("\"transform_hash\":"), std::string::npos) << body;
  EXPECT_NE(body.find("\"raw_view_z\":"), std::string::npos) << body;

  ps1::metrics::resetTraceForTesting();
  unsetenv("PS1_TRACE_JSONL");
  unsetenv("PS1_TRACE_GTE");
  std::remove(path);
}
