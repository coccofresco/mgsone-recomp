#include "runtime/cdrom/bin_reader.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

class BinReaderTest : public ::testing::Test {
protected:
  void SetUp() override {
    testBinPath = fs::temp_directory_path() / "test_dummy.bin";
    std::ofstream off(testBinPath, std::ios::binary);

    // Create 2 dummy sectors (4704 bytes)
    std::vector<uint8_t> dummyData(ps1::cdrom::SECTOR_SIZE_RAW * 2, 0);

    // Fill Sector 0 payload with 0xAA
    std::fill(dummyData.begin() + 24, dummyData.begin() + 24 + 2048, 0xAA);
    // Fill Sector 1 payload with 0xBB
    std::fill(dummyData.begin() + ps1::cdrom::SECTOR_SIZE_RAW + 24,
              dummyData.begin() + ps1::cdrom::SECTOR_SIZE_RAW + 24 + 2048,
              0xBB);

    off.write(reinterpret_cast<const char *>(dummyData.data()),
              dummyData.size());
  }

  void TearDown() override {
    if (fs::exists(testBinPath)) {
      fs::remove(testBinPath);
    }
  }

  fs::path testBinPath;
};

TEST_F(BinReaderTest, OpenAndReadTotalSectors) {
  ps1::cdrom::BinReader reader;
  EXPECT_TRUE(reader.open(testBinPath));
  EXPECT_EQ(reader.getTotalSectors(), 2);
}

TEST_F(BinReaderTest, ReadValidSectors) {
  ps1::cdrom::BinReader reader;
  EXPECT_TRUE(reader.open(testBinPath));

  auto s0 = reader.readSector(0);
  ASSERT_TRUE(s0.has_value());
  EXPECT_EQ(s0->getDataMode2Form1()[0], 0xAA);

  auto s1 = reader.readSector(1);
  ASSERT_TRUE(s1.has_value());
  EXPECT_EQ(s1->getDataMode2Form1()[0], 0xBB);
}

TEST_F(BinReaderTest, ReadInvalidSector) {
  ps1::cdrom::BinReader reader;
  EXPECT_TRUE(reader.open(testBinPath));

  auto s2 = reader.readSector(2); // Out of bounds
  EXPECT_FALSE(s2.has_value());
}

TEST_F(BinReaderTest, ConcurrentReadsKeepSectorBoundaries) {
  ps1::cdrom::BinReader reader;
  ASSERT_TRUE(reader.open(testBinPath));

  std::atomic<bool> valid{true};
  std::vector<std::thread> readers;
  for (uint32_t threadIndex = 0; threadIndex < 8; ++threadIndex) {
    readers.emplace_back([&, threadIndex] {
      for (uint32_t iteration = 0; iteration < 1000; ++iteration) {
        const uint32_t sectorIndex = (threadIndex + iteration) & 1U;
        const uint8_t expected = sectorIndex == 0 ? 0xAA : 0xBB;
        const auto sector = reader.readSector(sectorIndex);
        if (!sector || sector->getDataMode2Form1()[0] != expected ||
            sector->getDataMode2Form1().back() != expected) {
          valid.store(false, std::memory_order_relaxed);
          return;
        }
      }
    });
  }

  for (auto &readerThread : readers) {
    readerThread.join();
  }

  EXPECT_TRUE(valid.load(std::memory_order_relaxed));
}
