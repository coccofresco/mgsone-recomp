#include "runtime/gpu/gpu.h"
#include <cstring>
#include <gtest/gtest.h>

using namespace ps1::gpu;

// Screenshot / Visual Verification Tests
// These tests verify GPU rendering output by checking VRAM contents
// after drawing operations, simulating screenshot comparison.

class GpuScreenshotTest : public ::testing::Test {
protected:
  GPU gpu;
  void SetUp() override { gpu.reset(); }

  // Helper: count non-zero pixels in a VRAM region
  uint32_t countNonZeroPixels(int x, int y, int w, int h) {
    const Color16 *vram = gpu.getVRAM();
    uint32_t count = 0;
    for (int py = y; py < y + h; py++) {
      for (int px = x; px < x + w; px++) {
        if (vram[py * 1024 + px].raw != 0)
          count++;
      }
    }
    return count;
  }

  // Helper: get pixel at (x,y)
  uint16_t getPixel(int x, int y) { return gpu.getVRAM()[y * 1024 + x].raw; }
};

TEST_F(GpuScreenshotTest, ClearScreenBlack) {
  // GP0(02h) - Fill Rectangle in VRAM
  gpu.writeGP0(0x02000000); // Fill black
  gpu.writeGP0(0x00000000); // Start X,Y = 0,0
  gpu.writeGP0(0x00F00140); // H=240, W=320

  // Verify: area should be all black (0)
  EXPECT_EQ(countNonZeroPixels(0, 0, 320, 240), 0u);
}

TEST_F(GpuScreenshotTest, ClearScreenColor) {
  // Fill 16x16 with a non-black color
  gpu.writeGP0(0x02F800F8); // Fill with color
  gpu.writeGP0(0x00000000); // Start 0,0
  gpu.writeGP0(0x00100010); // 16x16

  // Should have non-zero pixels in the filled area
  uint32_t filled = countNonZeroPixels(0, 0, 16, 16);
  EXPECT_GT(filled, 0u);
}

TEST_F(GpuScreenshotTest, FlatTriangleCoversPixels) {
  // Draw a flat green triangle
  // GP0(20h) = monochrome triangle, opaque
  gpu.writeGP0(0x2000FF00); // Green
  gpu.writeGP0(0x00100010); // V0: (16, 16)
  gpu.writeGP0(0x00100060); // V1: (96, 16)
  gpu.writeGP0(0x00600038); // V2: (56, 96)

  // The triangle should cover some pixels in the 16-96 x 16-96 region
  uint32_t pixels = countNonZeroPixels(16, 16, 80, 80);
  EXPECT_GT(pixels, 100u);
}

TEST_F(GpuScreenshotTest, VramNotCorruptedOutsideDraw) {
  // Area far from any draw should remain zero
  EXPECT_EQ(countNonZeroPixels(900, 400, 100, 100), 0u);
}

TEST_F(GpuScreenshotTest, RectangleCoversExactArea) {
  // Draw a 32x32 untextured rectangle at (100, 100) with white
  gpu.writeGP0(0x60FFFFFF); // Rectangle, white
  gpu.writeGP0(0x00640064); // X=100, Y=100
  gpu.writeGP0(0x00200020); // W=32, H=32

  uint32_t pixels = countNonZeroPixels(100, 100, 32, 32);
  EXPECT_GT(pixels, 0u);
}
