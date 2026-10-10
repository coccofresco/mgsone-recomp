#include "runtime/bios/file_io.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/input/input.h"
#include "runtime/memory.h"
#include "runtime/save_files.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <numeric>
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

TEST_F(PersistentCardTest, SessionHistorySurvivesManyWritesAndFreshInstances) {
  using Card = ps1::input::MemoryCard;
  const auto path = (directory / "session.mcd").string();
  { Card card; ASSERT_TRUE(card.attachFile(path)); ASSERT_EQ(card.createFile("progress", 1), 1); }
  for (uint8_t session = 1; session <= 5; ++session) {
    Card card; ASSERT_TRUE(card.attachFile(path));
    for (uint8_t write = 1; write <= 20; ++write) ASSERT_EQ(card.writeFile(1, 0, &session, 1), 1);
    ASSERT_TRUE(card.flush()); // redundant BIOS close does not rotate history
  }
  const auto backups = ps1::storage::backups(path);
  ASSERT_EQ(backups.size(), 3u);
  for (unsigned i = 0; i < backups.size(); ++i) {
    Card copy; ASSERT_TRUE(copy.loadFromFile(backups[i].string()));
    uint8_t marker = 0; ASSERT_EQ(copy.readFile(1, 0, &marker, 1), 1);
    EXPECT_EQ(marker, 4-i);
  }
}

TEST_F(PersistentCardTest, TruncatedOrMissingCardRecoversPreviousSessionWithoutOverwritingIt) {
  using Card = ps1::input::MemoryCard;
  const auto path = (directory / "recovery.mcd").string();
  { Card card; ASSERT_TRUE(card.attachFile(path)); ASSERT_EQ(card.createFile("progress", 1), 1); }
  { Card card; ASSERT_TRUE(card.attachFile(path)); uint8_t value = 17; ASSERT_EQ(card.writeFile(1, 0, &value, 1), 1); }
  std::ofstream(path, std::ios::trunc) << "damaged";
  Card recovered; ASSERT_TRUE(recovered.attachFile(path));
  EXPECT_FALSE(recovered.recoverySource().empty()); EXPECT_EQ(std::filesystem::file_size(path), 7u);
  uint8_t value = 99; ASSERT_EQ(recovered.readFile(1, 0, &value, 1), 1); EXPECT_EQ(value, 0);
  std::filesystem::remove(path);
  Card missing; ASSERT_TRUE(missing.attachFile(path)); EXPECT_FALSE(missing.recoverySource().empty());
  EXPECT_FALSE(std::filesystem::exists(path));
  value = 21; ASSERT_EQ(missing.writeFile(1, 0, &value, 1), 1);
  Card final; ASSERT_TRUE(final.attachFile(path)); EXPECT_TRUE(final.recoverySource().empty());
  ASSERT_EQ(final.readFile(1, 0, &value, 1), 1); EXPECT_EQ(value, 21);
}

TEST_F(PersistentCardTest, FailedBackupRollsBackCardRamAndLeavesDiskReadable) {
  using Card = ps1::input::MemoryCard;
  const auto path = (directory / "blocked.mcd").string();
  { Card card; ASSERT_TRUE(card.attachFile(path)); ASSERT_EQ(card.createFile("progress", 1), 1); }
  std::filesystem::remove_all(path + ".backups"); std::ofstream(path + ".backups") << "blocked";
  Card card; ASSERT_TRUE(card.attachFile(path)); const auto before = card.snapshot();
  uint8_t value = 42; EXPECT_EQ(card.writeFile(1, 0, &value, 1), -1); EXPECT_EQ(card.snapshot().data, before.data);
  Card reopened; ASSERT_TRUE(reopened.loadFromFile(path)); EXPECT_EQ(reopened.snapshot().data, before.data);
}

TEST_F(PersistentCardTest, MissingCardWithUnreadableHistoryDoesNotBecomeBlank) {
  const auto path = (directory / "missing.mcd").string();
  std::filesystem::create_directory(path + ".backups"); std::ofstream(path + ".backups/001.bak") << "truncated";
  ps1::input::MemoryCard card; EXPECT_FALSE(card.attachFile(path)); EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(PersistentCardTest, BadHeaderWithCorrectFileSizeUsesRecoveryWithoutMutatingOriginal) {
  const auto path = (directory / "header.mcd").string();
  { ps1::input::MemoryCard card; ASSERT_TRUE(card.attachFile(path)); ASSERT_EQ(card.createFile("progress", 1), 1); }
  { std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary); file.put('X'); }
  ps1::input::MemoryCard card; ASSERT_TRUE(card.attachFile(path)); EXPECT_FALSE(card.recoverySource().empty());
  EXPECT_EQ(std::filesystem::file_size(path), ps1::input::MemoryCard::CARD_SIZE);
  std::ifstream original(path); EXPECT_EQ(original.get(), 'X');
}

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

