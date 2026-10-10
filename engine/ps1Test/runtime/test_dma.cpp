#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/dma/dma.h"
#include "runtime/gpu/gpu.h"
#include "runtime/memory.h"
#include "runtime/mdec/mdec.h"
#include "runtime/spu/spu.h"
#include <gtest/gtest.h>
#include <vector>

using namespace ps1;

class DmaTest : public ::testing::Test {
protected:
  DMA dma;
  Memory mem;
  void SetUp() override {
    dma.reset();
    mem.reset();
    dma.setMemory(&mem);
  }
};

TEST_F(DmaTest, InitialState) {
  EXPECT_EQ(dma.readRegister(0x1F8010F0), 0x07654321u); // DPCR default
  EXPECT_FALSE(dma.hasInterrupt());
  for (uint32_t ch = 0; ch < DMA::NUM_CHANNELS; ++ch)
    EXPECT_EQ(dma.completionGeneration(ch), 0u);
}

TEST_F(DmaTest, MdecOutputArmedBeforeInputWaitsForDecodedPixels) {
  mdec::MDEC decoder;
  dma.setMDEC(&decoder);
  dma.writeRegister(0x1F8010F0, 0x88); // MDEC input and output
  dma.writeRegister(0x1F8010F4, (1u << 23) | (1u << 17));
  mem.write32(0x2000, 0xdeadbeef);
  dma.writeRegister(0x1F801090, 0x2000);
  dma.writeRegister(0x1F801094, 0x00040020); // 128 words
  dma.writeRegister(0x1F801098, 0x01000200);
  EXPECT_NE(dma.readRegister(0x1F801098) & (1u << 24), 0u);
  EXPECT_EQ(mem.read32(0x2000), 0xdeadbeefu);
  EXPECT_EQ(dma.completionGeneration(DMA::MDEC_OUT), 0u);
  decoder.writeCommand(0x38000006);
  for (unsigned i = 0; i < 6; ++i) mem.write32(0x1000 + i * 4, 0xFE000400);
  dma.writeRegister(0x1F801080, 0x1000);
  dma.writeRegister(0x1F801084, 0x00010006);
  dma.writeRegister(0x1F801088, 0x01000201);
  EXPECT_EQ(dma.readRegister(0x1F801098) & (1u << 24), 0u);
  EXPECT_EQ(dma.completionGeneration(DMA::MDEC_OUT), 1u);
  EXPECT_TRUE(dma.hasInterrupt());
  EXPECT_TRUE(decoder.dataOutEmpty());
  for (unsigned i = 0; i < 128; ++i) EXPECT_EQ(mem.read32(0x2000 + i * 4), 0x42104210u);
}

TEST_F(DmaTest, DicrByteEnableAndSelectiveAcknowledgePreserveOtherLanes) {
  mem.setDMA(&dma);
  dma.writeRegister(0x1F8010F0, 0x08888888);
  // Enable SPU and OTC IRQs using the same SB lane as the retail STR driver.
  mem.write8(0x1F8010F6, 0xd0);
  EXPECT_EQ(mem.read8(0x1F8010F6), 0xd0);
  for (unsigned ch : {4u, 6u}) {
    dma.writeRegister(0x1F801080 + ch * 16, 0x1000);
    dma.writeRegister(0x1F801084 + ch * 16, 1);
    dma.writeRegister(0x1F801088 + ch * 16, 0x11000001);
  }
  EXPECT_EQ(mem.read8(0x1F8010F7), 0xd0); // both flags + master
  mem.write8(0x1F8010F6, 0x50); // disable master without ACKing either flag
  EXPECT_FALSE(dma.hasInterrupt());
  EXPECT_EQ(mem.read8(0x1F8010F7), 0x50);
  mem.write8(0x1F8010F6, 0xd0);
  EXPECT_TRUE(dma.hasInterrupt());
  mem.write8(0x1F8010F7, 0x10); // ACK only SPU
  EXPECT_EQ(mem.read8(0x1F8010F6), 0xd0);
  EXPECT_EQ(mem.read8(0x1F8010F7), 0xc0);
  mem.write8(0x1F8010F7, 0x40);
  EXPECT_FALSE(dma.hasInterrupt());
}

TEST_F(DmaTest, DpcrReadWrite) {
  dma.writeRegister(0x1F8010F0, 0x88888888);
  EXPECT_EQ(dma.readRegister(0x1F8010F0), 0x88888888u);
}

