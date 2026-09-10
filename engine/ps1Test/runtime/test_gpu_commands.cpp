#include "runtime/gpu/gpu.h"
#include <cstdint>
#include <gtest/gtest.h>

using namespace ps1::gpu;

class GpuCommandTest : public ::testing::Test {
protected:
  GPU gpu;

  void SetUp() override { gpu.reset(); }
};

TEST_F(GpuCommandTest, ResetClearsVRAMAndState) {
  // Dirty VRAM and status manually (simulate some commands)
  gpu.writeGP0(0x02000000 | 0xFFFFFF); // Fill rect with white
  gpu.writeGP0((0 << 16) | 0);         // at 0,0
  gpu.writeGP0((10 << 16) | 10);       // size 10x10

  // Write some GP1 commands to dirty GPUSTAT
  gpu.writeGP1(0x04000002); // DMA Direction

  gpu.reset();

  EXPECT_EQ(gpu.readGPUSTAT(), 0x14802000);
  EXPECT_EQ(gpu.readGPUREAD(), 0);

  // Check VRAM is zeroed
  const Color16 *vram = gpu.getVRAM();
  bool allZero = true;
  for (uint32_t i = 0; i < GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT; ++i) {
    if (vram[i].raw != 0) {
      allZero = false;
      break;
    }
  }
  EXPECT_TRUE(allZero);
}

TEST_F(GpuCommandTest, GP1SoftResetPreservesVRAM) {
  gpu.writeGP0(0x020000FFu);
  gpu.writeGP0(0u);
  gpu.writeGP0((1u << 16) | 1u);
  ASSERT_EQ(gpu.getVRAM()[0].raw & 0x1Fu, 31u);

  gpu.writeGP1(0x00000000u);
  EXPECT_EQ(gpu.getVRAM()[0].raw & 0x1Fu, 31u);
}

TEST_F(GpuCommandTest, GP0E3E4E5Attributes) {
  // E3: Draw Area top left
  gpu.writeGP0(0xE3000000 | (100 << 10) | 50); // Y:100, X:50
  // No direct way to read internal state, but ideally these affect
  // rasterization later. We just ensure the command parser doesn't crash or get
  // stuck

  // E4: Draw Area bottom right
  gpu.writeGP0(0xE4000000 | (200 << 10) | 150); // Y:200, X:150

  // E5: Draw Offset
  gpu.writeGP0(0xE5000000 | (10 << 11) | 20); // Y:10, X:20
}

TEST_F(GpuCommandTest, GP0FillRectFillsArea) {
  // Command: Opcode 0x02, Color RGB(255, 0, 0) -> 0x020000FF
  gpu.writeGP0(0x020000FF);
  // Pos: X=10, Y=20 -> 0x0014000A
  gpu.writeGP0(0x0014000A);
  // Size: W=5, H=5 -> 0x00050005
  gpu.writeGP0(0x00050005);

  const Color16 *vram = gpu.getVRAM();

  // Convert RGB(255,0,0) to 15-bit
  uint16_t expectedRaw = (255 >> 3) | 0 | 0 | 0; // r=31, g=0, b=0, a=0

  EXPECT_EQ(vram[20 * GPU::VRAM_WIDTH + 0].raw, expectedRaw);
  EXPECT_EQ(vram[24 * GPU::VRAM_WIDTH + 15].raw, expectedRaw);
  EXPECT_EQ(vram[19 * GPU::VRAM_WIDTH + 10].raw, 0); // Outside top
  EXPECT_EQ(vram[20 * GPU::VRAM_WIDTH + 16].raw, 0); // Outside right
}

