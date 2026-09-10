#include "runtime/bios/file_io.h"
#include <cstdint>
#include <fmt/core.h>
#include <string>
#include "runtime/input/input.h"
#include <algorithm>

namespace ps1::bios {

FileIO::FileIO(cdrom::VirtualFs &fs, Memory &mem) : fs_(fs), mem_(mem) {
  fds_.resize(MAX_FDS);
  for (auto &fd : fds_) {
    fd.inUse = false;
    fd.cursor = 0;
  }
}

int32_t FileIO::open(const char *path, int32_t mode) {
  std::string pathStr(path);
  fmt::print("[BIOS] open('{}', {})\n", pathStr, mode);

  int32_t fdIdx = -1;
  for (int32_t i = 3; i < MAX_FDS; i++) {
    if (!fds_[i].inUse) {
      fdIdx = i;
      break;
    }
  }

  if (fdIdx == -1) {
    fmt::print("[BIOS] open failed: Too many open files\n");
    return -1;
  }

  if (pathStr.rfind("bu00:", 0) == 0 || pathStr.rfind("bu10:", 0) == 0) {
    const unsigned slot = pathStr[2] == '1' ? 1 : 0;
    if (!input_ || !input_->getMemoryCard(slot).isPresent()) return -1;
    auto& card = input_->getMemoryCard(slot);
    const auto name = pathStr.substr(5);
    int first = card.findFile(name);
    if (!first && (mode & 0x200)) first = card.createFile(name, uint32_t(mode) >> 16);
    if (!first || card.fileChain(first).empty()) return -1;
    auto& file = fds_[fdIdx];
    file.inUse = true; file.path = pathStr; file.cursor = 0;
    file.card = &card; file.firstBlock = first; file.cardSlot = slot;
    return fdIdx;
  }

  // In actual PS1, "cdrom:" specifies the device.
  // Usually paths look like "cdrom:\\SYSTEM.CNF;1"
  std::string fsPath = pathStr;

  // Rudimentary stripping of cdrom prefix and version endings
  if (fsPath.find("cdrom:") == 0) {
    fsPath = fsPath.substr(6);
  }

  // Remove leading slashes/backslashes
  while (!fsPath.empty() && (fsPath[0] == '\\' || fsPath[0] == '/')) {
    fsPath = fsPath.substr(1);
  }

  // Remove version suffix like ";1"
  size_t semPos = fsPath.find(';');
  if (semPos != std::string::npos) {
    fsPath = fsPath.substr(0, semPos);
  }

  // Read the file from the CD-ROM filesystem
  auto fileData = fs_.readFile(fsPath);
  if (!fileData) {
    fmt::print("[BIOS] open failed: File not found '{}'\n", fsPath);
    return -1;
  }

  fds_[fdIdx].card = nullptr;
  fds_[fdIdx].inUse = true;
  fds_[fdIdx].path = fsPath;
  fds_[fdIdx].data = std::move(*fileData);
  fds_[fdIdx].cursor = 0;

  return fdIdx; // Small numbers returned to the game
}

int32_t FileIO::close(int32_t fd) {
  fmt::print("[BIOS] close({})\n", fd);
  if (fd < 0 || fd >= MAX_FDS || !fds_[fd].inUse)
    return -1;

  if (fds_[fd].card && !fds_[fd].card->flush()) return -1;
  fds_[fd].card = nullptr;
  fds_[fd].inUse = false;
  fds_[fd].data.clear();
  fds_[fd].path.clear();
  fds_[fd].cursor = 0;

  return fd;
}

int32_t FileIO::read(int32_t fd, uint32_t dstAddr, int32_t length) {
  if (fd < 0 || fd >= MAX_FDS || !fds_[fd].inUse)
    return -1;

  auto &fDesc = fds_[fd];
  if (length < 0) return -1;
  if (fDesc.card) {
    if (!fDesc.card->isPresent()) return -1;
    std::vector<uint8_t> bytes(std::min<uint32_t>(length, input::MemoryCard::CARD_SIZE));
    const int result = fDesc.card->readFile(fDesc.firstBlock, fDesc.cursor, bytes.data(), bytes.size());
    if (result >= 0) {
      for (int i = 0; i < result; ++i) mem_.write8(dstAddr + i, bytes[i]);
      fDesc.cursor += result;
    }
    if (cardCompletion_) cardCompletion_(fDesc.cardSlot, result >= 0 ? result : 0, result >= 0);
    return result;
  }
  int32_t bytesToRead = length;
  int32_t available = (int32_t)fDesc.data.size() - fDesc.cursor;

  if (bytesToRead > available) {
    bytesToRead = available;
  }

  if (bytesToRead <= 0)
    return 0; // EOF

  // Copy to emulated memory
  for (int32_t i = 0; i < bytesToRead; i++) {
    mem_.write8(dstAddr + i, fDesc.data[fDesc.cursor + i]);
  }

  fDesc.cursor += bytesToRead;

  fmt::print("[BIOS] read(fd: {}, dst: 0x{:08X}, len: {}) -> read {}\n", fd,
             dstAddr, length, bytesToRead);

  // Notify overlay manager that data was written to RAM
  if (overlayMgr_) {
    overlayMgr_->notifyMemWrite(dstAddr, static_cast<uint32_t>(bytesToRead));
  }

  return bytesToRead;
}

int32_t FileIO::lseek(int32_t fd, int32_t offset, int32_t whence) {
  if (fd < 0 || fd >= MAX_FDS || !fds_[fd].inUse)
    return -1;

  auto &fDesc = fds_[fd];
  const int32_t fileSize = fDesc.card ? fDesc.card->fileSize(fDesc.firstBlock) : fDesc.data.size();
  int64_t newCursor = fDesc.cursor;

  switch (whence) {
  case 0: // SEEK_SET
    newCursor = offset;
    break;
  case 1: // SEEK_CUR
    newCursor += offset;
    break;
  case 2: // SEEK_END
    newCursor = int64_t(fileSize) + offset;
    break;
  default:
    return -1;
  }

  if (newCursor < 0)
    newCursor = 0;
  if (newCursor > fileSize)
    newCursor = fileSize;

  fDesc.cursor = newCursor;

  fmt::print("[BIOS] lseek(fd: {}, off: {}, whence: {}) -> pos {}\n", fd,
             offset, whence, newCursor);

  return newCursor;
}

} // namespace ps1::bios

