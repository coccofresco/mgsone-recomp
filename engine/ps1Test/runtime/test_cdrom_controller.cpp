#include "runtime/cdrom/cdrom_controller.h"
#include <gtest/gtest.h>
#include <algorithm>

using namespace ps1::cdrom;

class CdromControllerTest : public ::testing::Test {
protected:
  CdromController cdrom;
  void SetUp() override { cdrom.reset(); }
};

namespace {
struct UnpacedTestDisc : VirtualFs {
  uint32_t first = 18, end = 22;
  std::vector<uint32_t> reads;
  bool failRead = false;
  bool allowsUnpacedData(uint32_t lba) const override {
    return lba >= first && lba < end;
  }
  std::optional<Sector> readSector(uint32_t lba) override {
    reads.push_back(lba);
    if (failRead) return std::nullopt;
    Sector sector{};
    sector.raw[24] = static_cast<uint8_t>(lba);
    return sector;
  }
};
}

TEST_F(CdromControllerTest, UnpacedDataBypassesSeekButRetainsOrderedCallbackHandshake) {
  UnpacedTestDisc disc;
  cdrom.attachVirtualFs(&disc);
  auto state = cdrom.snapshot();
  state.state = CdromState::ReadingData;
  state.currentLba = 18;
  state.readCycleCounter = -20000000;
  state.interruptFlag = INT_ACKNOWLEDGE;
  cdrom.restore(state);
  EXPECT_FALSE(cdrom.serviceMaturedSector()); // ReadN ACK precedes payload.
  EXPECT_TRUE(disc.reads.empty());
  cdrom.ackInterrupt(0x1f);
  for (uint32_t lba = 18; lba < 22; ++lba) {
    ASSERT_TRUE(cdrom.serviceMaturedSector()); // No tick/borrowed time.
    EXPECT_EQ(cdrom.readDataByte(), lba);
    EXPECT_FALSE(cdrom.serviceMaturedSector());
    cdrom.ackInterrupt(0x1f);
    EXPECT_FALSE(cdrom.serviceMaturedSector()); // Consumer still owns buffer.
    cdrom.clearSectorReady(); // Consumer may discard a partial final sector.
    cdrom.clearWaitingForAck();
  }
  EXPECT_EQ(disc.reads, (std::vector<uint32_t>{18, 19, 20, 21}));
  EXPECT_FALSE(cdrom.unpacedDataRead());
  EXPECT_FALSE(cdrom.serviceMaturedSector()); // First non-asset must wait.
  EXPECT_EQ(cdrom.snapshot().readCycleCounter, 0);
  cdrom.tick(cdrom.getCyclesPerSector() - 1);
  EXPECT_EQ(disc.reads.size(), 4u);
  cdrom.tick(1);
  EXPECT_EQ(disc.reads.back(), 22u);
}

TEST_F(CdromControllerTest, UnpacedPolicyNeverAcceleratesXaOrNonReadingStates) {
  UnpacedTestDisc disc;
  cdrom.attachVirtualFs(&disc);
  auto state = cdrom.snapshot();
  state.currentLba = 18;
  for (auto status : {CdromState::Idle, CdromState::Paused, CdromState::Seeking,
                     CdromState::Playing, CdromState::ReadingData}) {
    state.state = status;
    state.mode = 0x40;
    cdrom.restore(state);
    EXPECT_FALSE(cdrom.unpacedDataRead());
    EXPECT_FALSE(cdrom.serviceMaturedSector());
  }
  EXPECT_TRUE(disc.reads.empty());
}

TEST_F(CdromControllerTest, UnpacedRestoreRetainsPendingPayloadAndCurrentHostPolicy) {
  UnpacedTestDisc disc;
  cdrom.attachVirtualFs(&disc);
  auto state = cdrom.snapshot();
  state.state = CdromState::ReadingData;
  state.currentLba = 18;
  cdrom.restore(state);
  ASSERT_TRUE(cdrom.serviceMaturedSector());
  state = cdrom.snapshot();
  cdrom.reset();
  cdrom.restore(state);
  EXPECT_FALSE(cdrom.serviceMaturedSector());
  EXPECT_EQ(cdrom.readDataByte(), 18);
  cdrom.ackInterrupt(0x1f);
  cdrom.clearWaitingForAck();
  disc.end = disc.first; // Disabled host option is not overridden by savestate.
  EXPECT_FALSE(cdrom.serviceMaturedSector());
  cdrom.tick(cdrom.getCyclesPerSector());
  EXPECT_EQ(disc.reads, (std::vector<uint32_t>{18, 19}));
}

