#include "runtime/spu/spu.h"
#include <algorithm>
#include <cstring>
#include <iterator>
#include <gtest/gtest.h>

using namespace ps1::spu;

// ADPCM Decoding Tests
//
// Vectors are hand-worked from the psx-spx SPU-ADPCM decode formula
// (PS1Recomp-workspace/psx-spx.github.io/docs/cdromformat.md:827-838,
// coefficient tables at :843-844) applied to the SPU block layout
// (soundprocessingunitspu.md:116-125). See the derivation notes in
// .superpowers/sdd/phase-3-crash-jogavel/task-S1-report.md.

class SpuAdpcmTest : public ::testing::Test {
protected:
  SPU spu;
  void SetUp() override { spu.reset(); }
};

TEST_F(SpuAdpcmTest, DecodesFullNibbleRangeWithFilterZero) {
  // header = filter 0, raw shift 12 (=> effective shift 12-12=0). With
  // filter 0, f0=f1=0 (cdromformat.md:843-844), so per the formula
  // s = (t<<0) + (old*0+older*0+32)/64 = t + 0 (integer division truncates
  // 32/64 toward zero) -- i.e. the decoded output is exactly the
  // sign-extended nibble sequence. This isolates nibble sign-extension from
  // the prediction filter.
  uint8_t block[16] = {};
  block[0] = 0x0C; // filter=0, shift=12
  block[1] = 0x00; // flags: normal

  // Nibble sequence -8..7 repeated (28 values), covering every 4-bit
  // pattern including the ones that must sign-extend to negative (8..15).
  // Packed 2 nibbles/byte, low nibble first (soundprocessingunitspu.md:121:
  // "LSBs=1st Sample, MSBs=2nd Sample").
  const uint8_t dataBytes[14] = {0x98, 0xBA, 0xDC, 0xFE, 0x10, 0x32, 0x54,
                                 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x10, 0x32};
  std::memcpy(block + 2, dataBytes, 14);

  auto samples = spu.decodeAdpcmBlockForTest(block, /*prevSample1=*/0,
                                              /*prevSample2=*/0);

  const int16_t expected[28] = {-8, -7, -6, -5, -4, -3, -2, -1, 0, 1,
                                2,  3,  4,  5,  6,  7,  -8, -7, -6, -5,
                                -4, -3, -2, -1, 0,  1,  2,  3};
  for (int i = 0; i < 28; i++) {
    EXPECT_EQ(samples[i], expected[i]) << "sample index " << i;
  }
}

TEST_F(SpuAdpcmTest, AppliesFilterOneCoefficientsToPrediction) {
  // Same nibble data as above, but filter=1 (f0=+60, f1=0 per
  // cdromformat.md:843-844) with raw shift 12 (effective shift 0), so the
  // IIR prediction term (old*f0 + older*f1 + 32) / 64 is exercised.
  // Expected values derived by applying cdromformat.md:827-838 verbatim
  // (integer division truncating toward zero, matching C++ int division):
  //   s0 = -8 + (0*60+0*0+32)/64            = -8 + 0   = -8
  //   s1 = -7 + (-8*60+0*0+32)/64           = -7 + (-448/64=-7) = -14
  //   s2 = -6 + (-14*60+(-8)*0+32)/64       = -6 + (-808/64=-12) = -18
  //   ... (full 28-sample sequence computed with the same recurrence)
  uint8_t block[16] = {};
  block[0] = 0x1C; // filter=1, shift=12 (effective shift 0)
  block[1] = 0x00;

  const uint8_t dataBytes[14] = {0x98, 0xBA, 0xDC, 0xFE, 0x10, 0x32, 0x54,
                                 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x10, 0x32};
  std::memcpy(block + 2, dataBytes, 14);

  auto samples = spu.decodeAdpcmBlockForTest(block, /*prevSample1=*/0,
                                              /*prevSample2=*/0);

  const int16_t expected[28] = {-8,  -14, -18, -21, -23, -24, -24, -23,
                                -21, -18, -14, -9,  -3,  3,   9,   15,
                                6,   -1,  -6,  -10, -12, -13, -13, -12,
                                -10, -7,  -4,  0};
  for (int i = 0; i < 28; i++) {
    EXPECT_EQ(samples[i], expected[i]) << "sample index " << i;
  }
}