namespace ps1::bios {
int32_t FileIO::write(int32_t fd, uint32_t source, int32_t size) {
  if (fd < 0 || fd >= MAX_FDS || !fds_[fd].inUse || size < 0) return -1;
  auto& file = fds_[fd];
  if (!file.card || !file.card->isPresent()) return -1;
  std::vector<uint8_t> bytes(std::min<uint32_t>(size, input::MemoryCard::CARD_SIZE));
  for (unsigned i = 0; i < bytes.size(); ++i) bytes[i] = mem_.read8(source + i);
  const int result = file.card->writeFile(file.firstBlock, file.cursor, bytes.data(), bytes.size());
  if (result >= 0) file.cursor += result;
  if (cardCompletion_) cardCompletion_(file.cardSlot, result >= 0 ? result : 0, result >= 0);
  return result;
}
namespace {
bool matches(const char* pattern, const char* name) {
  if (!*pattern) return !*name;
  if (*pattern == '*') return matches(pattern + 1, name) || (*name && matches(pattern, name + 1));
  return *name && (*pattern == '?' || *pattern == *name) && matches(pattern + 1, name + 1);
}
}
uint32_t FileIO::firstFile(const std::string& pattern, uint32_t output) {
  directory_.clear(); directoryIndex_ = 0;
  if (!input_ || (pattern.rfind("bu00:", 0) && pattern.rfind("bu10:", 0))) return 0;
  auto& card = input_->getMemoryCard(pattern[2] == '1' ? 1 : 0);
  if (!card.isPresent() || pattern.size() > 25) return 0;
  for (unsigned block = 1; block <= input::MemoryCard::NUM_BLOCKS; ++block) {
    const auto name = card.fileName(block);
    if (!name.empty() && card.findFile(name) == int(block) && matches(pattern.c_str() + 5, name.c_str()))
      directory_.push_back({name, card.fileSize(block)});
  }
  return nextFile(output);
}
uint32_t FileIO::nextFile(uint32_t output) {
  if (!output || directoryIndex_ >= directory_.size()) return 0;
  const auto& entry = directory_[directoryIndex_++];
  for (unsigned i = 0; i < 40; ++i) mem_.write8(output + i, 0);
  for (unsigned i = 0; i < entry.first.size(); ++i) mem_.write8(output + i, entry.first[i]);
  mem_.write32(output + 20, 0x51);
  mem_.write32(output + 24, entry.second);
  return output;
}
bool FileIO::erase(const std::string& path) {
  if (!input_ || (path.rfind("bu00:", 0) && path.rfind("bu10:", 0))) return false;
  auto& card = input_->getMemoryCard(path[2] == '1' ? 1 : 0);
  return card.isPresent() && card.eraseFile(path.substr(5));
}
}

namespace ps1::bios {
bool FileIO::format(const std::string& path) {
  if (!input_ || (path != "bu00:" && path != "bu10:")) return false;
  auto& card = input_->getMemoryCard(path[2] == '1' ? 1 : 0);
  if (!card.isPresent()) return false;
  const auto before = card.snapshot();
  card.reset();
  if (!card.flush()) { card.restore(before); return false; }
  return true;
}
}