TEST_F(CdromControllerTest, UnpacedReadFailureDoesNotPublishOrAdvance) {
  UnpacedTestDisc disc;
  disc.failRead = true;
  cdrom.attachVirtualFs(&disc);
  auto state = cdrom.snapshot();
  state.state = CdromState::ReadingData;
  state.currentLba = 18;
  cdrom.restore(state);
  EXPECT_FALSE(cdrom.serviceMaturedSector());
  EXPECT_FALSE(cdrom.hasSectorReady());
  EXPECT_EQ(cdrom.interruptFlag(), INT_NONE);
  EXPECT_EQ(cdrom.snapshot().currentLba, 18u);
  disc.failRead = false;
  EXPECT_TRUE(cdrom.serviceMaturedSector());
  EXPECT_EQ(cdrom.readDataByte(), 18);
}

TEST_F(CdromControllerTest, InitialState) {
  EXPECT_EQ(cdrom.getState(), CdromState::Idle);
  EXPECT_FALSE(cdrom.hasInterrupt());
}

TEST_F(CdromControllerTest, GetStatCommand) {
  // Write index 0 to select command registers
  cdrom.writeRegister(0x1F801800, 0x00);
  // Send GetStat (0x01)
  cdrom.writeRegister(0x1F801801, 0x01);
  // Tick to process command
  cdrom.tick(100000);
  // Should have interrupt response
  EXPECT_TRUE(cdrom.hasInterrupt());
  // Read response - should be status byte
  uint8_t stat = cdrom.readRegister(0x1F801801);
  EXPECT_NE(stat, 0); // Motor should be on after GetStat
}

TEST_F(CdromControllerTest, SetLocCommand) {
  cdrom.writeRegister(0x1F801800, 0x00);
  // Set parameters: minute=0, second=2, sector=0 (LBA 0)
  cdrom.writeRegister(0x1F801802, 0x00); // minute BCD
  cdrom.writeRegister(0x1F801802, 0x02); // second BCD
  cdrom.writeRegister(0x1F801802, 0x00); // sector BCD
  // Send SetLoc (0x02)
  cdrom.writeRegister(0x1F801801, 0x02);
  cdrom.tick(100000);
  EXPECT_TRUE(cdrom.hasInterrupt());
}

TEST_F(CdromControllerTest, InitCommand) {
  cdrom.writeRegister(0x1F801800, 0x00);
  cdrom.writeRegister(0x1F801801, 0x0A); // Init
  cdrom.tick(100000);
  EXPECT_EQ(cdrom.getState(), CdromState::Idle);
  EXPECT_TRUE(cdrom.hasInterrupt());
}

TEST_F(CdromControllerTest, StatusRegisterReflectsState) {
  // Read status register (port 0)
  uint8_t status = cdrom.readRegister(0x1F801800);
  // Index bits should be 0
  EXPECT_EQ(status & 3, 0);
}

TEST_F(CdromControllerTest, InterruptAcknowledge) {
  cdrom.writeRegister(0x1F801800, 0x00);
  cdrom.writeRegister(0x1F801801, 0x01); // GetStat
  cdrom.tick(100000);
  EXPECT_TRUE(cdrom.hasInterrupt());
  // Acknowledge interrupt
  cdrom.ackInterrupt(0x1F);
  EXPECT_FALSE(cdrom.hasInterrupt());
}

TEST_F(CdromControllerTest, MsfLbaConversion) {
  // MSF 00:02:00 = LBA 150 (pregap) -> 0 data
  EXPECT_EQ(CdromController::msfToLba(0, 2, 0), 150u);
  EXPECT_EQ(CdromController::msfToLba(0, 0, 0), 0u);
  EXPECT_EQ(CdromController::msfToLba(1, 0, 0), 4500u); // 60*75

  uint8_t m, s, f;
  CdromController::lbaToMsf(150, m, s, f);
  EXPECT_EQ(m, 0);
  EXPECT_EQ(s, 2);
  EXPECT_EQ(f, 0);
}

