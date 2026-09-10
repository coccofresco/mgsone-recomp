#include "runtime/gpu/gpu.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

using namespace ps1::gpu;

class GpuVramTest : public ::testing::Test {
protected:
  GPU gpu;

  void SetUp() override { gpu.reset(); }
};

TEST_F(GpuVramTest, CPUToVRAMTransfer) {
  // Command 0xA0: Copy Rectangle (CPU to VRAM)
  // X=10, Y=20, Width=4, Height=2 -> 8 pixels -> 4 words
  gpu.writeGP0(0xA0000000);      // Command
  gpu.writeGP0((20 << 16) | 10); // Y=20, X=10
  gpu.writeGP0((2 << 16) | 4);   // H=2, W=4

  // Now send the data words
  gpu.writeGP0(0x11112222); // Pixel 1: 2222, Pixel 2: 1111
  gpu.writeGP0(0x33334444); // Pixel 3: 4444, Pixel 4: 3333
  gpu.writeGP0(0x55556666); // Pixel 5: 6666, Pixel 6: 5555
  gpu.writeGP0(0x77778888); // Pixel 7: 8888, Pixel 8: 7777

  const auto *vram = gpu.getVRAM();
  EXPECT_EQ(vram[20 * 1024 + 10].raw, 0x2222);
  EXPECT_EQ(vram[20 * 1024 + 11].raw, 0x1111);
  EXPECT_EQ(vram[20 * 1024 + 12].raw, 0x4444);
  EXPECT_EQ(vram[20 * 1024 + 13].raw, 0x3333);

  EXPECT_EQ(vram[21 * 1024 + 10].raw, 0x6666);
  EXPECT_EQ(vram[21 * 1024 + 11].raw, 0x5555);
  EXPECT_EQ(vram[21 * 1024 + 12].raw, 0x8888);
  EXPECT_EQ(vram[21 * 1024 + 13].raw, 0x7777);
}

TEST_F(GpuVramTest, VRAMToVRAMTransfer) {
  // First, manually fill some pixels in VRAM
  // We'll write to rect at X=0, Y=0 (4x2)
  gpu.writeGP0(0xA0000000);    // CPU to VRAM
  gpu.writeGP0(0);             // 0,0
  gpu.writeGP0((2 << 16) | 4); // H=2, W=4
  gpu.writeGP0(0xAAAAA000);
  gpu.writeGP0(0xCCCCC000);
  gpu.writeGP0(0xEEEEEE00);
  gpu.writeGP0(0xFFFFFF00);

  // Now, VRAM to VRAM copy!
  // Move 4x2 rect from 0,0 to 100,50
  gpu.writeGP0(0x80000000);       // Copy Rectangle
  gpu.writeGP0((0 << 16) | 0);    // Src Y=0, X=0
  gpu.writeGP0((50 << 16) | 100); // Dest Y=50, X=100
  gpu.writeGP0((2 << 16) | 4);    // H=2, W=4

  const auto *vram = gpu.getVRAM();
  EXPECT_EQ(vram[50 * 1024 + 100].raw, 0xA000);
  EXPECT_EQ(vram[50 * 1024 + 101].raw, 0xAAAA);
  EXPECT_EQ(vram[50 * 1024 + 102].raw, 0xC000);
  EXPECT_EQ(vram[50 * 1024 + 103].raw, 0xCCCC);

  EXPECT_EQ(vram[51 * 1024 + 100].raw, 0xEE00);
  EXPECT_EQ(vram[51 * 1024 + 101].raw, 0xEEEE);
  EXPECT_EQ(vram[51 * 1024 + 102].raw, 0xFF00);
  EXPECT_EQ(vram[51 * 1024 + 103].raw, 0xFFFF);
}

TEST_F(GpuVramTest, VRAMToCPUTransfer) {
  // Setup VRAM: write to 0,0 a 2x1 block = 2 pixels = 1 word
  gpu.writeGP0(0xA0000000);    // CPU to VRAM
  gpu.writeGP0(0);             // 0,0
  gpu.writeGP0((1 << 16) | 2); // H=1, W=2
  gpu.writeGP0(0xBEEFBABE);    // pixel1=BABE, pixel2=BEEF

  // Now request it back
  gpu.writeGP0(0xC0000000);    // VRAM to CPU
  gpu.writeGP0(0);             // 0,0
  gpu.writeGP0((1 << 16) | 2); // H=1, W=2

  uint32_t val = gpu.readGPUREAD();
  EXPECT_EQ(val, 0xBEEFBABE);
}

