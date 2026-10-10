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

  void replaceDirectory(const std::vector<uint8_t> &data) {
    std::fstream file(testBinPath, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(17 * ps1::cdrom::SECTOR_SIZE_RAW + 24);
    file.write(reinterpret_cast<const char *>(data.data()), data.size());
  }
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

TEST_F(Iso9660Test, DirectoryLengthIsAnExactBoundaryNotSectorPadding) {
  ps1::cdrom::BinReader reader;
  ASSERT_TRUE(reader.open(testBinPath));
  ps1::cdrom::Iso9660Parser parser(reader);
  ASSERT_TRUE(parser.initialize());
  EXPECT_TRUE(parser.readDirectory(17, 43).empty()); // first record is truncated
  const auto entries = parser.readDirectory(17, 44);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].name, "SYSTEM.CNF");
  EXPECT_TRUE(parser.readDirectory(19, 4096).empty()); // extent exceeds image
  EXPECT_TRUE(parser.readDirectory(0xffffffff, 4096).empty());
}

TEST_F(Iso9660Test, RejectsEveryShortRecordAndNameCrossingRecordBoundary) {
  for (unsigned length = 1; length <= 33; ++length) {
    SCOPED_TRACE(length);
    std::vector<uint8_t> sector(2048);
    sector[0] = length;
    replaceDirectory(sector);
    ps1::cdrom::BinReader reader;
    ASSERT_TRUE(reader.open(testBinPath));
    ps1::cdrom::Iso9660Parser parser(reader);
    EXPECT_TRUE(parser.readDirectory(17, 2048).empty());
  }
  for (unsigned nameLength : {0u, 2u, 32u, 255u}) {
    std::vector<uint8_t> sector(2048);
    sector[0] = 34;
    sector[32] = nameLength; // only one byte is available inside this record
    replaceDirectory(sector);
    ps1::cdrom::BinReader reader;
    ASSERT_TRUE(reader.open(testBinPath));
    ps1::cdrom::Iso9660Parser parser(reader);
    EXPECT_TRUE(parser.readDirectory(17, 2048).empty());
  }
}

TEST_F(Iso9660Test, ReinitializationFailureClearsPreviousRoot) {
  ps1::cdrom::BinReader reader;
  ASSERT_TRUE(reader.open(testBinPath));
  ps1::cdrom::Iso9660Parser parser(reader);
  ASSERT_TRUE(parser.initialize());
  ASSERT_FALSE(reader.open(testBinPath.string() + ".missing"));
  EXPECT_FALSE(parser.initialize());
  EXPECT_EQ(parser.getRootLba(), 0);
  EXPECT_EQ(parser.getRootLength(), 0);
}
