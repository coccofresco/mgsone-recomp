#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/cpu_context.h"
#include "runtime/dma/dma.h"
#include "runtime/gpu/gpu.h"
#include "runtime/input/input.h"
#include "runtime/mdec/mdec.h"
#include "runtime/memory.h"
#include "runtime/spu/spu.h"
#include "runtime/timers/timers.h"
#include <gtest/gtest.h>

using namespace ps1;

// Game Validation Framework
// Tests that verify the system can boot and run game-like workloads
// without crashing. These don't test actual commercial games (which
// require disc images), but validate the full hardware integration.

class GameValidationTest : public ::testing::Test {
protected:
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  mdec::MDEC mdec;

  void SetUp() override {
    mem.setGPU(&gpu);
    mem.setSPU(&spu);
    mem.setDMA(&dma);
    mem.setCDROM(&cdrom);
    mem.setInput(&input);
    mem.setMDEC(&mdec);
    mem.setTimers(&timers);
    mem.setInterruptController(&irq);
    dma.setMemory(&mem);
    dma.setGPU(&gpu);
    dma.setSPU(&spu);
    dma.setCDROM(&cdrom);
  }
};

// Boot Sequence Tests

TEST_F(GameValidationTest, MemoryIORoutingDoesntCrash) {
  // Simulate a game's boot sequence: writing to all I/O ports
  // GPU
  mem.write32(0x1F801810, 0x00000000); // GP0 NOP
  mem.write32(0x1F801814, 0x00000000); // GP1 reset
  auto gpustat = mem.read32(0x1F801814);
  EXPECT_NE(gpustat, 0u);

  // SPU
  mem.write16(0x1F801DAA, 0x0000); // SPUCNT
  auto spustat = mem.read16(0x1F801DAE);
  (void)spustat;

  // DMA
  auto dpcr = mem.read32(0x1F8010F0);
  EXPECT_EQ(dpcr, 0x07654321u);

  // IRQ
  mem.write32(0x1F801074, 0x7FF); // Enable all IRQs
  auto imask = mem.read32(0x1F801074);
  EXPECT_EQ(imask, 0x7FFu);

  // Timers
  mem.write16(0x1F801104, 0x0000);
  mem.write16(0x1F801108, 1000); // Target

  // Input
  auto joystat = mem.read32(0x1F801044);
  (void)joystat;

  // CDROM
  mem.write8(0x1F801800, 0x00); // Index 0

  // Memory Control
  mem.write32(0x1F801000, 0x1F000000); // Expansion 1
  auto ramSize = mem.read32(0x1F801060);
  EXPECT_EQ(ramSize, 0x00000B88u);
}

TEST_F(GameValidationTest, TypicalGameFrameTick) {
  // Simulate one frame of a typical game:
  // 1. Set up IRQ mask
  irq.writeIMask(IRQ_VBLANK | IRQ_CDROM | IRQ_DMA);

  // 2. Tick timers for 263 scanlines
  for (uint32_t line = 0; line < 263; line++) {
    timers.tick(3413, true, false);
  }

  // 3. VBlank
  irq.raiseInterrupt(IRQ_VBLANK);
  EXPECT_TRUE(irq.hasPendingInterrupt());

  // 4. Acknowledge
  irq.writeIStat(~IRQ_VBLANK);
  EXPECT_FALSE(irq.hasPendingInterrupt());
}

TEST_F(GameValidationTest, DMALinkedListGPU) {
  // Build a simple GPU ordering table in RAM
  // Entry at 0x1000: 1 word payload, next -> end
  mem.write32(0x1000, 0x01FFFFFF); // 1 word, next = end
  mem.write32(0x1004, 0x20FF0000); // GP0: flat tri (blue) -- just a cmd

  // Set up DMA ch2 for linked list transfer
  dma.writeRegister(0x1F8010A0, 0x00001000); // Base = 0x1000
  dma.writeRegister(0x1F8010A4, 0x00000000); // Block (unused for linked list)
  dma.writeRegister(0x1F8010A8,
                    0x01000401); // CHCR: from RAM, linked list, start

  // Shouldn't crash
  EXPECT_TRUE(true);
}

TEST_F(GameValidationTest, SPUMixingDoesntCrash) {
  // Generate 1024 stereo samples
  int16_t buffer[2048] = {};
  spu.generateSamples(buffer, 1024);

  // In silence, output should be mostly zeros
  bool allZero = true;
  for (int i = 0; i < 2048; i++) {
    if (buffer[i] != 0) {
      allZero = false;
      break;
    }
  }
  EXPECT_TRUE(allZero); // No keys on = silence
}

TEST_F(GameValidationTest, CDROMGetStatDoesntCrash) {
  cdrom.writeRegister(0x1F801800, 0x00); // Index 0
  cdrom.writeRegister(0x1F801801, 0x01); // GetStat
  cdrom.tick(100000);
  EXPECT_TRUE(cdrom.hasInterrupt());
  cdrom.ackInterrupt(0x1F);
}

TEST_F(GameValidationTest, InputPollingDoesntCrash) {
  // Simulate controller polling sequence
  input.press(ps1::input::BTN_CROSS, 0);
  mem.write32(0x1F801040, 0x01); // Start comms
  auto resp = mem.read32(0x1F801040);
  (void)resp;
  input.release(ps1::input::BTN_CROSS, 0);
}

TEST_F(GameValidationTest, MDECDecodeSmoke) {
  // Send a decode command with end markers
  mdec.writeCommand((1u << 29) | 2); // Decode, 2 words
  mdec.writeCommand(0xFE00FE00);     // End markers
  mdec.writeCommand(0xFE00FE00);
  EXPECT_FALSE(mdec.isBusy());
}

TEST_F(GameValidationTest, FullSubsystemResetDoesntCrash) {
  // Reset everything -- simulates game restart
  mem.reset();
  gpu.reset();
  spu.reset();
  dma.reset();
  cdrom.reset();
  timers.reset();
  irq.reset();
  input.reset();
  mdec.reset();
  EXPECT_TRUE(true);
}

TEST_F(GameValidationTest, StressTest1000Frames) {
  // Run 1000 frames of hardware ticking
  for (int frame = 0; frame < 1000; frame++) {
    // Tick CDROM
    cdrom.tick(564480);

    // Tick timers
    timers.tick(564480);

    // VBlank
    irq.raiseInterrupt(IRQ_VBLANK);
    irq.writeIStat(~IRQ_VBLANK); // ACK

    // SPU
    int16_t buf[128];
    spu.generateSamples(buf, 64);
  }
  EXPECT_TRUE(true); // Survived 1000 frames without crash
}
