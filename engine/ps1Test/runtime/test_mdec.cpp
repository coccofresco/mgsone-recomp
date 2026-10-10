#include "runtime/mdec/mdec.h"
#include "runtime/metrics.h"
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

TEST_F(MdecTest, RetailColorModesHaveExactMacroblockSizesAndNeutralPixels) {
  for (unsigned depth : {2u, 3u}) {
    mdec.reset();
    mdec.writeCommand((1u << 29) | (depth << 27) | 6u);
    for (int block = 0; block < 6; ++block)
      mdec.writeCommand(0xFE000400u); // q_scale=1, DC=0, end-of-block
    const unsigned words = depth == 2 ? 192 : 128;
    EXPECT_EQ(mdec.dmaOutRemaining(), words);
    EXPECT_EQ((mdec.readStatus() >> 25) & 3u, depth);
    for (unsigned i = 0; i < words; ++i)
      EXPECT_EQ(mdec.readData(), depth == 2 ? 0x80808080u : 0x42104210u);
    EXPECT_TRUE(mdec.dataOutEmpty());
  }
}

TEST_F(MdecTest, ScaleUploadPreservesQuantizationAndPartlyConsumedPixelsRestore) {
  mdec.writeCommand(0x40000001);
  for (int i = 0; i < 32; ++i) mdec.writeCommand(0x10101010);
  mdec.writeCommand(0x60000000);
  for (int i = 0; i < 32; ++i) mdec.writeCommand(0); // zero matrix -> neutral pixels
  mdec.writeCommand(0x38000008);
  mdec.writeCommand(0xFE00FE00); // leading DMA padding is not a macroblock
  for (int i = 0; i < 6; ++i) mdec.writeCommand(0xFE000480);
  mdec.writeCommand(0xFE00FE00); // trailing padding must not add pixels
  ASSERT_EQ(mdec.dmaOutRemaining(), 128u);
  EXPECT_EQ(mdec.readData(), 0x42104210u);
  const auto state = mdec.snapshot();
  for (auto q : state.lumaQuantTable) EXPECT_EQ(q, 16);
  for (auto q : state.chromaQuantTable) EXPECT_EQ(q, 16);
  mdec.reset();
  mdec.restore(state);
  ASSERT_EQ(mdec.dmaOutRemaining(), 127u);
  for (unsigned i = 0; i < 127; ++i) EXPECT_EQ(mdec.readData(), 0x42104210u);
  mdec.writeCommand(0x38000006);
  for (int i = 0; i < 6; ++i) mdec.writeCommand(0xFE000480);
  EXPECT_EQ(mdec.readData(), 0x42104210u); // restored matrix drives the next frame too
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

TEST_F(MdecTest, RetailMovieMaskBitAndSignedPixelsFollowCommand) {
  for (unsigned depth : {2u, 3u}) {
    for (bool sign : {false, true}) {
      for (bool mask : {false, true}) {
        mdec.reset();
        mdec.writeCommand((1u << 29) | (depth << 27) | (uint32_t(sign) << 26) |
                          (uint32_t(mask) << 25) | 6);
        for (int block = 0; block < 6; ++block) mdec.writeCommand(0xfe000400);
        const uint32_t gray = sign ? 0 : depth == 2 ? 0x80808080 : 0x42104210;
        EXPECT_EQ(mdec.readData(), gray | (depth == 3 && mask ? 0x80008000 : 0));
      }
    }
  }
}

namespace {
std::vector<uint32_t> twoMacroblocks() {
  std::vector<uint16_t> halves;
  for (unsigned block = 0; block < 12; ++block) {
    halves.push_back(0x0400u | (block * 7)); // DC and one nonzero AC
    halves.push_back(3);
    halves.push_back(0xfe00);
  }
  std::vector<uint32_t> words;
  for (size_t i = 0; i < halves.size(); i += 2)
    words.push_back(uint32_t(halves[i]) | (uint32_t(halves[i + 1]) << 16));
  return words;
}

std::vector<uint32_t> drain(MDEC &decoder) {
  std::vector<uint32_t> result;
  while (!decoder.dataOutEmpty()) result.push_back(decoder.readData());
  return result;
}
} // namespace

TEST_F(MdecTest, EveryTruncatedCommandKeepsOnlyCompleteMacroblocks) {
  const auto words = twoMacroblocks();
  for (unsigned depth : {2u, 3u}) {
    const unsigned size = depth == 2 ? 192 : 128;
    mdec.reset();
    mdec.writeCommand((1u << 29) | (depth << 27) | words.size());
    mdec.dmaIn(words.data(), words.size());
    const auto complete = drain(mdec);
    ASSERT_EQ(complete.size(), size * 2);
    for (size_t cut = 1; cut <= words.size(); ++cut) {
      SCOPED_TRACE(cut);
      mdec.reset();
      mdec.writeCommand((1u << 29) | (depth << 27) | cut);
      mdec.dmaIn(words.data(), cut);
      const auto actual = drain(mdec);
      ASSERT_EQ(actual.size(), (cut / 9) * size);
      EXPECT_TRUE(std::equal(actual.begin(), actual.end(), complete.begin()));
    }
  }
}

TEST_F(MdecTest, EveryDmaSplitAndMidInputRestoreMatchesContinuousDecode) {
  const auto words = twoMacroblocks();
  for (unsigned depth : {2u, 3u}) {
    mdec.reset();
    const uint32_t command = (1u << 29) | (depth << 27) | words.size();
    mdec.writeCommand(command);
    mdec.dmaIn(words.data(), words.size());
    const auto expected = drain(mdec);
    for (size_t cut = 0; cut <= words.size(); ++cut) {
      SCOPED_TRACE(cut);
      mdec.reset();
      mdec.writeCommand(command);
      mdec.dmaIn(words.data(), cut);
      MDEC resumed;
      resumed.restore(mdec.snapshot());
      mdec.dmaIn(words.data() + cut, words.size() - cut);
      for (size_t i = cut; i < words.size(); ++i) resumed.writeCommand(words[i]);
      EXPECT_EQ(drain(mdec), expected);
      EXPECT_EQ(drain(resumed), expected);
    }
  }
}

TEST_F(MdecTest, PaddingAndTruncationCannotCertifyDecodedFrames) {
  ps1::metrics::setEnabledForTesting(true);
  ps1::metrics::reset();
  mdec.writeCommand(0x38000002);
  for (int i = 0; i < 2; ++i) mdec.writeCommand(0xfe00fe00);
  EXPECT_EQ(ps1::metrics::get("mdec.decoded_frame"), 0u);
  EXPECT_EQ(ps1::metrics::get("mdec.truncated_command"), 0u);
  const auto words = twoMacroblocks();
  mdec.writeCommand(0x38000011); // Second macroblock lacks its final EOB.
  mdec.dmaIn(words.data(), 17);
  EXPECT_EQ(ps1::metrics::get("mdec.decoded_frame"), 0u);
  EXPECT_EQ(ps1::metrics::get("mdec.decoded_macroblock"), 1u);
  EXPECT_EQ(ps1::metrics::get("mdec.truncated_command"), 1u);
  ps1::metrics::setEnabledForTesting(false);
}