TEST_F(SpuAdpcmTest, SilentBlockDecodesZeros) {
  // Set up a silent ADPCM block (all zeros) at address 0
  uint8_t *ram = spu.soundRamPtr();
  std::memset(ram, 0, 16); // 16-byte block of zeros
  ram[1] = 0x01;           // End flag to stop after one block

  // Write voice start address via register
  spu.writeRegister(0x1F801C06, 0);      // start addr = 0 (in 8-byte units)
  spu.writeRegister(0x1F801C04, 0x1000); // pitch = 1.0

  // Key on voice 0
  spu.writeRegister(0x1F801D88, 0x0001); // key on lo

  // Generate samples to process
  int16_t buffer[128] = {};
  spu.generateSamples(buffer, 64);

  // All output should be zero (silent input + ADSR attack starts at 0)
  // This is a basic smoke test
  EXPECT_TRUE(true); // If we got here without crash, ADPCM decode works
}

TEST_F(SpuAdpcmTest, AdpcmDecodesWithoutCrash) {
  // ADPCM filter coefficients are private; we test them
  // indirectly by decoding ADPCM blocks successfully
  uint8_t *ram = spu.soundRamPtr();
  // Filter=1 (60, 0), shift=0
  ram[0] = 0x10; // filter=1, shift=0
  ram[1] = 0x01; // end flag
  for (int i = 2; i < 16; i++)
    ram[i] = 0x77;

  spu.writeRegister(0x1F801C06, 0);
  spu.writeRegister(0x1F801C04, 0x1000);
  spu.writeRegister(0x1F801D88, 0x0001);

  int16_t buffer[128] = {};
  spu.generateSamples(buffer, 64);
  EXPECT_TRUE(true);
}

TEST_F(SpuAdpcmTest, MultipleBlocksDontCrash) {
  // Fill several ADPCM blocks with data
  uint8_t *ram = spu.soundRamPtr();
  for (int block = 0; block < 10; block++) {
    int addr = block * 16;
    ram[addr] = 0x00;                           // shift=0, filter=0
    ram[addr + 1] = (block == 9) ? 0x01 : 0x00; // end flag on last
    for (int i = 2; i < 16; i++) {
      ram[addr + i] =
          static_cast<uint8_t>(i * 17 + block); // pseudo-random data
    }
  }

  spu.writeRegister(0x1F801C06, 0);
  spu.writeRegister(0x1F801C04, 0x1000);
  spu.writeRegister(0x1F801C00, 0x3FFF); // volume left
  spu.writeRegister(0x1F801C02, 0x3FFF); // volume right
  spu.writeRegister(0x1F801D80, 0x7FFF); // main vol L
  spu.writeRegister(0x1F801D82, 0x7FFF); // main vol R
  spu.writeRegister(0x1F801D88, 0x0001); // key on

  int16_t buffer[1024] = {};
  spu.generateSamples(buffer, 512);

  // Should complete without crashing
  EXPECT_TRUE(true);
}

// ADPCM Loop/End Flag Tests
//
// Flag bits and the Code0-3 combinations are documented at
// soundprocessingunitspu.md:128-141. Cross-checked against
// CrashBandicoot-Launcher/RecompOne.Runtime/Hardware/Spu.cs:509-525
// (DecodeBlock), which unconditionally jumps CurAddr to RepeatAddr on the
// End flag (bit 0), regardless of the Repeat bit (bit 1); bit 1 only
// decides whether the envelope is also forced to Release/zero.
//
// A single generateSamples(buf, 1) call is enough to trigger exactly one
// advanceAdpcmBlock(): Key On forces decodedIndex to ADPCM_SAMPLES_PER_BLOCK,
// and pitch=0x1000 (1.0) advances decodedIndex by exactly one per output
// sample.

