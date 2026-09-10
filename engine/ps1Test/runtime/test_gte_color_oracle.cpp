#include "runtime/gte.h"
#include "runtime/gte_color_math.h"

#include <array>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>

TEST(GteColorRegression, IntermediateSignedSaturationSurvivesPositiveFinalIr) {
  ps1::CPUContext ctx{};
  ctx.cop2d[ps1::GTE_RGBC] = 0xabffffff;
  ctx.cop2d[ps1::GTE_IR0] = 128;
  for (uint32_t channel = 0; channel < 3; ++channel)
    ctx.cop2c[ps1::GTE_RFC + channel] = static_cast<uint32_t>(-32768);
  ps1::GTE::DPCS(&ctx, true, true);
  EXPECT_EQ(ctx.cop2d[ps1::GTE_RGB2], 0xabbfbfbfu);
  EXPECT_EQ(ctx.cop2d[ps1::GTE_MAC1], 3056u);
  EXPECT_EQ(ctx.cop2d[ps1::GTE_IR1], 3056u);
  EXPECT_EQ(ctx.cop2c[ps1::GTE_FLAG], 0x81c00000u);
}

TEST(GteColorRegression, SfZeroAppliesToBothInterpolationStages) {
  ps1::CPUContext ctx{};
  ctx.cop2d[ps1::GTE_RGBC] = 0x00646464;
  ctx.cop2d[ps1::GTE_IR0] = 1;
  for (uint32_t channel = 0; channel < 3; ++channel)
    ctx.cop2c[ps1::GTE_RFC + channel] = 100;
  ps1::GTE::DPCS(&ctx, false, false);
  EXPECT_EQ(ctx.cop2d[ps1::GTE_MAC1], 6520832u);
  EXPECT_EQ(ctx.cop2d[ps1::GTE_IR1], 32767u);
  EXPECT_EQ(ctx.cop2d[ps1::GTE_RGB2], 0x00ffffffu);
}

TEST(GteColorRegression, NormalLightingAddsBackgroundOnceAndPushesOnce) {
  ps1::CPUContext ctx{};
  // Zero light matrix, nonzero background, identity colour matrix.
  ctx.cop2c[ps1::GTE_LR1LR2] = 4096;
  ctx.cop2c[ps1::GTE_LG2LG3] = 4096;
  ctx.cop2c[ps1::GTE_LB3] = 4096;
  for (uint32_t channel = 0; channel < 3; ++channel)
    ctx.cop2c[ps1::GTE_RBK + channel] = 4096;
  ctx.cop2d[ps1::GTE_RGBC] = 0x23808080;
  ctx.cop2d[ps1::GTE_RGB0] = 1;
  ctx.cop2d[ps1::GTE_RGB1] = 2;
  ctx.cop2d[ps1::GTE_RGB2] = 3;
  ps1::GTE::NCDS(&ctx, true, false);
  EXPECT_EQ(ctx.cop2d[ps1::GTE_MAC1], 2048u);
  EXPECT_EQ(ctx.cop2d[ps1::GTE_RGB0], 2u);
  EXPECT_EQ(ctx.cop2d[ps1::GTE_RGB1], 3u);
  EXPECT_EQ(ctx.cop2d[ps1::GTE_RGB2], 0x23808080u);
}

TEST(GteColorRegression, MacOverflowUses44BitAccumulatorAndHardwareBitLayout) {
  using namespace ps1;
  EXPECT_EQ(gte_flag::MAC1_OF, 1u << 30);
  EXPECT_EQ(gte_flag::MAC2_OF, 1u << 29);
  EXPECT_EQ(gte_flag::MAC3_OF, 1u << 28);
  EXPECT_EQ(gte_flag::MAC1_UF, 1u << 27);
  EXPECT_EQ(gte_flag::MAC2_UF, 1u << 26);
  EXPECT_EQ(gte_flag::MAC3_UF, 1u << 25);
  CPUContext ctx{};
  ctx.cop2c[GTE_TRX] = 1000000;
  GTE::MVMVA(&ctx, 0, 0, 0, true, false);
  EXPECT_EQ(ctx.cop2d[GTE_MAC1], 1000000u);
  EXPECT_EQ(ctx.cop2c[GTE_FLAG] & 0x7e000000u, 0u); // 4.096e9 fits 44 bits.
  ctx.cop2c[GTE_TRY] = 0x7fffffff;
  ctx.cop2c[GTE_RT22RT23] = 32767;
  ctx.cop2d[GTE_VXY0] = 32767u << 16;
  GTE::MVMVA(&ctx, 0, 0, 0, true, false);
  EXPECT_EQ(ctx.cop2c[GTE_FLAG] & 0x7e000000u, 1u << 29);
}

