#include "runtime/save_files.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <fstream>
#include <mutex>
#include <system_error>
#include <utility>
#if defined(_WIN32)
#include <windows.h>
#include <atomic>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include <fmt/core.h>

namespace ps1::storage {
namespace {
namespace fs = std::filesystem;
std::mutex warningMutex;
std::string warning;
fs::path parent(const fs::path &path) { return path.has_parent_path() ? path.parent_path() : fs::path("."); }
fs::path history(const fs::path &path) { return path.string() + ".backups"; }
void fail(const char *operation) { throw std::system_error(errno, std::generic_category(), operation); }
void regularOrAbsent(const fs::path &path) {
  const auto status = fs::symlink_status(path);
  if (fs::exists(status) && !fs::is_regular_file(status))
    throw std::runtime_error("save target is not a regular file: " + path.string());
}
fs::path temporary(const fs::path &dir, const std::string &prefix) {
#if defined(_WIN32)
  static std::atomic<uint64_t> serial{0};
  for (unsigned attempt = 0; attempt < 128; ++attempt) {
    const auto name = dir / (prefix + std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
        std::to_string(serial.fetch_add(1)));
    HANDLE file = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); return name; }
    const auto error = GetLastError();
    if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
      throw std::system_error(error, std::system_category(), "create save temporary");
  }
  throw std::runtime_error("cannot allocate unique save temporary");
#else
  std::string name = (dir / (prefix + "XXXXXX")).string();
  int fd = ::mkstemp(name.data());
  if (fd < 0) fail("create save temporary");
  if (::close(fd) != 0) { const int error = errno; ::unlink(name.c_str()); errno = error; fail("close save temporary"); }
  return name;
#endif
}
void sync(const fs::path &path, bool directory = false) {
#if defined(_WIN32)
  // Windows has no supported equivalent of POSIX directory fsync. File data
  // is flushed here and publication uses a same-volume write-through rename.
  if (directory) return;
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    throw std::system_error(GetLastError(), std::system_category(), "open save for flush");
  const bool ok = FlushFileBuffers(file);
  const auto error = GetLastError();
  CloseHandle(file);
  if (!ok) throw std::system_error(error, std::system_category(), "flush save");
#else
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (directory ? O_DIRECTORY : 0));
  if (fd < 0) fail("open for save sync");
  int result;
  do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
  const int error = errno;
  const int closed = ::close(fd);
  if (result < 0) { errno = error; fail("sync save"); }
  if (closed < 0) fail("close synced save");
#endif
}
void publish(const fs::path &from, const fs::path &to) {
#if defined(_WIN32)
  if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw std::system_error(GetLastError(), std::system_category(), "publish save");
#else
  fs::rename(from, to);
#endif
}
bool identical(const fs::path &a, const fs::path &b) {
  if (!fs::exists(b) || fs::file_size(a) != fs::file_size(b)) return false;
  std::ifstream left(a, std::ios::binary), right(b, std::ios::binary);
  if (!left || !right) throw std::runtime_error("cannot compare previous save");
  std::array<char, 16384> x{}, y{};
  while (left) {
    left.read(x.data(), x.size()); right.read(y.data(), y.size());
    if (left.gcount() != right.gcount() || !std::equal(x.begin(), x.begin() + left.gcount(), y.begin())) return false;
  }
  if (!left.eof() || right.bad()) throw std::runtime_error("cannot read previous save");
  return true;
}
void preserve(const fs::path &target) {
  const auto dir = history(target);
  const auto status = fs::symlink_status(dir);
  if (fs::exists(status) && !fs::is_directory(status)) throw std::runtime_error("invalid save backup directory");
  fs::create_directory(dir);
  auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  // Wall-clock correction/suspend must not make a new recovery point sort
  // before older ones and get pruned immediately. The profile has one writer.
  for (const auto &entry : backups(target)) {
    const auto name = entry.filename().string();
    int64_t previous = 0;
    const auto end = name.find('-');
    if (end != std::string::npos) {
      const auto parsed = std::from_chars(name.data(), name.data() + end, previous);
      if (parsed.ec == std::errc{} && parsed.ptr == name.data() + end && previous < INT64_MAX)
        stamp = std::max<int64_t>(stamp, previous + 1);
    }
  }
  auto temp = temporary(dir, ".pending-");
  try {
    fs::copy_file(target, temp, fs::copy_options::overwrite_existing);
    fs::last_write_time(temp, fs::last_write_time(target));
    sync(temp);
    publish(temp, dir / (std::to_string(stamp) + "-" + temp.filename().string().substr(9) + ".bak"));
    sync(dir, true);
    sync(parent(target), true); // persist a newly created history directory too
  } catch (...) { std::error_code ignored; fs::remove(temp, ignored); throw; }
}
}

Transaction::Transaction(const fs::path &target) : target_(target) {
  regularOrAbsent(target_);
  temporary_ = storage::temporary(parent(target_), "." + target_.filename().string() + ".pending-");
}
Transaction::~Transaction() { std::error_code ignored; if (!temporary_.empty()) fs::remove(temporary_, ignored); }
std::string takeWarning() { std::lock_guard lock(warningMutex); return std::exchange(warning, {}); }
std::vector<fs::path> backups(const fs::path &target) {
  std::vector<fs::path> result;
  std::error_code error;
  if (!fs::is_directory(fs::symlink_status(history(target), error)) || error) return result;
  for (fs::directory_iterator it(history(target), error), end; !error && it != end; it.increment(error)) {
    if (it->path().extension() == ".bak" && fs::is_regular_file(it->symlink_status(error)) && !error)
      result.push_back(it->path());
  }
  std::sort(result.begin(), result.end(), std::greater<fs::path>());
  return result;
}
bool Transaction::commit(bool backupPrevious, bool *changed) {
  if (changed) *changed = false;
  try {
    regularOrAbsent(target_);
    if (identical(temporary_, target_)) return true; // don't rotate unchanged cards/states
    sync(temporary_);
    if (backupPrevious && fs::exists(target_)) preserve(target_);
    publish(temporary_, target_);
    temporary_.clear();
    if (changed) *changed = true;
  } catch (const std::exception &error) {
    fmt::print(stderr, "[SaveFiles] Save rejected for '{}': {}\n", target_.string(), error.what());
    return false;
  }
  // Once rename succeeds the new file is authoritative. A directory-sync
  // failure cannot be reported as an unpublished write (card RAM would roll
  // back while disk already contains the new bytes). Retain every backup.
  try { sync(parent(target_), true); }
  catch (const std::exception &error) {
    fmt::print(stderr, "[SaveFiles] Published '{}', durability warning: {}\n", target_.string(), error.what());
    std::lock_guard lock(warningMutex);
    warning = "Saved " + target_.filename().string() + ", but disk synchronization failed. Previous backups retained. Check disk space and export a backup from the launcher.";
    return true;
  }
  const auto previous = backups(target_);
  for (size_t i = kBackupCount; i < previous.size(); ++i) {
    std::error_code ignored; fs::remove(previous[i], ignored);
  }
  return true;
}
} // namespace ps1::storage