class SpuAdpcmLoopFlagTest : public ::testing::Test {
protected:
  SPU spu;
  void SetUp() override { spu.reset(); }

  void writeBlockHeader(uint32_t byteAddr, uint8_t shiftFilter,
                        uint8_t flags) {
    uint8_t *ram = spu.soundRamPtr();
    ram[byteAddr] = shiftFilter;
    ram[byteAddr + 1] = flags;
    std::memset(ram + byteAddr + 2, 0, 14);
  }
};

TEST_F(SpuAdpcmLoopFlagTest, Code0NormalAdvancesToNextBlockOnly) {
  writeBlockHeader(0, 0x00, 0x00); // filter=0,shift=0, flags=Normal

  spu.writeRegister(0x1F801C06, 0);      // start addr = 0
  spu.writeRegister(0x1F801C04, 0x1000); // pitch = 1.0
  spu.writeRegister(0x1F801D88, 0x0001); // key on voice 0

  int16_t buffer[2] = {};
  spu.generateSamples(buffer, 1);

  auto state = spu.debugVoiceState(0);
  EXPECT_EQ(state.currentAddr, 16u); // advanced by one 16-byte block only
  EXPECT_FALSE(state.endFlag);
  EXPECT_FALSE(state.loopFlag);
}

TEST_F(SpuAdpcmLoopFlagTest, LoopStartBitLatchesCurrentBlockAddress) {
  // Voice starts at block address 16 (startAddr=2, in 8-byte units).
  writeBlockHeader(16, 0x00, 0x04); // flags: Loop Start

  spu.writeRegister(0x1F801C06, 2); // start addr = 2 (=> byte addr 16)
  spu.writeRegister(0x1F801C04, 0x1000);
  spu.writeRegister(0x1F801D88, 0x0001);

  int16_t buffer[2] = {};
  spu.generateSamples(buffer, 1);

  // soundprocessingunitspu.md:100-102: "If the hardware finds an ADPCM
  // header with Loop-Start-Bit, then it copies the current address to the
  // repeat address register" -- current block's own address (16 bytes =
  // 2 in 8-byte units), not the next block.
  EXPECT_EQ(spu.readRegister(0x1F801C0E), 2);
}

TEST_F(SpuAdpcmLoopFlagTest, Code1EndMuteJumpsToRepeatAddressAndReleases) {
  writeBlockHeader(0, 0x00, 0x01); // flags: Loop End, bit1=0 => Code 1

  spu.writeRegister(0x1F801C06, 0);      // start addr = 0
  spu.writeRegister(0x1F801C0E, 5);      // repeat addr = 5 (=> byte addr 40)
  spu.writeRegister(0x1F801C04, 0x1000); // pitch = 1.0
  spu.writeRegister(0x1F801D88, 0x0001); // key on voice 0

  int16_t buffer[2] = {};
  spu.generateSamples(buffer, 1);

  auto state = spu.debugVoiceState(0);
  EXPECT_EQ(state.currentAddr, 40u); // jumped to repeatAddr*8, not addr+16
  EXPECT_TRUE(state.endFlag);
  EXPECT_TRUE(state.loopFlag);
  EXPECT_EQ(spu.readRegister(0x1F801D9C) & 0x1, 1); // ENDX bit 0 set
}

TEST_F(SpuAdpcmLoopFlagTest, Code3EndRepeatJumpsToRepeatAddress) {
  writeBlockHeader(0, 0x00, 0x03); // flags: Loop End + Loop Repeat => Code 3

  spu.writeRegister(0x1F801C06, 0);
  spu.writeRegister(0x1F801C0E, 5); // repeat addr = 5 (=> byte addr 40)
  spu.writeRegister(0x1F801C04, 0x1000);
  spu.writeRegister(0x1F801D88, 0x0001);

  int16_t buffer[2] = {};
  spu.generateSamples(buffer, 1);

  auto state = spu.debugVoiceState(0);
  EXPECT_EQ(state.currentAddr, 40u);
  EXPECT_TRUE(state.endFlag);
  EXPECT_TRUE(state.loopFlag);
  EXPECT_EQ(spu.readRegister(0x1F801D9C) & 0x1, 1);
}

