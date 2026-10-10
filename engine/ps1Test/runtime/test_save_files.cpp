#include "runtime/save_files.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <cerrno>

// Link-time syscall injection only in the test executable, never the game.
static int failSyncAt = 0, syncCalls = 0, syncError = ENOSPC;
static bool exitAtSync = false;
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  if (failSyncAt && ++syncCalls == failSyncAt) {
    if (exitAtSync) _exit(0);
    errno = syncError; return -1;
  }
  return __real_fsync(fd);
}

namespace fs = std::filesystem;
using ps1::storage::Transaction;
class SaveFilesTest : public ::testing::Test {
protected:
  fs::path dir, path;
  void SetUp() override {
    std::string pattern = (fs::temp_directory_path() / "mgs-save-files-XXXXXX").string();
    ASSERT_NE(mkdtemp(pattern.data()), nullptr);
    dir = pattern; path = dir / "slot.ps1s";
  }
  void TearDown() override { failSyncAt = syncCalls = 0; exitAtSync = false; ps1::storage::takeWarning(); fs::permissions(dir, fs::perms::owner_all); fs::remove_all(dir); }
  static std::string read(const fs::path &p) { std::ifstream f(p); return {std::istreambuf_iterator<char>(f), {}}; }
  bool write(const std::string &text) {
    Transaction file(path);
    { std::ofstream out(file.temporary()); out << text; if (!out) return false; }
    return file.commit();
  }
};

TEST_F(SaveFilesTest, RetainsThreePreviousVersionsAndIgnoresIdenticalWrites) {
  for (unsigned i = 0; i < 8; ++i) ASSERT_TRUE(write(std::to_string(i)));
  auto backups = ps1::storage::backups(path);
  ASSERT_EQ(backups.size(), 3u);
  EXPECT_EQ(read(path), "7");
  for (unsigned i = 0; i < 3; ++i) EXPECT_EQ(read(backups[i]), std::to_string(6-i));
  ASSERT_TRUE(write("7"));
  EXPECT_EQ(ps1::storage::backups(path), backups);
}

TEST_F(SaveFilesTest, AbandonedWriteDoesNotTouchPrimaryOrHistory) {
  ASSERT_TRUE(write("old"));
  fs::path pending;
  { Transaction file(path); pending = file.temporary(); std::ofstream(pending) << "incomplete"; }
  EXPECT_FALSE(fs::exists(pending));
  EXPECT_EQ(read(path), "old");
  EXPECT_TRUE(ps1::storage::backups(path).empty());
}

TEST_F(SaveFilesTest, BackwardWallClockCannotEvictNewestRecoveryPoint) {
  ASSERT_TRUE(write("first")); ASSERT_TRUE(write("second"));
  auto previous = ps1::storage::backups(path); ASSERT_EQ(previous.size(), 1u);
  // A prior session's clock was ahead of the current time.
  fs::rename(previous[0], previous[0].parent_path() / "2000000000000000000-clock.bak");
  ASSERT_TRUE(write("third"));
  previous = ps1::storage::backups(path); ASSERT_EQ(previous.size(), 2u);
  EXPECT_EQ(read(previous[0]), "second"); EXPECT_EQ(read(previous[1]), "first");
}

TEST_F(SaveFilesTest, AbruptProcessExitLeavesPreviousSaveReadableAndTemporaryIgnored) {
  ASSERT_TRUE(write("old"));
  const auto pid = fork(); ASSERT_GE(pid, 0);
  if (pid == 0) {
    Transaction file(path); std::ofstream out(file.temporary()); out << "partial"; out.flush();
    _exit(0); // deliberately skip destructors, as a killed writer would
  }
  int status; ASSERT_EQ(waitpid(pid, &status, 0), pid); ASSERT_EQ(status, 0);
  EXPECT_EQ(read(path), "old"); EXPECT_TRUE(ps1::storage::backups(path).empty());
  ASSERT_TRUE(write("new")); EXPECT_EQ(read(path), "new");
  EXPECT_EQ(read(ps1::storage::backups(path).front()), "old");
}

