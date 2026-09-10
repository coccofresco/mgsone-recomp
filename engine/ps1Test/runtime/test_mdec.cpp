#include "runtime/mdec/mdec.h"
#include <gtest/gtest.h>

using namespace ps1::mdec;

class MdecTest : public ::testing::Test {
protected:
  MDEC mdec;
  void SetUp() override { mdec.reset(); }
};

TEST_F(MdecTest, InitialState) {
  EXPECT_FALSE(mdec.isBusy());
  EXPECT_TRUE(mdec.dataOutEmpty());
  uint32_t status = mdec.readStatus();
  EXPECT_TRUE(status & (1 << 31)); // Data-out empty
}

TEST_F(MdecTest, ResetCommand) {
  mdec.writeControl(1u << 31); // Reset
  EXPECT_FALSE(mdec.isBusy());
  EXPECT_TRUE(mdec.dataOutEmpty());
}

TEST_F(MdecTest, QuantTableUpload) {
  // Command: upload quant table (opcode 2, 16 words = 64 bytes)
  uint32_t cmd = (2u << 29) | 16;
  mdec.writeCommand(cmd);
  EXPECT_TRUE(mdec.isBusy());

  // Feed 16 words (64 bytes of quantization data)
  for (int i = 0; i < 16; i++) {
    mdec.writeCommand(0x10101010);
  }

  EXPECT_FALSE(mdec.isBusy());
}

TEST_F(MdecTest, DecodeCommandSetsBusy) {
  // Command: decode macroblock (opcode 1, with some data words)
  uint32_t cmd = (1u << 29) | 4;
  mdec.writeCommand(cmd);
  EXPECT_TRUE(mdec.isBusy());
}

TEST_F(MdecTest, StatusRegisterBits) {
  uint32_t status = mdec.readStatus();
  // Data-out empty flag should be set
  EXPECT_TRUE(status & (1 << 31));
  // Data-in not full
  EXPECT_FALSE(status & (1 << 30));
  // Not busy
  EXPECT_FALSE(status & (1 << 29));
}

TEST_F(MdecTest, DmaInterfaceSmoke) {
  // Send a command + data via DMA
  uint32_t cmd = (1u << 29) | 2;
  mdec.writeCommand(cmd);

  uint32_t data[2] = {0xFE00FE00, 0xFE00FE00}; // End markers
  mdec.dmaIn(data, 2);

  // Should process without crash
  EXPECT_FALSE(mdec.isBusy());
}

// Zigzag table sanity

TEST_F(MdecTest, ZigzagTableValid) {
  // The zigzag table should contain each value 0-63 exactly once
  bool seen[64] = {};
  for (int i = 0; i < 64; i++) {
    uint8_t val = MDEC::ZIGZAG[i];
    ASSERT_LT(val, 64u);
    EXPECT_FALSE(seen[val]) << "Duplicate zigzag entry: " << (int)val;
    seen[val] = true;
  }
}