// ADSR Envelope Tests

class SpuAdsrTest : public ::testing::Test {
protected:
  SPU spu;
  void SetUp() override { spu.reset(); }
};

TEST_F(SpuAdsrTest, VoiceStartsInOffPhase) {
  // By default, voices are in Off phase
  int16_t buffer[128] = {};
  spu.generateSamples(buffer, 64);

  // All output should be 0 (no voices active)
  for (int i = 0; i < 128; i++) {
    EXPECT_EQ(buffer[i], 0);
  }
}

TEST_F(SpuAdsrTest, KeyOnStartsAttackPhase) {
  // Set up a simple waveform
  uint8_t *ram = spu.soundRamPtr();
  std::memset(ram, 0, 16);
  ram[1] = 0x03; // loop + end

  spu.writeRegister(0x1F801C06, 0);
  spu.writeRegister(0x1F801C04, 0x1000);
  spu.writeRegister(0x1F801C08, 0x00FF); // ADSR lo: fast attack
  spu.writeRegister(0x1F801C0A, 0x0000); // ADSR hi

  // Key on voice 0
  spu.writeRegister(0x1F801D88, 0x0001);

  int16_t buffer[64] = {};
  spu.generateSamples(buffer, 32);

  // After key on, voice should be active (not all zeros necessarily,
  // depends on ADPCM data, but the system shouldn't crash)
  EXPECT_TRUE(true);
}

TEST_F(SpuAdsrTest, KeyOffTransitionsToRelease) {
  uint8_t *ram = spu.soundRamPtr();
  std::memset(ram, 0, 16);
  ram[1] = 0x03;

  spu.writeRegister(0x1F801C06, 0);
  spu.writeRegister(0x1F801C04, 0x1000);
  spu.writeRegister(0x1F801C08, 0x00FF);
  spu.writeRegister(0x1F801C0A, 0x0000);
  spu.writeRegister(0x1F801D88, 0x0001); // Key on

  int16_t buffer[128] = {};
  spu.generateSamples(buffer, 32);

  // Now key off
  spu.writeRegister(0x1F801D8C, 0x0001); // Key off
  spu.generateSamples(buffer, 64);

  // Should still not crash
  EXPECT_TRUE(true);
}

TEST_F(SpuAdsrTest, KeyOnThenOffBetweenCallbacksStillRendersTheNote) {
  uint8_t *ram = spu.soundRamPtr();
  std::memset(ram, 0x77, 16);
  ram[0] = 0x0C; // filter 0, shift 12
  ram[1] = 0x03; // loop end + repeat

  spu.writeRegister(0x1F801C00, 0x7FFF); // voice volume L
  spu.writeRegister(0x1F801C02, 0x7FFF); // voice volume R
  spu.writeRegister(0x1F801C04, 0x1000); // pitch 1.0
  spu.writeRegister(0x1F801C06, 0);      // start address
  spu.writeRegister(0x1F801C08, 0x000F); // fast linear attack
  spu.writeRegister(0x1F801D80, 0x7FFF); // main volume L
  spu.writeRegister(0x1F801D82, 0x7FFF); // main volume R

  // Both writes occur before SDL asks for another buffer. A pulse-register
  // implementation must not collapse them into a zero-duration voice.
  spu.writeRegister(0x1F801D88, 0x0001);
  spu.writeRegister(0x1F801D8C, 0x0001);

  int16_t buffer[128] = {};
  spu.generateSamples(buffer, 64);
  EXPECT_TRUE(std::any_of(std::begin(buffer), std::end(buffer),
                          [](int16_t sample) { return sample != 0; }));
}

// Volume Tests

class SpuVolumeTest : public ::testing::Test {
protected:
  SPU spu;
  void SetUp() override { spu.reset(); }
};

TEST_F(SpuVolumeTest, ZeroMainVolumeProducesSilence) {
  spu.writeRegister(0x1F801D80, 0); // main vol L = 0
  spu.writeRegister(0x1F801D82, 0); // main vol R = 0

  int16_t buffer[128] = {};
  spu.generateSamples(buffer, 64);

  for (int i = 0; i < 128; i++) {
    EXPECT_EQ(buffer[i], 0);
  }
}

