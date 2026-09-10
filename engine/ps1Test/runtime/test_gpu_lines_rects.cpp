#include "runtime/gpu/gpu.h"
#include <gtest/gtest.h>
#include <vector>

using namespace ps1::gpu;

namespace {

ps1::PGXPValue preciseLineRectVertex(uint32_t word, float x, float y,
                                     float depth) {
  ps1::PGXPValue value;
  value.x = x;
  value.y = y;
  value.z = depth;
  value.value = word;
  value.flags = ps1::PGXPValue::VALID_XY | ps1::PGXPValue::VALID_Z;
  return value;
}

void drawPreciseLineRectTriangle(GPU &gpu, uint32_t command, float depth) {
  constexpr uint32_t v0 = (10u << 16) | 10u;
  constexpr uint32_t v1 = (10u << 16) | 20u;
  constexpr uint32_t v2 = (20u << 16) | 10u;
  const ps1::PGXPValue p0 = preciseLineRectVertex(v0, 10.0f, 10.0f, depth);
  const ps1::PGXPValue p1 = preciseLineRectVertex(v1, 20.0f, 10.0f, depth);
  const ps1::PGXPValue p2 = preciseLineRectVertex(v2, 10.0f, 20.0f, depth);
  gpu.writeGP0(command);
  gpu.writeGP0(v0, UINT32_MAX, &p0);
  gpu.writeGP0(v1, UINT32_MAX, &p1);
  gpu.writeGP0(v2, UINT32_MAX, &p2);
}

std::vector<Color32> copyModernLineRectDisplay(GPU &gpu, uint32_t &width) {
  gpu.snapshotDisplayBuffer();
  std::vector<Color32> display;
  uint32_t height = 0;
  EXPECT_TRUE(gpu.copyDisplayRGBA8(display, width, height));
  return display;
}

} // namespace

class GpuLinesRectsTest : public ::testing::Test {
protected:
  GPU gpu;

  void SetUp() override { gpu.reset(); }
};

TEST_F(GpuLinesRectsTest, ExecuteMonochromeLine) {
  // Command 0x40 (Mono Line)
  // X0,Y0 = 10,10. X1,Y1 = 15,10
  // Color: Red (255, 0, 0)
  gpu.writeGP0(0x400000FF);
  gpu.writeGP0((10 << 16) | 10);
  gpu.writeGP0((10 << 16) | 15);

  const auto *vram = gpu.getVRAM();
  uint16_t red = 31 | (0 << 5) | (0 << 10);

  EXPECT_EQ(vram[10 * 1024 + 10].raw, red);
  EXPECT_EQ(vram[10 * 1024 + 12].raw, red);
  EXPECT_EQ(vram[10 * 1024 + 15].raw, red);
  EXPECT_EQ(vram[10 * 1024 + 16].raw, 0); // Past line
}

TEST_F(GpuLinesRectsTest, Execute16x16Rect) {
  // Command 0x78 (16x16 Rect)
  // X0,Y0 = 20, 30
  // Color: Green (0, 255, 0)
  gpu.writeGP0(0x7800FF00);
  gpu.writeGP0((30 << 16) | 20);

  const auto *vram = gpu.getVRAM();
  uint16_t green = 0 | (31 << 5) | (0 << 10);

  EXPECT_EQ(vram[30 * 1024 + 20].raw, green);
  EXPECT_EQ(vram[45 * 1024 + 35].raw, green);
  EXPECT_EQ(vram[46 * 1024 + 20].raw, 0); // Past rect height
  EXPECT_EQ(vram[30 * 1024 + 36].raw, 0); // Past rect width
}

TEST_F(GpuLinesRectsTest, GouraudLineInterpolatesBothEndpointColors) {
  gpu.writeGP0(0x500000FF);      // red endpoint
  gpu.writeGP0((50 << 16) | 50); // (50,50)
  gpu.writeGP0(0x00FF0000);      // blue endpoint
  gpu.writeGP0((50 << 16) | 54); // (54,50)

  const auto *vram = gpu.getVRAM();
  EXPECT_EQ(vram[50 * 1024 + 50].raw & 0x7FFF, 31);
  EXPECT_EQ(vram[50 * 1024 + 54].raw & 0x7FFF, 31 << 10);
  EXPECT_NE(vram[50 * 1024 + 52].raw & 0x7FFF, 31)
      << "middle of a Gouraud line must not keep color 0";
}

TEST_F(GpuLinesRectsTest,
       MonochromePolylineWaitsForTerminatorAndDrawsEverySegment) {
  gpu.writeGP0(0x480000FFu);
  gpu.writeGP0((60u << 16) | 60u);
  gpu.writeGP0((60u << 16) | 64u);
  EXPECT_EQ(gpu.getVRAM()[60u * GPU::VRAM_WIDTH + 60u].raw, 0u)
      << "a partial variable-length command must not execute";

  gpu.writeGP0((64u << 16) | 64u);
  EXPECT_EQ(gpu.getVRAM()[60u * GPU::VRAM_WIDTH + 60u].raw, 0u);
  gpu.writeGP0(0x50005000u);

  const auto *vram = gpu.getVRAM();
  EXPECT_EQ(vram[60u * GPU::VRAM_WIDTH + 60u].raw & 0x1Fu, 31u);
  EXPECT_EQ(vram[60u * GPU::VRAM_WIDTH + 64u].raw & 0x1Fu, 31u);
  EXPECT_EQ(vram[64u * GPU::VRAM_WIDTH + 64u].raw & 0x1Fu, 31u)
      << "the segment following the old fixed-size prefix must execute";

  gpu.writeGP0(0x6800FF00u);
  gpu.writeGP0((70u << 16) | 70u);
  EXPECT_EQ(vram[70u * GPU::VRAM_WIDTH + 70u].raw & 0x03E0u, 31u << 5)
      << "the command immediately after the terminator must decode normally";
}

