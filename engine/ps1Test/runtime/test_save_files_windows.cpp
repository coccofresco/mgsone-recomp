#include "runtime/save_files.h"
#include <gtest/gtest.h>
#include <windows.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
class WindowsSaveFilesTest : public ::testing::Test {
protected:
  fs::path dir, path;
  void SetUp() override {
    dir = fs::temp_directory_path() / (L"mgs-salvataggi-\u65e5\u672c-" + std::to_wstring(GetCurrentProcessId()));
    ASSERT_TRUE(fs::create_directory(dir));
    path = dir / L"stato-\u00e8.ps1s";
  }
  void TearDown() override { ps1::storage::takeWarning(); fs::remove_all(dir); }
  std::string read(const fs::path& file) { std::ifstream in(file); return {std::istreambuf_iterator<char>(in), {}}; }
  bool write(const std::string& text) {
    ps1::storage::Transaction transaction(path);
    { std::ofstream stream(transaction.temporary()); stream << text; if (!stream) return false; }
    return transaction.commit();
  }
};

TEST_F(WindowsSaveFilesTest, UnicodePathsAtomicReplacementAndThreeBackups) {
  for (int i=0; i<8; ++i) ASSERT_TRUE(write(std::to_string(i)));
  EXPECT_EQ(read(path), "7");
  auto backups = ps1::storage::backups(path);
  ASSERT_EQ(backups.size(), 3u);
  for (int i=0; i<3; ++i) EXPECT_EQ(read(backups[i]), std::to_string(6-i));
  ASSERT_TRUE(write("7"));
  EXPECT_EQ(backups, ps1::storage::backups(path));
}

TEST_F(WindowsSaveFilesTest, AbandonedOrFailedBackupPreservesPrimary) {
  ASSERT_TRUE(write("old"));
  fs::path pending;
  { ps1::storage::Transaction transaction(path); pending = transaction.temporary(); std::ofstream(pending) << "partial"; }
  EXPECT_FALSE(fs::exists(pending));
  EXPECT_EQ(read(path), "old");
  std::ofstream(fs::path(path.wstring()+L".backups")) << "obstruction";
  EXPECT_FALSE(write("new"));
  EXPECT_EQ(read(path), "old");
}

TEST_F(WindowsSaveFilesTest, SharingViolationRetainsSaveAndRecoveryPoint) {
  ASSERT_TRUE(write("old"));
  HANDLE held = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, 0, nullptr); // deny replacement
  ASSERT_NE(held, INVALID_HANDLE_VALUE);
  EXPECT_FALSE(write("new"));
  EXPECT_EQ(read(path), "old");
  CloseHandle(held);
  ASSERT_TRUE(write("new"));
  EXPECT_EQ(read(path), "new");
  EXPECT_EQ(read(ps1::storage::backups(path).front()), "old");
}