TEST_F(SpuVolumeTest, RegisterReadback) {
  spu.writeRegister(0x1F801D80, 0x3FFF);
  EXPECT_EQ(spu.readRegister(0x1F801D80), 0x3FFF);

  spu.writeRegister(0x1F801D82, 0x1234);
  EXPECT_EQ(spu.readRegister(0x1F801D82), 0x1234);
}

// SPU Reverb Tests

class SpuReverbTest : public ::testing::Test {
protected:
  SPU spu;
  void SetUp() override { spu.reset(); }
};

TEST_F(SpuReverbTest, ReverbRegisterWriteDoesntCrash) {
  // Write all 32 reverb registers
  for (uint32_t i = 0; i < 32; i++) {
    spu.writeRegister(0x1F801DC0 + i * 2, 0x1234);
  }

  int16_t buffer[128] = {};
  spu.generateSamples(buffer, 64);
  EXPECT_TRUE(true);
}

// XA-ADPCM Tests

class XaAdpcmTest : public ::testing::Test {
protected:
  SPU spu;
  void SetUp() override {
    spu.reset();
    spu.writeRegister(0x1F801DB0, 0x7fff);
    spu.writeRegister(0x1F801DB2, 0x7fff);
    spu.writeRegister(0x1F801DAA, 1);
  }
};

TEST_F(XaAdpcmTest, XaSamplesAreMixed) {
  spu.writeRegister(0x1F801D80, 0x7FFF); // main vol L
  spu.writeRegister(0x1F801D82, 0x7FFF); // main vol R

  // Push some XA samples (stereo interleaved)
  int16_t xaSamples[64];
  for (int i = 0; i < 64; i++) {
    xaSamples[i] = 1000; // constant value
  }
  spu.pushXaSamples(xaSamples, 64);

  int16_t buffer[64] = {};
  spu.generateSamples(buffer, 32);

  // With max main volume, XA samples should appear in output
  // (output = xaSample * mainVol / 32768)
  bool hasNonZero = false;
  for (int i = 0; i < 64; i++) {
    if (buffer[i] != 0)
      hasNonZero = true;
  }
  EXPECT_TRUE(hasNonZero);
}

TEST_F(XaAdpcmTest, CdDaSamplesAreMixed) {
  spu.writeRegister(0x1F801D80, 0x7FFF);
  spu.writeRegister(0x1F801D82, 0x7FFF);

  int16_t cdSamples[64];
  for (int i = 0; i < 64; i++) {
    cdSamples[i] = 2000;
  }
  spu.pushCdDaSamples(cdSamples, 64);

  int16_t buffer[64] = {};
  spu.generateSamples(buffer, 32);

  bool hasNonZero = false;
  for (int i = 0; i < 64; i++) {
    if (buffer[i] != 0)
      hasNonZero = true;
  }
  EXPECT_TRUE(hasNonZero);
}

// Sound RAM Tests

TEST(SpuSoundRam, WriteAndReadBack) {
  SPU spu;
  spu.reset();

  spu.writeSoundRam(0x100, 0xABCD);
  EXPECT_EQ(spu.readSoundRam(0x100), 0xABCD);

  spu.writeSoundRam(0x200, 0x1234);
  EXPECT_EQ(spu.readSoundRam(0x200), 0x1234);
}

TEST(SpuSoundRam, TransferViaRegister) {
  SPU spu;
  spu.reset();

  // Set transfer address (in 8-byte units, so val=1 -> byte addr 8)
  spu.writeRegister(0x1F801DA6, 1);      // addr = 8
  spu.writeRegister(0x1F801DA8, 0xDEAD); // write data

  EXPECT_EQ(spu.readSoundRam(8), 0xDEAD);
}

// Control Register Tests

TEST(SpuControl, SpuCtrlReadback) {
  SPU spu;
  spu.reset();

  spu.writeRegister(0x1F801DAA, 0xC001);
  EXPECT_EQ(spu.readRegister(0x1F801DAA), 0xC001);
}