// GP0(02h) does not share the A0h/C0h "zero means the full dimension" rule: a
// zero width or height fills nothing.  Reading a zero height as 512 made the
// game's per-frame clear at y=12 wrap past the bottom of VRAM and erase the
// texture pages at y=384..511, so every textured sprite sampled transparent
// texels and the screen stayed black.
TEST_F(GpuCommandTest, GP0FillRectWithZeroHeightFillsNothing) {
  // Seed a pixel inside the region a wrapping 512-row fill would have reached.
  gpu.writeGP0(0x020000FF);
  gpu.writeGP0((400 << 16) | 600); // (600,400) -- texture-page territory
  gpu.writeGP0((1 << 16) | 1);     // 1x1
  const Color16 *vram = gpu.getVRAM();
  const uint16_t seeded = vram[400 * GPU::VRAM_WIDTH + 600].raw;
  ASSERT_NE(seeded, 0) << "seed fill did not land; test cannot detect the wipe";

  // Height field of 0: the clear the game actually issues.
  gpu.writeGP0(0x02000000);
  gpu.writeGP0((12 << 16) | 512); // (512,12), as observed in Crash
  gpu.writeGP0((0 << 16) | 512);  // 512 wide, height field 0

  EXPECT_EQ(vram[400 * GPU::VRAM_WIDTH + 600].raw, seeded)
      << "zero-height fill wrapped and wiped the texture page region";
}

TEST_F(GpuCommandTest, GP0FillRectWithZeroWidthFillsNothing) {
  gpu.writeGP0(0x020000FF);
  gpu.writeGP0((100 << 16) | 100);
  gpu.writeGP0((1 << 16) | 1);
  const Color16 *vram = gpu.getVRAM();
  const uint16_t seeded = vram[100 * GPU::VRAM_WIDTH + 100].raw;
  ASSERT_NE(seeded, 0);

  gpu.writeGP0(0x02000000);
  gpu.writeGP0((100 << 16) | 100);
  gpu.writeGP0((8 << 16) | 0); // width field 0, height 8

  EXPECT_EQ(vram[100 * GPU::VRAM_WIDTH + 100].raw, seeded)
      << "zero-width fill was treated as 1024 wide";
}

TEST_F(GpuCommandTest, GP1DMADirectionUpdatesGPUSTAT) {
  uint32_t initialStat = gpu.readGPUSTAT();

  // Send GP1 0x04: DMA Direction (bits 0-1 set bits 29-30 in GPUSTAT)
  // Set to 2 (CPU to VRAM)
  gpu.writeGP1(0x04000002);

  uint32_t newStat = gpu.readGPUSTAT();
  EXPECT_EQ((newStat >> 29) & 3, 2);

  // Ensure rest of struct is unchanged
  EXPECT_EQ(newStat & ~(3 << 29), initialStat & ~(3 << 29));
}

TEST_F(GpuCommandTest, GP1TextureDisableGateOnlyAffectsLaterDrawModeWrites) {
  constexpr uint32_t textureDisable = 1u << 11u;
  constexpr uint32_t gpuStatTextureDisable = 1u << 15u;

  EXPECT_EQ(gpu.readGPUSTAT() & gpuStatTextureDisable, 0u);
  gpu.writeGP0(0xE1000000u | textureDisable);
  EXPECT_EQ(gpu.readGPUSTAT() & gpuStatTextureDisable, 0u)
      << "E1.11 is masked while GP1(09) remains at its reset value";

  gpu.writeGP1(0x09000001u);
  EXPECT_EQ(gpu.readGPUSTAT() & gpuStatTextureDisable, 0u)
      << "GP1(09) changes the write mask, not latched draw mode";
  gpu.writeGP0(0xE1000000u | textureDisable);
  EXPECT_NE(gpu.readGPUSTAT() & gpuStatTextureDisable, 0u);

  gpu.writeGP1(0x09000000u);
  EXPECT_NE(gpu.readGPUSTAT() & gpuStatTextureDisable, 0u)
      << "closing the gate is likewise non-retroactive";
  gpu.writeGP0(0xE1000000u | textureDisable);
  EXPECT_EQ(gpu.readGPUSTAT() & gpuStatTextureDisable, 0u);
}
