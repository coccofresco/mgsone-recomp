#include "runtime/gpu/gpu.h"
#include <gtest/gtest.h>

using namespace ps1::gpu;

class GpuBlendTest : public ::testing::Test {
protected:
  GPU gpu;

  void SetUp() override {
    // Basic setup
    gpu.writeGP1(0x00000000); // Reset
  }

  Color16 makeColor(uint8_t r, uint8_t g, uint8_t b, bool stp = false) {
    Color16 c;
    c.raw = ((r >> 3) & 0x1F) | (((g >> 3) & 0x1F) << 5) |
            (((b >> 3) & 0x1F) << 10) | (stp ? 0x8000 : 0);
    return c;
  }
};

TEST_F(GpuBlendTest, BlendMode0_HalfF_HalfB) {
  // Mode 0: 0.5 * B + 0.5 * F
  gpu.writeGP0(0xE1000000); // Blend Mode 0

  Color16 fg = makeColor(100, 200, 50);
  Color16 bg = makeColor(50, 100, 200);

  // Scaled values:
  // F: R=96, G=200, B=48
  // B: R=48, G=96,  B=200
  // Mode 0: R=(96+48)/2=72, G=(200+96)/2=148, B=(48+200)/2=124
  Color16 expected = makeColor(72, 148, 124);
  Color16 actual = gpu.applyBlend(fg, bg);

  EXPECT_EQ(expected.raw, actual.raw);
}

TEST_F(GpuBlendTest, BlendMode1_F_Plus_B) {
  // Mode 1: 1.0 * B + 1.0 * F
  gpu.writeGP0(0xE1000020); // Blend Mode 1

  Color16 fg = makeColor(100, 200, 50);
  Color16 bg = makeColor(50, 100, 200);

  // Scaled values:
  // F: R=96, G=200, B=48
  // B: R=48, G=96,  B=200
  // Mode 1: R=96+48=144, G=200+96=296(255), B=48+200=248
  Color16 expected = makeColor(144, 255, 248);
  Color16 actual = gpu.applyBlend(fg, bg);

  EXPECT_EQ(expected.raw, actual.raw);
}

TEST_F(GpuBlendTest, BlendMode2_B_Minus_F) {
  // Mode 2: 1.0 * B - 1.0 * F
  gpu.writeGP0(0xE1000040); // Blend Mode 2

  Color16 fg = makeColor(100, 200, 50);
  Color16 bg = makeColor(200, 100, 200);

  // Scaled:
  // F: R=96,  G=200, B=48
  // B: R=200, G=96,  B=200
  // Mode 2: R=200-96=104, G=96-200=0, B=200-48=152
  Color16 expected = makeColor(104, 0, 152);
  Color16 actual = gpu.applyBlend(fg, bg);

  EXPECT_EQ(expected.raw, actual.raw);
}

TEST_F(GpuBlendTest, BlendMode3_B_Plus_QuarterF) {
  // Mode 3: 1.0 * B + 0.25 * F
  gpu.writeGP0(0xE1000060); // Blend Mode 3

  Color16 fg = makeColor(100, 200, 50);
  Color16 bg = makeColor(100, 100, 100);

  // Scaled:
  // F: R=96, G=200, B=48
  // B: R=96, G=96,  B=96
  // Mode 3: R=96+96/4=120, G=96+200/4=146, B=96+48/4=108
  Color16 expected = makeColor(120, 146, 108);
  Color16 actual = gpu.applyBlend(fg, bg);

  EXPECT_EQ(expected.raw, actual.raw);
}