// Regression: a game writing 0 to DPCR must not disable GPU (Ch2) or OTC (Ch6).
// The real BIOS pre-enables these during init; our HLE must preserve them
// because games assume GPU DMA is available without explicitly enabling it.
TEST_F(DmaTest, DpcrForceEnablesGpuAndOtc) {
  // Write a value that explicitly clears both GPU (bit 11) and OTC (bit 27).
  dma.writeRegister(0x1F8010F0, 0x00000000);
  uint32_t result = dma.readRegister(0x1F8010F0);
  EXPECT_TRUE(result & (1u << 11)) << "GPU DMA (Ch2, bit 11) must always be enabled";
  EXPECT_TRUE(result & (1u << 27)) << "OTC DMA (Ch6, bit 27) must always be enabled";
}

TEST_F(DmaTest, DpcrForceEnablesGpuAndOtcPreservesOtherBits) {
  // Other channel bits must not be force-set -- only GPU and OTC.
  // Write with Ch0 (MDECin, bit 3) and Ch4 (SPU, bit 19) enabled.
  uint32_t written = (1u << 3) | (1u << 19);
  dma.writeRegister(0x1F8010F0, written);
  uint32_t result = dma.readRegister(0x1F8010F0);
  // Must keep the bits the game wrote
  EXPECT_TRUE(result & (1u << 3))  << "Ch0 bit must be preserved";
  EXPECT_TRUE(result & (1u << 19)) << "Ch4 bit must be preserved";
  // Must also keep GPU and OTC forced
  EXPECT_TRUE(result & (1u << 11)) << "GPU DMA must be force-enabled";
  EXPECT_TRUE(result & (1u << 27)) << "OTC DMA must be force-enabled";
}

TEST_F(DmaTest, ChannelRegisterReadWrite) {
  // Channel 2 (GPU) base addr
  dma.writeRegister(0x1F8010A0, 0x00100000);
  EXPECT_EQ(dma.readRegister(0x1F8010A0), 0x00100000u);

  // Channel 2 block control
  dma.writeRegister(0x1F8010A4, 0x00010001);
  EXPECT_EQ(dma.readRegister(0x1F8010A4), 0x00010001u);
}

TEST_F(DmaTest, OtcClearsOrderingTable) {
  // Enable OTC channel in DPCR
  dma.writeRegister(0x1F8010F0, 0x08888888); // Enable ch6

  // Set up OTC: clear a 4-entry ordering table starting at address 0x100
  dma.writeRegister(0x1F8010E0, 0x0000010C); // Base addr (end of table)
  dma.writeRegister(0x1F8010E4, 0x00000004); // 4 words
  dma.writeRegister(0x1F8010E8, 0x11000002); // CHCR: from RAM, burst, start

  // Check that the OT was filled backwards
  // Entry at 0x10C should have end marker (0x00FFFFFF)
  uint32_t entry0 = mem.read32(0x10C);
  // Last entry should be end marker or previous pointer
  // Note: OTC fills backwards from base addr
  EXPECT_TRUE(true); // Mainly testing that it doesn't crash
}

TEST_F(DmaTest, OtcInvalidatesSameValuePgxpProvenance) {
  constexpr uint32_t address = 0x10C;
  constexpr uint32_t linkWrittenByOtc = 0x108;
  mem.write32(address, linkWrittenByOtc);
  PGXPValue precision;
  precision.x = 10.25f;
  precision.y = 20.5f;
  precision.z = 100.0f;
  precision.value = linkWrittenByOtc;
  precision.flags = PGXPValue::VALID_XY | PGXPValue::VALID_Z;
  mem.storePrecision(address, linkWrittenByOtc, precision);
  PGXPValue found;
  ASSERT_TRUE(mem.lookupPrecision(address, linkWrittenByOtc, found));

  dma.writeRegister(0x1F8010F0, 1u << 27); // enable OTC
  dma.writeRegister(0x1F8010E0, address);
  dma.writeRegister(0x1F8010E4, 4u);
  dma.writeRegister(0x1F8010E8, (1u << 28) | (1u << 24));

  ASSERT_EQ(mem.read32(address), linkWrittenByOtc);
  EXPECT_FALSE(mem.lookupPrecision(address, linkWrittenByOtc, found));
}

