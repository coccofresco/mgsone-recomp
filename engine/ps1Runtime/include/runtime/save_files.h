#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace ps1::storage {
// Linux alpha: single profile writer (launcher lock), same-filesystem publish.
// The live file stays in place until a complete, synced replacement is ready.
class Transaction {
public:
  explicit Transaction(const std::filesystem::path &target);
  ~Transaction();
  Transaction(const Transaction &) = delete;
  Transaction &operator=(const Transaction &) = delete;
  const std::filesystem::path &temporary() const { return temporary_; }
  bool commit(bool backupPrevious = true, bool *changed = nullptr);
private:
  std::filesystem::path target_, temporary_;
};
// Newest first; only committed regular backup files, never unfinished writes.
std::vector<std::filesystem::path> backups(const std::filesystem::path &target);
// Cross-thread notice when publication succeeds but final durability cannot
// be confirmed. The player UI must not silently label this a durable save.
std::string takeWarning();
constexpr unsigned kBackupCount = 3;
} // namespace ps1::storage