TEST_F(CdromControllerTest, BcdConversion) {
  EXPECT_EQ(CdromController::toBcd(0), 0x00);
  EXPECT_EQ(CdromController::toBcd(10), 0x10);
  EXPECT_EQ(CdromController::toBcd(59), 0x59);
  EXPECT_EQ(CdromController::fromBcd(0x59), 59);
  EXPECT_EQ(CdromController::fromBcd(0x10), 10);
}

TEST_F(CdromControllerTest, StreamingDataCannotOverwriteSetlocAcknowledgement) {
  struct Disc : VirtualFs {
    unsigned reads = 0;
    std::optional<Sector> readSector(uint32_t) override {
      ++reads;
      return Sector{};
    }
  } disc;
  cdrom.attachVirtualFs(&disc);
  auto state = cdrom.snapshot();
  state.state = CdromState::ReadingData;
  state.currentLba = 18;
  state.motorOn = true;
  cdrom.restore(state);
  cdrom.writeRegister(0x1F801800, 0);
  for (uint8_t value : {0, 2, 16}) cdrom.writeRegister(0x1F801802, value);
  cdrom.writeRegister(0x1F801801, 2); // Setloc during an ongoing ReadN
  cdrom.tick(cdrom.getCyclesPerSector() * 3);
  EXPECT_EQ(cdrom.interruptFlag(), INT_ACKNOWLEDGE);
  EXPECT_EQ(disc.reads, 0u);
  EXPECT_EQ(cdrom.snapshot().responseFifo.size(), 1u);
  cdrom.ackInterrupt(0x1f);
  cdrom.tick(0);
  EXPECT_EQ(cdrom.interruptFlag(), INT_DATA_READY);
  EXPECT_EQ(disc.reads, 1u);
  // Even clearing the data gate cannot consume a still unacknowledged INT1.
  cdrom.clearWaitingForAck();
  cdrom.tick(cdrom.getCyclesPerSector());
  EXPECT_EQ(disc.reads, 1u);
  cdrom.ackInterrupt(0x1f);
  cdrom.tick(0);
  EXPECT_EQ(disc.reads, 2u);
}

namespace {
struct XaDisc : VirtualFs {
  std::vector<Sector> sectors;
  std::optional<Sector> readSector(uint32_t lba) override {
    if (lba >= sectors.size()) return {};
    return sectors[lba];
  }
};
Sector xaSector(uint8_t coding = 1, uint8_t file = 1, uint8_t channel = 1) {
  Sector sector{};
  sector.raw[15] = 2;
  sector.raw[16] = file;
  sector.raw[17] = channel;
  sector.raw[18] = 0x64;
  sector.raw[19] = coding;
  for (unsigned g = 0; g < 18; ++g) {
    auto *p = sector.raw + 24 + g * 128;
    for (unsigned i = 4; i < 12; ++i) p[i] = 0x14; // predictor 1, shift 4
    for (unsigned i = 16; i < 128; ++i) p[i] = uint8_t(i * 17 + g * 29);
  }
  return sector;
}
void beginXa(CdromController &cd, XaDisc &disc) {
  cd.attachVirtualFs(&disc);
  auto s = cd.snapshot();
  s.state = CdromState::ReadingData;
  s.mode = 0xc0;
  s.cyclesPerSector = 33868800 / 150;
  cd.restore(s);
}
}

TEST(XaDecoder, AllRatesAndChannelLayoutsHaveCorrectDuration) {
  for (uint8_t coding : {0, 1, 4, 5, 16, 17, 20, 21}) {
    XaAdpcm decoder;
    auto sector = xaSector(coding);
    auto pcm = decoder.decode(sector.raw);
    const unsigned frames = 2352 * ((coding & 1) ? 1 : 2) *
        ((coding & 4) ? 2 : 1) / ((coding & 16) ? 2 : 1);
    ASSERT_EQ(pcm.size(), frames * 2u);
    EXPECT_TRUE(std::any_of(pcm.begin(), pcm.end(), [](auto v) { return v != 0; }));
    if (!(coding & 1))
      for (size_t i = 0; i < pcm.size(); i += 2) ASSERT_EQ(pcm[i], pcm[i+1]);
  }
}