TEST_F(SaveFilesTest, FileSizeLimitShortWriteCannotReplaceOrRotateExistingSave) {
  ASSERT_TRUE(write("old"));
  const auto pid = fork(); ASSERT_GE(pid, 0);
  if (pid == 0) {
    signal(SIGXFSZ, SIG_IGN);
    rlimit limit{16,16}; if (setrlimit(RLIMIT_FSIZE, &limit)) _exit(2);
    bool failed;
    { Transaction file(path); std::ofstream out(file.temporary()); out << std::string(4096, 'x'); out.close(); failed = !out; }
    _exit(failed ? 0 : 3);
  }
  int status; ASSERT_EQ(waitpid(pid, &status, 0), pid); ASSERT_EQ(status, 0);
  EXPECT_EQ(read(path), "old"); EXPECT_TRUE(ps1::storage::backups(path).empty());
}

TEST_F(SaveFilesTest, BackupFailurePreservesLiveFile) {
  ASSERT_TRUE(write("old")); std::ofstream(path.string()+".backups") << "obstruction";
  EXPECT_FALSE(write("new")); EXPECT_EQ(read(path), "old");
}

TEST_F(SaveFilesTest, DiskFullAndIoFailureBeforePublicationLeaveLiveSaveIntact) {
  ASSERT_TRUE(write("old"));
  for (int error : {ENOSPC, EIO, EROFS}) for (int phase : {1, 2, 3, 4}) {
    syncError = error; syncCalls = 0; failSyncAt = phase;
    EXPECT_FALSE(write("new")) << "phase " << phase << " errno " << error;
    EXPECT_EQ(read(path), "old");
    for (const auto &backup : ps1::storage::backups(path)) EXPECT_EQ(read(backup), "old");
  }
  failSyncAt = 0;
  ASSERT_TRUE(write("new")); EXPECT_LE(ps1::storage::backups(path).size(), 3u);
}

TEST_F(SaveFilesTest, InterruptedAfterBackupCommitStillKeepsOriginalAndRecoveryCopy) {
  ASSERT_TRUE(write("old"));
  const auto pid = fork(); ASSERT_GE(pid, 0);
  if (pid == 0) { failSyncAt = 4; syncCalls = 0; exitAtSync = true; write("new"); _exit(4); }
  int status; ASSERT_EQ(waitpid(pid, &status, 0), pid); ASSERT_EQ(status, 0);
  EXPECT_EQ(read(path), "old");
  auto history = ps1::storage::backups(path); ASSERT_EQ(history.size(), 1u);
  EXPECT_EQ(read(history.front()), "old");
}

TEST_F(SaveFilesTest, DirectorySyncFailureAfterRenameNeverClaimsWriteWasRolledBack) {
  ASSERT_TRUE(write("old"));
  failSyncAt = 5; syncCalls = 0; syncError = EIO;
  EXPECT_TRUE(write("new")); EXPECT_EQ(read(path), "new");
  EXPECT_NE(ps1::storage::takeWarning().find("disk synchronization failed"), std::string::npos);
  auto history = ps1::storage::backups(path); ASSERT_EQ(history.size(), 1u);
  EXPECT_EQ(read(history.front()), "old");
}

TEST_F(SaveFilesTest, UnwritableDirectoryFailsWithoutReplacingExistingData) {
  if (geteuid() == 0) GTEST_SKIP() << "root bypasses ordinary directory permissions";
  ASSERT_TRUE(write("old"));
  fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec);
  EXPECT_THROW(Transaction file(path), std::system_error);
  EXPECT_EQ(read(path), "old");
}

TEST_F(SaveFilesTest, SymlinksAreNeverFollowedForSavesOrHistory) {
  const auto other = dir / "other"; std::ofstream(other) << "untouched";
  fs::create_symlink(other, path);
  EXPECT_THROW(Transaction file(path), std::runtime_error);
  fs::remove(path); ASSERT_TRUE(write("old"));
  const auto external = dir / "external"; fs::create_directory(external);
  fs::create_directory_symlink(external, path.string()+".backups");
  EXPECT_FALSE(write("new")); EXPECT_EQ(read(path), "old"); EXPECT_EQ(read(other), "untouched");
  EXPECT_TRUE(ps1::storage::backups(path).empty());
  EXPECT_TRUE(fs::is_empty(external));
}
