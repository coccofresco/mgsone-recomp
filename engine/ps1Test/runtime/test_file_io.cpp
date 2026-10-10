#include "runtime/bios/file_io.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/memory.h"
#include "runtime/input/input.h"
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
  EXPECT_EQ(fd, 3); // BIOS descriptors 0-2 are reserved for standard streams.

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

  // Next open should reuse the released file descriptor.
  int32_t fd2 = file_io->open("SYSTEM.CNF", 0);
  EXPECT_EQ(fd2, fd);
}

TEST_F(FileIOTest, RestoreRebindsCardWritesAcrossSectorBlockAndEndBoundaries) {
  input::InputController original;
  input::InputController replacement;
  file_io->setInputController(&original);
  for (unsigned slot = 0; slot < 2; ++slot) {
    ASSERT_EQ(original.getMemoryCard(slot).createFile("CHAIN", 2), 1);
    replacement.getMemoryCard(slot).restore(original.getMemoryCard(slot).snapshot());
  }
  const int handles[] = {file_io->open("bu00:CHAIN", 0), file_io->open("bu10:CHAIN", 0)};
  for (uint32_t offset : {0u, 127u, 128u, 8191u, 8192u, 16383u, 16384u}) {
    SCOPED_TRACE(offset);
    for (int fd : handles) ASSERT_EQ(file_io->lseek(fd, offset, 0), offset);
    const auto saved = file_io->snapshot();
    FileIO resumed(*fs, *mem);
    resumed.setInputController(&replacement);
    ASSERT_TRUE(resumed.restore(saved));
    for (unsigned slot = 0; slot < 2; ++slot) {
      SCOPED_TRACE(slot);
      const uint8_t value = 0x80 + slot;
      mem->write8(0x5000, value);
      const auto expected = offset < 16384 ? 1 : 0;
      EXPECT_EQ(resumed.write(handles[slot], 0x5000, 1), expected);
      EXPECT_EQ(resumed.lseek(handles[slot], 0, 1), offset + expected);
      if (expected) {
        uint8_t oldByte = 0, restoredByte = 0;
        ASSERT_EQ(original.getMemoryCard(slot).readFile(1, offset, &oldByte, 1), 1);
        ASSERT_EQ(replacement.getMemoryCard(slot).readFile(1, offset, &restoredByte, 1), 1);
        EXPECT_EQ(oldByte, 0);
        EXPECT_EQ(restoredByte, value);
      }
    }
  }
}

TEST_F(FileIOTest, RestorePreservesFullDescriptorTableAndReleasedSlots) {
  for (int fd = 3; fd < 16; ++fd) {
    ASSERT_EQ(file_io->open("SYSTEM.CNF", 0), fd);
    ASSERT_EQ(file_io->lseek(fd, fd, 0), fd);
  }
  ASSERT_EQ(file_io->open("SYSTEM.CNF", 0), -1);
  ASSERT_EQ(file_io->close(8), 8);
  const auto saved = file_io->snapshot();
  FileIO resumed(*fs, *mem);
  ASSERT_TRUE(resumed.restore(saved));
  for (int fd = 3; fd < 16; ++fd)
    EXPECT_EQ(resumed.lseek(fd, 0, 1), fd == 8 ? -1 : fd);
  EXPECT_EQ(resumed.open("SYSTEM.CNF", 0), 8);
  EXPECT_EQ(resumed.open("SYSTEM.CNF", 0), -1);
}

TEST_F(FileIOTest, InvalidCardResourceDoesNotReplaceExistingDescriptors) {
  input::InputController input;
  file_io->setInputController(&input);
  ASSERT_EQ(input.getMemoryCard(0).createFile("FIRST", 1), 1);
  ASSERT_EQ(file_io->open("bu00:FIRST", 0), 3);
  ASSERT_EQ(file_io->lseek(3, 42, 0), 42);
  const auto valid = file_io->snapshot();
  for (unsigned mutation = 0; mutation < 6; ++mutation) {
    SCOPED_TRACE(mutation);
    auto bad = valid;
    auto &fd = bad.descriptors[3];
    if (mutation == 0) fd.cardSlot = 2;
    if (mutation == 1) fd.firstBlock = 0;
    if (mutation == 2) fd.firstBlock = 16;
    if (mutation == 3) fd.path = "bu10:FIRST";
    if (mutation == 4) fd.cursor = 15 * 8192 + 1;
    if (mutation == 5) fd.data.push_back(1);
    EXPECT_FALSE(file_io->restore(bad));
    EXPECT_EQ(file_io->lseek(3, 0, 1), 42);
  }
}