TEST(XaDecoder, EightBitUsesTheWholeByteAndReservedShiftsMatchNine) {
  auto sector = xaSector(17);
  for (unsigned g = 0; g < 18; ++g) {
    auto *p = sector.raw + 24 + g * 128;
    std::fill(p + 4, p + 12, 0);
    std::fill(p + 16, p + 128, 0x10); // +4096, not zero after truncating <<12
  }
  XaAdpcm decoder;
  auto pcm = decoder.decode(sector.raw);
  ASSERT_GT(pcm.size(), 100u);
  EXPECT_GT(pcm[100], 3500);
  auto reference = xaSector();
  auto reserved = reference;
  for (unsigned g = 0; g < 18; ++g)
    for (unsigned i = 4; i < 12; ++i) {
      reference.raw[24 + g * 128 + i] = 9;
      reserved.raw[24 + g * 128 + i] = 15;
    }
  XaAdpcm a, b;
  EXPECT_EQ(a.decode(reference.raw), b.decode(reserved.raw));
}

TEST_F(CdromControllerTest, XaConsumesDriveTimeWithoutDataInterruptOrBufferOverwrite) {
  XaDisc disc;
  disc.sectors = {xaSector(), Sector{}};
  beginXa(cdrom, disc);
  std::vector<int16_t> pcm;
  cdrom.setXaCallback([&](auto p, auto n) { pcm.assign(p, p+n); });
  cdrom.tick(cdrom.getCyclesPerSector());
  EXPECT_FALSE(cdrom.hasInterrupt());
  EXPECT_FALSE(cdrom.hasSectorReady());
  EXPECT_EQ(pcm.size(), 4704u);
  EXPECT_EQ(cdrom.snapshot().currentLba, 1u);
  cdrom.tick(0); // no fabricated time or duplicated audio
  EXPECT_EQ(cdrom.snapshot().currentLba, 1u);
  cdrom.tick(cdrom.getCyclesPerSector());
  EXPECT_EQ(cdrom.interruptFlag(), INT_DATA_READY);
  EXPECT_TRUE(cdrom.hasSectorReady());
}

TEST_F(CdromControllerTest, XaFilterMuteAndVolumePortsDoNotAcknowledgeInterrupts) {
  XaDisc disc;
  disc.sectors = {xaSector(1, 2), xaSector(), xaSector(), xaSector(), xaSector()};
  beginXa(cdrom, disc);
  auto s = cdrom.snapshot();
  s.xaFilterEnabled = true;
  s.xaFilterFile = s.xaFilterChannel = 1;
  cdrom.restore(s);
  unsigned calls = 0;
  std::vector<int16_t> pcm;
  cdrom.setXaCallback([&](auto p, auto n) { ++calls; pcm.assign(p, p+n); });
  cdrom.tick(cdrom.getCyclesPerSector());
  EXPECT_EQ(calls, 0u);
  cdrom.tick(cdrom.getCyclesPerSector());
  EXPECT_EQ(calls, 1u);
  // Stage a muted left output, normal right. It cannot ACK a command or set IE.
  cdrom.writeRegister(0x1f801800, 0);
  cdrom.writeRegister(0x1f801801, 1);
  cdrom.writeRegister(0x1f801800, 2);
  cdrom.writeRegister(0x1f801802, 0);
  cdrom.writeRegister(0x1f801803, 0);
  cdrom.writeRegister(0x1f801800, 3);
  cdrom.writeRegister(0x1f801801, 128);
  cdrom.writeRegister(0x1f801802, 0);
  EXPECT_EQ(cdrom.interruptFlag(), INT_ACKNOWLEDGE);
  EXPECT_EQ(cdrom.snapshot().interruptEnable, 31);
  EXPECT_EQ(cdrom.snapshot().audioVolume[0], 128);
  cdrom.writeRegister(0x1f801803, 0x20);
  EXPECT_EQ(cdrom.interruptFlag(), INT_ACKNOWLEDGE);
  cdrom.ackInterrupt(31);
  cdrom.tick(cdrom.getCyclesPerSector());
  EXPECT_EQ(calls, 2u);
  for (size_t i = 0; i < pcm.size(); i += 2) ASSERT_EQ(pcm[i], 0);
  cdrom.writeRegister(0x1f801803, 1); // ADPCM mute
  cdrom.tick(cdrom.getCyclesPerSector());
  EXPECT_EQ(calls, 2u);
  EXPECT_EQ(cdrom.snapshot().currentLba, 4u);
  cdrom.writeRegister(0x1f801803, 0);
  cdrom.tick(cdrom.getCyclesPerSector());
  EXPECT_EQ(calls, 3u);
}

