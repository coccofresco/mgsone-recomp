#include "runtime/cdrom/virtual_fs.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace fs = std::filesystem;

class VirtualFsTest : public ::testing::Test {
protected:
  void SetUp() override {
    testBinPath = fs::temp_directory_path() / "test_vfs.bin";
    std::ofstream off(testBinPath, std::ios::binary);

    std::vector<uint8_t> dummyData(ps1::cdrom::SECTOR_SIZE_RAW * 20, 0);

    // Standard ISO9660 PVD
    size_t pvdOffset = 16 * ps1::cdrom::SECTOR_SIZE_RAW + 24;
    dummyData[pvdOffset + 0] = 1;
    std::memcpy(&dummyData[pvdOffset + 1], "CD001", 5);

    uint32_t rootLba = 17;
    uint32_t rootLen = 2048;
    std::memcpy(&dummyData[pvdOffset + 156 + 2], &rootLba, 4);
    std::memcpy(&dummyData[pvdOffset + 156 + 10], &rootLen, 4);

    // Setup Root Directory at sector 17
    size_t rootOffset = 17 * ps1::cdrom::SECTOR_SIZE_RAW + 24;

    // SYSTEM.CNF record
    std::string sysCnfBody = "BOOT = cdrom:\\SLUS_123.45;1\r\nTCB = 4\r\n";
    uint8_t rec1_len = 34 + 10;
    dummyData[rootOffset + 0] = rec1_len;
    uint32_t sysCnfLba = 18;
    uint32_t sysCnfLen = sysCnfBody.length();
    std::memcpy(&dummyData[rootOffset + 2], &sysCnfLba, 4);
    std::memcpy(&dummyData[rootOffset + 10], &sysCnfLen, 4);
    dummyData[rootOffset + 25] = 0x00;
    dummyData[rootOffset + 32] = 10;
    std::memcpy(&dummyData[rootOffset + 33], "SYSTEM.CNF", 10);

    // SLUS_123.45 record
    size_t rootOffset2 = rootOffset + rec1_len;
    uint8_t rec2_len = 34 + 11;
    dummyData[rootOffset2 + 0] = rec2_len;
    uint32_t slusLba = 19;
    uint32_t slusLen = 8; // dummy exe length
    std::memcpy(&dummyData[rootOffset2 + 2], &slusLba, 4);
    std::memcpy(&dummyData[rootOffset2 + 10], &slusLen, 4);
    dummyData[rootOffset2 + 25] = 0x00;
    dummyData[rootOffset2 + 32] = 11;
    std::memcpy(&dummyData[rootOffset2 + 33], "SLUS_123.45", 11);

    // Write SYSTEM.CNF body at sector 18
    size_t sysCnfOffset = 18 * ps1::cdrom::SECTOR_SIZE_RAW + 24;
    std::memcpy(&dummyData[sysCnfOffset], sysCnfBody.data(),
                sysCnfBody.length());

    // Write SLUS_123.45 body at sector 19
    size_t slusOffset = 19 * ps1::cdrom::SECTOR_SIZE_RAW + 24;
    std::memcpy(&dummyData[slusOffset], "PS-X EXE", 8);

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

TEST_F(VirtualFsTest, LoadAndReadInfo) {
  ps1::cdrom::VirtualFs vfs;
  EXPECT_TRUE(vfs.loadDisc(testBinPath));

  auto bootPath = vfs.getBootPath();
  ASSERT_TRUE(bootPath.has_value());
  EXPECT_EQ(bootPath.value(), "SLUS_123.45");

  auto exeData = vfs.readFile(bootPath.value());
  ASSERT_TRUE(exeData.has_value());
  EXPECT_EQ(exeData->size(), 8);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(exeData->data()), 8),
            "PS-X EXE");
}
