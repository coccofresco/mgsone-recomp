#include "runtime/bios/file_io.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/input/input.h"
#include "runtime/memory.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

class PersistentCardTest : public ::testing::Test {
protected:
  std::filesystem::path directory;
  void SetUp() override {
    directory =
        std::filesystem::temp_directory_path() /
        ("mgs-card-test-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
  }
  void TearDown() override { std::filesystem::remove_all(directory); }
};

TEST_F(PersistentCardTest,
       BiosFileWriteSurvivesReopenAndCrossesAllocationBlocks) {
  ps1::Memory memory;
  ps1::cdrom::VirtualFs fs;
  const auto path = (directory / "card.mcd").string();
  {
    ps1::input::InputController input;
    ASSERT_TRUE(input.getMemoryCard(0).attachFile(path));
    ps1::bios::FileIO files(fs, memory);
    files.setInputController(&input);
    const int fd = files.open("bu00:BASLUS-00594TEST", (2 << 16) | 0x202);
    ASSERT_GE(fd, 3);
    ASSERT_EQ(files.lseek(fd, 8180, 0), 8180);
    for (unsigned i = 0; i < 256; ++i)
      memory.write8(0x1000 + i, i ^ 0xa5);
    EXPECT_EQ(files.write(fd, 0x1000, 256), 256);
    EXPECT_EQ(files.close(fd), fd);
  }
  {
    ps1::input::InputController input;
    ASSERT_TRUE(input.getMemoryCard(0).attachFile(path));
    ps1::bios::FileIO files(fs, memory);
    files.setInputController(&input);
    const int fd = files.open("bu00:BASLUS-00594TEST", 1);
    ASSERT_GE(fd, 3);
    files.lseek(fd, 8180, 0);
    ASSERT_EQ(files.read(fd, 0x2000, 256), 256);
    for (unsigned i = 0; i < 256; ++i)
      EXPECT_EQ(memory.read8(0x2000 + i), uint8_t(i ^ 0xa5));
    EXPECT_EQ(files.firstFile("bu00:BASLUS*", 0x3000), 0x3000u);
    EXPECT_EQ(memory.read32(0x3018), 16384u);
    EXPECT_EQ(files.nextFile(0x3000), 0u);
    EXPECT_TRUE(files.erase("bu00:BASLUS-00594TEST"));
  }
  ps1::input::MemoryCard reopened;
  ASSERT_TRUE(reopened.attachFile(path));
  EXPECT_EQ(reopened.findFile("BASLUS-00594TEST"), 0);
}

TEST_F(PersistentCardTest,
       MalformedImagesAreNotOverwrittenAndChainsCannotCycle) {
  const auto path = (directory / "short.mcd").string();
  {
    std::ofstream file(path);
    file << "do not replace";
  }
  ps1::input::MemoryCard card;
  EXPECT_FALSE(card.attachFile(path));
  EXPECT_EQ(std::filesystem::file_size(path), 14u);
  ASSERT_EQ(card.createFile("valid", 1), 1);
  auto snapshot = card.snapshot();
  snapshot.data[128 + 8] = 0;
  snapshot.data[128 + 9] = 0;
  card.restore(snapshot);
  uint8_t out = 0;
  EXPECT_EQ(card.readFile(1, 0, &out, 1), -1);
}

TEST_F(PersistentCardTest,
       FormattedDirectoryChecksumsAndFullCardAreConsistent) {
  ps1::input::MemoryCard card;
  const auto state = card.snapshot();
  for (unsigned frame = 0; frame <= 35; ++frame) {
    uint8_t check = 0;
    for (unsigned byte = 0; byte < 128; ++byte)
      check ^= state.data[frame * 128 + byte];
    EXPECT_EQ(check, 0);
  }
  EXPECT_EQ(card.createFile("full", 15), 1);
  EXPECT_EQ(card.createFile("overflow", 1), 0);
  EXPECT_TRUE(card.eraseFile("full"));
  EXPECT_EQ(card.createFile("reused", 15), 1);
}