TEST_F(CdromControllerTest, XaStateRestoresPredictorAndInterpolationWithoutDiscontinuity) {
  XaDisc disc;
  disc.sectors = {xaSector(), xaSector()};
  beginXa(cdrom, disc);
  std::vector<int16_t> expected, actual;
  cdrom.setXaCallback([&](auto p, auto n) { expected.assign(p, p+n); });
  cdrom.tick(cdrom.getCyclesPerSector());
  const auto saved = cdrom.snapshot();
  cdrom.tick(cdrom.getCyclesPerSector());
  CdromController resumed;
  resumed.attachVirtualFs(&disc);
  resumed.restore(saved);
  resumed.setXaCallback([&](auto p, auto n) { actual.assign(p, p+n); });
  resumed.tick(resumed.getCyclesPerSector());
  ASSERT_EQ(expected.size(), 4704u);
  EXPECT_EQ(actual, expected);
}

TEST(XaDecoder, FiveSectorsMatchIndependentSwanStationGolden) {
  // FNV-1a over little-endian PCM from the pinned donor's DecodeADPCMSector
  // and ResampleXAADPCM, with predictor/ring carried across all five sectors.
  const auto sector = xaSector();
  XaAdpcm decoder;
  uint64_t hash = 14695981039346656037ull;
  for (unsigned block = 0; block < 5; ++block) {
    for (int16_t value : decoder.decode(sector.raw))
      for (unsigned shift : {0, 8}) {
        hash ^= (uint16_t(value) >> shift) & 255;
        hash *= 1099511628211ull;
      }
  }
  EXPECT_EQ(hash, 0x3aedf009aafb4ee0ull);
}

TEST_F(CdromControllerTest, MaturedXaSectorDoesNotDelayFollowingDataUntilAnotherClockTick) {
  XaDisc disc;
  disc.sectors = {xaSector(), xaSector(), Sector{}};
  beginXa(cdrom, disc);
  unsigned audio = 0;
  cdrom.setXaCallback([&](auto, auto) { ++audio; });
  cdrom.accrueClockCycles(cdrom.getCyclesPerSector() * 3);
  cdrom.serviceClock();
  EXPECT_EQ(audio, 2u);
  EXPECT_EQ(cdrom.snapshot().currentLba, 3u);
  EXPECT_EQ(cdrom.interruptFlag(), INT_DATA_READY);
  EXPECT_TRUE(cdrom.hasSectorReady());
  cdrom.serviceMaturedSector();
  EXPECT_EQ(audio, 2u);
  EXPECT_EQ(cdrom.snapshot().currentLba, 3u);
}

TEST_F(CdromControllerTest, XaAutomaticFileFilterReleasesOnlyOnAcceptedEof) {
  XaDisc disc;
  auto eof = xaSector();
  eof.raw[18] |= 0x80;
  disc.sectors = {xaSector(), xaSector(1, 2), eof, xaSector(1, 2)};
  beginXa(cdrom, disc);
  unsigned audio = 0;
  cdrom.setXaCallback([&](auto, auto) { ++audio; });
  cdrom.accrueClockCycles(cdrom.getCyclesPerSector() * 4);
  cdrom.serviceClock();
  EXPECT_EQ(audio, 3u);
  EXPECT_EQ(cdrom.snapshot().xaCurrentFile, 2u);
  EXPECT_TRUE(cdrom.snapshot().xaCurrentSet);
}

TEST(XaDecoder, MutedInputUpdatesPredictionWithoutAdvancingInterpolation) {
  XaAdpcm a, b;
  const auto sector = xaSector();
  a.decode(sector.raw);
  b.decode(sector.raw);
  const auto before = a.snapshot();
  EXPECT_TRUE(a.decode(sector.raw, false).empty());
  b.decode(sector.raw);
  EXPECT_EQ(a.snapshot().history, b.snapshot().history);
  EXPECT_EQ(a.snapshot().ring, before.ring);
  EXPECT_EQ(a.snapshot().position, before.position);
  EXPECT_EQ(a.snapshot().sixstep, before.sixstep);
}