// The oracle is a separate process compiled from the real local donor source.
// This test checks every architectural word, including untouched registers,
// the three-deep RGB FIFO and intermediate-stage sticky flags.
TEST(GteColorOracle, OptInIndependentSwanStationVectors) {
  const char *path = std::getenv("PS1_GTE_COLOR_ORACLE_VECTORS");
  if (path == nullptr)
    GTEST_SKIP() << "Set PS1_GTE_COLOR_ORACLE_VECTORS to GTECOL1 vectors";
  std::ifstream input(path, std::ios::binary);
  ASSERT_TRUE(input);
  std::array<char, 8> magic{};
  ASSERT_TRUE(input.read(magic.data(), magic.size()));
  ASSERT_EQ(std::string(magic.data(), magic.size()),
            std::string("GTECOL1\0", 8));
  std::array<uint32_t, 130> row{};
  std::array<uint32_t, 64> commands{}, failures{};
  uint32_t count = 0, mismatchCount = 0;
  while (input.read(reinterpret_cast<char *>(row.data()), sizeof(row))) {
    ASSERT_LT(count++, 10000000u);
    ps1::CPUContext ctx{};
    std::copy_n(row.begin() + 2, 32, ctx.cop2d);
    std::copy_n(row.begin() + 34, 32, ctx.cop2c);
    const auto code = row[1] & 63u;
    const bool sf = row[1] & (1u << 19), lm = row[1] & (1u << 10);
    ++commands[code];
    using ps1::GTE;
    switch (code) {
    case 0x10:
      GTE::DPCS(&ctx, sf, lm);
      break;
    case 0x11:
      GTE::INTPL(&ctx, sf, lm);
      break;
    case 0x13:
      GTE::NCDS(&ctx, sf, lm);
      break;
    case 0x14:
      GTE::CDP(&ctx, sf, lm);
      break;
    case 0x16:
      GTE::NCDT(&ctx, sf, lm);
      break;
    case 0x1b:
      GTE::NCCS(&ctx, sf, lm);
      break;
    case 0x1c:
      GTE::CC(&ctx, sf, lm);
      break;
    case 0x1e:
      GTE::NCS(&ctx, sf, lm);
      break;
    case 0x20:
      GTE::NCT(&ctx, sf, lm);
      break;
    case 0x29:
      GTE::DCPL(&ctx, sf, lm);
      break;
    case 0x2a:
      GTE::DPCT(&ctx, sf, lm);
      break;
    case 0x3f:
      GTE::NCCT(&ctx, sf, lm);
      break;
    default:
      FAIL() << "Unsupported oracle command " << code;
    }
    bool mismatch = false;
    for (uint32_t reg = 0; reg < 64; ++reg) {
      const uint32_t actual = reg < 32 ? ctx.cop2d[reg] : ctx.cop2c[reg - 32];
      if (actual != row[66 + reg]) {
        mismatch = true;
        if (mismatchCount < 5)
          ADD_FAILURE() << "sample=" << count - 1 << " pc=" << std::hex
                        << row[0] << " instruction=" << row[1]
                        << " register=" << std::dec << reg
                        << " expected=" << std::hex << row[66 + reg]
                        << " actual=" << actual;
      }
    }
    if (mismatch) {
      ++mismatchCount;
      ++failures[code];
    }
  }
  ASSERT_TRUE(input.eof());
  ASSERT_EQ(input.gcount(), 0) << "Truncated oracle record";
  ASSERT_GT(count, 0u);
  for (uint32_t code = 0; code < 64; ++code)
    if (commands[code] != 0)
      EXPECT_EQ(failures[code], 0u) << "instruction=" << std::hex << code
                                    << " tested=" << std::dec << commands[code];
  EXPECT_EQ(mismatchCount, 0u) << "Total instructions " << count;
}