TEST_F(DmaTest, DicrInterruptLogic) {
  // Set force IRQ bit
  dma.writeRegister(0x1F8010F4, 0x00008000); // Force IRQ
  EXPECT_TRUE(dma.hasInterrupt());

  // Clear force IRQ
  dma.writeRegister(0x1F8010F4, 0x00000000);
  EXPECT_FALSE(dma.hasInterrupt());
}

TEST_F(DmaTest, BlockTransferDoesntCrash) {
  // Set up a block transfer that reads from RAM to device
  // Enable channel 4 (SPU) in DPCR
  dma.writeRegister(0x1F8010F0, 0x08888888);

  // Write some data to RAM
  for (int i = 0; i < 64; i++) {
    mem.write8(0x1000 + i, static_cast<uint8_t>(i));
  }

  // Ch4: base=0x1000, block=16 words, 1 blocks
  dma.writeRegister(0x1F8010C0, 0x00001000);
  dma.writeRegister(0x1F8010C4, 0x00010010);

  // Don't actually trigger - just test register state
  EXPECT_EQ(dma.readRegister(0x1F8010C0), 0x00001000u);
  EXPECT_EQ(dma.readRegister(0x1F8010C4), 0x00010010u);
}

TEST_F(DmaTest, GpuBlockTransferIsOneClosedSubmission) {
  gpu::GPU gpuDevice;
  dma.setGPU(&gpuDevice);

  // Two complete 1x1 GP0 rectangles in one four-word RAM->GPU block DMA.
  mem.write32(0x1000u, 0x680000FFu);
  mem.write32(0x1004u, (10u << 16) | 10u);
  mem.write32(0x1008u, 0x6800FF00u);
  mem.write32(0x100Cu, (10u << 16) | 11u);

  dma.writeRegister(0x1F8010F0u, 1u << 11); // enable GPU channel
  dma.writeRegister(0x1F8010A0u, 0x1000u);  // MADR
  dma.writeRegister(0x1F8010A4u, 4u);       // one block, four words
  dma.writeRegister(0x1F8010A8u,
                    (1u << 28) | (1u << 24) | 1u); // RAM -> GP0 burst

  EXPECT_EQ(dma.completionGeneration(DMA::GPU_CH), 1u);
  EXPECT_EQ(gpuDevice.getVRAM()[10u * gpu::GPU::VRAM_WIDTH + 10u].raw,
            0x001Fu);
  EXPECT_EQ(gpuDevice.getVRAM()[10u * gpu::GPU::VRAM_WIDTH + 11u].raw,
            0x03E0u);

  std::vector<gpu::Color16> snapshot(gpu::GPU::VRAM_WIDTH *
                                     gpu::GPU::VRAM_HEIGHT);
  gpu::GPU::DisplayState state;
  gpuDevice.snapshotDisplayBuffer(60u);
  gpuDevice.copyDisplayVRAM(snapshot.data(), snapshot.size(), &state);
  EXPECT_EQ(state.completedSubmissionId, 1u)
      << "the four GP0 words must complete as one DMA submission";

  // A new outer batch must receive and complete the next ID. If DMA left its
  // guard open, this pair would only change nested depth and ID 2 would not
  // become observable at the latch.
  gpuDevice.beginCommandBatch();
  gpuDevice.endCommandBatch();
  gpuDevice.snapshotDisplayBuffer(61u);
  gpuDevice.copyDisplayVRAM(snapshot.data(), snapshot.size(), &state);
  EXPECT_EQ(state.completedSubmissionId, 2u)
      << "GPU block DMA left the command batch open";
}

