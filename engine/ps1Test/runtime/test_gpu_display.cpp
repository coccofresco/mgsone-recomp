#include "runtime/gpu/gpu.h"
#include "runtime/metrics.h"
#include <gtest/gtest.h>
#include <vector>

using namespace ps1::gpu;

namespace {

class ScopedTestMetrics {
public:
  ScopedTestMetrics() {
    ps1::metrics::reset();
    ps1::metrics::setEnabledForTesting(true);
  }

  ~ScopedTestMetrics() {
    ps1::metrics::reset();
    ps1::metrics::setEnabledForTesting(false);
  }
};

} // namespace

class GpuDisplayTest : public ::testing::Test {
protected:
  GPU gpu;

  void SetUp() override {
    gpu.writeGP1(0x00000000); // Reset GPU
  }
};

TEST_F(GpuDisplayTest, DisplayEnableTogglesBit23) {
  // Initial reset state for bit 23 is 1 (Display Off)
  uint32_t stat = gpu.readGPUSTAT();
  EXPECT_NE(stat & (1 << 23), 0);

  // Turn Display On (val=0) means bit 23 goes 0.
  gpu.writeGP1(0x03000000);
  stat = gpu.readGPUSTAT();
  EXPECT_EQ(stat & (1 << 23), 0);

  // Turn Display Off (val=1) means bit 23 goes 1.
  gpu.writeGP1(0x03000001);
  stat = gpu.readGPUSTAT();
  EXPECT_NE(stat & (1 << 23), 0);
}

TEST_F(GpuDisplayTest, DisplayVRAMStartOffset) {
  gpu.writeGP1(0x05000000 | (123) | (456 << 10));

  uint32_t xStart, yStart;
  gpu.getDisplayArea(xStart, yStart);

  EXPECT_EQ(xStart, 123);
  EXPECT_EQ(yStart, 456);
}

TEST_F(GpuDisplayTest, CopiesStableVBlankSnapshot) {
  constexpr std::size_t pixelCount = GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT;
  std::vector<uint16_t> source(pixelCount, 0);
  source.front() = 0x1234;
  source.back() = 0x7ABC;
  gpu.loadVram(reinterpret_cast<const uint8_t *>(source.data()));
  gpu.snapshotDisplayBuffer();

  std::vector<Color16> copy(pixelCount);
  EXPECT_EQ(gpu.copyDisplayVRAM(copy.data(), copy.size()), pixelCount);
  EXPECT_EQ(copy.front().raw, 0x1234);
  EXPECT_EQ(copy.back().raw, 0x7ABC);
  EXPECT_EQ(gpu.copyDisplayVRAM(nullptr, copy.size()), 0u);
}

TEST_F(GpuDisplayTest, NativeVectorCopyReusesKnownContentRevision) {
  constexpr std::size_t pixelCount = GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT;
  std::vector<uint16_t> source(pixelCount, 0);
  source.front() = 0x1234;
  source.back() = 0x7ABC;
  gpu.loadVram(reinterpret_cast<const uint8_t *>(source.data()));
  gpu.snapshotDisplayBuffer(10u);

  std::vector<Color16> copy;
  GPU::DisplayState state;
  EXPECT_EQ(gpu.copyDisplayVRAMIfChanged(copy, UINT64_MAX, &state),
            GPU::DisplayCopyStatus::Copied);
  ASSERT_EQ(copy.size(), pixelCount);
  EXPECT_EQ(copy.front().raw, 0x1234);
  EXPECT_EQ(copy.back().raw, 0x7ABC);
  const uint64_t revision = state.contentRevision;

  // A VBlank advances temporal state without rematerializing identical
  // pixels. The backend must see the new latch while retaining its upload
  // buffer byte-for-byte.
  copy.front().raw = 0x55AA;
  gpu.snapshotDisplayBuffer(11u);
  EXPECT_EQ(gpu.copyDisplayVRAMIfChanged(copy, revision, &state),
            GPU::DisplayCopyStatus::Unchanged);
  EXPECT_EQ(copy.front().raw, 0x55AA);
  EXPECT_EQ(state.latchFrame, 11u);
  EXPECT_EQ(state.contentRevision, revision);

  gpu.writeGP1(0x05000000u | 512u);
  gpu.snapshotDisplayBuffer(12u);
  EXPECT_EQ(gpu.copyDisplayVRAMIfChanged(copy, revision, &state),
            GPU::DisplayCopyStatus::Copied);
  EXPECT_EQ(copy.front().raw, 0x1234);
  EXPECT_NE(state.contentRevision, revision);
  EXPECT_EQ(state.xStart, 512u);
}

TEST_F(GpuDisplayTest, DisplayOriginIsLatchedWithTheVramSnapshot) {
  ps1::metrics::setTraceFrame(37);
  gpu.writeGP1(0x05000000 | 512u);
  gpu.writeGP1(0x08000000 | 1u); // 320x240
  gpu.snapshotDisplayBuffer();

  // Simulate the game waking from VSync and selecting its other buffer before
  // the host renderer uploads the frame that was just latched.
  gpu.writeGP1(0x05000000);

  std::vector<Color16> copy(GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT);
  GPU::DisplayState state;
  gpu.copyDisplayVRAM(copy.data(), copy.size(), &state);
  EXPECT_EQ(state.xStart, 512u);
  EXPECT_EQ(state.yStart, 0u);
  EXPECT_TRUE(state.modeSet);
  EXPECT_FALSE(state.horizontalRangeSet);
  EXPECT_FALSE(state.verticalRangeSet);
  EXPECT_EQ(state.latchFrame, 37u);

  uint32_t liveX = 0, liveY = 0;
  gpu.getDisplayArea(liveX, liveY);
  EXPECT_EQ(liveX, 0u);
}