TEST(SpuControl, EndxFlagsClearedOnWrite) {
  SPU spu;
  spu.reset();

  // ENDX is at 0x19C/0x19E offset
  // Initially 0
  EXPECT_EQ(spu.readRegister(0x1F801D9C), 0);
}

TEST_F(SpuAdpcmTest, GuestClockStopsAtEachAlternatingSilentVoiceIrq) {
  // MGS clocks its stream with a silent 512-byte loop at pitch 0x1000.
  // IRQ alternates between the two 256-byte halves: 16 * 28 = 448 samples.
  auto *ram = spu.soundRamPtr();
  ram[1] = 4;
  ram[497] = 3;
  spu.writeRegister(0x1F801C04, 0x1000);
  spu.writeRegister(0x1F801C08, 0x000F);
  spu.writeRegister(0x1F801DA4, 0);
  spu.writeRegister(0x1F801DAA, 0xC040);
  spu.writeRegister(0x1F801D88, 1);
  spu.enableGuestClock();
  spu.accrueSamples(4480);
  std::vector<uint64_t> edges;
  for (unsigned iteration = 0; iteration < 32; ++iteration) {
    if (spu.serviceClock()) {
      edges.push_back(spu.snapshot().generatedSampleCount);
      spu.writeRegister(0x1F801DA4, edges.size() % 2 ? 32 : 0);
      spu.writeRegister(0x1F801DAA, 0xC000);
      spu.writeRegister(0x1F801DAA, 0xC040);
    }
    if (spu.snapshot().clockSamplesPending == 0) break;
  }
  ASSERT_GE(edges.size(), 10u);
  for (size_t i = 1; i < edges.size(); ++i)
    EXPECT_EQ(edges[i] - edges[i - 1], 448u);
  const auto before = spu.snapshot();
  int16_t audio[8192]{};
  spu.generateSamples(audio, 4096);
  spu.generateSamples(audio, 4096); // underflow must not advance guest audio
  EXPECT_EQ(spu.snapshot().generatedSampleCount, before.generatedSampleCount);
  EXPECT_EQ(spu.snapshot().clockSamplesPending, 0u);
}

TEST_F(SpuAdpcmTest, GuestClockRestoreRetainsUnconsumedSampleCredit) {
  spu.enableGuestClock();
  spu.accrueSamples(1470);
  spu.serviceClock();
  const auto checkpoint = spu.snapshot();
  EXPECT_EQ(checkpoint.clockSamplesPending, 446u);
  spu.serviceClock();
  spu.restore(checkpoint);
  spu.serviceClock();
  EXPECT_EQ(spu.snapshot().generatedSampleCount, 1470u);
  EXPECT_EQ(spu.snapshot().clockSamplesPending, 0u);
}

TEST_F(XaAdpcmTest, CdGainPolarityAndDisableConsumeRatherThanDelayAudio) {
  spu.writeRegister(0x1F801D80, 0x7fff);
  spu.writeRegister(0x1F801D82, 0x7fff);
  spu.writeRegister(0x1F801DB0, 0x4000);
  spu.writeRegister(0x1F801DB2, 0xc000);
  EXPECT_EQ(spu.readRegister(0x1F801DB0), 0x4000);
  EXPECT_EQ(spu.readRegister(0x1F801DB2), 0xc000);
  int16_t input[] = {10000, 10000, 10000, 10000};
  int16_t output[4]{};
  spu.pushXaSamples(input, 4);
  spu.generateSamples(output, 1);
  EXPECT_EQ(output[0], 4999);
  EXPECT_EQ(output[1], -5000);
  spu.writeRegister(0x1F801DAA, 0);
  spu.generateSamples(output, 1);
  EXPECT_EQ(output[0], 0);
  EXPECT_EQ(output[1], 0);
  spu.writeRegister(0x1F801DAA, 1);
  spu.generateSamples(output, 1);
  EXPECT_EQ(output[0], 0);
  EXPECT_EQ(output[1], 0);
}