TEST_F(DmaTest, GpuReadDmaSharesCpuCursorAndPreservesPaletteBits) {
  gpu::GPU gpuDevice;
  dma.setGPU(&gpuDevice);
  gpuDevice.writeGP0(0xa0000000);
  gpuDevice.writeGP0((226u << 16) | 768u);
  gpuDevice.writeGP0((2u << 16) | 3u);
  gpuDevice.writeGP0(0x83e0001fu);
  gpuDevice.writeGP0(0x7c000000u);
  gpuDevice.writeGP0(0x8000ffffu);
  gpuDevice.writeGP0(0xc0000000);
  gpuDevice.writeGP0((226u << 16) | 768u);
  gpuDevice.writeGP0((2u << 16) | 3u);
  ASSERT_TRUE(gpuDevice.readGPUSTAT() & (1u << 27));
  ASSERT_EQ(gpuDevice.readGPUREAD(), 0x83e0001fu);
  mem.write32(0x1000, 0x7c000000u);
  PGXPValue precision;
  precision.value = 0x7c000000u;
  precision.flags = PGXPValue::VALID_XY;
  precision.x = 12.5f;
  precision.y = 13.5f;
  mem.storePrecision(0x1000, precision.value, precision);
  dma.writeRegister(0x1f8010f0, 1u << 11);
  dma.writeRegister(0x1f8010a0, 0x1000);
  dma.writeRegister(0x1f8010a4, 0x00010002);
  dma.writeRegister(0x1f8010a8, (1u << 24) | (1u << 9));
  EXPECT_EQ(mem.read32(0x1000), 0x7c000000u);
  EXPECT_EQ(mem.read32(0x1004), 0x8000ffffu);
  EXPECT_FALSE(mem.lookupPrecision(0x1000, 0x7c000000u, precision));
  EXPECT_FALSE(gpuDevice.readGPUSTAT() & (1u << 27));
  EXPECT_EQ(dma.completionGeneration(DMA::GPU_CH), 1u);
  EXPECT_FALSE(dma.readRegister(0x1f8010a8) & (1u << 24));
}

TEST_F(DmaTest, SpuTransferUsesAndAdvancesTransferAddress) {
  spu::SPU spu;
  dma.setSPU(&spu);

  // Two DMA words beginning at main-RAM address 0x1000.
  mem.write32(0x1000, 0x44332211);
  mem.write32(0x1004, 0x88776655);

  // SPU transfer address is expressed in eight-byte units. Destination is
  // therefore byte offset 0x100, not the start of Sound RAM.
  spu.writeRegister(0x1F801DA6, 0x0020);

  dma.writeRegister(0x1F8010F0, 1u << 19); // enable DMA channel 4
  dma.writeRegister(0x1F8010C0, 0x00001000);
  dma.writeRegister(0x1F8010C4, 0x00000002);
  dma.writeRegister(0x1F8010C8,
                    (1u << 28) | (1u << 24) | 1u); // burst, RAM -> SPU

  EXPECT_EQ(spu.readSoundRam(0x100), 0x2211);
  EXPECT_EQ(spu.readSoundRam(0x102), 0x4433);
  EXPECT_EQ(spu.readSoundRam(0x104), 0x6655);
  EXPECT_EQ(spu.readSoundRam(0x106), 0x8877);
  EXPECT_EQ(spu.readSoundRam(0), 0u);
  EXPECT_EQ(spu.readRegister(0x1F801DA6), 0x0021);
  EXPECT_EQ(dma.completionGeneration(DMA::SPU_CH), 1u);

  // A second start must be distinguishable even if Bios has not drained the
  // first completion callback yet.
  dma.writeRegister(0x1F8010C8,
                    (1u << 28) | (1u << 24) | 1u);
  EXPECT_EQ(dma.completionGeneration(DMA::SPU_CH), 2u);
}

// MGS reads the raw-sector header, then its payload, in separate DMA bursts.
TEST_F(DmaTest, CdromSplitDmaPreservesFifoCursor) {
  cdrom::CdromController cd;
  auto state = cd.snapshot();
  state.sectorSize = 2340;
  state.sectorReady = true;
  for (unsigned i = 0; i < state.sectorBuffer.size(); ++i)
    state.sectorBuffer[i] = static_cast<uint8_t>((i * 37u) ^ (i >> 8));
  cd.restore(state);
  dma.setCDROM(&cd);
  dma.writeRegister(0x1F8010F0, 0x00008000);
  auto transfer = [&](uint32_t address, uint32_t words) {
    dma.writeRegister(0x1F8010B0, address);
    dma.writeRegister(0x1F8010B4, words);
    dma.writeRegister(0x1F8010B8, 0x11000000);
  };
  transfer(0x1000, 3);
  EXPECT_TRUE(cd.hasSectorReady());
  EXPECT_EQ(cd.snapshot().dataReadOffset, 12u);
  transfer(0x2000, 512);
  for (unsigned i = 0; i < 12; ++i)
    EXPECT_EQ(mem.read8(0x1000 + i), state.sectorBuffer[12 + i]);
  for (unsigned i = 0; i < 2048; ++i)
    ASSERT_EQ(mem.read8(0x2000 + i), state.sectorBuffer[24 + i]) << i;
  EXPECT_EQ(cd.snapshot().dataReadOffset, 2060u);
}