TEST_F(GpuVramTest, TransferDimensionsIgnoreBitsAboveTenByNineFields) {
  // Width 0x402 decodes as 2; height 0x201 decodes as 1. Exercise a
  // non-canonical opcode from every transfer family as well as parser
  // completion after the short A0 data phase.
  constexpr uint32_t highBitSize = (0x201u << 16) | 0x402u;
  gpu.writeGP0(0xBF000000u);
  gpu.writeGP0((10u << 16) | 10u);
  gpu.writeGP0(highBitSize);
  gpu.writeGP0(0x22221111u);

  EXPECT_EQ(gpu.getVRAM()[10u * GPU::VRAM_WIDTH + 10u].raw, 0x1111u);
  EXPECT_EQ(gpu.getVRAM()[10u * GPU::VRAM_WIDTH + 11u].raw, 0x2222u);
  EXPECT_EQ(gpu.getVRAM()[10u * GPU::VRAM_WIDTH + 12u].raw, 0u);

  gpu.writeGP0(0x9F000000u);
  gpu.writeGP0((10u << 16) | 10u);
  gpu.writeGP0((20u << 16) | 20u);
  gpu.writeGP0(highBitSize);

  EXPECT_EQ(gpu.getVRAM()[20u * GPU::VRAM_WIDTH + 20u].raw, 0x1111u);
  EXPECT_EQ(gpu.getVRAM()[20u * GPU::VRAM_WIDTH + 21u].raw, 0x2222u);
  EXPECT_EQ(gpu.getVRAM()[20u * GPU::VRAM_WIDTH + 22u].raw, 0u);

  gpu.writeGP0(0xDF000000u);
  gpu.writeGP0((20u << 16) | 20u);
  gpu.writeGP0(highBitSize);
  EXPECT_EQ(gpu.readGPUREAD(), 0x22221111u);
}

TEST_F(GpuVramTest, ZeroTransferFieldsExpandToFullVramDimensions) {
  constexpr std::size_t pixelCount =
      static_cast<std::size_t>(GPU::VRAM_WIDTH) * GPU::VRAM_HEIGHT;
  std::vector<uint16_t> initial(pixelCount, 0);
  for (uint32_t y = 0; y < GPU::VRAM_HEIGHT; ++y) {
    initial[y * GPU::VRAM_WIDTH + 200u] = static_cast<uint16_t>(y + 1u);
    initial[y * GPU::VRAM_WIDTH + 201u] = static_cast<uint16_t>(0x4000u + y);
  }
  gpu.loadVram(reinterpret_cast<const uint8_t *>(initial.data()));

  // Zero width is a 1024-pixel row, including wrap from x=100 back to x=99.
  gpu.writeGP0(0xA0000000u);
  gpu.writeGP0((7u << 16) | 100u);
  gpu.writeGP0(1u << 16);
  for (uint32_t word = 0; word < GPU::VRAM_WIDTH / 2u; ++word)
    gpu.writeGP0(0x13571357u);
  EXPECT_EQ(gpu.getVRAM()[7u * GPU::VRAM_WIDTH + 100u].raw, 0x1357u);
  EXPECT_EQ(gpu.getVRAM()[7u * GPU::VRAM_WIDTH + 99u].raw, 0x1357u);

  // The same zero-width rule applies to an 80h copy.
  gpu.writeGP0(0x80000000u);
  gpu.writeGP0(7u << 16);
  gpu.writeGP0(8u << 16);
  gpu.writeGP0(1u << 16);
  EXPECT_EQ(gpu.getVRAM()[8u * GPU::VRAM_WIDTH].raw, 0x1357u);
  EXPECT_EQ(gpu.getVRAM()[8u * GPU::VRAM_WIDTH + 1023u].raw, 0x1357u);

  // Zero height is 512 rows. Two pixels per row map exactly to one GPUREAD
  // word, making both the extent and transfer completion observable.
  GPU reader;
  reader.loadVram(reinterpret_cast<const uint8_t *>(initial.data()));
  reader.writeGP0(0xC0000000u);
  reader.writeGP0(200u);
  reader.writeGP0(2u);
  for (uint32_t y = 0; y < GPU::VRAM_HEIGHT; ++y) {
    const uint32_t expected = (0x4000u + y) << 16 | (y + 1u);
    EXPECT_EQ(reader.readGPUREAD(), expected) << "row " << y;
  }
}