namespace {
void directoryChecksum(ps1::input::MemoryCard::Snapshot &state, unsigned block) {
  const unsigned start = block * 128;
  uint8_t check = 0;
  for (unsigned i = 0; i < 127; ++i) check ^= state.data[start + i];
  state.data[start + 127] = check;
}
}

TEST_F(PersistentCardTest, CrossLinkedChainsCannotReadWriteOrEraseAnotherFile) {
  using Card = ps1::input::MemoryCard;
  for (bool sharedTail : {false, true}) {
    Card card;
    ASSERT_EQ(card.createFile("first", 2), 1);
    ASSERT_EQ(card.createFile("second", 1), 3);
    auto corrupt = card.snapshot();
    const unsigned changed = sharedTail ? 3 : 1;
    corrupt.data[changed * 128 + 8] = sharedTail ? 1 : 2;
    corrupt.data[changed * 128 + 9] = 0;
    directoryChecksum(corrupt, changed);
    card.restore(corrupt);
    uint8_t value = 42;
    EXPECT_EQ(card.readFile(changed, 0, &value, 1), -1);
    EXPECT_EQ(card.writeFile(changed, 0, &value, 1), -1);
    EXPECT_FALSE(card.eraseFile(sharedTail ? "second" : "first"));
    EXPECT_EQ(card.snapshot().data, corrupt.data);
  }
}

TEST_F(PersistentCardTest, EveryInvalidDirectoryHeadStateAndBadChecksumAreRejected) {
  ps1::input::MemoryCard card;
  ASSERT_EQ(card.createFile("file", 1), 1);
  const auto valid = card.snapshot();
  for (unsigned status = 0; status < 256; ++status) {
    if (status == 0x51) continue;
    auto corrupt = valid;
    corrupt.data[128] = status;
    directoryChecksum(corrupt, 1);
    card.restore(corrupt);
    EXPECT_TRUE(card.fileChain(1).empty()) << "status " << status;
  }
  auto corrupt = valid;
  corrupt.data[128 + 127] ^= 1;
  card.restore(corrupt);
  EXPECT_TRUE(card.fileChain(1).empty());
  EXPECT_FALSE(card.eraseFile("file"));
  EXPECT_EQ(card.snapshot().data, corrupt.data);
}

TEST_F(PersistentCardTest, FragmentedAllocationPreservesNeighboursAcrossEveryBoundary) {
  using Card = ps1::input::MemoryCard;
  Card card;
  const auto path = (directory / "fragmented.mcd").string();
  ASSERT_TRUE(card.attachFile(path));
  for (unsigned i = 1; i <= 15; ++i) {
    ASSERT_EQ(card.createFile(std::to_string(i), 1), int(i));
    const uint8_t marker = i;
    ASSERT_EQ(card.writeFile(i, 0, &marker, 1), 1);
  }
  for (unsigned i = 2; i <= 14; i += 2) ASSERT_TRUE(card.eraseFile(std::to_string(i)));
  ASSERT_EQ(card.createFile("fragmented", 7), 2);
  EXPECT_EQ(card.fileChain(2), (std::vector<unsigned>{2,4,6,8,10,12,14}));
  std::vector<uint8_t> expected(7 * Card::BLOCK_SIZE);
  std::iota(expected.begin(), expected.end(), uint8_t(37));
  ASSERT_EQ(card.writeFile(2, 0, expected.data(), expected.size()), int(expected.size()));
  Card reopened;
  ASSERT_TRUE(reopened.attachFile(path));
  for (unsigned boundary = 1; boundary < 7; ++boundary) {
    for (int delta : {-1, 0, 1}) {
      const unsigned offset = boundary * Card::BLOCK_SIZE + delta;
      uint8_t actual[9]{};
      ASSERT_EQ(reopened.readFile(2, offset, actual, sizeof(actual)), 9);
      EXPECT_TRUE(std::equal(std::begin(actual), std::end(actual), expected.begin() + offset));
    }
  }
  for (unsigned i = 1; i <= 15; i += 2) {
    uint8_t marker = 0;
    EXPECT_EQ(reopened.readFile(i, 0, &marker, 1), 1);
    EXPECT_EQ(marker, i);
  }
  EXPECT_EQ(reopened.createFile("overflow", 1), 0);
}