TEST_F(GpuDisplayTest, UnchangedSnapshotsReuseLatchUntilGp1Mutation) {
  ScopedTestMetrics metrics;

  gpu.snapshotDisplayBuffer(40u);
  EXPECT_EQ(ps1::metrics::get("gpu.frame.snapshot_reused"), 0u)
      << "the first latch must materialize a snapshot";

  gpu.snapshotDisplayBuffer(41u);
  EXPECT_EQ(ps1::metrics::get("gpu.frame.snapshot_reused"), 1u)
      << "an unchanged consecutive latch should reuse the snapshot";

  gpu.writeGP1(0x05000000u | 512u);
  gpu.snapshotDisplayBuffer(42u);
  EXPECT_EQ(ps1::metrics::get("gpu.frame.snapshot_reused"), 1u)
      << "a GP1 display mutation must force a fresh snapshot";

  gpu.snapshotDisplayBuffer(43u);
  EXPECT_EQ(ps1::metrics::get("gpu.frame.snapshot_reused"), 2u)
      << "the fresh post-GP1 snapshot should become reusable";
}

TEST_F(GpuDisplayTest, DisplayRangeHorizontal) {
  gpu.writeGP1(0x06000000 | 500 | (3000 << 12));

  uint32_t x1, x2, y1, y2;
  gpu.getDisplayRange(x1, x2, y1, y2);

  EXPECT_EQ(x1, 500);
  EXPECT_EQ(x2, 3000);

  gpu.snapshotDisplayBuffer();
  std::vector<Color16> copy(GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT);
  GPU::DisplayState state;
  gpu.copyDisplayVRAM(copy.data(), copy.size(), &state);
  EXPECT_TRUE(state.horizontalRangeSet);
  EXPECT_EQ(state.displayX1, 500u);
  EXPECT_EQ(state.displayX2, 3000u);
}

TEST_F(GpuDisplayTest, DisplayRangeVertical) {
  gpu.writeGP1(0x07000000 | 20 | (260 << 10));

  uint32_t x1, x2, y1, y2;
  gpu.getDisplayRange(x1, x2, y1, y2);

  EXPECT_EQ(y1, 20);
  EXPECT_EQ(y2, 260);

  gpu.snapshotDisplayBuffer();
  std::vector<Color16> copy(GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT);
  GPU::DisplayState state;
  gpu.copyDisplayVRAM(copy.data(), copy.size(), &state);
  EXPECT_TRUE(state.verticalRangeSet);
  EXPECT_EQ(state.displayY1, 20u);
  EXPECT_EQ(state.displayY2, 260u);
}

TEST_F(GpuDisplayTest, DisplayModeSetsGpuStatCorrectly) {
  // 0x08 - Display Mode
  // Bit 0: Hres1=1
  // Bit 1: Hres2=0 (together 1 means 320)
  // Bit 2: Vres=1 (480)
  // Bit 3: Video Mode=1 (PAL)
  // Bit 4: Depth=1 (24-bit)
  // Bit 5: Interlace=1
  uint32_t mode = (1) | (0 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5);
  gpu.writeGP1(0x08000000 | mode);

  uint32_t stat = gpu.readGPUSTAT();

  // GP1(08) bit 0-5 map directly to GPUSTAT bits 17-22
  EXPECT_NE(stat & (1 << 17), 0);
  EXPECT_EQ(stat & (1 << 18), 0);
  EXPECT_NE(stat & (1 << 19), 0); // Vres
  EXPECT_NE(stat & (1 << 20), 0); // Video Mode
  EXPECT_NE(stat & (1 << 21), 0); // Depth
  EXPECT_NE(stat & (1 << 22), 0); // Interlace
}

TEST_F(GpuDisplayTest, PalModeDoesNotSelect480LinesOrDisableDisplay) {
  gpu.writeGP1(0x03000000);        // display enabled
  gpu.writeGP1(0x08000000 | 0x09); // 320 px + PAL, non-interlaced 240
  const uint32_t stat = gpu.readGPUSTAT();
  EXPECT_EQ(stat & (1u << 19), 0u); // VRes remains 240
  EXPECT_NE(stat & (1u << 20), 0u); // PAL bit
  EXPECT_EQ(stat & (1u << 23), 0u); // display remains enabled
}

TEST_F(GpuDisplayTest, Hres2MapsToGpuStatBit16) {
  gpu.writeGP1(0x03000000);
  gpu.writeGP1(0x08000000 | 0x40); // 368-pixel HRes2 mode
  const uint32_t stat = gpu.readGPUSTAT();
  EXPECT_NE(stat & (1u << 16), 0u);
  EXPECT_EQ(stat & (1u << 23), 0u);
}