TEST_F(GpuVramTest, VramSelfCopyOnlyMutatesNativePixelsWhenE6ForcesMask) {
  gpu.writeGP0(0xA0000000u);
  gpu.writeGP0((30u << 16) | 30u);
  gpu.writeGP0((1u << 16) | 2u);
  gpu.writeGP0(0x03E0001Fu);

  constexpr std::size_t vramBytes = static_cast<std::size_t>(GPU::VRAM_WIDTH) *
                                    GPU::VRAM_HEIGHT * sizeof(Color16);
  std::vector<Color16> before(GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT);
  std::memcpy(before.data(), gpu.getVRAM(), vramBytes);

  gpu.writeGP0(0xE6000000u);
  gpu.writeGP0(0x80000000u);
  gpu.writeGP0((30u << 16) | 30u);
  gpu.writeGP0((30u << 16) | 30u);
  gpu.writeGP0((1u << 16) | 2u);
  EXPECT_EQ(std::memcmp(before.data(), gpu.getVRAM(), vramBytes), 0);

  gpu.writeGP0(0xE6000001u);
  gpu.writeGP0(0x80000000u);
  gpu.writeGP0((30u << 16) | 30u);
  gpu.writeGP0((30u << 16) | 30u);
  gpu.writeGP0((1u << 16) | 2u);
  EXPECT_EQ(gpu.getVRAM()[30u * GPU::VRAM_WIDTH + 30u].raw, 0x801Fu);
  EXPECT_EQ(gpu.getVRAM()[30u * GPU::VRAM_WIDTH + 31u].raw, 0x83E0u);
}

TEST_F(GpuVramTest, MaskBitRulesApplyToCpuUploadsAndVramCopies) {
  // E6 bit 0 forces mask on uploads.
  gpu.writeGP0(0xE6000001);
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0((30 << 16) | 20);
  gpu.writeGP0((1 << 16) | 2);
  gpu.writeGP0(0x001F03E0);
  ASSERT_EQ(gpu.getVRAM()[30 * 1024 + 20].raw, 0x83E0);
  ASSERT_EQ(gpu.getVRAM()[30 * 1024 + 21].raw, 0x801F);

  // E6 bit 1 protects already-masked destinations from another upload.
  gpu.writeGP0(0xE6000002);
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0((30 << 16) | 20);
  gpu.writeGP0((1 << 16) | 2);
  gpu.writeGP0(0x7FFF7FFF);
  EXPECT_EQ(gpu.getVRAM()[30 * 1024 + 20].raw, 0x83E0);
  EXPECT_EQ(gpu.getVRAM()[30 * 1024 + 21].raw, 0x801F);

  // The same protection is required for VRAM-to-VRAM copies.
  gpu.writeGP0(0xE6000000);
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0((31 << 16) | 20);
  gpu.writeGP0((1 << 16) | 1);
  gpu.writeGP0(0x00007C00);
  gpu.writeGP0(0xE6000002);
  gpu.writeGP0(0x80000000);
  gpu.writeGP0((31 << 16) | 20);
  gpu.writeGP0((30 << 16) | 20);
  gpu.writeGP0((1 << 16) | 1);
  EXPECT_EQ(gpu.getVRAM()[30 * 1024 + 20].raw, 0x83E0);
}

TEST_F(GpuVramTest, OddCpuUploadDoesNotWritePaddingPixel) {
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0((40 << 16) | 40);
  gpu.writeGP0((1 << 16) | 1);
  gpu.writeGP0(0xBEEF1234);

  const auto *vram = gpu.getVRAM();
  EXPECT_EQ(vram[40 * 1024 + 40].raw, 0x1234);
  EXPECT_EQ(vram[41 * 1024 + 40].raw, 0)
      << "high halfword is transport padding for an odd-sized upload";
}

TEST_F(GpuVramTest, LoadVramDiscardsPendingCpuTransfer) {
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0(0);
  gpu.writeGP0((1u << 16) | 2u);
  // Deliberately leave the two-pixel upload waiting for its data word.

  std::vector<uint8_t> loaded(
      GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT * sizeof(Color16), 0);
  gpu.loadVram(loaded.data());

  gpu.writeGP0(0x020000FF); // red FillRect
  gpu.writeGP0(0);
  gpu.writeGP0((1u << 16) | 1u);
  EXPECT_EQ(gpu.getVRAM()[0].raw, 0x001F);
}

TEST_F(GpuVramTest, LoadVramDiscardsPartialPrimitiveCommand) {
  gpu.writeGP0(0x200000FF); // monochrome triangle
  gpu.writeGP0((10u << 16) | 10u);
  // The command parser is still waiting for two vertices.

  std::vector<uint8_t> loaded(
      GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT * sizeof(Color16), 0);
  gpu.loadVram(loaded.data());

  gpu.writeGP0(0x020000FF); // independent red FillRect
  gpu.writeGP0(0);
  gpu.writeGP0((1u << 16) | 1u);
  EXPECT_EQ(gpu.getVRAM()[0].raw, 0x001F);
}