TEST_F(GpuLinesRectsTest,
       GouraudPolylineIgnoresTerminatorPatternInCoordinateSlot) {
  gpu.writeGP0(0x580000FFu);      // red vertex 0
  gpu.writeGP0(0u);               // (0,0)
  gpu.writeGP0(0x0000FF00u);      // green vertex 1
  gpu.writeGP0(0x50005000u);      // valid XY slot, not a terminator slot
  gpu.writeGP0(0x00FF0000u);      // blue vertex 2
  gpu.writeGP0((4u << 16) | 4u);  // (4,4)
  gpu.writeGP0(0x50005000u);      // terminator in the next color slot

  EXPECT_NE(gpu.getVRAM()[4u * GPU::VRAM_WIDTH + 4u].raw, 0u)
      << "the third Gouraud vertex proves the second segment was retained";
}

TEST_F(GpuLinesRectsTest, CommandBufferResetDropsIncompletePolylineState) {
  gpu.writeGP0(0x480000FFu);
  gpu.writeGP0((80u << 16) | 80u);
  gpu.writeGP0((80u << 16) | 84u);
  gpu.writeGP1(0x01000000u);

  gpu.writeGP0(0x6800FF00u);
  gpu.writeGP0((90u << 16) | 90u);
  EXPECT_EQ(gpu.getVRAM()[80u * GPU::VRAM_WIDTH + 80u].raw, 0u);
  EXPECT_EQ(gpu.getVRAM()[90u * GPU::VRAM_WIDTH + 90u].raw & 0x03E0u,
            31u << 5);
}

TEST_F(GpuLinesRectsTest, CoordinatesUseSignedElevenBitGpuEncoding) {
  gpu.writeGP0(0xE5000001u); // draw offset +1 X
  gpu.writeGP0(0x680000FFu); // red 1x1 dot
  gpu.writeGP0(0x000007FFu); // signed-11 X=-1, therefore destination X=0

  EXPECT_EQ(gpu.getVRAM()[0].raw & 0x1Fu, 31u);
}

TEST_F(GpuLinesRectsTest, VariableRectangleMasksHardwareDimensions) {
  gpu.writeGP0(0x600000FFu);
  gpu.writeGP0(0u);
  gpu.writeGP0((0x0201u << 16) | 0x0401u); // masked to 1x1

  EXPECT_EQ(gpu.getVRAM()[0].raw & 0x1Fu, 31u);
  EXPECT_EQ(gpu.getVRAM()[1].raw, 0u);
  EXPECT_EQ(gpu.getVRAM()[GPU::VRAM_WIDTH].raw, 0u);
}

TEST_F(GpuLinesRectsTest, RejectsLineAtHardwareExtentLimit) {
  gpu.writeGP0(0x400000FFu);
  gpu.writeGP0(0u);
  gpu.writeGP0(0x00000400u); // signed-11 X=-1024, delta is exactly 1024

  EXPECT_EQ(gpu.getVRAM()[0].raw, 0u);
}

TEST_F(GpuLinesRectsTest,
       MaskRejectedLineAndRectangleDoNotUpdateModernCompanion) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setTrueDepth(true);

  // The near red surface wins modern Z. A farther forced-mask green surface
  // remains authoritative in native VRAM, producing the intentional mask
  // divergence that catches companion writes not gated by writePixel().
  drawPreciseLineRectTriangle(gpu, 0x200000FFu, 1.0f);
  gpu.writeGP0(0xE6000001u); // force destination mask
  drawPreciseLineRectTriangle(gpu, 0x2000FF00u, 10.0f);
  ASSERT_EQ(gpu.getVRAM()[12u * GPU::VRAM_WIDTH + 12u].raw, 0x83E0u);
  ASSERT_EQ(gpu.getVRAM()[13u * GPU::VRAM_WIDTH + 12u].raw, 0x83E0u);

  gpu.writeGP0(0xE6000002u); // reject writes to masked native destinations
  gpu.writeGP0(0x40FF0000u); // blue line
  gpu.writeGP0((12u << 16) | 12u);
  gpu.writeGP0((12u << 16) | 13u);
  gpu.writeGP0(0x68FFFFFFu); // white 1x1 rectangle
  gpu.writeGP0((13u << 16) | 12u);

  EXPECT_EQ(gpu.getVRAM()[12u * GPU::VRAM_WIDTH + 12u].raw, 0x83E0u);
  EXPECT_EQ(gpu.getVRAM()[13u * GPU::VRAM_WIDTH + 12u].raw, 0x83E0u);
  uint32_t width = 0;
  const std::vector<Color32> modern = copyModernLineRectDisplay(gpu, width);
  ASSERT_GT(modern.size(), static_cast<std::size_t>(13u * width + 12u));
  EXPECT_EQ(modern[12u * width + 12u], (Color32{255, 0, 0, 255}));
  EXPECT_EQ(modern[13u * width + 12u], (Color32{255, 0, 0, 255}));
}
