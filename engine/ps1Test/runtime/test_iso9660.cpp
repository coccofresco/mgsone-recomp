#include "runtime/cdrom/bin_reader.h"
#include "runtime/cdrom/iso9660.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <vector>

namespace fs = std::filesystem;

class Iso9660Test : public ::testing::Test {
protected:
  void SetUp() override {
    testBinPath = fs::temp_directory_path() / "test_iso.bin";
    std::ofstream off(testBinPath, std::ios::binary);

    // We prepare a dummy file 20 sectors long
    std::vector<uint8_t> dummyData(ps1::cdrom::SECTOR_SIZE_RAW * 20, 0);

    // Write "CD001" at sector 16 payload
    size_t pvdOffset = 16 * ps1::cdrom::SECTOR_SIZE_RAW + 24;
    dummyData[pvdOffset + 0] = 1; // Type = PVD
    std::memcpy(&dummyData[pvdOffset + 1], "CD001", 5);

    // Write Root Directory Record at PVD offset 156
    uint32_t rootLba = 17;
    uint32_t rootLen = 2048;
    std::memcpy(&dummyData[pvdOffset + 156 + 2], &rootLba, 4);  // LBA (LSB)
    std::memcpy(&dummyData[pvdOffset + 156 + 10], &rootLen, 4); // Length (LSB)

    // Setup Root Directory at sector 17
    size_t rootOffset = 17 * ps1::cdrom::SECTOR_SIZE_RAW + 24;

    // Record 1: SYSTEM.CNF
    uint8_t rec1_len = 34 + 10;
    dummyData[rootOffset + 0] = rec1_len;
    uint32_t sysCnfLba = 18;
    uint32_t sysCnfLen = 123;
    std::memcpy(&dummyData[rootOffset + 2], &sysCnfLba, 4);
    std::memcpy(&dummyData[rootOffset + 10], &sysCnfLen, 4);
    dummyData[rootOffset + 25] = 0x00; // File
    dummyData[rootOffset + 32] = 10;   // Name len
    std::memcpy(&dummyData[rootOffset + 33], "SYSTEM.CNF", 10);

    // Next record offset
    size_t rootOffset2 = rootOffset + rec1_len;

    // Record 2: DIRECTORY
    uint8_t rec2_len = 34 + 3;
    dummyData[rootOffset2 + 0] = rec2_len;
    uint32_t dirLba = 19;
    uint32_t dirLen = 2048;
    std::memcpy(&dummyData[rootOffset2 + 2], &dirLba, 4);
    std::memcpy(&dummyData[rootOffset2 + 10], &dirLen, 4);
    dummyData[rootOffset2 + 25] = 0x02; // Directory
    dummyData[rootOffset2 + 32] = 3;    // Name len
    std::memcpy(&dummyData[rootOffset2 + 33], "DIR", 3);

    // EOF record marker
    dummyData[rootOffset2 + rec2_len] = 0;

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

TEST_F(Iso9660Test, InitializeAndReadPVD) {
  ps1::cdrom::BinReader reader;
  EXPECT_TRUE(reader.open(testBinPath));

  ps1::cdrom::Iso9660Parser parser(reader);
  EXPECT_TRUE(parser.initialize());

  EXPECT_EQ(parser.getRootLba(), 17);
  EXPECT_EQ(parser.getRootLength(), 2048);
}

TEST_F(Iso9660Test, FindFileRoot) {
  ps1::cdrom::BinReader reader;
  EXPECT_TRUE(reader.open(testBinPath));

  ps1::cdrom::Iso9660Parser parser(reader);
  EXPECT_TRUE(parser.initialize());

  auto file = parser.findFile("SYSTEM.CNF");
  ASSERT_TRUE(file.has_value());
  EXPECT_EQ(file->name, "SYSTEM.CNF");
  EXPECT_EQ(file->extent_lba, 18);
  EXPECT_EQ(file->data_length, 123);
  EXPECT_FALSE(file->is_directory);
}

TEST_F(Iso9660Test, FindDirectory) {
  ps1::cdrom::BinReader reader;
  EXPECT_TRUE(reader.open(testBinPath));

  ps1::cdrom::Iso9660Parser parser(reader);
  EXPECT_TRUE(parser.initialize());

  auto file = parser.findFile("DIR");
  ASSERT_TRUE(file.has_value());
  EXPECT_EQ(file->name, "DIR");
  EXPECT_EQ(file->extent_lba, 19);
  EXPECT_EQ(file->data_length, 2048);
  EXPECT_TRUE(file->is_directory);
}
