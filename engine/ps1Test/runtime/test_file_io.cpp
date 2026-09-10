#include "runtime/bios/file_io.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/memory.h"
#include <gtest/gtest.h>
#include <vector>

using namespace ps1::bios;
using namespace ps1::cdrom;
using namespace ps1;

class MockVirtualFs : public VirtualFs {
public:
  std::optional<std::vector<uint8_t>>
  readFile(const std::string &path) override {
    if (path == "SYSTEM.CNF") {
      return std::vector<uint8_t>{'B', 'O', 'O', 'T', ' ', '=',  ' ', 'c',
                                  'd', 'r', 'o', 'm', ':', '\\', 'B', 'O',
                                  'O', 'T', '.', 'E', 'X', 'E',  ';', '1'};
    }
    return std::nullopt;
  }
};

class FileIOTest : public ::testing::Test {
protected:
  void SetUp() override {
    mem = std::make_unique<Memory>();
    fs = std::make_unique<MockVirtualFs>();
    file_io = std::make_unique<FileIO>(*fs, *mem);
  }

  std::unique_ptr<Memory> mem;
  std::unique_ptr<MockVirtualFs> fs;
  std::unique_ptr<FileIO> file_io;
};

TEST_F(FileIOTest, OpenFileReturnsValidFd) {
  int32_t fd = file_io->open("cdrom:\\SYSTEM.CNF;1", 0);
  EXPECT_EQ(fd, 0); // Should be first FD

  int32_t fd2 = file_io->open("INVALID.CNF", 0);
  EXPECT_EQ(fd2, -1);
}

TEST_F(FileIOTest, ReadOutputsIntoMemory) {
  int32_t fd = file_io->open("SYSTEM.CNF", 0);

  int32_t bytesRead = file_io->read(fd, 0x80000000, 4);
  EXPECT_EQ(bytesRead, 4);

  EXPECT_EQ(mem->read8(0x80000000), 'B');
  EXPECT_EQ(mem->read8(0x80000001), 'O');
  EXPECT_EQ(mem->read8(0x80000002), 'O');
  EXPECT_EQ(mem->read8(0x80000003), 'T');

  // Read past EOF
  bytesRead = file_io->read(fd, 0x80000004, 100);
  EXPECT_EQ(bytesRead, 20); // 24 total - 4 = 20 remaining

  bytesRead = file_io->read(fd, 0x80001000, 10);
  EXPECT_EQ(bytesRead, 0); // EOF
}

TEST_F(FileIOTest, LseekChangesCursorPosition) {
  int32_t fd = file_io->open("SYSTEM.CNF", 0);

  // Seek to SET 2
  int32_t pos = file_io->lseek(fd, 2, 0);
  EXPECT_EQ(pos, 2);

  int32_t bytesRead = file_io->read(fd, 0x80000000, 2);
  EXPECT_EQ(bytesRead, 2);
  EXPECT_EQ(mem->read8(0x80000000), 'O'); // index 2
  EXPECT_EQ(mem->read8(0x80000001), 'T'); // index 3

  // Seek CUR -1
  pos = file_io->lseek(fd, -1, 1);
  EXPECT_EQ(pos, 3);

  bytesRead = file_io->read(fd, 0x80000000, 1);
  EXPECT_EQ(mem->read8(0x80000000), 'T'); // read index 3 again

  // Seek END -2
  pos = file_io->lseek(fd, -2, 2);
  EXPECT_EQ(pos, 22); // 24 - 2
}

TEST_F(FileIOTest, CloseFreesFd) {
  int32_t fd = file_io->open("SYSTEM.CNF", 0);

  EXPECT_EQ(file_io->close(fd), fd);

  // Next open should reuse FD 0
  int32_t fd2 = file_io->open("SYSTEM.CNF", 0);
  EXPECT_EQ(fd2, fd);
}
